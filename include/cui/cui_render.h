/*
 * CUI renderers.
 *
 * CUI produces a plain list of vertices, indices and draw commands
 * (cui_draw_data). The renderers that ship with the library:
 *
 *   - OpenGL 3.3 core / OpenGL ES 3.0 (cui_gl_*)
 *   - Direct3D 12 on Windows, Vulkan on Linux and Metal on macOS (cui_gpu_*),
 *     in builds with CUI_D3D12, CUI_VULKAN or CUI_METAL
 *   - A portable software rasterizer (cui_sw_*) that renders into any 32 bit
 *     pixel buffer. It needs no GPU, which makes it useful for tests,
 *     screenshots, servers and as a fallback.
 *
 * Writing your own renderer (WebGPU, sokol, bgfx) takes one shader. See
 * docs/rendering.md.
 */
#ifndef CUI_RENDER_H
#define CUI_RENDER_H

#include "cui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- software renderer ---- */

typedef enum cui_sw_format {
    CUI_SW_RGBA = 0,   /* bytes R, G, B, A in memory */
    CUI_SW_BGRA = 1    /* bytes B, G, R, A in memory (Win32 DIBs, X11, Cairo) */
} cui_sw_format;

typedef struct cui_sw_renderer cui_sw_renderer;

CUI_API cui_sw_renderer *cui_sw_create(void);
CUI_API void cui_sw_destroy(cui_sw_renderer *r);
/* Texture callbacks to pass to cui_create / cui_set_renderer. */
CUI_API cui_renderer_iface cui_sw_iface(cui_sw_renderer *r);
/* Renders draw data into a pixel buffer of width x height. stride is in bytes. */
CUI_API void cui_sw_render(cui_sw_renderer *r, const cui_draw_data *dd, void *pixels,
                           int width, int height, int stride, cui_sw_format format);
/* Number of worker threads (1 = single threaded, default: number of cores up to 8). */
CUI_API void cui_sw_set_threads(cui_sw_renderer *r, int threads);
/* Incremental rendering: when the same buffer is passed again, with what the last
 * call drew still in it, only the parts that changed are drawn again. A frame
 * where only a button's hover changed costs that button. Off by default.
 * cui_sw_last_damage gives the rectangle the last call drew (in pixels), so only
 * that part needs copying to the screen; it returns false when nothing changed. */
CUI_API void cui_sw_set_incremental(cui_sw_renderer *r, bool on);
CUI_API bool cui_sw_last_damage(const cui_sw_renderer *r, int *x, int *y, int *w, int *h);

/* Writes a PNG file (RGBA pixels). Returns false on failure. */
CUI_API bool cui_write_png(const char *path, const void *rgba, int width, int height, int stride);

/* ---- Damage ---- */

/* What one frame's draw data changes against the frame before, so that only that rectangle is
 * drawn again. Works on the cui_instance records of cui_ctx_desc.instanced_quads; draw data
 * that is all vertices damages the whole of what its commands can draw in. See cui_damage.c. */
typedef struct cui_damage cui_damage;
typedef struct cui_damage_stats {
    uint64_t frames;      /* cui_damage_track calls */
    uint64_t unchanged;   /* nothing changed: nothing to draw or present */
    uint64_t partial;     /* a rectangle changed */
    uint64_t full;        /* the whole window is to be drawn */
    uint64_t pixels;      /* the pixels of the partial and full frames' rectangles, added up */
} cui_damage_stats;

CUI_API cui_damage *cui_damage_create(void);
CUI_API void cui_damage_destroy(cui_damage *d);
/* Compares dd with the draw data of the last call and remembers dd. False when nothing changed.
 * Otherwise rect is the changed rectangle in pixels (top-left origin), and *full says the whole
 * window is to be drawn: the first frame, another size or clear color, or most of it changed. */
CUI_API bool cui_damage_track(cui_damage *d, const cui_draw_data *dd, cui_rect *rect, bool *full);
/* What the last cui_damage_track changed as boxes: one, the rectangle round all of it. Returns how
 * many (0: nothing changed), or -1 for the whole picture. */
#define CUI_DAMAGE_BOXES 1
CUI_API int cui_damage_boxes(const cui_damage *d, cui_rect *out, int cap);
/* dd with only the commands and records that touch rect, their clips cut to it. Valid until
 * the next call with d. */
CUI_API const cui_draw_data *cui_damage_clip(cui_damage *d, const cui_draw_data *dd, cui_rect rect);
/* The same for the last cui_damage_track's boxes. The boxes go to out, CUI_DAMAGE_BOXES of room,
 * to be cleared before the data is drawn. NULL when out of memory: draw the whole frame. */
CUI_API const cui_draw_data *cui_damage_clip_boxes(cui_damage *d, const cui_draw_data *dd, cui_rect *out, int *count);
/* The same for any boxes that do not overlap, in dd's pixels: what a picture that missed frames
 * is brought up to date from. NULL when out of memory: draw the whole frame. */
CUI_API const cui_draw_data *cui_damage_clip_rects(cui_damage *d, const cui_draw_data *dd, const cui_rect *rects, int count);
/* True while frame after frame changes much of the window (scrolling, a resize, a video that fills
 * it): a picture kept to redraw part of then costs a copy of the window each frame and saves
 * nothing, so a renderer draws these frames straight into the window, and its kept picture goes
 * stale. False again at the first frame that changes little: that one is drawn whole into the kept
 * picture, and those after it in part. */
CUI_API bool cui_damage_direct(const cui_damage *d);
/* Several lists drawn into one target, one after another: the window's list first, at 0, 0 and
 * of the target's size, then each surface's (cui_get_surface) at its place, lowest order first.
 * Each has a tracker of its own; id says which list it is (0 for the window's, else the
 * surface's id), for a renderer to see that the lists are not the ones of the frame before. */
typedef struct cui_render_list {
    const cui_draw_data *draw;
    int x, y;                  /* where its top left lies in the target, pixels */
    struct cui_damage *damage;
    uint32_t id;
} cui_render_list;
/* cui_damage_track for each of them, and what changed in the target as boxes that do not touch
 * (pixels, top-left origin): a list's own changes where it lies, and all of a list that moved,
 * where it was and where it is. Returns how many (0: nothing changed), or -1 for the whole
 * target. A box is then redrawn from every list that reaches into it, in order, each cut to it
 * with cui_damage_clip. */
CUI_API int cui_damage_track_lists(const cui_render_list *lists, int count, cui_rect *boxes, int cap);
/* The next cui_damage_track is a full one: the picture drawn so far was lost. */
CUI_API void cui_damage_reset(cui_damage *d);
CUI_API cui_damage_stats cui_damage_get_stats(const cui_damage *d);

/* ---- Instances as vertices ---- */

/* For a renderer that draws vertices only: draw data whose cui_instance records
 * (cui_ctx_desc.instanced_quads) are written out as four vertices and six indices each, in
 * the same draw order. Keep one zeroed cui_expanded with the renderer and free it with it. */
typedef struct cui_expanded {
    cui_vertex *vertices;
    uint32_t *indices;
    cui_draw_cmd *commands;
    int vertex_cap, index_cap, command_cap;
    cui_draw_data data;
} cui_expanded;

/* dd itself when it has no instances, otherwise &e->data, valid until the next call with e.
 * NULL when memory runs out. */
CUI_API const cui_draw_data *cui_expand_instances(cui_expanded *e, const cui_draw_data *dd);
CUI_API void cui_expanded_free(cui_expanded *e);

/* ---- OpenGL renderer ---- */

typedef void *(*cui_gl_loader)(const char *name);
typedef struct cui_gl_renderer cui_gl_renderer;

/* Create after the GL context is current. `loader` resolves GL functions
 * (wglGetProcAddress, glXGetProcAddress, SDL_GL_GetProcAddress,
 * glfwGetProcAddress, ...). Returns NULL if the context lacks GL 3.3 / ES 3. */
CUI_API cui_gl_renderer *cui_gl_create(cui_gl_loader loader);
CUI_API void cui_gl_destroy(cui_gl_renderer *r);
CUI_API cui_renderer_iface cui_gl_iface(cui_gl_renderer *r);
/* Renders into the currently bound framebuffer. Set clear to false when
 * drawing the UI on top of your own scene. */
CUI_API void cui_gl_render(cui_gl_renderer *r, const cui_draw_data *dd, bool clear);
/* Reads the bound framebuffer into a top-down RGBA buffer. */
CUI_API void cui_gl_read_pixels(cui_gl_renderer *r, int width, int height, void *rgba);
/* cui_gl_render for the part of the window that changed since the last call: the renderer keeps
 * an image of the window, draws only the damaged rectangle into it (the whole of it when d says
 * so) and copies the image to the framebuffer that is bound. False when nothing changed and
 * nothing was drawn or copied, so there is nothing to swap; present_anyway copies the image even
 * then (the window was exposed). d is the caller's, one per window. */
CUI_API bool cui_gl_render_damaged(cui_gl_renderer *r, const cui_draw_data *dd, struct cui_damage *d, bool present_anyway);
/* One surface (cui_get_surface), whole, into the framebuffer that is bound, which is of the
 * surface's size: cleared to nothing at all, then drawn. */
CUI_API void cui_gl_render_surface(cui_gl_renderer *r, const cui_draw_data *dd);
/* A list into part of the framebuffer that is bound, which is target_height pixels high: its top
 * left at x, y pixels from the framebuffer's top left. Nothing is cleared. For a surface's list
 * drawn into the window's own picture, after cui_gl_render of the window's list. */
CUI_API void cui_gl_render_at(cui_gl_renderer *r, const cui_draw_data *dd, int x, int y, int target_height);
/* cui_gl_render_damaged for a frame of several lists that share the window's picture: the kept
 * image is redrawn in the boxes cui_damage_track_lists finds, each from the lists that reach
 * into it, and copied to the framebuffer that is bound. With one list it is cui_gl_render_damaged. */
CUI_API bool cui_gl_render_lists(cui_gl_renderer *r, const cui_render_list *lists, int count, bool present_anyway);
/* True when the OpenGL driver draws on the CPU (Mesa llvmpipe, SwiftShader and the
 * like, as in virtual machines and remote sessions). CUI's own software renderer,
 * which redraws only what changed, is then much cheaper. */
CUI_API bool cui_gl_is_software(const cui_gl_renderer *r);

/* ---- Direct3D 12, Vulkan and Metal (built with CUI_D3D12, CUI_VULKAN, CUI_METAL) ---- */

/* The platform's GPU renderer: the draw data the OpenGL renderer takes (vertices, indices,
 * commands), drawn the same way with the same shader, into the window's own swapchain or
 * layer. One per platform: Direct3D 12 on Windows, Vulkan on Linux (X11, Wayland), Metal on
 * macOS. */
typedef struct cui_gpu_renderer cui_gpu_renderer;

/* Linux: the window cui_gpu_create takes. The app keeps width and height
 * (pixels) current and keeps it alive as long as the renderer. For a Wayland
 * window, wl_surface is set and display is the wl_display *: the app sets the
 * surface's scale or viewport, the renderer only attaches pictures of that size. */
typedef struct cui_x11_window {
    void *display;              /* Display *, or wl_display * */
    unsigned long window;       /* Window (X11) */
    int width, height;
    void *wl_surface;           /* wl_surface * (Wayland); NULL for an X11 window */
    bool translucent;           /* the window's picture has an alpha: where a frame draws nothing, what lies below the surface shows */
    bool host;                  /* Wayland, a program's own device: wl_surface is a surface of the program's that the renderer hangs
                                 * a subsurface for each surface under and commits when it places them; no picture of the window's
                                 * size is made, as with a cui_metal_host and a cui_d3d12_host */
    float scale;                /* with host: pixels a point, for placing the subsurfaces; 0 is 1 */
} cui_x11_window;

/* macOS, Metal, a program's own device (cui_gpu_create_shared): where the renderer's layers go in
 * place of a window. Every surface is a CALayer the renderer adds under `layer`, a layer of the
 * program's (a view's, as a rule), with zPosition 1 and up, each keeping its distance from the top when that layer is resized; nothing the size of the window is
 * made. The program keeps scale (pixels a point) current and the struct alive as long as the
 * renderer. top_left says the layer's coordinates start at its top left corner (a flipped view's);
 * otherwise at its bottom left. */
typedef struct cui_metal_host {
    void *layer;                /* CALayer * */
    float scale;
    bool top_left;
} cui_metal_host;

/* Windows, Direct3D 12, a program's own device (cui_gpu_create_shared): where the renderer's
 * visuals go in place of a window. Every surface is a DirectComposition visual the renderer adds
 * under `visual`, a visual of the program's own tree, each with a composition swap chain of the
 * surface's size on the program's queue; nothing the size of the window is made. The renderer
 * calls Commit on `composition` when it places, shows or hides one. The program keeps both alive
 * as long as the renderer. */
typedef struct cui_d3d12_host {
    void *composition;          /* IDCompositionDevice * */
    void *visual;               /* IDCompositionVisual * */
} cui_d3d12_host;

/* window: an HWND, on Linux a cui_x11_window * (X11 or Wayland), on macOS an NSWindow * (its
 * content view gets a CAMetalLayer). NULL when the device or window cannot be set up. CUI_GPU
 * set to part of an adapter's name (e.g. "nvidia") picks the GPU. */
CUI_API cui_gpu_renderer *cui_gpu_create(void *window, bool vsync);
/* A device the app made, for a renderer that draws with it instead of making its own: the app's
 * own pictures are then textures the renderer can draw (cui_gpu_texture_wrap), with no copy
 * between two devices. Vulkan: the handles as their Vulkan types. The instance has
 * VK_KHR_surface and the window system's surface extension; the device is Vulkan 1.3 with
 * dynamicRendering and timelineSemaphore, has VK_KHR_swapchain, and its queue (of queue_family)
 * draws and presents to the window. The renderer submits to and presents on that queue from the
 * thread that calls it, which is then the only thread to use the queue. The app destroys the
 * device and the instance, after cui_gpu_destroy.
 * Metal: device is the id<MTLDevice> and queue an id<MTLCommandQueue> of it; the other fields are
 * not read. The renderer commits to that queue from the thread that calls it.
 * Direct3D 12: device is the ID3D12Device and queue an ID3D12CommandQueue of it (a direct one);
 * the other fields are not read. The renderer executes its lists on that queue from the thread
 * that calls it, and signals a fence of its own there. */
typedef struct cui_gpu_device {
    void *instance;             /* VkInstance */
    void *physical_device;      /* VkPhysicalDevice */
    void *device;               /* VkDevice, id<MTLDevice>, or ID3D12Device * */
    void *queue;                /* VkQueue, id<MTLCommandQueue>, or ID3D12CommandQueue * */
    uint32_t queue_family;
} cui_gpu_device;
/* cui_gpu_create on the app's device. NULL when the window cannot be drawn to with it. With Metal
 * window is a cui_metal_host *, and there is no picture of the window's size at all: surfaces are
 * layers under the host's, and cui_gpu_render and cui_gpu_render_damaged draw the window's own
 * list into one more layer, below the surfaces. cui_gpu_render_capture is false there.
 * With Direct3D 12 window is a cui_d3d12_host * and the same holds, with visuals for layers: the
 * window's own list goes into one more visual, with a swap chain of its own, below the surfaces. */
CUI_API cui_gpu_renderer *cui_gpu_create_shared(void *window, bool vsync, const cui_gpu_device *device);
/* A picture of the app's own as a texture for cui_draw_image and cui_image: native is a
 * VkImageView of a color image on the renderer's device (cui_gpu_create_shared), or with Metal an
 * id<MTLTexture> of it, or with Direct3D 12 an ID3D12Resource of it (a 2D texture left in a state
 * the pixel shader can read), which the renderer retains until it is unwrapped. The app keeps
 * the image in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL whenever a frame that draws it is
 * rendered, and alive until the texture was given to cui_gpu_texture_unwrap and the frames
 * already rendered have been shown (cui_gpu_flush waits for them). 0 when there is no room. */
CUI_API cui_texture cui_gpu_texture_wrap(cui_gpu_renderer *r, void *native);
CUI_API void cui_gpu_texture_unwrap(cui_gpu_renderer *r, cui_texture texture);
/* Waits until all the renderer has submitted is done. */
CUI_API void cui_gpu_flush(cui_gpu_renderer *r);
CUI_API void cui_gpu_destroy(cui_gpu_renderer *r);
CUI_API cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r);
/* Draws and presents. */
CUI_API void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd);
/* cui_gl_render_damaged's counterpart: the frame is kept as an image and only what d found changed
 * is drawn into it. False when nothing changed and nothing was shown. Each frame shown is shown
 * whole: the system is not told what changed. */
CUI_API bool cui_gpu_render_damaged(cui_gpu_renderer *r, const cui_draw_data *dd, struct cui_damage *d, bool present_anyway);
/* Draws, reads the frame back into rgba (width_px * height_px * 4, top-down) and presents. */
CUI_API bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba);
/* Surfaces (cui_get_surface) as pictures of the system's own, which it lays over the window: a
 * CALayer each on macOS, whose contents is an IOSurface the renderer drew (no drawables: a picture
 * shown and one to draw into, swapped in the transaction that places the layer), a composition visual with a swap chain each on Windows, a Wayland
 * subsurface with a swapchain each on Linux. The window's
 * list is then drawn alone (cui_gpu_render, cui_gpu_render_damaged), and each surface's into its
 * own picture, of the surface's size. cui_gpu_surfaces is false where the renderer has none to
 * give: the lists then go into the window's picture. cui_gpu_surface_draw draws a list into the
 * picture of `slot` and shows it with its top left at x, y pixels of the window, over the surfaces
 * of lower order; false when it could not. d holds the last cui_damage_track of dd: only its boxes,
 * and what the picture drawn into missed, are drawn again. NULL draws all of it. cui_gpu_surface_place puts one that was drawn
 * somewhere else, or elsewhere in the order, without drawing it. cui_gpu_surface_hide takes one
 * that holds nothing this frame off the screen. */
CUI_API bool cui_gpu_surfaces(cui_gpu_renderer *r);
CUI_API bool cui_gpu_surface_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, const cui_damage *d, int x, int y, int order);
CUI_API void cui_gpu_surface_place(cui_gpu_renderer *r, int slot, int x, int y, int order);
CUI_API void cui_gpu_surface_hide(cui_gpu_renderer *r, int slot);
/* Linux, Wayland: the wl_surface * a slot's surface is shown on, a subsurface of the window's
 * that the app made and places; there, cui_gpu_surface_place does nothing, cui_gpu_surface_hide
 * only lets go of the slot's buffers, and the app moves and hides its subsurface itself. Its
 * buffers can be larger than the surface: the app's wp_viewport source crops them to the surface's
 * size in buffer pixels. Given before cui_gpu_surface_draw. */
CUI_API void cui_gpu_surface_window(cui_gpu_renderer *r, int slot, void *wl_surface);
/* Vulkan on Wayland with cui_x11_window.host: the same calls as Direct3D 12 and Metal below. Each
 * surface is a subsurface the renderer makes under the program's surface, with a swapchain of the
 * surface's own size and no input region; the window's own list (cui_gpu_render,
 * cui_gpu_render_damaged) goes to one more, below them. cui_gpu_surface_draw redraws the
 * tracker's boxes and those the swapchain image it draws into missed (the swapchain's images are
 * the only pictures: nothing is kept beside them and nothing copied). cui_gpu_surfaces_end
 * commits the program's surface once after the frame's subsurfaces were drawn, placed or
 * reordered, so a subsurface's place, size and picture change together.
 * cui_gpu_surface_hide takes a surface off the screen and lets its swapchain go; one is made again
 * when the size changes, and the old goes when its work is done: nothing waits.
 * cui_gpu_left_out is true once after a surface's frame was left out because the compositor held
 * every image. */
CUI_API void cui_gpu_surfaces_end(cui_gpu_renderer *r);
/* Direct3D 12: cui_gpu_surface_draw redraws its boxes and those the back buffer missed; a surface
 * that was not shown or took another size is drawn whole. */
/* Metal: the same three. cui_gpu_surface_draw draws the tracker's boxes, as one rectangle,
 * and what the picture drawn into missed while the other was shown. cui_gpu_surfaces_end commits
 * the frame's one transaction. */
/* True once after a surface's frame was left out (no picture free, or the pool made room for one):
 * draw the frame again. */
CUI_API bool cui_gpu_left_out(cui_gpu_renderer *r);
/* The pool tracker for the element (cui_surface.id) a slot shows; NULL: no pool, keep your own. */
CUI_API cui_damage *cui_gpu_surface_damage(cui_gpu_renderer *r, int slot, uint32_t element);
/* Seconds until cui_gpu_surfaces_end should run again to free a spare picture; below 0: none. */
CUI_API float cui_gpu_next_wakeup(cui_gpu_renderer *r);
/* Vulkan on Wayland: true while the window is dragged to a new size (xdg_toplevel resizing state). */
CUI_API void cui_gpu_live_resize(cui_gpu_renderer *r, bool on);
/* A window's finished frame, shown: its own list with cui_gpu_render_damaged, then each surface the
 * frame declared, drawn where its list changed, moved where only its place did and hidden once it
 * is gone, and cui_gpu_surfaces_end. A frame the system left out is asked for again, whole.
 * desc may be NULL. window is the program's tracker of the window's own list, NULL for the
 * presenter's own. surface, where the program makes the system's surface of each slot itself (a
 * Wayland window of its own), gives it for a slot about to be drawn or moved; hidden tells that a
 * slot's surface was hidden. cui_presenter_show returns what it showed (CUI_PRESENTED_*), and
 * CUI_PRESENTED_LOST when a surface could not be given a picture of its own. cui_presenter_reset
 * hides every surface and forgets what was drawn, so the next frame is drawn whole. */
typedef struct cui_presenter cui_presenter;
typedef struct cui_presenter_desc {
    cui_damage *window;
    void *user;
    void *(*surface)(void *user, int slot, int x, int y, int w, int h, int order);
    void (*hidden)(void *user, int slot);
} cui_presenter_desc;
enum { CUI_PRESENTED_WINDOW = 1, CUI_PRESENTED_SURFACES = 2, CUI_PRESENTED_LOST = 4 };
CUI_API cui_presenter *cui_presenter_create(const cui_presenter_desc *desc);
CUI_API void cui_presenter_destroy(cui_presenter *p);
CUI_API int cui_presenter_show(cui_presenter *p, cui_gpu_renderer *r, cui_ctx *ui, bool force);
CUI_API void cui_presenter_reset(cui_presenter *p, cui_gpu_renderer *r);
/* A window's frames, paced by its presenter; now is the program's clock in seconds.
 * cui_presenter_due is true when a frame of w by h points at scale should be made: input or an
 * animation asks for one, a wakeup the frame before asked for has come, the size or the scale
 * changed, or cui_presenter_invalidate was called. cui_presenter_begin begins that frame
 * (cui_set_viewport, cui_begin_frame) and is false when none is due. cui_presenter_end ends it and
 * shows it with cui_presenter_show, whole after a change of size or scale and after an invalidate
 * with `whole`; false when nothing was shown. */
CUI_API bool cui_presenter_due(const cui_presenter *p, cui_ctx *ui, float w, float h, float scale, double now);
CUI_API bool cui_presenter_begin(cui_presenter *p, cui_ctx *ui, float w, float h, float scale, double now);
CUI_API bool cui_presenter_end(cui_presenter *p, cui_gpu_renderer *r, cui_ctx *ui, double now);
CUI_API void cui_presenter_invalidate(cui_presenter *p, bool whole);

#ifdef __cplusplus
}
#endif

#endif /* CUI_RENDER_H */
