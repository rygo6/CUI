/* Vulkan renderer (cui_gpu_*, built with CUI_VULKAN; Linux, on the X11 window, or on the
 * Wayland one when built with CUI_PLATFORM=wayland):the draw data the OpenGL renderer takes (cui_draw_data's vertices, indices and
 * commands), drawn the same way. One pipeline; its fragment shader is cui_render_gl.c's
 * (cui_render_vulkan.frag), branching on the vertex's mode. Keep the two in step.
 *
 * - Vulkan 1.3 (dynamic rendering) with a timeline semaphore. CUI_GPU picks the device,
 *   (the validation layer is switched on outside the program, with VK_LOADER_LAYERS_ENABLE).
 * - Two command buffers in turn, each reused once the timeline value of its last submission
 *   has passed: texture uploads go in as CUI makes them, the frame's draws after.
 * - A frame's vertices and indices are copied into a mapped buffer (one per command buffer),
 *   then a draw per command with its clip rectangle as the scissor and its texture's
 *   descriptor set (a set per texture, written once when the texture is made).
 * - Drawn straight into a UNORM swapchain image, premultiplied source over, as the OpenGL
 *   renderer blends. Paced by acquiring the image (FIFO with vsync). */
#include "../cui_internal.h"
#include "cui/cui_render.h"
#include "cui_pool.h"

#if defined(CUI_VULKAN) && defined(__linux__)

#ifdef CUI_WAYLAND   /* the app's window is a Wayland one (CUI_PLATFORM=wayland), otherwise an X11 one */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#else
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#endif
#include <vulkan/vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "generated/cui_render_vulkan_spirv.h"

#define CUI__TV_FRAMES 2            /* command buffers (and upload buffers) in flight */
#define CUI__TV_IMAGES 3            /* swapchain images asked for */
#define CUI__TV_MAX_IMAGES 16
#define CUI__TV_TEX_CAP 4096        /* textures alive at once */
#define CUI__TV_MISSED 24           /* boxes an image can be behind by before all of it is copied */
#define CUI__TV_ZONES (CUI_SURFACES + 1)   /* the surfaces, and the window's own list on a surface fitted to it (slot -1) */
#define CUI__TV_ACQUIRE_NS 2000000  /* how long a surface's draw waits for an image the compositor still holds: then it is the next frame's */

typedef struct cui__vk_buffer {     /* a mapped buffer */
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *ptr;
    size_t cap, used;
} cui__vk_buffer;

typedef struct cui__vk_tex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    int w, h;
    int bpp;                        /* 0: the slot is free */
    bool borrowed;                  /* the app's image view (cui_gpu_texture_wrap): only the set is the renderer's */
} cui__vk_tex;

/* what goes once the GPU is past `value` */
typedef struct cui__vk_later {
    uint64_t value;
    VkBuffer buffer; VkImage image; VkImageView view; VkDeviceMemory memory; VkDescriptorSet set;
    int slot;                       /* a texture slot to free, or -1 */
} cui__vk_later;

/* A surface of the UI on a surface of the system's own (a Wayland subsurface), with a swapchain of its size.
 * Its images are all the pictures it has: one that comes round again is redrawn only where the
 * frames it missed changed. */
#define CUI__TV_FIT 64   /* a renderer owned surface's images are a multiple of this many pixels each way */

/* a picture handed to the compositor as a dma-buf wl_buffer, at its pool slot's index */
typedef struct cui__vk_plain {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSemaphore reads;              /* waits for the compositor's reads before the picture is drawn into again */
    void *wl_buffer;
    int fd;                         /* the dma-buf */
    VkBuffer linear;                /* plain_copied: what the compositor reads, in system memory */
    VkDeviceMemory linear_memory;
    uint32_t pitch;                 /* linear's bytes a row */
    uint32_t w, h;
    int part_x, part_y, part_w, part_h;   /* the part of the list's frame it was drawn for */
    bool made;
} cui__vk_plain;

typedef struct cui__vk_zone {
    void *wl_surface;               /* the app's: where it lies and above what is the app's to say */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[CUI__TV_MAX_IMAGES];
    VkImageView views[CUI__TV_MAX_IMAGES];
    VkSemaphore present_sem[CUI__TV_MAX_IMAGES];
    VkSemaphore acquire_sem[CUI__TV_FRAMES];
    bool can_capture;               /* its images can be copied from */
    /* on a program's surface (cui_x11_window.host) the renderer makes, places and hides the subsurface itself */
    bool own;                       /* wl_surface is the renderer's */
    bool shown;
    void *sub, *viewport;           /* wl_subsurface *, wp_viewport * */
    int x, y, w, h, order;          /* where its picture lies in the window, pixels */
    int src_w, src_h;               /* the part of its images the viewport shows, when the renderer owns it */
    int part_x, part_y;             /* where its picture's top left lies in its list's frame */
    uint64_t value;                 /* the last submission that drew into its images */
    cui_rect missed[CUI__TV_MAX_IMAGES][CUI__TV_MISSED];   /* what each image has not seen since it was last drawn, in the list's pixels */
    int missed_count[CUI__TV_MAX_IMAGES];                   /* -1: all of it */
    cui_id element;                 /* whose pictures the pool gives it; 0: the slot's own */
    uint16_t plain_key;             /* the pool bucket of its pictures; 0: it has none */
    int plain_front;                /* the pool slot last given to the compositor, plus one */
    bool tracked;                   /* its tracker is the pool's */
    bool attached;                  /* a picture of this element is on its surface */
} cui__vk_zone;

/* a swapchain that was replaced or given back: it goes once the GPU is past `value` */
typedef struct cui__vk_retired {
    uint64_t value;
    VkSwapchainKHR swapchain;
    VkSurfaceKHR surface;
    VkSurfaceKHR on;   /* the surface it still holds, when that surface stays */
    VkImageView views[CUI__TV_MAX_IMAGES];
    VkSemaphore sems[CUI__TV_MAX_IMAGES + CUI__TV_FRAMES];
} cui__vk_retired;

/* a plain picture drawn in the open command buffer, to be shown with its submission */
typedef struct cui__vk_shown {
    cui__vk_zone *zone;
    uint32_t image;
} cui__vk_shown;

struct cui_gpu_renderer {
    cui_x11_window *xwin;
    bool vsync;
    bool shared;                    /* the instance and the device are the app's (cui_gpu_create_shared) */
    bool pooled;                    /* the instance is the process's, shared by its windows (cui__vk_process) */
    bool shared_device;             /* pooled, and the device is the process's too */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties mem;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd[CUI__TV_FRAMES];
    uint64_t cmd_value[CUI__TV_FRAMES];
    cui__vk_buffer upload[CUI__TV_FRAMES];
    cui__vk_buffer kept;            /* a captured frame whose copy was not known to be done: freed at teardown */
    int frame;                     /* the command buffer being recorded */
    bool open;
    VkSemaphore timeline;
    uint64_t value;                 /* the last submission's */
    cui__vk_later *later; int later_count, later_cap;
    VkSampler sampler;
    VkDescriptorPool set_pool;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkPipeline instance_pipeline;   /* cui_instance records: a four vertex strip per instance */
    cui__vk_tex *tex;               /* by cui_texture - 1 */
    int tex_count, tex_cap;
    int none;                       /* a black texel, for a command whose texture is gone */
    VkSurfaceKHR surface;
    VkFormat format;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count, image;
    VkImage images[CUI__TV_MAX_IMAGES];
    VkImageView views[CUI__TV_MAX_IMAGES];
    VkSemaphore present_sem[CUI__TV_MAX_IMAGES];
    VkSemaphore acquire_sem[CUI__TV_FRAMES];
    bool stale;
    bool lost;                      /* a submission failed: nothing more is drawn */
    bool can_capture;               /* the swapchain images can be copied from */
    /* damage tracking (cui_gpu_render_damaged) */
    bool can_copy_in;               /* the swapchain images can be copied to */
    bool owed;                      /* a frame was drawn and could not be shown: the next one is shown whole */
    bool kept_stale;                /* frames went straight into the window (cui_damage_direct): the kept image lacks them */
    VkImage kept_image;                 /* the window's picture, redrawn only where a frame changed */
    VkDeviceMemory kept_memory;
    VkImageView kept_view;
    VkExtent2D kept_extent;
    VkImageLayout kept_layout;
    cui_rect missed[CUI__TV_MAX_IMAGES][CUI__TV_MISSED];   /* what each swapchain image has not seen since it was last shown */
    int missed_count[CUI__TV_MAX_IMAGES];                   /* -1: all of it */
    cui__vk_zone zone[CUI__TV_ZONES];  /* cui_gpu_surface_draw; the last is slot -1 */
    cui__vk_shown shown[CUI__TV_ZONES];
    int shown_count;
    cui__vk_retired *retired; int retired_count, retired_cap;
    bool host;                      /* cui_x11_window.host: every list on a subsurface the renderer makes under the program's surface */
    bool place_due;                 /* a subsurface was placed, reordered or presented: the program's surface is committed */
    bool left_out;                  /* a surface's frame could not be drawn (cui_gpu_left_out) */
    void *wl_compositor, *wl_subcompositor, *wp_viewporter;
    void *dmabuf, *dmabuf_feedback;      /* zwp_linux_dmabuf_v1 at version 3 (modifiers) and 4 (the main device) */
    uint64_t modifiers[64]; int modifier_count;
    uint64_t main_device; bool main_device_known;
    bool plain;                          /* surfaces are dma-buf wl_buffers from the pool, not swapchains */
    bool plain_copied;                   /* another GPU composites: drawn in video memory, copied to its dma-buf */
    cui_pool pictures;                   /* which picture each surface draws into, and the spares */
    cui__vk_plain plain_at[CUI_POOL_SLOTS];   /* by the pool's slot */
    double reap_due;                     /* seconds until a spare picture is due to go, <0: none */
    bool live_resize;                    /* the window is being dragged to a new size */
    bool plain_none;                     /* the last take got none: the compositor holds them */
    cui_draw_cmd *clip_cmds; int clip_cap;   /* clipped commands: a pool tracker owns no memory */
    cui_draw_data clip_data;
    VkSemaphore plain_done;              /* signalled by the submission that drew the frame's pictures; exported as a sync file */
    void *plain_queue;                   /* struct wl_event_queue *, the renderer's own: a picture's wl_buffer is made on it */
    VkSemaphore plain_waits[CUI__TV_ZONES]; uint32_t plain_wait_count;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier;
    bool zones_failed;              /* a surface could not be given a swapchain of its own: the lists go into the window's picture */
};

#ifdef CUI_WAYLAND
static void cui__vk_plain_setup(cui_gpu_renderer *r);
static void cui__vk_plain_drop(cui_gpu_renderer *r, cui__vk_zone *z);
static void cui__vk_plain_show(cui_gpu_renderer *r, int n);
static void cui__vk_plain_reap(cui_gpu_renderer *r);
static void cui__vk_plain_free(cui_gpu_renderer *r, cui__vk_plain *b);
#endif

/* ---- memory ---- */

static uint32_t cui__vk_memory_type(cui_gpu_renderer *r, uint32_t bits, VkMemoryPropertyFlags want) {
    const VkPhysicalDeviceMemoryProperties *mp = &r->mem;
    /* host memory when mapped, video memory otherwise */
    bool host = (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want &&
            (!host || !(mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) return i;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool cui__vk_buffer_make(cui_gpu_renderer *r, cui__vk_buffer *b, size_t cap, VkBufferUsageFlags usage) {
    VkDevice dev = r->device;
    cui__vk_buffer made = { 0 };
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = cap;
    info.usage = usage;
    if (vkCreateBuffer(dev, &info, NULL, &made.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, made.buffer, &req);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, NULL, &made.memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, made.buffer, NULL);
        return false;
    }
    if (vkBindBufferMemory(dev, made.buffer, made.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev, made.memory, 0, VK_WHOLE_SIZE, 0, (void **)&made.ptr) != VK_SUCCESS) {
        vkDestroyBuffer(dev, made.buffer, NULL);
        vkFreeMemory(dev, made.memory, NULL);
        return false;
    }
    made.cap = cap;
    *b = made;
    return true;
}

static void cui__vk_buffer_free(cui_gpu_renderer *r, cui__vk_buffer *b) {
    if (b->buffer) vkDestroyBuffer(r->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(r->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

/* ---- submissions ---- */

/* True when nothing runs on the device any more (it is idle, or lost). */
static bool cui__vk_idle_or_gone(cui_gpu_renderer *r) {
    VkResult res = vkDeviceWaitIdle(r->device);
    return res == VK_SUCCESS || res == VK_ERROR_DEVICE_LOST;
}

/* Waits until the submission that signals `value` is done. False when that is not known (the
 * wait failed and the device could not be made idle): what the submission uses must stay. */
static bool cui__vk_wait(cui_gpu_renderer *r, uint64_t value) {
    if (value == 0) return true;
    if (r->lost) return cui__vk_idle_or_gone(r);   /* (a submission failed: the timeline does not reach it) */
    VkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    info.semaphoreCount = 1;
    info.pSemaphores = &r->timeline;
    info.pValues = &value;
    if (vkWaitSemaphores(r->device, &info, UINT64_MAX) != VK_SUCCESS) {   /* (nothing more is drawn) */
        r->lost = true;
        return cui__vk_idle_or_gone(r);
    }
    return true;
}

static void cui__vk_later_free(cui_gpu_renderer *r, const cui__vk_later *l) {
    VkDevice dev = r->device;
    if (l->set) vkFreeDescriptorSets(dev, r->set_pool, 1, &l->set);
    if (l->view) vkDestroyImageView(dev, l->view, NULL);
    if (l->image) vkDestroyImage(dev, l->image, NULL);
    if (l->buffer) vkDestroyBuffer(dev, l->buffer, NULL);
    if (l->memory) vkFreeMemory(dev, l->memory, NULL);
    if (l->slot >= 0) r->tex[l->slot].bpp = 0;
}

static void cui__vk_retired_free(cui_gpu_renderer *r, const cui__vk_retired *t) {
    VkDevice dev = r->device;
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (t->views[i]) vkDestroyImageView(dev, t->views[i], NULL);
    for (int i = 0; i < CUI__TV_MAX_IMAGES + CUI__TV_FRAMES; i++) if (t->sems[i]) vkDestroySemaphore(dev, t->sems[i], NULL);
    if (t->swapchain) vkDestroySwapchainKHR(dev, t->swapchain, NULL);
    if (t->surface) vkDestroySurfaceKHR(r->instance, t->surface, NULL);
}

/* the objects whose submission is done (all of them when idle); their texture slots are free again */
static void cui__vk_collect(cui_gpu_renderer *r, bool all) {
    uint64_t done = UINT64_MAX;
    if (!all && vkGetSemaphoreCounterValue(r->device, r->timeline, &done) != VK_SUCCESS) return;   /* not known: nothing is freed yet */
    int n = 0;
    for (int i = 0; i < r->later_count; i++) {
        if (r->later[i].value <= done) cui__vk_later_free(r, &r->later[i]);
        else r->later[n++] = r->later[i];
    }
    r->later_count = n;
    n = 0;
    for (int i = 0; i < r->retired_count; i++) {
        if (r->retired[i].value <= done) cui__vk_retired_free(r, &r->retired[i]);
        else r->retired[n++] = r->retired[i];
    }
    r->retired_count = n;
}

/* frees l after what is recorded so far has run */
static void cui__vk_release_later(cui_gpu_renderer *r, cui__vk_later l) {
    l.value = r->value + 1;
    if (r->later_count == r->later_cap) {
        int cap = r->later_cap ? r->later_cap * 2 : 64;
        cui__vk_later *grown = realloc(r->later, (size_t)cap * sizeof *grown);
        if (!grown) return;   /* out of memory: commands recorded and not yet run may use it, so it is left alive */
        r->later = grown;
        r->later_cap = cap;
    }
    r->later[r->later_count++] = l;
}

/* The open command buffer, begun on the next one once its last submission is done. Null when
 * the renderer has stopped or the buffer will not begin: the caller records nothing. */
static VkCommandBuffer cui__vk_cmd(cui_gpu_renderer *r) {
    if (r->lost) return VK_NULL_HANDLE;
    if (!r->open) {
        int f = r->frame;
        if (!cui__vk_wait(r, r->cmd_value[f]) || r->lost) return VK_NULL_HANDLE;   /* (its last submission may still run: it is not reset) */
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(r->cmd[f], 0) != VK_SUCCESS || vkBeginCommandBuffer(r->cmd[f], &begin) != VK_SUCCESS) {
            r->lost = true;
            return VK_NULL_HANDLE;
        }
        r->upload[f].used = 0;
        r->open = true;
    }
    return r->cmd[r->frame];
}

/* Submits the open command buffer, waiting for `waits` (at the color output, up to CUI__TV_ZONES)
 * and signalling `signal` (a binary semaphore, may be null) and the timeline. */
static void cui__vk_submit(cui_gpu_renderer *r, const VkSemaphore *waits, uint32_t wait_count, VkSemaphore signal) {
    if (!r->open) return;
    int f = r->frame;
    if (vkEndCommandBuffer(r->cmd[f]) != VK_SUCCESS) r->lost = true;
    if (r->lost) {   /* nothing of it runs, and nothing waits for it */
        r->open = false;
        return;
    }
    uint64_t value = ++r->value;
    VkSemaphore all[2] = { r->timeline, signal };
    uint64_t values[2] = { value, 0 };
    uint32_t signal_count = signal ? 1 : 0;
    VkPipelineStageFlags stages[CUI__TV_ZONES];
    uint64_t wait_values[CUI__TV_ZONES] = { 0 };
    for (uint32_t i = 0; i < wait_count; i++) stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkTimelineSemaphoreSubmitInfo tl = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tl.waitSemaphoreValueCount = wait_count;
    tl.pWaitSemaphoreValues = wait_values;
    tl.signalSemaphoreValueCount = 1 + signal_count;
    tl.pSignalSemaphoreValues = values;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.pNext = &tl;
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &r->cmd[f];
    submit.signalSemaphoreCount = 1 + signal_count;
    submit.pSignalSemaphores = all;
    if (vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) r->lost = true;   /* nothing is drawn from here on */
    r->cmd_value[f] = value;
    r->frame = (f + 1) % CUI__TV_FRAMES;
    r->open = false;
    cui__vk_collect(r, false);
}

static void cui__vk_idle(cui_gpu_renderer *r) {
    cui__vk_submit(r, NULL, 0, VK_NULL_HANDLE);
    cui__vk_wait(r, r->value);
}

/* the stages and accesses of an image in a layout, as a barrier's source or destination */
static void cui__vk_layout_use(VkImageLayout layout, bool dst, VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (dst ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : 0);
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = dst ? VK_ACCESS_TRANSFER_READ_BIT : 0; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; *access = dst ? VK_ACCESS_SHADER_READ_BIT : 0; break;
    /* undefined, present: nothing to wait for or make visible (the acquire semaphore waits at
     * the color output, and present waits on its semaphore) */
    default: *stage = dst ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; *access = 0; break;
    }
}

static void cui__vk_barrier(cui_gpu_renderer *r, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkPipelineStageFlags src_stage, dst_stage;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    cui__vk_layout_use(from, false, &src_stage, &b.srcAccessMask);
    cui__vk_layout_use(to, true, &dst_stage, &b.dstAccessMask);
    /* (from undefined to a color attachment: after the acquire, which waits at the color output) */
    if ((from == VK_IMAGE_LAYOUT_UNDEFINED || from == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    /* (a swapchain image about to be copied into: the same) */
    if ((from == VK_IMAGE_LAYOUT_UNDEFINED || from == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (cmd) vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* room in the open command buffer's upload buffer, valid until its submission is done */
static uint8_t *cui__vk_upload(cui_gpu_renderer *r, size_t bytes, size_t align, VkBuffer *buffer, size_t *offset) {
    if (!cui__vk_cmd(r)) return NULL;
    cui__vk_buffer *u = &r->upload[r->frame];
    size_t at = (u->used + align - 1) & ~(align - 1);
    if (!u->buffer || at + bytes > u->cap) {
        size_t cap = u->cap ? u->cap * 2 : (size_t)1 << 20;
        while (cap < bytes) cap *= 2;
        cui__vk_buffer grown;
        if (!cui__vk_buffer_make(r, &grown, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return NULL;
        if (u->buffer) {   /* what the open buffer already uses from it stays */
            cui__vk_later l = { 0 };
            l.buffer = u->buffer; l.memory = u->memory; l.slot = -1;
            cui__vk_release_later(r, l);
        }
        *u = grown;
        at = 0;
    }
    u->used = at + bytes;
    *buffer = u->buffer;
    *offset = at;
    return u->ptr + at;
}

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static bool cui__vk_tex_upload(cui_gpu_renderer *r, cui__vk_tex *t, int x, int y, int w, int h, const void *pixels, int stride, bool fresh) {
    size_t row = (size_t)w * (size_t)t->bpp, from = stride ? (size_t)stride : row;
    VkBuffer src;
    size_t offset;
    uint8_t *p = cui__vk_upload(r, row * (size_t)h, 16, &src, &offset);
    if (!p) return false;   /* no memory for the pixels */
    for (int k = 0; k < h; k++) memcpy(p + (size_t)k * row, (const uint8_t *)pixels + (size_t)k * from, row);
    cui__vk_barrier(r, t->image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = { 0 };
    copy.bufferOffset = offset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = x; copy.imageOffset.y = y;
    copy.imageExtent.width = (uint32_t)w; copy.imageExtent.height = (uint32_t)h; copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cui__vk_cmd(r), src, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    cui__vk_barrier(r, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

/* A free texture slot, or -1 when there is no room for one more. */
static int cui__vk_tex_slot(cui_gpu_renderer *r) {
    cui__vk_collect(r, false);   /* (slots whose textures are done with) */
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].bpp && !r->tex[i].image && !r->tex[i].borrowed) return i;
    if (r->tex_count == CUI__TV_TEX_CAP) return -1;
    if (r->tex_count == r->tex_cap) {
        int cap = r->tex_cap ? r->tex_cap * 2 : 16;
        void *n = realloc(r->tex, sizeof(*r->tex) * (size_t)cap);
        if (!n) return -1;
        r->tex = n;
        r->tex_cap = cap;
    }
    return r->tex_count++;
}

static cui_texture cui__vk_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_gpu_renderer *r = user;
    if (w <= 0 || h <= 0 || r->lost) return 0;
    int slot = cui__vk_tex_slot(r);
    if (slot < 0) return 0;
    VkDevice dev = r->device;
    cui__vk_tex made = { 0 };
    VkFormat format = fmt == CUI_TEXTURE_R8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)w; info.extent.height = (uint32_t)h; info.extent.depth = 1;
    info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    if (vkCreateImage(dev, &info, NULL, &made.image) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, made.image, &req);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, 0);
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = made.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VkDescriptorSetAllocateInfo sa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    sa.descriptorPool = r->set_pool;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts = &r->set_layout;
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, NULL, &made.memory) != VK_SUCCESS ||
        vkBindImageMemory(dev, made.image, made.memory, 0) != VK_SUCCESS || vkCreateImageView(dev, &view, NULL, &made.view) != VK_SUCCESS ||
        vkAllocateDescriptorSets(dev, &sa, &made.set) != VK_SUCCESS) {
        if (made.view) vkDestroyImageView(dev, made.view, NULL);
        vkDestroyImage(dev, made.image, NULL);
        if (made.memory) vkFreeMemory(dev, made.memory, NULL);
        return 0;
    }
    /* its own set, written once: no frame in flight reads a set that changes */
    VkDescriptorImageInfo image = { VK_NULL_HANDLE, made.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = made.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
    made.bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
    made.w = w; made.h = h;
    r->tex[slot] = made;
    if (pixels && !cui__vk_tex_upload(r, &r->tex[slot], 0, 0, w, h, pixels, 0, true)) {
        /* no texture without its pixels: the caller is told, and can make it again (no command uses it yet) */
        cui__vk_later gone = { 0 };
        gone.image = made.image; gone.view = made.view; gone.memory = made.memory; gone.set = made.set; gone.slot = slot;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
        cui__vk_later_free(r, &gone);
        return 0;
    }
    if (!pixels && !cui__vk_cmd(r)) {   /* (stopped: no texture) */
        cui__vk_later gone = { 0 };
        gone.image = made.image; gone.view = made.view; gone.memory = made.memory; gone.set = made.set; gone.slot = slot;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
        cui__vk_later_free(r, &gone);
        return 0;
    }
    if (!pixels) cui__vk_barrier(r, made.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (cui_texture)(slot + 1);
}

static void cui__vk_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].image || w <= 0 || h <= 0 || !pixels || r->lost) return;
    cui__vk_tex_upload(r, &r->tex[slot], x, y, w, h, pixels, stride, false);
}

static void cui__vk_tex_destroy(void *user, cui_texture tex) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || (!r->tex[slot].image && !r->tex[slot].borrowed)) return;
    /* draws already recorded may sample it: it goes, and its slot is free, once they have run */
    cui__vk_tex *t = &r->tex[slot];
    cui__vk_later l = { 0 };
    l.image = t->image; l.view = t->borrowed ? VK_NULL_HANDLE : t->view; l.memory = t->memory; l.set = t->set; l.slot = slot;   /* (a borrowed view stays the app's) */
    cui__vk_release_later(r, l);
    t->image = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE; t->memory = VK_NULL_HANDLE; t->set = VK_NULL_HANDLE; t->borrowed = false;
}

cui_texture cui_gpu_texture_wrap(cui_gpu_renderer *r, void *native) {
    if (!r || !native || r->lost) return 0;
    int slot = cui__vk_tex_slot(r);
    if (slot < 0) return 0;
    cui__vk_tex made = { 0 };
    made.view = (VkImageView)native;
    VkDescriptorSetAllocateInfo sa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    sa.descriptorPool = r->set_pool;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts = &r->set_layout;
    memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    if (vkAllocateDescriptorSets(r->device, &sa, &made.set) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo image = { VK_NULL_HANDLE, made.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = made.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    made.bpp = 4;
    made.borrowed = true;
    r->tex[slot] = made;
    return (cui_texture)(slot + 1);
}

void cui_gpu_texture_unwrap(cui_gpu_renderer *r, cui_texture texture) {
    int slot = (int)texture - 1;
    if (r && slot >= 0 && slot < r->tex_count && r->tex[slot].borrowed) cui__vk_tex_destroy(r, texture);
}

void cui_gpu_flush(cui_gpu_renderer *r) {
    if (r && r->device && r->timeline) cui__vk_idle(r);
}

/* ---- the instance and the device ---- */

/* one instance and device for all of a process's cui_gpu_create windows, freed with the last */
static struct {
    pthread_mutex_t lock;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    int windows;
} cui__vk_process = { PTHREAD_MUTEX_INITIALIZER };

static bool cui__vk_instance(cui_gpu_renderer *r) {
#ifdef CUI_WAYLAND
    const char *exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
#else
    const char *exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
#endif
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "cui";
    app.pEngineName = "cui";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = exts;
    return vkCreateInstance(&info, NULL, &r->instance) == VK_SUCCESS;
}

static VkResult cui__vk_surface(cui_gpu_renderer *r) {
#ifdef CUI_WAYLAND
    VkWaylandSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    info.display = (struct wl_display *)r->xwin->display;
    info.surface = (struct wl_surface *)r->xwin->wl_surface;
    return vkCreateWaylandSurfaceKHR(r->instance, &info, NULL, &r->surface);
#else
    VkXlibSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    info.dpy = (Display *)r->xwin->display;
    info.window = (Window)r->xwin->window;
    return vkCreateXlibSurfaceKHR(r->instance, &info, NULL, &r->surface);
#endif
}

/* a device that draws to the surface with what the renderer needs: CUI_GPU's, else a discrete GPU, else any */
static bool cui__vk_pick(cui_gpu_renderer *r) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(r->instance, &n, NULL);
    VkPhysicalDevice *pds = calloc(n ? n : 1, sizeof *pds);
    if (!pds) return false;
    vkEnumeratePhysicalDevices(r->instance, &n, pds);
    const char *want = getenv("CUI_GPU");
    if (want && !*want) want = NULL;
    int best = -1, best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
        VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
        vkGetPhysicalDeviceFeatures2(pds[i], &f);
        if (!f13.dynamicRendering || !f12.timelineSemaphore) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, NULL);
        VkQueueFamilyProperties qs[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qs);
        int family = -1;
        for (uint32_t q = 0; q < qn && family < 0; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, r->surface, &present);
            if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = (int)q;
        }
        if (family < 0) continue;
        int score = want ? (cui__contains_ci(props.deviceName, want) ? 3 : 0)
                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > best_score) { best = (int)i; best_score = score; best_family = (uint32_t)family; }
    }
    if (best >= 0) {
        r->physical = pds[best];
        r->family = best_family;
        if (want) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pds[best], &props);
            fprintf(stderr, "cui: GPU %s\n", props.deviceName);
        }
    }
    free(pds);
    return best >= 0;
}

static VkShaderModule cui__vk_module(cui_gpu_renderer *r, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(r->device, &info, NULL, &m);
    return m;
}

/* One of the two pipelines: vertices as a triangle list, or cui_instance records as strips. */
static VkPipeline cui__vk_pipeline_of(cui_gpu_renderer *r, bool instanced) {
    VkPipeline made = VK_NULL_HANDLE;
    VkShaderModule vs = instanced ? cui__vk_module(r, cui__vk_instance_vs, sizeof cui__vk_instance_vs) : cui__vk_module(r, cui__vk_vs, sizeof cui__vk_vs);
    VkShaderModule fs = cui__vk_module(r, cui__vk_fs, sizeof cui__vk_fs);
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
        };
        /* one attribute per group of cui_vertex fields, as the OpenGL renderer's */
        VkVertexInputBindingDescription binding = { 0, sizeof(cui_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[] = {
            { 0, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, x) },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, u) },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, lx) },
            { 3, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, hw) },
            { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(cui_vertex, r_tl) },
            { 5, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, border) },
            { 6, 0, VK_FORMAT_R8G8B8A8_UNORM,      (uint32_t)offsetof(cui_vertex, color) },
            { 7, 0, VK_FORMAT_R8G8B8A8_UNORM,      (uint32_t)offsetof(cui_vertex, border_color) },
        };
        /* one attribute per group of cui_instance fields, read once per instance */
        VkVertexInputBindingDescription instance_binding = { 0, sizeof(cui_instance), VK_VERTEX_INPUT_RATE_INSTANCE };
        VkVertexInputAttributeDescription instance_attrs[] = {
            { 0, 0, VK_FORMAT_R16G16B16A16_SINT,  (uint32_t)offsetof(cui_instance, x0) },
            { 1, 0, VK_FORMAT_R16G16B16A16_UNORM, (uint32_t)offsetof(cui_instance, u0) },
            { 2, 0, VK_FORMAT_R16G16B16A16_UINT,  (uint32_t)offsetof(cui_instance, r_tl) },
            { 3, 0, VK_FORMAT_R16_UINT,           (uint32_t)offsetof(cui_instance, param) },
            { 4, 0, VK_FORMAT_R8G8_UINT,          (uint32_t)offsetof(cui_instance, mode) },
            { 5, 0, VK_FORMAT_R8G8B8A8_UNORM,     (uint32_t)offsetof(cui_instance, color_top) },
            { 6, 0, VK_FORMAT_R8G8B8A8_UNORM,     (uint32_t)offsetof(cui_instance, color_bottom) },
            { 7, 0, VK_FORMAT_R8G8B8A8_UNORM,     (uint32_t)offsetof(cui_instance, border_color) },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = instanced ? &instance_binding : &binding;
        vi.vertexAttributeDescriptionCount = (uint32_t)(instanced ? sizeof instance_attrs / sizeof instance_attrs[0] : sizeof attrs / sizeof attrs[0]);
        vi.pVertexAttributeDescriptions = instanced ? instance_attrs : attrs;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = instanced ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        /* premultiplied source over */
        VkPipelineColorBlendAttachmentState blend = { 0 };
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &r->format;
        VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = r->layout;
        vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info, NULL, &made);
    }
    if (vs) vkDestroyShaderModule(r->device, vs, NULL);
    if (fs) vkDestroyShaderModule(r->device, fs, NULL);
    return made;
}

static bool cui__vk_pipeline(cui_gpu_renderer *r) {
    r->pipeline = cui__vk_pipeline_of(r, false);
    r->instance_pipeline = cui__vk_pipeline_of(r, true);
    return r->pipeline != VK_NULL_HANDLE && r->instance_pipeline != VK_NULL_HANDLE;
}

static bool cui__vk_setup(cui_gpu_renderer *r);
#ifdef CUI_WAYLAND
static bool cui__vk_host(cui_gpu_renderer *r);
#endif

static bool cui__vk_device(cui_gpu_renderer *r) {
    if (!cui__vk_pick(r)) { fprintf(stderr, "cui: no Vulkan 1.3 device draws to this window\n"); return false; }
    float priority = 1;
    VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = r->family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    const char *exts[5] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const char *dmabuf[4] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                              VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    uint32_t have_count = 0, ext_count = 1, dmabuf_found = 0;
    vkEnumerateDeviceExtensionProperties(r->physical, NULL, &have_count, NULL);
    VkExtensionProperties *have = have_count ? malloc(sizeof *have * have_count) : NULL;
    if (have && vkEnumerateDeviceExtensionProperties(r->physical, NULL, &have_count, have) == VK_SUCCESS)
        for (uint32_t i = 0; i < have_count; i++)
            for (int k = 0; k < 4; k++) dmabuf_found += strcmp(have[i].extensionName, dmabuf[k]) == 0;
    free(have);
#ifdef CUI_WAYLAND
    if (dmabuf_found == 4) for (int k = 0; k < 4; k++) exts[ext_count++] = dmabuf[k];   /* (for plain pictures) */
#endif
    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = ext_count;
    info.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(r->physical, &info, NULL, &r->device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(r->device, r->family, 0, &r->queue);
    return cui__vk_setup(r);
}

/* What the renderer keeps on its device, its own or the app's: the timeline, the command buffers, the pipelines. */
static bool cui__vk_setup(cui_gpu_renderer *r) {
    VkDevice dev = r->device;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &r->mem);

    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
    if (vkCreateSemaphore(dev, &sem, NULL, &r->timeline) != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = r->family;
    if (vkCreateCommandPool(dev, &pool, NULL, &r->pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cba.commandPool = r->pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = CUI__TV_FRAMES;
    if (vkAllocateCommandBuffers(dev, &cba, r->cmd) != VK_SUCCESS) return false;

    /* sampler: linear, clamp (textures have one level) */
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, NULL, &r->sampler) != VK_SUCCESS) return false;

    /* set 0: the command's texture with the sampler; the viewport is a push constant */
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CUI__TV_TEX_CAP };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = CUI__TV_TEX_CAP;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &r->set_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &r->sampler };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(dev, &li, NULL, &r->set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r->set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, NULL, &r->layout) != VK_SUCCESS) return false;

    /* a UNORM format (blending in sRGB): BGRA, else RGBA */
    VkSurfaceFormatKHR fmts[64];
    uint32_t n = 64;
    VkResult listed = vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &n, fmts);
    if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) n = 0;
    for (uint32_t i = 0; i < n && !r->format; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) r->format = fmts[i].format;
    for (uint32_t i = 0; i < n && !r->format; i++) if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) r->format = fmts[i].format;
    if (!r->format || !cui__vk_pipeline(r)) return false;
    for (int i = 0; i < CUI__TV_FRAMES; i++) {
        VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(dev, &s, NULL, &r->acquire_sem[i]) != VK_SUCCESS) return false;
    }
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    r->none = (int)cui__vk_tex_create(r, 1, 1, CUI_TEXTURE_RGBA8, black) - 1;
    cui__vk_idle(r);
    return r->none >= 0 && !r->lost;   /* (a renderer that stopped while starting is none: the app draws with OpenGL) */
}

/* ---- the window ---- */

/* The swapchain's views and its presentation semaphores: a semaphore a failed presentation left
 * signalled must not meet the next swapchain's images. The caller has made the device idle. */
static void cui__vk_views_free(cui_gpu_renderer *r) {
    for (uint32_t i = 0; i < r->image_count; i++) { vkDestroyImageView(r->device, r->views[i], NULL); r->views[i] = VK_NULL_HANDLE; }
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (r->present_sem[i]) { vkDestroySemaphore(r->device, r->present_sem[i], NULL); r->present_sem[i] = VK_NULL_HANDLE; }
    r->image_count = 0;
}

/* (re)makes the swapchain; the caller has made the device idle */
static bool cui__vk_swapchain(cui_gpu_renderer *r) {
    VkDevice dev = r->device;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   /* (Wayland) the window's size is the app's to say */
        extent.width = (uint32_t)(r->xwin->width > 0 ? r->xwin->width : 0);
        extent.height = (uint32_t)(r->xwin->height > 0 ? r->xwin->height : 0);
    }
    if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    if (extent.width == 0 || extent.height == 0) return false;   /* minimized */
    /* vsync: FIFO; else tearing (immediate), or mailbox */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!r->vsync) {
        VkPresentModeKHR modes[16];
        uint32_t n = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &n, modes);
        for (uint32_t i = 0; i < n; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    uint32_t count = caps.minImageCount > CUI__TV_IMAGES ? caps.minImageCount : CUI__TV_IMAGES;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    /* reading a frame back (screenshots) needs the images as a copy's source, which a surface may not allow */
    r->can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    r->can_copy_in = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) r->missed_count[i] = -1;   /* new images hold nothing */
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (r->xwin->translucent && (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)) alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = r->surface;
    info.minImageCount = count;
    info.imageFormat = r->format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (r->can_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) | (r->can_copy_in ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = r->swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &info, NULL, &sc) != VK_SUCCESS) {
        /* the old one is retired even so, and may not be handed in again: the next try starts from none */
        cui__vk_views_free(r);
        if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
        r->swapchain = VK_NULL_HANDLE;
        r->extent.width = r->extent.height = 0;
        r->stale = true;
        return false;
    }
    cui__vk_views_free(r);
    if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
    r->swapchain = sc;
    r->extent = extent;
    r->image_count = 0;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, sc, &n, NULL);
    /* (a driver may make more images than asked for; more than there is room for cannot be drawn into) */
    if (n == 0 || n > CUI__TV_MAX_IMAGES || vkGetSwapchainImagesKHR(dev, sc, &n, r->images) != VK_SUCCESS) { r->stale = true; return false; }
    bool made = true;
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo v = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        v.image = r->images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = r->format;
        v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &v, NULL, &r->views[i]) != VK_SUCCESS) made = false;
        if (!r->present_sem[i]) {
            VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(dev, &s, NULL, &r->present_sem[i]) != VK_SUCCESS) made = false;
        }
    }
    r->image_count = n;
    r->stale = !made;
    return made;
}

/* The caller has made the device idle. */
static void cui__vk_zone_free(cui_gpu_renderer *r, int slot) {
    cui__vk_zone *z = &r->zone[slot];
    VkDevice dev = r->device;
    for (uint32_t i = 0; i < z->image_count; i++) if (z->views[i]) vkDestroyImageView(dev, z->views[i], NULL);
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (z->present_sem[i]) vkDestroySemaphore(dev, z->present_sem[i], NULL);
    for (int i = 0; i < CUI__TV_FRAMES; i++) if (z->acquire_sem[i]) vkDestroySemaphore(dev, z->acquire_sem[i], NULL);
    if (z->swapchain) vkDestroySwapchainKHR(dev, z->swapchain, NULL);
    if (z->surface) vkDestroySurfaceKHR(r->instance, z->surface, NULL);
    void *wl = z->wl_surface;
#ifdef CUI_WAYLAND
    if (z->own) {   /* the renderer's own subsurface goes with what was made on it */
        wl_proxy_marshal_flags(z->viewport, 0, NULL, wl_proxy_get_version(z->viewport), WL_MARSHAL_FLAG_DESTROY);
        wl_subsurface_destroy(z->sub);
        wl_surface_destroy(wl);
        wl = NULL;
    }
#endif
    memset(z, 0, sizeof *z);
    z->wl_surface = wl;
}

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    VkDevice dev = r->device;
    if (dev) {
        if (r->timeline) cui__vk_idle(r);
        if (!cui__vk_idle_or_gone(r)) {   /* (presents too: their semaphores go below) */
            /* Not known to be done with anything: what the device may still use is left alone,
             * and only the memory on this side is given back. */
            free(r->later);
            free(r->retired);
            free(r->tex);
            free(r->clip_cmds);
            free(r);
            return;
        }
        cui__vk_views_free(r);
        for (int i = 0; i < r->retired_count; i++) cui__vk_retired_free(r, &r->retired[i]);   /* (before the surfaces they were made on) */
        r->retired_count = 0;
        for (int i = 0; i < CUI__TV_ZONES; i++) cui__vk_zone_free(r, i);
#ifdef CUI_WAYLAND
        for (int i = 0; i < CUI_POOL_SLOTS; i++) if (r->plain_at[i].made) cui__vk_plain_free(r, &r->plain_at[i]);
        if (r->plain_done) vkDestroySemaphore(dev, r->plain_done, NULL);
        if (r->dmabuf) wl_proxy_marshal_flags(r->dmabuf, 0, NULL, 3, WL_MARSHAL_FLAG_DESTROY);
        if (r->plain_queue) wl_event_queue_destroy(r->plain_queue);
#endif
        if (r->kept_view) vkDestroyImageView(dev, r->kept_view, NULL);
        if (r->kept_image) vkDestroyImage(dev, r->kept_image, NULL);
        if (r->kept_memory) vkFreeMemory(dev, r->kept_memory, NULL);
        if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
        for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (r->present_sem[i]) vkDestroySemaphore(dev, r->present_sem[i], NULL);
        for (int i = 0; i < CUI__TV_FRAMES; i++) if (r->acquire_sem[i]) vkDestroySemaphore(dev, r->acquire_sem[i], NULL);
        for (int i = 0; i < r->tex_count; i++) cui__vk_tex_destroy(r, (cui_texture)(i + 1));
        cui__vk_collect(r, true);
        if (r->pipeline) vkDestroyPipeline(dev, r->pipeline, NULL);
        if (r->instance_pipeline) vkDestroyPipeline(dev, r->instance_pipeline, NULL);
        if (r->layout) vkDestroyPipelineLayout(dev, r->layout, NULL);
        if (r->set_layout) vkDestroyDescriptorSetLayout(dev, r->set_layout, NULL);
        if (r->set_pool) vkDestroyDescriptorPool(dev, r->set_pool, NULL);
        if (r->sampler) vkDestroySampler(dev, r->sampler, NULL);
        for (int i = 0; i < CUI__TV_FRAMES; i++) cui__vk_buffer_free(r, &r->upload[i]);
        cui__vk_buffer_free(r, &r->kept);
        if (r->pool) vkDestroyCommandPool(dev, r->pool, NULL);
        if (r->timeline) vkDestroySemaphore(dev, r->timeline, NULL);
    }
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->pooled) {
        pthread_mutex_lock(&cui__vk_process.lock);
        bool last = --cui__vk_process.windows == 0;
        if (dev && !r->shared_device) vkDestroyDevice(dev, NULL);
        if (last) {
            if (cui__vk_process.device) vkDestroyDevice(cui__vk_process.device, NULL);
            if (cui__vk_process.instance) vkDestroyInstance(cui__vk_process.instance, NULL);
            memset(&cui__vk_process.instance, 0, sizeof cui__vk_process - offsetof(__typeof__(cui__vk_process), instance));
        }
        pthread_mutex_unlock(&cui__vk_process.lock);
    } else {
        if (dev && !r->shared) vkDestroyDevice(dev, NULL);
        if (r->instance && !r->shared) vkDestroyInstance(r->instance, NULL);
    }
    free(r->later);
    free(r->retired);
    free(r->tex);
    free(r->clip_cmds);
    free(r);
}

/* the process's instance, and its device when that presents to this window's surface */
static bool cui__vk_create_pooled(cui_gpu_renderer *r) {
    if (!cui__vk_process.instance) {
        if (!cui__vk_instance(r)) return false;
        cui__vk_process.instance = r->instance;
    }
    r->instance = cui__vk_process.instance;
    r->pooled = true;
    cui__vk_process.windows++;
    if (cui__vk_surface(r) != VK_SUCCESS) { r->surface = VK_NULL_HANDLE; return false; }
    VkBool32 present = VK_FALSE;
    if (cui__vk_process.device && vkGetPhysicalDeviceSurfaceSupportKHR(cui__vk_process.physical, cui__vk_process.family, r->surface, &present) == VK_SUCCESS && present) {
        r->physical = cui__vk_process.physical;
        r->family = cui__vk_process.family;
        r->device = cui__vk_process.device;
        r->queue = cui__vk_process.queue;
        r->shared_device = true;
        return cui__vk_setup(r);
    }
    if (!cui__vk_device(r)) return false;
    if (!cui__vk_process.device) {
        cui__vk_process.physical = r->physical;
        cui__vk_process.family = r->family;
        cui__vk_process.device = r->device;
        cui__vk_process.queue = r->queue;
        r->shared_device = true;
    }
    return true;
}

cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    if (!window) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->xwin = window;
    r->vsync = vsync;
    pthread_mutex_lock(&cui__vk_process.lock);
    bool made = cui__vk_create_pooled(r);
    pthread_mutex_unlock(&cui__vk_process.lock);
    if (!made) { cui_gpu_destroy(r); return NULL; }
#ifdef CUI_WAYLAND
    if (cui__vk_host(r)) cui__vk_plain_setup(r);   /* (surfaces of the program's own get plain pictures too) */
#endif
    /* The swapchain now, while the app can still draw with OpenGL instead. (A window without a
     * size yet has none to make: it comes with the first frame.) */
    r->stale = true;
    if (r->xwin->width > 0 && r->xwin->height > 0 && !cui__vk_swapchain(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

cui_gpu_renderer *cui_gpu_create_shared(void *window, bool vsync, const cui_gpu_device *device) {
    if (!window || !device || !device->instance || !device->physical_device || !device->device || !device->queue) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->xwin = window;
    r->vsync = vsync;
    r->shared = true;
    r->instance = (VkInstance)device->instance;
    r->physical = (VkPhysicalDevice)device->physical_device;
    r->family = device->queue_family;
    if (cui__vk_surface(r) != VK_SUCCESS) { r->surface = VK_NULL_HANDLE; cui_gpu_destroy(r); return NULL; }
    VkBool32 present = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(r->physical, r->family, r->surface, &present) != VK_SUCCESS || !present) { cui_gpu_destroy(r); return NULL; }
    r->device = (VkDevice)device->device;
    r->queue = (VkQueue)device->queue;
    if (!cui__vk_setup(r)) { cui_gpu_destroy(r); return NULL; }
    r->stale = true;
#ifdef CUI_WAYLAND
    r->host = r->xwin->host && r->xwin->wl_surface;
    if (r->host) {
        if (!cui__vk_host(r)) { cui_gpu_destroy(r); return NULL; }
        cui__vk_plain_setup(r);
        return r;
    }
#endif
    if (r->xwin->width > 0 && r->xwin->height > 0 && !cui__vk_swapchain(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__vk_tex_create;
    i.texture_update = cui__vk_tex_update;
    i.texture_destroy = cui__vk_tex_destroy;
    return i;
}

/* ---- frames ---- */

static bool cui__vk_resized(const cui_gpu_renderer *r) {
    int w = r->xwin->width, h = r->xwin->height;
    return r->stale || (w > 0 && h > 0 && ((uint32_t)w != r->extent.width || (uint32_t)h != r->extent.height));
}

/* What one frame's phases share */
typedef struct cui__vk_frame {
    VkCommandBuffer cmd;
    VkSemaphore acquire;
    VkImage image;
    cui__vk_buffer readback;   /* the frame copied out, when it was asked for */
} cui__vk_frame;

/* The image to draw into; the swapchain is made again when the window's size changed. */
static bool cui__vk_acquire(cui_gpu_renderer *r, cui__vk_frame *f) {
    VkDevice dev = r->device;
    f->cmd = cui__vk_cmd(r);
    if (!f->cmd) return false;
    f->acquire = r->acquire_sem[r->frame];
    bool acquired = false;
    for (int attempt = 0; attempt < 2 && !acquired; attempt++) {
        if (cui__vk_resized(r)) {
            cui__vk_idle(r);
            if (r->lost || vkDeviceWaitIdle(dev) != VK_SUCCESS) {   /* (not known to be done with the old images: they stay) */
                r->lost = true;
                return false;
            }
            if (!cui__vk_swapchain(r)) break;
            f->cmd = cui__vk_cmd(r);
            if (!f->cmd) return false;
            f->acquire = r->acquire_sem[r->frame];
        }
        VkResult res = vkAcquireNextImageKHR(dev, r->swapchain, UINT64_MAX, f->acquire, VK_NULL_HANDLE, &r->image);
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) { acquired = true; r->stale = res == VK_SUBOPTIMAL_KHR; }
        else r->stale = true;
    }
    if (!acquired) {   /* (minimized) what is recorded still goes */
        cui__vk_submit(r, NULL, 0, VK_NULL_HANDLE);
        return false;
    }
    f->image = r->images[r->image];
    return true;
}

/* What a frame is drawn into */
typedef struct cui__vk_target {
    VkImage image;
    VkImageView view;
    VkImageLayout from;      /* the layout it is in */
    VkExtent2D extent;
    const cui_rect *part;    /* only these boxes of it are cleared, and the rest kept; NULL: all of it */
    int parts;
    int ox, oy;              /* where its top left lies in the draw data's frame (the boxes are in that frame too) */
} cui__vk_target;

/* Into the target: clear, then a draw per command (cui_gl_render's counterpart). It is left a color attachment. */
static bool cui__vk_draw(cui_gpu_renderer *r, const cui__vk_target *t, const cui_draw_data *dd) {
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (!cmd) return false;
    /* the frame's vertices and indices */
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE;
    size_t vat = 0, iat = 0;
    uint8_t *v = dd->index_count ? cui__vk_upload(r, vbytes, 16, &vbuf, &vat) : NULL;
    uint8_t *ix = v ? cui__vk_upload(r, ibytes, 16, &ibuf, &iat) : NULL;   /* (a buffer outgrown between the two lives until this frame has run) */
    /* and its cui_instance records, drawn by the instance pipeline */
    size_t nbytes = sizeof(cui_instance) * (size_t)(dd->instance_count > 0 ? dd->instance_count : 0);
    VkBuffer nbuf = VK_NULL_HANDLE;
    size_t nat = 0;
    uint8_t *n = nbytes ? cui__vk_upload(r, nbytes, 16, &nbuf, &nat) : NULL;
    bool drawn = (!dd->index_count || (v && ix)) && (!nbytes || n);

    cui__vk_barrier(r, t->image, t->from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = t->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = t->part ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = dd->clear_color.r / 255.0f;
    color.clearValue.color.float32[1] = dd->clear_color.g / 255.0f;
    color.clearValue.color.float32[2] = dd->clear_color.b / 255.0f;
    color.clearValue.color.float32[3] = dd->clear_color.r || dd->clear_color.g || dd->clear_color.b || dd->clear_color.a ? 1.0f : 0.0f;   /* (a surface clears to nothing) */
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = t->extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    for (int i = 0; t->part && i < t->parts; i++) {
        const cui_rect *p = &t->part[i];
        int x0 = (int)p->x - t->ox, y0 = (int)p->y - t->oy, x1 = (int)(p->x + p->w) - t->ox, y1 = (int)(p->y + p->h) - t->oy;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)t->extent.width) x1 = (int)t->extent.width;
        if (y1 > (int)t->extent.height) y1 = (int)t->extent.height;
        VkClearAttachment what = { VK_IMAGE_ASPECT_COLOR_BIT, 0, color.clearValue };
        VkClearRect where = { { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } }, 0, 1 };
        if (x1 > x0 && y1 > y0) vkCmdClearAttachments(cmd, 1, &what, 1, &where);
    }
    bool indexed = v && ix;
    if (indexed || n) {
        if (indexed) { memcpy(v, dd->vertices, vbytes); memcpy(ix, dd->indices, ibytes); }
        if (n) memcpy(n, dd->instances, nbytes);
        int W = (int)t->extent.width, H = (int)t->extent.height;
        VkViewport vp = { -(float)t->ox, -(float)t->oy, (float)dd->width_px, (float)dd->height_px, 0, 1 };
        float viewport[2] = { (float)dd->width_px, (float)dd->height_px };
        VkDeviceSize voffset = vat, noffset = nat;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        if (indexed) vkCmdBindIndexBuffer(cmd, ibuf, iat, VK_INDEX_TYPE_UINT32);
        int kind = -1;   /* 0: the vertex pipeline is bound, 1: the instance pipeline */
        int bound = -1;
        for (int i = 0; i < dd->command_count; i++) {
            const cui_draw_cmd *c = &dd->commands[i];
            int want = c->instance_count ? 1 : 0;
            if (want ? !n : (!indexed || !c->index_count)) continue;
            int x0 = (int)floorf(c->clip.x) - t->ox, y0 = (int)floorf(c->clip.y) - t->oy;
            int x1 = (int)ceilf(c->clip.x + c->clip.w) - t->ox, y1 = (int)ceilf(c->clip.y + c->clip.h) - t->oy;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W) x1 = W;
            if (y1 > H) y1 = H;
            if (x1 <= x0 || y1 <= y0) continue;
            if (want != kind) {
                /* both pipelines share the layout, so the texture binding carries over; the push constants are set again */
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want ? r->instance_pipeline : r->pipeline);
                vkCmdBindVertexBuffers(cmd, 0, 1, want ? &nbuf : &vbuf, want ? &noffset : &voffset);
                vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof viewport, viewport);
                kind = want;
            }
            VkRect2D sc = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            cui_texture t = c->texture ? c->texture : dd->font_atlas;
            int slot = (int)t - 1;
            if (slot < 0 || slot >= r->tex_count || (!r->tex[slot].image && !r->tex[slot].borrowed)) slot = r->none;
            if (slot != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->tex[slot].set, 0, NULL);
                bound = slot;
            }
            if (want) vkCmdDraw(cmd, 4, c->instance_count, 0, c->instance_offset);   /* the first instance picks the first record */
            else vkCmdDrawIndexed(cmd, (uint32_t)c->index_count, 1, (uint32_t)c->index_offset, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    return drawn;   /* false: the vertices or the records found no room, and what did is all that is shown */
}

/* The frame copied out, if asked, and the image made ready to present. */
static void cui__vk_copy_out(cui_gpu_renderer *r, cui__vk_frame *f, bool wanted) {
    if (wanted && r->can_capture && cui__vk_buffer_make(r, &f->readback, (size_t)r->extent.width * r->extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = r->extent.width;
        region.imageExtent.height = r->extent.height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(f->cmd, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->readback.buffer, 1, &region);
        /* waiting for the submission orders the read after the copy, and this makes what the copy wrote the host's to read */
        VkBufferMemoryBarrier host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = f->readback.buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
}

static void cui__vk_present(cui_gpu_renderer *r, const cui__vk_frame *f) {
    VkSemaphore present = r->present_sem[r->image];
    cui__vk_cmd(r);   /* (a frame whose work already ran still signals the present) */
    cui__vk_submit(r, &f->acquire, f->acquire ? 1 : 0, present);
    if (r->lost) return;   /* (nothing was submitted: there is nothing to present) */
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &present;
    pi.swapchainCount = 1;
    pi.pSwapchains = &r->swapchain;
    pi.pImageIndices = &r->image;
    if (vkQueuePresentKHR(r->queue, &pi) != VK_SUCCESS) r->stale = true;
}

/* Waits for the copy and writes it as RGBA; false if one was asked for and could not be made. */
static bool cui__vk_read_back(cui_gpu_renderer *r, cui__vk_frame *f, const cui_draw_data *dd, void *rgba) {
    bool ok = !rgba;
    if (f->readback.buffer) {
        if (!cui__vk_wait(r, r->value)) {   /* the copy may still be writing it: it goes when the renderer does */
            r->kept = f->readback;          /* (a failed wait stops the renderer, so there is only ever one) */
            memset(&f->readback, 0, sizeof f->readback);
            return false;
        }
        int w = (int)r->extent.width, h = (int)r->extent.height;
        if (!r->lost && w == dd->width_px && h == dd->height_px) {
            const uint8_t *src = f->readback.ptr;
            uint8_t *dst = rgba;
            bool bgra = r->format == VK_FORMAT_B8G8R8A8_UNORM;
            for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++, src += 4, dst += 4) {
                dst[0] = src[bgra ? 2 : 0]; dst[1] = src[1]; dst[2] = src[bgra ? 0 : 2]; dst[3] = 255;
            }
            ok = true;
        }
        cui__vk_buffer_free(r, &f->readback);
    }
    return ok;
}

static bool cui__vk_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd || r->lost) return false;
    cui__vk_frame f = { 0 };
    if (!cui__vk_acquire(r, &f)) return false;
    cui__vk_target target = { f.image, r->views[r->image], VK_IMAGE_LAYOUT_UNDEFINED, r->extent, NULL, 0 };
    bool drawn = cui__vk_draw(r, &target, dd);
    f.cmd = cui__vk_cmd(r);
    cui__vk_copy_out(r, &f, rgba != NULL);
    cui__vk_present(r, &f);
    r->missed_count[r->image] = -1;   /* (should cui_gpu_render_damaged follow: this image was not made from the kept one) */
    return cui__vk_read_back(r, &f, dd, rgba) && drawn;   /* (a capture of a frame that was not drawn is none) */
}

/* ---- damage: the frame kept as an image, redrawn and shown only where it changed ---- */

static bool cui__vk_keep(cui_gpu_renderer *r, uint32_t w, uint32_t h) {
    VkDevice dev = r->device;
    if (r->kept_image && r->kept_extent.width == w && r->kept_extent.height == h) return true;
    cui__vk_idle(r);
    vkDeviceWaitIdle(dev);
    if (r->kept_view) vkDestroyImageView(dev, r->kept_view, NULL);
    if (r->kept_image) vkDestroyImage(dev, r->kept_image, NULL);
    if (r->kept_memory) vkFreeMemory(dev, r->kept_memory, NULL);
    r->kept_view = VK_NULL_HANDLE; r->kept_image = VK_NULL_HANDLE; r->kept_memory = VK_NULL_HANDLE;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = r->format;
    info.extent.width = w; info.extent.height = h; info.extent.depth = 1;
    info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(dev, &info, NULL, &r->kept_image) != VK_SUCCESS) { r->kept_image = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(dev, r->kept_image, &need);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = cui__vk_memory_type(r, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo v = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = r->format;
    v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v.subresourceRange.levelCount = 1;
    v.subresourceRange.layerCount = 1;
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, NULL, &r->kept_memory) != VK_SUCCESS ||
        vkBindImageMemory(dev, r->kept_image, r->kept_memory, 0) != VK_SUCCESS || (v.image = r->kept_image, vkCreateImageView(dev, &v, NULL, &r->kept_view)) != VK_SUCCESS) {
        if (r->kept_view) vkDestroyImageView(dev, r->kept_view, NULL);
        vkDestroyImage(dev, r->kept_image, NULL);
        if (r->kept_memory) vkFreeMemory(dev, r->kept_memory, NULL);
        r->kept_view = VK_NULL_HANDLE; r->kept_image = VK_NULL_HANDLE; r->kept_memory = VK_NULL_HANDLE;
        return false;
    }
    r->kept_extent.width = w; r->kept_extent.height = h;
    r->kept_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

/* The frame, or the part of it inside the boxes, drawn into the kept image. */
static bool cui__vk_draw_kept(cui_gpu_renderer *r, const cui_draw_data *dd, const cui_rect *part, int parts) {
    cui__vk_target target = { r->kept_image, r->kept_view, r->kept_layout, r->kept_extent, r->kept_layout == VK_IMAGE_LAYOUT_UNDEFINED ? NULL : part, parts };
    bool drawn = cui__vk_draw(r, &target, dd);
    cui__vk_barrier(r, r->kept_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    r->kept_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return drawn;
}

#ifdef CUI_WAYLAND
static bool cui__vk_base(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d);
#endif

bool cui_gpu_render_damaged(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d, bool present_anyway) {
    if (!r || !dd || !d || r->lost) return false;
    int W = dd->width_px, H = dd->height_px;
    if (W <= 0 || H <= 0) return false;
#ifdef CUI_WAYLAND
    if (r->host) {   /* the list on a subsurface of the window's size, when it changed */
        cui_rect changed;
        bool whole = false;
        if (!cui_damage_track(d, dd, &changed, &whole) && !present_anyway) return false;
        return cui__vk_base(r, dd, present_anyway ? NULL : d);
    }
#endif
    if (cui__vk_resized(r)) {   /* the swapchain first: whether its images can be copied to comes with it */
        cui__vk_idle(r);
        vkDeviceWaitIdle(r->device);
        if (!cui__vk_swapchain(r)) return false;
        present_anyway = true;
    }
    if (!r->can_copy_in) { cui__vk_render(r, dd, NULL); return true; }
    if (!r->kept_image || r->kept_extent.width != (uint32_t)W || r->kept_extent.height != (uint32_t)H) {
        if (!cui__vk_keep(r, (uint32_t)W, (uint32_t)H)) { cui__vk_render(r, dd, NULL); return true; }
        cui_damage_reset(d);   /* the new image holds nothing yet */
    }
    cui_rect rect;
    bool full = false;
    bool changed = cui_damage_track(d, dd, &rect, &full);
    if (r->owed) present_anyway = true;
    if (!changed && !present_anyway) return false;
    if (cui_damage_direct(d)) {   /* much of the window keeps changing: straight into it, with no copy */
        r->kept_stale = true;
        for (uint32_t i = 0; i < r->image_count; i++) r->missed_count[i] = -1;
        if (!cui__vk_render(r, dd, NULL) && !r->lost) r->owed = true;
        else r->owed = false;
        return !r->lost;
    }
    if (r->kept_stale) { changed = full = present_anyway = true; r->kept_stale = false; }   /* back to the kept image: all of it first */
    if (changed) {
        cui_rect part[CUI_DAMAGE_BOXES];
        int parts = 0;
        const cui_draw_data *cut = full ? NULL : cui_damage_clip_boxes(d, dd, part, &parts);
        if (!cut) { full = true; r->kept_layout = VK_IMAGE_LAYOUT_UNDEFINED; }
        cui__vk_draw_kept(r, cut ? cut : dd, cut ? part : NULL, parts);
    }
    cui__vk_frame f = { 0 };
    if (!cui__vk_acquire(r, &f)) { r->owed = true; return false; }
    r->owed = false;
    /* into the swapchain image: what this frame changed, and what the image has not seen since it was last shown */
    cui_rect boxes[CUI_DAMAGE_BOXES + CUI__TV_MISSED];
    int own = changed && !present_anyway ? cui_damage_boxes(d, boxes, CUI_DAMAGE_BOXES) : -1, n = own;
    int behind = r->missed_count[r->image];
    bool fits = r->extent.width == r->kept_extent.width && r->extent.height == r->kept_extent.height;
    bool part = own >= 0 && behind >= 0 && fits;
    if (part) { memcpy(boxes + n, r->missed[r->image], sizeof *boxes * (size_t)behind); n += behind; }
    cui__vk_barrier(r, f.image, part ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy regions[CUI_DAMAGE_BOXES + CUI__TV_MISSED];
    uint32_t region_count = 0;
    VkImageCopy all = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                        { r->extent.width < r->kept_extent.width ? r->extent.width : r->kept_extent.width,
                          r->extent.height < r->kept_extent.height ? r->extent.height : r->kept_extent.height, 1 } };
    for (int i = 0; part && i < n; i++) {
        VkImageCopy one = all;
        one.srcOffset.x = one.dstOffset.x = (int32_t)boxes[i].x; one.srcOffset.y = one.dstOffset.y = (int32_t)boxes[i].y;
        one.extent.width = (uint32_t)boxes[i].w; one.extent.height = (uint32_t)boxes[i].h;
        if (boxes[i].w >= 1 && boxes[i].h >= 1) regions[region_count++] = one;
    }
    if (!part) regions[region_count++] = all;
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (cmd && region_count) vkCmdCopyImage(cmd, r->kept_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count, regions);
    cui__vk_barrier(r, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    cui__vk_present(r, &f);
    for (uint32_t i = 0; i < r->image_count; i++) {
        if (i == r->image) { r->missed_count[i] = 0; continue; }
        if (own < 0 || r->missed_count[i] < 0 || r->missed_count[i] + own > CUI__TV_MISSED) { r->missed_count[i] = -1; continue; }
        memcpy(r->missed[i] + r->missed_count[i], boxes, sizeof(cui_rect) * (size_t)own);
        r->missed_count[i] += own;
    }
    return !r->lost;
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) {
#ifdef CUI_WAYLAND
    if (r && dd && r->host) { cui__vk_base(r, dd, NULL); return; }
#endif
    cui__vk_render(r, dd, NULL);
}
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return rgba && r && !r->host && cui__vk_render(r, dd, rgba); }

/* ---- surfaces: a swapchain each, on a surface of the system's that the app lays over the window ---- */

#ifdef CUI_WAYLAND
bool cui_gpu_surfaces(cui_gpu_renderer *r) { return r && !r->lost && !r->zones_failed && r->xwin && r->xwin->wl_surface; }

static void cui__vk_zone_retire(cui_gpu_renderer *r, cui__vk_zone *z, bool surface);

/* wp_viewporter, written out: the two requests the renderer makes of each interface, so that it
 * needs no generated protocol code and shares no symbol with a program that has its own. */
static const struct wl_interface cui__wp_viewport_interface;
static const struct wl_interface *cui__wp_types[] = { NULL, NULL, NULL, NULL, &cui__wp_viewport_interface, &wl_surface_interface };
static const struct wl_message cui__wp_viewporter_requests[] = { { "destroy", "", cui__wp_types }, { "get_viewport", "no", cui__wp_types + 4 } };
static const struct wl_interface cui__wp_viewporter_interface = { "wp_viewporter", 1, 2, cui__wp_viewporter_requests, 0, NULL };
static const struct wl_message cui__wp_viewport_requests[] = { { "destroy", "", cui__wp_types }, { "set_source", "ffff", cui__wp_types }, { "set_destination", "ii", cui__wp_types } };
static const struct wl_interface cui__wp_viewport_interface = { "wp_viewport", 1, 3, cui__wp_viewport_requests, 0, NULL };

/* linux-dmabuf, written out the same way: what the plain pictures are made and shown with */
static const struct wl_interface cui__zwp_params_interface, cui__zwp_feedback_interface;
static const struct wl_interface *cui__zwp_types[] = { NULL, NULL, NULL, NULL, NULL, NULL, &cui__zwp_params_interface, &cui__zwp_feedback_interface,
                                                       &cui__zwp_feedback_interface, &wl_surface_interface, &wl_buffer_interface, NULL, NULL, NULL, NULL };
static const struct wl_message cui__zwp_dmabuf_requests[] = { { "destroy", "", cui__zwp_types }, { "create_params", "n", cui__zwp_types + 6 },
                                                              { "get_default_feedback", "4n", cui__zwp_types + 7 }, { "get_surface_feedback", "4no", cui__zwp_types + 8 } };
static const struct wl_message cui__zwp_dmabuf_events[] = { { "format", "u", cui__zwp_types }, { "modifier", "3uuu", cui__zwp_types } };
static const struct wl_interface cui__zwp_dmabuf_interface = { "zwp_linux_dmabuf_v1", 4, 4, cui__zwp_dmabuf_requests, 2, cui__zwp_dmabuf_events };
static const struct wl_message cui__zwp_params_requests[] = { { "destroy", "", cui__zwp_types }, { "add", "huuuuu", cui__zwp_types },
                                                              { "create", "iiuu", cui__zwp_types }, { "create_immed", "2niiuu", cui__zwp_types + 10 } };
static const struct wl_message cui__zwp_params_events[] = { { "created", "n", cui__zwp_types + 10 }, { "failed", "", cui__zwp_types } };
static const struct wl_interface cui__zwp_params_interface = { "zwp_linux_buffer_params_v1", 4, 4, cui__zwp_params_requests, 2, cui__zwp_params_events };
static const struct wl_message cui__zwp_feedback_requests[] = { { "destroy", "", cui__zwp_types } };
static const struct wl_message cui__zwp_feedback_events[] = { { "done", "", cui__zwp_types }, { "format_table", "hu", cui__zwp_types },
                                                              { "main_device", "a", cui__zwp_types }, { "tranche_done", "", cui__zwp_types },
                                                              { "tranche_target_device", "a", cui__zwp_types }, { "tranche_formats", "a", cui__zwp_types },
                                                              { "tranche_flags", "u", cui__zwp_types } };
static const struct wl_interface cui__zwp_feedback_interface = { "zwp_linux_dmabuf_feedback_v1", 4, 1, cui__zwp_feedback_requests, 7, cui__zwp_feedback_events };

#define CUI__DRM_ARGB8888 0x34325241u   /* fourcc AR24: B, G, R, A in memory, premultiplied */
#define CUI__DRM_ABGR8888 0x34324241u   /* fourcc AB24: R, G, B, A in memory */

static void cui__zwp_format(void *data, void *dmabuf, uint32_t format) { (void)data; (void)dmabuf; (void)format; }
static void cui__zwp_modifier(void *data, void *dmabuf, uint32_t format, uint32_t hi, uint32_t lo) {
    cui_gpu_renderer *r = data;
    (void)dmabuf;
    if (format == CUI__DRM_ARGB8888 && r->modifier_count < (int)(sizeof r->modifiers / sizeof r->modifiers[0])) r->modifiers[r->modifier_count++] = ((uint64_t)hi << 32) | lo;
}
static void cui__zwp_fb_done(void *data, void *fb) { (void)data; (void)fb; }
static void cui__zwp_fb_table(void *data, void *fb, int32_t fd, uint32_t size) { (void)data; (void)fb; (void)size; close(fd); }
static void cui__zwp_fb_main(void *data, void *fb, struct wl_array *dev) {
    cui_gpu_renderer *r = data;
    (void)fb;
    if (dev->size == sizeof(dev_t)) { dev_t d; memcpy(&d, dev->data, sizeof d); r->main_device = (uint64_t)d; r->main_device_known = true; }
}
static void cui__zwp_fb_tranche(void *data, void *fb) { (void)data; (void)fb; }
static void cui__zwp_fb_array(void *data, void *fb, struct wl_array *a) { (void)data; (void)fb; (void)a; }
static void cui__zwp_fb_flags(void *data, void *fb, uint32_t flags) { (void)data; (void)fb; (void)flags; }

static void cui__wl_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    cui_gpu_renderer *r = data;
    if (!strcmp(iface, "wl_compositor")) r->wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, "wl_subcompositor")) r->wl_subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "wp_viewporter")) r->wp_viewporter = wl_registry_bind(registry, name, &cui__wp_viewporter_interface, 1);
    else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && version >= 3) {
        static const void *modifiers[] = { (void *)cui__zwp_format, (void *)cui__zwp_modifier };
        r->dmabuf = wl_registry_bind(registry, name, &cui__zwp_dmabuf_interface, 3);
        wl_proxy_add_listener(r->dmabuf, (void (**)(void))modifiers, r);
        if (version >= 4) {
            static const void *feedback[] = { (void *)cui__zwp_fb_done, (void *)cui__zwp_fb_table, (void *)cui__zwp_fb_main, (void *)cui__zwp_fb_tranche,
                                               (void *)cui__zwp_fb_array, (void *)cui__zwp_fb_array, (void *)cui__zwp_fb_flags };
            void *four = wl_registry_bind(registry, name, &cui__zwp_dmabuf_interface, 4);
            r->dmabuf_feedback = wl_proxy_marshal_flags(four, 2, &cui__zwp_feedback_interface, 4, 0, NULL);
            wl_proxy_add_listener(r->dmabuf_feedback, (void (**)(void))feedback, r);
            wl_proxy_marshal_flags(four, 0, NULL, 4, WL_MARSHAL_FLAG_DESTROY);
        }
    }
}
static void cui__wl_global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }

/* What subsurfaces are made with, bound on the program's display through a queue of the renderer's own. */
static bool cui__vk_host(cui_gpu_renderer *r) {
    static const struct wl_registry_listener listener = { cui__wl_global, cui__wl_global_remove };
    struct wl_display *display = r->xwin->display;
    struct wl_event_queue *queue = wl_display_create_queue(display);
    struct wl_display *wrapper = queue ? wl_proxy_create_wrapper(display) : NULL;
    if (!wrapper) { if (queue) wl_event_queue_destroy(queue); return false; }
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
    struct wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &listener, r);
    wl_display_roundtrip_queue(display, queue);
    if (r->dmabuf) wl_display_roundtrip_queue(display, queue);   /* (the modifiers and the main device) */
    wl_registry_destroy(registry);
    if (r->wl_compositor) wl_proxy_set_queue(r->wl_compositor, NULL);
    if (r->wl_subcompositor) wl_proxy_set_queue(r->wl_subcompositor, NULL);
    if (r->wp_viewporter) wl_proxy_set_queue(r->wp_viewporter, NULL);
    if (r->dmabuf_feedback) { wl_proxy_marshal_flags(r->dmabuf_feedback, 0, NULL, 4, WL_MARSHAL_FLAG_DESTROY); r->dmabuf_feedback = NULL; }
    r->plain_queue = queue;
    return r->wl_compositor && r->wl_subcompositor && r->wp_viewporter;
}

/* Whole points and the pixels they are: a subsurface is placed and sized in points, so a picture
 * is shown one pixel to one only when its box of pixels falls on them. */
static void cui__vk_grid(const cui_gpu_renderer *r, int *points, int *pixels) {
    float scale = r->xwin->scale > 0 ? r->xwin->scale : 1;
    *points = 1;
    *pixels = (int)(scale + 0.5f) > 0 ? (int)(scale + 0.5f) : 1;
    for (int g = 1; g <= 8; g++) {
        float px = (float)g * scale;
        if (fabsf(px - roundf(px)) < 0.001f) { *points = g; *pixels = (int)roundf(px); return; }
    }
}

static int cui__vk_floor_to(int v, int step) { return (int)floorf((float)v / (float)step) * step; }
static int cui__vk_ceil_to(int v, int step) { return (int)ceilf((float)v / (float)step) * step; }

/* A zone's own subsurface of the program's surface: no input, and nothing shown until it is drawn. */
static bool cui__vk_zone_own(cui_gpu_renderer *r, cui__vk_zone *z) {
    if (z->wl_surface) return true;
    struct wl_surface *surface = wl_compositor_create_surface(r->wl_compositor);
    if (!surface) return false;
    struct wl_region *none = wl_compositor_create_region(r->wl_compositor);
    wl_surface_set_input_region(surface, none);
    wl_region_destroy(none);
    z->sub = wl_subcompositor_get_subsurface(r->wl_subcompositor, surface, r->xwin->wl_surface);   /* (synchronized) */
    z->viewport = wl_proxy_marshal_flags(r->wp_viewporter, 1, &cui__wp_viewport_interface, wl_proxy_get_version(r->wp_viewporter), 0, NULL, surface);
    z->wl_surface = surface;
    z->own = true;
    return true;
}

/* Where a zone's picture lies and how large it is shown, from the program's surface's next commit. */
static void cui__vk_zone_place(cui_gpu_renderer *r, cui__vk_zone *z, int x, int y, int w, int h, int order) {
    int points, pixels;
    cui__vk_grid(r, &points, &pixels);
    if (z->x != x || z->y != y || !z->shown) wl_subsurface_set_position(z->sub, x / pixels * points, y / pixels * points);
    if (z->w != w || z->h != h || !z->shown) wl_proxy_marshal_flags(z->viewport, 2, NULL, wl_proxy_get_version(z->viewport), 0, w / pixels * points, h / pixels * points);
    z->x = x; z->y = y; z->w = w; z->h = h; z->order = order;
    r->place_due = true;
}

/* The zones' subsurfaces in their order, and the program's surface's commit: place, size and picture land together. */
static void cui__vk_zones_stack(cui_gpu_renderer *r) {
    int sorted[CUI__TV_ZONES], n = 0;
    sorted[n++] = CUI_SURFACES;
    for (int i = 0; i < CUI_SURFACES; i++) {
        int k = n++;
        while (k > 1 && r->zone[sorted[k - 1]].order > r->zone[i].order) { sorted[k] = sorted[k - 1]; k--; }
        sorted[k] = i;
    }
    struct wl_surface *below = r->xwin->wl_surface;
    for (int i = 0; i < n; i++) {
        cui__vk_zone *z = &r->zone[sorted[i]];
        if (!z->own || !z->shown) continue;
        wl_subsurface_place_above(z->sub, below);
        below = z->wl_surface;
    }
    wl_surface_commit(r->xwin->wl_surface);
    r->place_due = false;
}

/* A zone that holds nothing: off the screen, and its swapchain let go of. */
static double cui__vk_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* off the screen, holding nothing */
static void cui__vk_zone_hide(cui_gpu_renderer *r, cui__vk_zone *z) {
    if (z->own && z->shown) {
        for (int i = 0; i < r->shown_count; i++) if (r->shown[i].zone == z) { cui_gpu_surfaces_end(r); break; }   /* (its last picture is shown first) */
        wl_surface_attach(z->wl_surface, NULL, 0, 0);
        wl_surface_commit(z->wl_surface);
        z->shown = false;
        r->place_due = true;   /* (it goes with the program's surface's next commit) */
        z->src_w = z->src_h = 0;
    }
    cui__vk_zone_retire(r, z, false);
}

static cui__vk_zone *cui__vk_zone_of(cui_gpu_renderer *r, int slot) {
    if (!r || slot < -1 || slot >= CUI_SURFACES) return NULL;
    return &r->zone[slot < 0 ? CUI_SURFACES : slot];
}

/* A zone's swapchain (and, with `surface`, the surface it is on) is let go of: it goes once the
 * last submission that drew into it is done. Nothing waits. */
static void cui__vk_zone_retire(cui_gpu_renderer *r, cui__vk_zone *z, bool surface) {
    for (int i = 0; i < r->shown_count; i++) if (r->shown[i].zone == z) { cui_gpu_surfaces_end(r); break; }
#ifdef CUI_WAYLAND
    if (r->plain) { cui__vk_plain_drop(r, z); return; }
#endif
    if (!z->swapchain && !(surface && z->surface)) return;
    if (r->retired_count == r->retired_cap) {
        int cap = r->retired_cap ? r->retired_cap * 2 : 16;
        cui__vk_retired *grown = realloc(r->retired, (size_t)cap * sizeof *grown);
        if (grown) { r->retired = grown; r->retired_cap = cap; }
    }
    cui__vk_retired gone;
    memset(&gone, 0, sizeof gone);
    gone.value = z->value;
    gone.swapchain = z->swapchain;
    gone.on = surface ? VK_NULL_HANDLE : z->surface;
    memcpy(gone.views, z->views, sizeof gone.views);
    memcpy(gone.sems, z->present_sem, sizeof z->present_sem);
    if (surface) {
        gone.surface = z->surface;
        memcpy(gone.sems + CUI__TV_MAX_IMAGES, z->acquire_sem, sizeof z->acquire_sem);
        memset(z->acquire_sem, 0, sizeof z->acquire_sem);
        z->surface = VK_NULL_HANDLE;
    }
    z->swapchain = VK_NULL_HANDLE;
    memset(z->views, 0, sizeof z->views);
    memset(z->present_sem, 0, sizeof z->present_sem);
    z->image_count = 0;
    z->extent.width = z->extent.height = 0;
    if (r->retired_count < r->retired_cap) r->retired[r->retired_count++] = gone;
    else if (cui__vk_wait(r, gone.value)) cui__vk_retired_free(r, &gone);   /* (out of memory for the list: waited for, this once) */
}

void cui_gpu_surface_window(cui_gpu_renderer *r, int slot, void *wl_surface) {
    cui__vk_zone *z = cui__vk_zone_of(r, slot);
    if (!z || z->own || z->wl_surface == wl_surface) return;
    cui__vk_zone_retire(r, z, true);   /* another surface of the system's: what was made on the old one goes */
    z->wl_surface = wl_surface;
}

/* (re)makes a zone's swapchain at w by h. The one before it goes when its work is done. */
static bool cui__vk_zone_swapchain(cui_gpu_renderer *r, cui__vk_zone *z, uint32_t w, uint32_t h) {
    VkDevice dev = r->device;
    if (!z->surface) {
        VkWaylandSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
        info.display = (struct wl_display *)r->xwin->display;
        info.surface = (struct wl_surface *)z->wl_surface;
        VkBool32 can = VK_FALSE;
        if (vkCreateWaylandSurfaceKHR(r->instance, &info, NULL, &z->surface) != VK_SUCCESS) { z->surface = VK_NULL_HANDLE; return false; }
        if (vkGetPhysicalDeviceSurfaceSupportKHR(r->physical, r->family, z->surface, &can) != VK_SUCCESS || !can) return false;
    }
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, z->surface, &caps) != VK_SUCCESS) return false;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;   /* clear where it draws nothing */
    if (!(caps.supportedCompositeAlpha & alpha)) return false;
    /* as few images as the surface allows: one is rarely drawn, and each is a picture of its whole size */
    uint32_t count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = z->swapchain;
    for (int i = r->retired_count - 1; i >= 0 && !old; i--) if (r->retired[i].on == z->surface) old = r->retired[i].swapchain;   /* one let go of while hidden still holds the surface */
    cui__vk_zone_retire(r, z, false);
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = z->surface;
    info.minImageCount = count;
    info.imageFormat = r->format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent.width = w; info.imageExtent.height = h;
    info.imageArrayLayers = 1;
    z->can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (z->can_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &info, NULL, &sc) != VK_SUCCESS) return false;
    z->swapchain = sc;
    z->extent = info.imageExtent;
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) z->missed_count[i] = -1;   /* new images hold nothing */
    uint32_t images = 0;
    vkGetSwapchainImagesKHR(dev, sc, &images, NULL);
    if (images == 0 || images > CUI__TV_MAX_IMAGES || vkGetSwapchainImagesKHR(dev, sc, &images, z->images) != VK_SUCCESS) return false;
    z->image_count = images;
    for (uint32_t i = 0; i < images; i++) {
        VkImageViewCreateInfo v = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        v.image = z->images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = r->format;
        v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateImageView(dev, &v, NULL, &z->views[i]) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(dev, &sem, NULL, &z->present_sem[i]) != VK_SUCCESS) return false;
    }
    for (int i = 0; i < CUI__TV_FRAMES; i++) {
        VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!z->acquire_sem[i] && vkCreateSemaphore(dev, &sem, NULL, &z->acquire_sem[i]) != VK_SUCCESS) return false;
    }
    return true;
}

/* Boxes joined until none meets another: what lies in two would be drawn twice. */
static int cui__vk_boxes_apart(cui_rect *boxes, int n) {
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            cui_rect p = boxes[a], q = boxes[b];
            if (p.x >= q.x + q.w || q.x >= p.x + p.w || p.y >= q.y + q.h || q.y >= p.y + p.h) continue;
            float x0 = p.x < q.x ? p.x : q.x, y0 = p.y < q.y ? p.y : q.y;
            float x1 = p.x + p.w > q.x + q.w ? p.x + p.w : q.x + q.w, y1 = p.y + p.h > q.y + q.h ? p.y + p.h : q.y + q.h;
            boxes[a] = (cui_rect){ x0, y0, x1 - x0, y1 - y0 };
            boxes[b] = boxes[--n];
            a = -1;
            break;
        }
    }
    return n;
}

void cui_gpu_surfaces_end(cui_gpu_renderer *r) {
    if (!r) return;
    if (r->plain) cui__vk_plain_reap(r);
    if (!r->shown_count) {   /* no plain picture drawn: surfaces that were presented, only moved, or left */
        if (r->place_due) cui__vk_zones_stack(r);
        return;
    }
    int n = r->shown_count;
    r->shown_count = 0;
    cui__vk_plain_show(r, n);
    cui__vk_zones_stack(r);   /* (after the pictures: their commits wait for the program's surface's) */
}

/* Draws dd into the slot's picture, which is the rectangle `part` of dd's frame in pixels and of
 * part's size. With d, the tracker cui_damage_track was just called on with dd, only the boxes that
 * changed are drawn, and those the image it draws into missed since it was last drawn; without, all
 * of it. With rgba the picture is drawn whole and read back, which waits for it. False when it
 * could not be drawn. */
/* ---- plain pictures: the renderer's own images handed to the compositor as dma-buf wl_buffers ---- */

static void cui__vk_plain_release(void *data, struct wl_buffer *buffer) {
    cui_gpu_renderer *r = data;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) if (r->plain_at[i].wl_buffer == (void *)buffer) cui_pool_held(&r->pictures, i, false);
}
static const struct wl_buffer_listener cui__vk_plain_listener = { cui__vk_plain_release };
static void cui__vk_plain_created(void *data, void *params, struct wl_buffer *buffer) { (void)params; *(struct wl_buffer **)data = buffer; }
static void cui__vk_plain_failed(void *data, void *params) { (void)data; (void)params; }

static void cui__vk_plain_free(cui_gpu_renderer *r, cui__vk_plain *b) {
    if (b->wl_buffer) wl_buffer_destroy(b->wl_buffer);
    if (b->view) vkDestroyImageView(r->device, b->view, NULL);
    if (b->image) vkDestroyImage(r->device, b->image, NULL);
    if (b->memory) vkFreeMemory(r->device, b->memory, NULL);
    if (b->linear) vkDestroyBuffer(r->device, b->linear, NULL);
    if (b->linear_memory) vkFreeMemory(r->device, b->linear_memory, NULL);
    if (b->reads) vkDestroySemaphore(r->device, b->reads, NULL);
    if (b->made && b->fd >= 0) close(b->fd);
    memset(b, 0, sizeof *b);
}

/* plain_copied: the picture's exported linear copy in system memory, rows on 256 bytes */
static bool cui__vk_plain_linear(cui_gpu_renderer *r, cui__vk_plain *b) {
    VkDevice dev = r->device;
    b->pitch = (b->w * 4 + 255) & ~255u;
    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext };
    bci.size = (VkDeviceSize)b->pitch * b->h;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, NULL, &b->linear) != VK_SUCCESS) { b->linear = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->linear, &req);
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b->linear };
    VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exported, req.size, cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &b->linear_memory) != VK_SUCCESS) { b->linear_memory = VK_NULL_HANDLE; return false; }
    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, b->linear_memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    return vkBindBufferMemory(dev, b->linear, b->linear_memory, 0) == VK_SUCCESS && r->get_memory_fd(dev, &gfi, &b->fd) == VK_SUCCESS;
}

/* A w by h picture the compositor imports, at pool slot `at`: one plane, or a linear copy */
static bool cui__vk_plain_make(cui_gpu_renderer *r, int at, uint32_t w, uint32_t h) {
    cui__vk_plain *b = &r->plain_at[at];
    memset(b, 0, sizeof *b);
    b->made = true;
    b->fd = -1;
    b->w = w; b->h = h;
    VkDevice dev = r->device;
    VkImageDrmFormatModifierListCreateInfoEXT mods = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT };
    mods.drmFormatModifierCount = (uint32_t)r->modifier_count;
    mods.pDrmFormatModifiers = r->modifiers;
    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mods, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    bool copied = r->plain_copied;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, copied ? NULL : &ext };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = r->format;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = copied ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (copied ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req;
    VkImageDrmFormatModifierPropertiesEXT chosen = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    VkImageSubresource plane0 = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout layout;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    bool ok = vkCreateImage(dev, &ici, NULL, &b->image) == VK_SUCCESS;
    if (ok) {
        vkGetImageMemoryRequirements(dev, b->image, &req);
        VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, b->image, VK_NULL_HANDLE };
        VkExportMemoryAllocateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, copied ? (void *)&dedicated : (void *)&exported, req.size,
                                     cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        ok = mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(dev, &mai, NULL, &b->memory) == VK_SUCCESS && vkBindImageMemory(dev, b->image, b->memory, 0) == VK_SUCCESS;
    }
    if (ok && copied) {
        ok = cui__vk_plain_linear(r, b);
        chosen.drmFormatModifier = 0;   /* (DRM_FORMAT_MOD_LINEAR) */
        layout.offset = 0;
        layout.rowPitch = b->pitch;
    } else if (ok) {
        VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, b->memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        ok = r->get_memory_fd(dev, &gfi, &b->fd) == VK_SUCCESS && r->get_modifier(dev, b->image, &chosen) == VK_SUCCESS;
        if (ok) vkGetImageSubresourceLayout(dev, b->image, &plane0, &layout);
    }
    if (ok) {
        VkImageViewCreateInfo v = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        v.image = b->image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = r->format;
        v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        ok = vkCreateImageView(dev, &v, NULL, &b->view) == VK_SUCCESS && vkCreateSemaphore(dev, &sci, NULL, &b->reads) == VK_SUCCESS;
    }
    if (ok) {
        static const void *listener[] = { (void *)cui__vk_plain_created, (void *)cui__vk_plain_failed };
        struct wl_buffer *made = NULL;
        uint32_t fourcc = r->format == VK_FORMAT_B8G8R8A8_UNORM ? CUI__DRM_ARGB8888 : CUI__DRM_ABGR8888;
        struct wl_proxy *params = wl_proxy_marshal_flags(r->dmabuf, 1, &cui__zwp_params_interface, 3, 0, NULL);
        wl_proxy_add_listener(params, (void (**)(void))listener, &made);
        wl_proxy_marshal_flags(params, 1, NULL, 3, 0, b->fd, 0u, (uint32_t)layout.offset, (uint32_t)layout.rowPitch,
                               (uint32_t)(chosen.drmFormatModifier >> 32), (uint32_t)chosen.drmFormatModifier);
        wl_proxy_marshal_flags(params, 2, NULL, 3, 0, (int32_t)w, (int32_t)h, fourcc, 0u);
        wl_display_roundtrip_queue(r->xwin->display, r->plain_queue);
        wl_proxy_marshal_flags(params, 0, NULL, 3, WL_MARSHAL_FLAG_DESTROY);
        if ((ok = made != NULL)) {
            wl_proxy_set_queue((struct wl_proxy *)made, NULL);
            wl_buffer_add_listener(made, &cui__vk_plain_listener, r);
            b->wl_buffer = made;
        }
    }
    if (!ok) { cui__vk_plain_free(r, b); return false; }
    return true;
}

/* Plain pictures are used when the compositor imports what this device makes; else each surface has a swapchain. */
static void cui__vk_plain_setup(cui_gpu_renderer *r) {
    if (!r->dmabuf || !r->modifier_count || !r->main_device_known) return;
    r->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(r->device, "vkGetMemoryFdKHR");
    r->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(r->device, "vkGetSemaphoreFdKHR");
    r->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(r->device, "vkImportSemaphoreFdKHR");
    r->get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(r->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!r->get_memory_fd || !r->get_semaphore_fd || !r->import_semaphore_fd || !r->get_modifier) return;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(r->physical, NULL, &n, NULL);
    VkExtensionProperties *have = n ? malloc(sizeof *have * n) : NULL;
    bool drm = false;
    if (have && vkEnumerateDeviceExtensionProperties(r->physical, NULL, &n, have) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) drm = drm || !strcmp(have[i].extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    free(have);
    if (!drm) return;
    VkPhysicalDeviceDrmPropertiesEXT nodes = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &nodes };
    vkGetPhysicalDeviceProperties2(r->physical, &props);
    dev_t main = (dev_t)r->main_device;
    bool same = (nodes.hasPrimary && makedev(nodes.primaryMajor, nodes.primaryMinor) == main) || (nodes.hasRender && makedev(nodes.renderMajor, nodes.renderMinor) == main);
    if (!same) {   /* another GPU composites: it reads a linear copy in system memory, as PRIME does */
        VkPhysicalDeviceExternalBufferInfo ebi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkExternalBufferProperties ebp = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        vkGetPhysicalDeviceExternalBufferProperties(r->physical, &ebi, &ebp);
        bool linear = false;
        for (int i = 0; i < r->modifier_count; i++) linear = linear || r->modifiers[i] == 0;
        if (!linear || !(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) return;
        r->plain_copied = true;
    } else {
        VkDrmFormatModifierPropertiesEXT listed[64];
        VkDrmFormatModifierPropertiesListEXT list = { VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, NULL, 64, listed };
        VkFormatProperties2 fp = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list };
        vkGetPhysicalDeviceFormatProperties2(r->physical, r->format, &fp);
        int kept = 0;
        for (int i = 0; i < r->modifier_count; i++)
            for (uint32_t k = 0; k < list.drmFormatModifierCount; k++)
                if (listed[k].drmFormatModifier == r->modifiers[i] && listed[k].drmFormatModifierPlaneCount == 1 &&
                    (listed[k].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) { r->modifiers[kept++] = r->modifiers[i]; break; }
        r->modifier_count = kept;
        if (!kept) return;
    }
    VkExportSemaphoreCreateInfo exported = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, NULL, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exported };
    if (vkCreateSemaphore(r->device, &sci, NULL, &r->plain_done) != VK_SUCCESS) { r->plain_done = VK_NULL_HANDLE; return; }
    if (!cui__vk_plain_make(r, 0, CUI__TV_FIT, CUI__TV_FIT)) return;   /* a probe */
    cui__vk_plain_free(r, &r->plain_at[0]);
    r->reap_due = -1;
    r->plain = true;
}

/* The element a zone's pictures are kept for: the one it was told, else the slot's own. */
static cui_id cui__vk_zone_id(cui_gpu_renderer *r, const cui__vk_zone *z) {
    return z->element ? z->element : 0xFFFFFF00u + (cui_id)(z - r->zone);
}

/* A zone's pictures become spare, as they are: the element that comes back finds them. */
static void cui__vk_plain_drop(cui_gpu_renderer *r, cui__vk_zone *z) {
    cui_pool_give_back(&r->pictures, cui__vk_zone_id(r, z), cui__vk_seconds());
    z->image_count = 0;
    z->plain_front = 0;
    z->plain_key = 0;
    z->element = 0;
    z->tracked = z->attached = false;
}

/* plain_copied: what a frame drew (boxes of the list's frame, or all) goes to the linear copy. */
static void cui__vk_plain_copy(cui_gpu_renderer *r, cui__vk_plain *b, bool before, const cui_rect *boxes, int n, int ox, int oy) {
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (!cmd) return;
    VkImageMemoryBarrier drawn = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    drawn.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    drawn.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    drawn.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    drawn.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    drawn.srcQueueFamilyIndex = drawn.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    drawn.image = b->image;
    drawn.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    drawn.subresourceRange.levelCount = 1;
    drawn.subresourceRange.layerCount = 1;
    VkBufferMemoryBarrier take = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    take.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    take.srcQueueFamilyIndex = before ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
    take.dstQueueFamilyIndex = before ? r->family : VK_QUEUE_FAMILY_IGNORED;
    take.buffer = b->linear;
    take.size = VK_WHOLE_SIZE;
    /* (from the color output, where the submission waits for the compositor's reads) */
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &take, 1, &drawn);
    VkBufferImageCopy regions[CUI_DAMAGE_BOXES + CUI__TV_MISSED];
    uint32_t count = 0;
    for (int i = 0; i < (boxes ? n : 1) && count < sizeof regions / sizeof regions[0]; i++) {
        int x0 = 0, y0 = 0, x1 = (int)b->w, y1 = (int)b->h;
        if (boxes) {   /* (whole pixels around the box: all a draw clipped to it can touch) */
            int bx0 = (int)floorf(boxes[i].x) - ox, by0 = (int)floorf(boxes[i].y) - oy;
            int bx1 = (int)ceilf(boxes[i].x + boxes[i].w) - ox, by1 = (int)ceilf(boxes[i].y + boxes[i].h) - oy;
            if (bx0 > x0) x0 = bx0;
            if (by0 > y0) y0 = by0;
            if (bx1 < x1) x1 = bx1;
            if (by1 < y1) y1 = by1;
        }
        if (x1 <= x0 || y1 <= y0) continue;
        VkBufferImageCopy *c = &regions[count++];
        memset(c, 0, sizeof *c);
        c->bufferOffset = (VkDeviceSize)y0 * b->pitch + (VkDeviceSize)x0 * 4;
        c->bufferRowLength = b->pitch / 4;
        c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c->imageSubresource.layerCount = 1;
        c->imageOffset.x = x0; c->imageOffset.y = y0;
        c->imageExtent.width = (uint32_t)(x1 - x0); c->imageExtent.height = (uint32_t)(y1 - y0); c->imageExtent.depth = 1;
    }
    if (count) vkCmdCopyImageToBuffer(cmd, b->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->linear, count, regions);
    VkBufferMemoryBarrier give = take;
    give.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    give.dstAccessMask = 0;
    give.srcQueueFamilyIndex = r->family;
    give.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &give, 0, NULL);
}

/* What cui__vk_zone_update does for a zone of the renderer's own, with pool pictures instead of a swapchain. */
static bool cui__vk_plain_update(cui_gpu_renderer *r, cui__vk_zone *z, const cui_draw_data *dd, cui_damage *d, int ox, int oy, int W, int H) {
    cui_pool *pool = &r->pictures;
    cui_id id = cui__vk_zone_id(r, z);
    int kw = W, kh = H;
    if (r->live_resize && z->plain_key) {   /* a drag keeps what fits, grows by a quarter; settled once it ends */
        int cw = cui_pool_key_width(z->plain_key), ch = cui_pool_key_height(z->plain_key);
        kw = W <= cw ? cw : CUI_MIN(W + W / 4, 8192);
        kh = H <= ch ? ch : CUI_MIN(H + H / 4, 8192);
    }
    uint16_t key = cui_pool_key(kw, kh);
    double now = cui__vk_seconds();
    if (z->plain_key && z->plain_key != key) {   /* another bucket: the old size's spares are first to go */
        cui_pool_give_back(pool, id, now - CUI_POOL_SPARE_SECONDS);
        z->plain_front = 0;
    }
    z->plain_key = key;
    bool make = false;
    int evict = -1, at = cui_pool_take(pool, id, kw, kh, z->plain_front - 1, now, &make, &evict);
    if (evict >= 0) {   /* a full table: this frame is drawn again next tick, into a new picture */
        if (cui__vk_wait(r, pool->slot[evict].gpu_value)) cui__vk_plain_free(r, &r->plain_at[evict]);
        else { memset(&r->plain_at[evict], 0, sizeof r->plain_at[evict]); r->lost = true; }
        r->left_out = true;
    }
    r->plain_none = at < 0 && key;
    if (at < 0) return false;   /* all held: drawn with the next frame */
    cui__vk_plain *b = &r->plain_at[at];
    uint32_t bw = (uint32_t)cui_pool_key_width(key), bh = (uint32_t)cui_pool_key_height(key);   /* the surface crops */
    if (!make && (b->w != bw || b->h != bh)) {
        if (!cui__vk_wait(r, pool->slot[at].gpu_value)) return false;
        cui__vk_plain_free(r, b);
        cui_pool_cleared(pool, at, true);
        make = true;
    }
    if (make && !cui__vk_plain_make(r, at, bw, bh)) { cui_pool_drop(pool, at); return false; }
    if (b->part_x != ox || b->part_y != oy || b->part_w != W || b->part_h != H) cui_pool_cleared(pool, at, false);   /* drawn for another part */
    b->part_x = ox; b->part_y = oy; b->part_w = W; b->part_h = H;
    if (!z->tracked && !z->attached) d = NULL;   /* the caller's tracker knows nothing of a pool picture */
    const cui_pool_slot *kept = &pool->slot[at];
    bool before = (kept->flags & CUI_POOL_DRAWN) != 0;   /* drawn into: the compositor may read it */
    z->extent.width = b->w; z->extent.height = b->h;
    if (z->own && (z->src_w != W || z->src_h != H)) {
        wl_proxy_marshal_flags(z->viewport, 1, NULL, wl_proxy_get_version(z->viewport), 0, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(W), wl_fixed_from_int(H));
        z->src_w = W; z->src_h = H;
    }
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (!cmd) return false;
    if (before) {   /* what the compositor still reads finishes before it is drawn into */
        struct dma_buf_export_sync_file out = { DMA_BUF_SYNC_WRITE, -1 };
        if (ioctl(b->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &out) == 0 && out.fd >= 0) {
            VkImportSemaphoreFdInfoKHR in = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, NULL, b->reads, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, out.fd };
            if (r->import_semaphore_fd(r->device, &in) == VK_SUCCESS && r->plain_wait_count < CUI__TV_ZONES) r->plain_waits[r->plain_wait_count++] = b->reads;
            else close(out.fd);
        }
    }
    cui_rect boxes[CUI_DAMAGE_BOXES + CUI__TV_MISSED], told[CUI_DAMAGE_BOXES];
    int own = d ? cui_damage_boxes(d, told, CUI_DAMAGE_BOXES) : -1, n = own > 0 ? own : 0;
    int behind = kept->missed_count;
    bool partial = own >= 0 && behind >= 0 && before;
    const cui_draw_data *cut = NULL;
    if (partial) {
        memcpy(boxes, told, sizeof *boxes * (size_t)n);
        memcpy(boxes + n, kept->missed, sizeof *boxes * (size_t)behind);
        n = cui__vk_boxes_apart(boxes, n + behind);
        int need = n ? cui__damage_clip_need(dd, n) : 0;
        if (need > r->clip_cap) {
            cui_draw_cmd *grown = realloc(r->clip_cmds, sizeof *grown * (size_t)need * 2);
            if (grown) { r->clip_cmds = grown; r->clip_cap = need * 2; }
        }
        if (n && need <= r->clip_cap) {
            r->clip_data = *dd;
            r->clip_data.commands = r->clip_cmds;
            r->clip_data.command_count = cui__damage_clip_to(dd, boxes, n, r->clip_cmds);
            cut = &r->clip_data;
        }
        if (n && !cut) partial = false;
    }
    VkImageMemoryBarrier take = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    take.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    take.oldLayout = before ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    take.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    take.srcQueueFamilyIndex = before ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
    take.dstQueueFamilyIndex = before ? r->family : VK_QUEUE_FAMILY_IGNORED;
    take.image = b->image;
    take.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    take.subresourceRange.levelCount = 1;
    take.subresourceRange.layerCount = 1;
    if (!r->plain_copied) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &take);
    bool untouched = partial && !n;
    VkImageLayout from = !r->plain_copied ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : before ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    cui__vk_target target = { b->image, b->view, from, z->extent, partial ? boxes : NULL, n, ox, oy };
    bool drawn = untouched || cui__vk_draw(r, &target, partial ? cut : dd);
    cmd = cui__vk_cmd(r);
    if (!cmd) return false;
    if (r->plain_copied && !untouched) cui__vk_plain_copy(r, b, before, partial ? boxes : NULL, n, ox, oy);
    VkImageMemoryBarrier give = take;
    give.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    give.dstAccessMask = 0;
    give.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    give.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    give.srcQueueFamilyIndex = r->family;
    give.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    if (!r->plain_copied) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &give);
    cui__vk_shown *s = &r->shown[r->shown_count++];
    s->zone = z;
    s->image = (uint32_t)at;
    z->value = r->value + 1;
    cui_pool_drawn(pool, at, z->value, told, own);
    return drawn && !r->lost;
}

/* The frame's plain pictures: one submission, its fence put on each picture's dma-buf, then attach and commit. */
static void cui__vk_plain_show(cui_gpu_renderer *r, int n) {
    if (!cui__vk_cmd(r)) return;
    VkSemaphore done = r->plain_done;
    cui__vk_submit(r, r->plain_waits, r->plain_wait_count, done);
    r->plain_wait_count = 0;
    if (r->lost) return;
    int fence = -1;
    VkSemaphoreGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, NULL, done, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    if (r->get_semaphore_fd(r->device, &gfi, &fence) != VK_SUCCESS) fence = -1;
    bool fenced = fence >= 0;
    for (int i = 0; i < n && fenced; i++) {
        cui__vk_plain *b = &r->plain_at[r->shown[i].image];
        struct dma_buf_import_sync_file in = { DMA_BUF_SYNC_WRITE, fence };
        fenced = ioctl(b->fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &in) == 0;
    }
    if (fence >= 0) close(fence);
    /* GNOME applies a moved subsurface's place at once but holds its picture for the fence: drawn first */
    if (!fenced || r->place_due) cui__vk_wait(r, r->value);
    for (int i = 0; i < n; i++) {
        const cui__vk_shown *s = &r->shown[i];
        cui__vk_zone *z = s->zone;
        cui__vk_plain *b = &r->plain_at[s->image];
        wl_surface_attach(z->wl_surface, b->wl_buffer, 0, 0);
        wl_surface_damage_buffer(z->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(z->wl_surface);
        cui_pool_held(&r->pictures, (int)s->image, true);
        z->plain_front = (int)s->image + 1;
        z->attached = true;
    }
    cui__vk_collect(r, false);
}

/* At most one spare picture goes, once the GPU and the compositor are done with it. */
static void cui__vk_plain_reap(cui_gpu_renderer *r) {
    uint64_t done = 0;
    double now = cui__vk_seconds();
    if (vkGetSemaphoreCounterValue(r->device, r->timeline, &done) != VK_SUCCESS) return;
    int gone = cui_pool_reap(&r->pictures, now, done, &r->reap_due);
    if (gone < 0) return;
    cui__vk_plain_free(r, &r->plain_at[gone]);
}

static bool cui__vk_zone_update(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, cui_damage *d, cui_rect part, void *rgba) {
    cui__vk_zone *z = cui__vk_zone_of(r, slot);
    int W = (int)part.w, H = (int)part.h, ox = (int)part.x, oy = (int)part.y;
    if (!z || !dd || W <= 0 || H <= 0 || !cui_gpu_surfaces(r) || !z->wl_surface) return false;
    for (int i = 0; i < r->shown_count; i++) if (r->shown[i].zone == z) { cui_gpu_surfaces_end(r); break; }
    if (rgba) cui_gpu_surfaces_end(r);   /* (this one is a submission of its own) */
    if (r->plain) return !rgba && cui__vk_plain_update(r, z, dd, d, ox, oy, W, H);
    /* an owned subsurface shows the top left W by H of larger images, which a resize reuses */
    uint32_t ew = z->extent.width, eh = z->extent.height;
    bool snug = ew < (uint32_t)W + 2 * CUI__TV_FIT && eh < (uint32_t)H + 2 * CUI__TV_FIT;
    bool fits = z->swapchain && (z->own ? (uint32_t)W <= ew && (uint32_t)H <= eh && (snug || (long long)ew * eh <= 2LL * W * H)
                                        : ew == (uint32_t)W && eh == (uint32_t)H);
    if (!fits) {
        int cw = W, ch = H;
        if (z->own) {
            if (z->swapchain && (uint32_t)W > ew) cw = W + W / 4;
            if (z->swapchain && (uint32_t)H > eh) ch = H + H / 4;
            cw = (cw + CUI__TV_FIT - 1) / CUI__TV_FIT * CUI__TV_FIT;
            ch = (ch + CUI__TV_FIT - 1) / CUI__TV_FIT * CUI__TV_FIT;
        }
        if (!cui__vk_zone_swapchain(r, z, (uint32_t)cw, (uint32_t)ch)) { cui__vk_zone_retire(r, z, false); return false; }
    }
    if (z->own && (z->src_w != W || z->src_h != H)) {
        wl_proxy_marshal_flags(z->viewport, 1, NULL, wl_proxy_get_version(z->viewport), 0, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(W), wl_fixed_from_int(H));
        z->src_w = W; z->src_h = H;
    }
    if (z->part_x != ox || z->part_y != oy) {   /* another part of the frame: nothing its images hold is where it was */
        for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) z->missed_count[i] = -1;
        z->part_x = ox; z->part_y = oy;
    }
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (!cmd) return false;
    VkSemaphore acquire = z->acquire_sem[r->frame];
    uint32_t image = 0;
    VkResult got = vkAcquireNextImageKHR(r->device, z->swapchain, CUI__TV_ACQUIRE_NS, acquire, VK_NULL_HANDLE, &image);
    if (got == VK_TIMEOUT || got == VK_NOT_READY) return false;   /* (the compositor holds them all: drawn with the next frame) */
    if (got != VK_SUCCESS && got != VK_SUBOPTIMAL_KHR) { cui__vk_zone_retire(r, z, false); return false; }

    /* what this frame changed, and what the image has not seen since it was last drawn */
    cui_rect boxes[CUI_DAMAGE_BOXES + CUI__TV_MISSED], told[CUI_DAMAGE_BOXES];
    int own = d ? cui_damage_boxes(d, told, CUI_DAMAGE_BOXES) : -1, n = own > 0 ? own : 0;
    int behind = z->missed_count[image];
    bool partial = own >= 0 && behind >= 0 && !rgba;
    const cui_draw_data *cut = NULL;
    if (partial) {
        memcpy(boxes, told, sizeof *boxes * (size_t)n);
        memcpy(boxes + n, z->missed[image], sizeof *boxes * (size_t)behind);
        n = cui__vk_boxes_apart(boxes, n + behind);
        cut = n ? cui_damage_clip_rects(d, dd, boxes, n) : NULL;
        if (n && !cut) partial = false;
    }
    bool drawn = true, untouched = partial && !n;   /* (the image already holds this frame) */
    cui__vk_target target = { z->images[image], z->views[image], partial ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED, z->extent, partial ? boxes : NULL, n, ox, oy };
    if (!untouched) drawn = cui__vk_draw(r, &target, partial ? cut : dd);
    cui__vk_buffer readback = { 0 };
    cmd = cui__vk_cmd(r);
    if (rgba && cmd && z->can_capture && cui__vk_buffer_make(r, &readback, (size_t)W * (size_t)H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        cui__vk_barrier(r, z->images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)W; region.imageExtent.height = (uint32_t)H; region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, z->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        VkBufferMemoryBarrier host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        cui__vk_barrier(r, z->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else if (!untouched) {
        cui__vk_barrier(r, z->images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    z->value = r->value + 1;
    for (uint32_t i = 0; i < z->image_count; i++) {
        if (i == image) { z->missed_count[i] = 0; continue; }
        if (own < 0 || z->missed_count[i] < 0 || z->missed_count[i] + own > CUI__TV_MISSED) { z->missed_count[i] = -1; continue; }
        memcpy(z->missed[i] + z->missed_count[i], told, sizeof(cui_rect) * (size_t)own);
        z->missed_count[i] += own;
    }
    VkSemaphore present = z->present_sem[image];
    if (cui__vk_cmd(r)) cui__vk_submit(r, &acquire, 1, present);   /* (a frame whose work already ran still signals) */
    if (!r->lost) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &present;
        pi.swapchainCount = 1;
        pi.pSwapchains = &z->swapchain;
        pi.pImageIndices = &image;
        VkResult res = vkQueuePresentKHR(r->queue, &pi);
        r->place_due = true;   /* a synchronized subsurface's picture lands with the program's surface's commit */
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) cui__vk_zone_retire(r, z, false);   /* made again with its next draw */
    }
    if (!rgba) return drawn && !r->lost;
    cui_gpu_surfaces_end(r);
    if (!readback.buffer) return false;
    if (!cui__vk_wait(r, r->value)) {   /* the copy may still be writing it: it goes when the renderer does */
        r->kept = readback;
        return false;
    }
    bool read = !r->lost;
    if (read) {
        const uint8_t *src = readback.ptr;
        uint8_t *dst = rgba;
        bool bgra = r->format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0, count = (size_t)W * (size_t)H; i < count; i++, src += 4, dst += 4) {
            dst[0] = src[bgra ? 2 : 0]; dst[1] = src[1]; dst[2] = src[bgra ? 0 : 2]; dst[3] = src[3];
        }
    }
    cui__vk_buffer_free(r, &readback);
    return read && drawn;
}

/* A surface's list on a subsurface of the renderer's own: placed and drawn. */
static bool cui__vk_zone_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, cui_damage *d, int ox, int oy, int bx, int by, int bw, int bh, int order) {
    cui__vk_zone *z = cui__vk_zone_of(r, slot);
    if (!z || !cui__vk_zone_own(r, z)) return false;
    bool resized = z->w != bw || z->h != bh;
    if (!z->shown || resized || z->x != bx || z->y != by || z->order != order) cui__vk_zone_place(r, z, bx, by, bw, bh, order);
    cui_rect part = { (float)(bx - ox), (float)(by - oy), (float)bw, (float)bh };
    if (!cui__vk_zone_update(r, slot, dd, r->plain || (z->shown && !resized) ? d : NULL, part, NULL)) {
        r->left_out = true;
        return z->shown;
    }
    z->shown = true;
    return true;
}

/* The window's own list on a program's surface: on a subsurface of the window's size, below every surface. */
static bool cui__vk_base(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d) {
    int points, pixels;
    cui__vk_grid(r, &points, &pixels);
    int w = cui__vk_ceil_to(dd->width_px, pixels), h = cui__vk_ceil_to(dd->height_px, pixels);   /* (whole points) */
    return cui__vk_zone_draw(r, -1, dd, d, 0, 0, 0, 0, w, h, -1);
}

bool cui_gpu_surface_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, const cui_damage *d, int x, int y, int order) {
    if (!dd || slot < 0 || slot >= CUI_SURFACES || dd->width_px <= 0 || dd->height_px <= 0 || !cui_gpu_surfaces(r)) return false;
    if (!r->host) {   /* the program made the subsurface and places it (cui_gpu_surface_window) */
        cui_rect all = { 0, 0, (float)dd->width_px, (float)dd->height_px };
        if (!r->zone[slot].wl_surface) return false;
        if (cui__vk_zone_update(r, slot, dd, (cui_damage *)d, all, NULL)) return true;
        if (!r->plain || !r->plain_none) return false;
        r->left_out = true;   /* all held, or the table full: the frame is made again next tick */
        return true;
    }
    int points, pixels;
    cui__vk_grid(r, &points, &pixels);
    int x0 = cui__vk_floor_to(x, pixels), y0 = cui__vk_floor_to(y, pixels);
    return cui__vk_zone_draw(r, slot, dd, (cui_damage *)d, x, y, x0, y0, cui__vk_ceil_to(x + dd->width_px, pixels) - x0, cui__vk_ceil_to(y + dd->height_px, pixels) - y0, order);
}

void cui_gpu_surface_place(cui_gpu_renderer *r, int slot, int x, int y, int order) {
    cui__vk_zone *z = slot >= 0 ? cui__vk_zone_of(r, slot) : NULL;
    if (!z || !z->own || !z->shown) return;   /* (a surface of the program's own is the program's to place) */
    int points, pixels;
    cui__vk_grid(r, &points, &pixels);
    cui__vk_zone_place(r, z, cui__vk_floor_to(x, pixels), cui__vk_floor_to(y, pixels), z->w, z->h, order);
}

void cui_gpu_surface_hide(cui_gpu_renderer *r, int slot) {
    cui__vk_zone *z = slot >= 0 ? cui__vk_zone_of(r, slot) : NULL;
    if (z) cui__vk_zone_hide(r, z);
}

#else
/* an X11 window has no surfaces of the system's to lay over it: the lists go into the window's picture */
bool cui_gpu_surfaces(cui_gpu_renderer *r) { (void)r; return false; }
void cui_gpu_surface_window(cui_gpu_renderer *r, int slot, void *wl_surface) { (void)r; (void)slot; (void)wl_surface; }
bool cui_gpu_surface_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, const cui_damage *d, int x, int y, int order) {
    (void)r; (void)slot; (void)dd; (void)d; (void)x; (void)y; (void)order;
    return false;
}
void cui_gpu_surfaces_end(cui_gpu_renderer *r) { (void)r; }
void cui_gpu_surface_place(cui_gpu_renderer *r, int slot, int x, int y, int order) { (void)r; (void)slot; (void)x; (void)y; (void)order; }
void cui_gpu_surface_hide(cui_gpu_renderer *r, int slot) { (void)r; (void)slot; }
#endif

void cui_gpu_live_resize(cui_gpu_renderer *r, bool on) {
    if (!r || r->live_resize == on) return;
    r->live_resize = on;
    if (!on) r->left_out = true;   /* drawn again whole, into settled sizes */
}

float cui_gpu_next_wakeup(cui_gpu_renderer *r) {
#ifdef CUI_WAYLAND
    if (r && r->plain) return (float)r->reap_due;
#endif
    (void)r;
    return -1;
}

cui_damage *cui_gpu_surface_damage(cui_gpu_renderer *r, int slot, uint32_t element) {
#ifdef CUI_WAYLAND
    cui__vk_zone *z = slot >= 0 ? cui__vk_zone_of(r, slot) : NULL;
    if (!z || !r->plain || !element) return NULL;
    if (z->element != element) {
        for (int i = 0; i < r->shown_count; i++) if (r->shown[i].zone == z) { cui_gpu_surfaces_end(r); break; }
        if (z->plain_key || z->element) cui__vk_plain_drop(r, z);   /* another element's: spare until it is back */
        z->element = element;
    }
    int owner = cui_pool_owner_of(&r->pictures, element);
    z->tracked = owner >= 0;
    return owner < 0 ? NULL : &r->pictures.owner[owner].damage.head;
#else
    (void)r; (void)slot; (void)element;
    return NULL;
#endif
}

bool cui_gpu_left_out(cui_gpu_renderer *r) {
    bool was = r && r->left_out;
    if (r) r->left_out = false;
    return was;
}

#endif
