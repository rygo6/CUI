/* Direct3D 12 renderer (cui_gpu_*, built with CUI_D3D12): the draw data
 * the OpenGL renderer takes (cui_draw_data's vertices, indices and commands), drawn the same
 * way. One pipeline; its pixel shader is cui_render_gl.c's fragment shader in HLSL, branching
 * on the vertex's mode. Keep the two in step.
 *
 * - A frame's vertices and indices are copied into an upload buffer (one per command
 *   allocator, reused once that allocator's submission is done), then a draw per command with
 *   its clip rectangle as the scissor and its texture's descriptor.
 * - Drawn straight into a UNORM back buffer of a flip-model swapchain, premultiplied source
 *   over, as the OpenGL renderer blends.
 * - Paced by a waitable swapchain (3 buffers, at most 2 frames queued): after presenting, the
 *   frame waits until a buffer is free.
 * - On a program's own device (cui_gpu_create_shared) there is no swapchain of the window's size
 *   and no window: the surfaces' visuals go under a visual the program names, and the window's own
 *   list into a swap chain fitted to what it draws, none at all when it draws nothing. Nothing on
 *   the program's queue is waited for: a surface of another size gets another swap chain and the
 *   old one is let go of once its work has run, and a command allocator still in use is left
 *   alone for another. */
#include "../cui_internal.h"
#include "cui/cui_render.h"
#include "cui_pool.h"

#if defined(CUI_D3D12) && defined(_WIN32)

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUI__T12_FRAMES 2           /* command allocators (and upload buffers) in flight */
#define CUI__T12_FRAMES_MAX 16      /* on a program's own queue: more are made, up to this, sooner than its work is waited for */
#define CUI__T12_BASE CUI_SURFACES  /* a program's own device: the zone of the window's own list */
#define CUI__T12_BUFFERS 3          /* back buffers */
#define CUI__T12_TEX_CAP 4096       /* textures alive at once */
#define CUI__T12_ZONE_BUFFERS 2     /* back buffers of a surface's own swap chain */

static const char cui__d3d12_hlsl[] =
"cbuffer Globals : register(b0) { float2 u_viewport; };\n"
"Texture2D u_tex : register(t0);\n"
"SamplerState u_smp : register(s0);\n"
"struct VI {\n"
"  float2 pos : POSITION; float2 uv : TEXCOORD0; float2 local : TEXCOORD1; float2 half_ : TEXCOORD2;\n"
"  float4 radii : TEXCOORD3; float2 params : TEXCOORD4; float4 color : COLOR0; float4 bcolor : COLOR1;\n"
"};\n"
"struct VO {\n"
"  float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 local : TEXCOORD1;\n"
"  nointerpolation float2 half_ : TEXCOORD2; nointerpolation float4 radii : TEXCOORD3;\n"
"  nointerpolation float2 params : TEXCOORD4; float4 color : COLOR0; nointerpolation float4 bcolor : COLOR1;\n"
"};\n"
"VO vs_main(VI i) {\n"
"  VO o;\n"
"  o.uv = i.uv; o.local = i.local; o.half_ = i.half_; o.radii = i.radii;\n"
"  o.params = i.params; o.color = i.color; o.bcolor = i.bcolor;\n"
"  o.pos = float4(i.pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / u_viewport.y * 2.0, 0.0, 1.0);\n"
"  return o;\n"
"}\n"
"float sd_rrect(float2 p, float2 b, float4 r) {\n"
"  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
"  rr = min(rr, min(b.x, b.y));\n"
"  float2 q = abs(p) - b + rr;\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;\n"
"}\n"
"float erf_(float x) {\n"
"  float s = sign(x); x = abs(x);\n"
"  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
"  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);\n"
"  return s * y;\n"
"}\n"
"float bayer8(float2 p) {\n"
"  float2 f = floor(p);\n"
"  int2 q = int2(f - 8.0 * floor(f / 8.0));\n"
"  int a = q.x ^ q.y, y = q.y;\n"
"  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);\n"
"  return (float(m) + 0.5) / 64.0;\n"
"}\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  int mode = int(i.params.y + 0.5);\n"
"  float4 c = i.color;\n"
"  if (mode == 4) return float4(c.rgb * c.a, c.a);\n"
"  float4 t = u_tex.SampleLevel(u_smp, i.uv, 0);\n"   /* (textures have one level) */
"  if (mode == 0) { float a = c.a * t.r; return float4(c.rgb * a, a); }\n"
"  float d = sd_rrect(i.local, i.half_, i.radii);\n"
"  if (mode == 1) {\n"
"    float outer = clamp(0.5 - d, 0.0, 1.0);\n"
"    float bw = i.params.x;\n"
"    float inner = bw > 0.0 ? clamp(0.5 - (d + bw), 0.0, 1.0) : outer;\n"
"    float fa = c.a * inner, ba = i.bcolor.a * (outer - inner);\n"
"    return float4(c.rgb * fa + i.bcolor.rgb * ba, fa + ba);\n"
"  } else if (mode == 2) {\n"
"    float s = max(i.params.x, 0.01);\n"
"    float a = c.a * 0.5 * (1.0 - erf_(d / (s * 1.41421356)));\n"
"    return float4(c.rgb * a, a);\n"
"  } else if (mode == 5) {\n"
"    float th = bayer8(i.uv);\n"
"    float k = th >= i.params.x && th < c.a ? i.bcolor.a * clamp(0.5 - d, 0.0, 1.0) : 0.0;\n"
"    return float4(i.bcolor.rgb * k, k);\n"
"  }\n"
"  float cov = clamp(0.5 - d, 0.0, 1.0);\n"
"  float k = c.a * cov;\n"
"  return float4(t.rgb * t.a * c.rgb * k, t.a * k);\n"
"}\n";

typedef struct cui__d3d12_buffer {      /* a mapped buffer */
    ID3D12Resource *res;
    uint8_t *ptr;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    size_t cap, used;
} cui__d3d12_buffer;

typedef struct cui__d3d12_tex { ID3D12Resource *res; int bpp; DXGI_FORMAT fmt; } cui__d3d12_tex;
typedef struct cui__d3d12_later { IUnknown *obj; uint64_t fence; int slot; } cui__d3d12_later;

/* DirectComposition, as far as it is used: the system lays each surface's swap chain over the window.
 * Its header declares overloaded methods and so cannot be read as C: the objects are called through
 * their tables by place, in the order the header gives. */
typedef struct cui__dcomp { void **vtbl; } cui__dcomp;
HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi, REFIID iid, void **device);
static const GUID cui__IID_IDCompositionDevice = { 0xc37ea93a, 0xe7aa, 0x450d, { 0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3 } };
#define CUI__DC(obj, at, type) ((type)(obj)->vtbl[at])
static void cui__dc_retain(cui__dcomp *p) { CUI__DC(p, 1, ULONG (STDMETHODCALLTYPE *)(void *))(p); }
static void cui__dc_release(cui__dcomp **p) { if (*p) { CUI__DC(*p, 2, ULONG (STDMETHODCALLTYPE *)(void *))(*p); *p = NULL; } }
static HRESULT cui__dc_commit(cui__dcomp *dev) { return CUI__DC(dev, 3, HRESULT (STDMETHODCALLTYPE *)(void *))(dev); }
static HRESULT cui__dc_target(cui__dcomp *dev, HWND hwnd, cui__dcomp **out) { return CUI__DC(dev, 6, HRESULT (STDMETHODCALLTYPE *)(void *, HWND, BOOL, void **))(dev, hwnd, TRUE, (void **)out); }
static HRESULT cui__dc_visual(cui__dcomp *dev, cui__dcomp **out) { return CUI__DC(dev, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **))(dev, (void **)out); }
static HRESULT cui__dc_set_root(cui__dcomp *target, cui__dcomp *visual) { return CUI__DC(target, 3, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(target, visual); }
static HRESULT cui__dc_offset(cui__dcomp *visual, float x, float y) {
    HRESULT a = CUI__DC(visual, 4, HRESULT (STDMETHODCALLTYPE *)(void *, float))(visual, x);
    HRESULT b = CUI__DC(visual, 6, HRESULT (STDMETHODCALLTYPE *)(void *, float))(visual, y);
    return FAILED(a) ? a : b;
}
static HRESULT cui__dc_clip(cui__dcomp *visual, float w, float h) {
    float rect[4] = { 0, 0, w, h };
    return CUI__DC(visual, 14, HRESULT (STDMETHODCALLTYPE *)(void *, const float *))(visual, rect);
}
static HRESULT cui__dc_content(cui__dcomp *visual, IUnknown *content) { return CUI__DC(visual, 15, HRESULT (STDMETHODCALLTYPE *)(void *, IUnknown *))(visual, content); }
static HRESULT cui__dc_add(cui__dcomp *parent, cui__dcomp *child, BOOL above, cui__dcomp *of) { return CUI__DC(parent, 16, HRESULT (STDMETHODCALLTYPE *)(void *, void *, BOOL, void *))(parent, child, above, of); }
static HRESULT cui__dc_remove(cui__dcomp *parent, cui__dcomp *child) { return CUI__DC(parent, 17, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(parent, child); }

/* A pool picture: a composition swap chain of a bucket's size, and the size last drawn into it. */
typedef struct cui__d3d12_pic {
    IDXGISwapChain3 *swapchain;
    ID3D12Resource *buffers[CUI__T12_ZONE_BUFFERS];
    int cw, ch;
    int drawn;        /* frames shown since it was made or drawn for another size */
    int size_w, size_h;
    cui_rect aged[CUI_DAMAGE_BOXES];   /* what the frame before this one changed: the other back buffer lacks it */
    int aged_count;   /* -1: all of it */
} cui__d3d12_pic;

/* A surface of the UI, shown by a visual over the window whose content is a pool swap chain. */
typedef struct cui__d3d12_zone {
    cui__dcomp *visual;
    int x, y, w, h, order;
    int at;           /* the pool slot it shows, -1: none */
    uint16_t key;     /* the pool bucket of it */
    uint32_t element; /* whose swap chain the pool gives it; 0: the slot's own */
    bool shown;       /* its visual is among the window's */
    bool unshown;     /* its swap chain is not its visual's content yet: that waits for its first picture */
    int reclip;       /* pictures to show before its visual is cut to the surface's new size: one, or two when it shrank */
} cui__d3d12_zone;

struct cui_gpu_renderer {
    cui_expanded expanded;          /* instances written out as vertices: this renderer draws vertices only */
    HWND hwnd;
    bool vsync, tearing;
    IDXGIFactory4 *factory;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    bool shared;                      /* the device, the queue and the composition tree are the program's (cui_gpu_create_shared) */
    ID3D12CommandAllocator *alloc[CUI__T12_FRAMES_MAX];
    uint64_t alloc_fence[CUI__T12_FRAMES_MAX];
    cui__d3d12_buffer upload[CUI__T12_FRAMES_MAX];
    int frames;                       /* how many of them there are */
    ID3D12GraphicsCommandList *list;
    bool open;
    int frame;                        /* the allocator being recorded */
    ID3D12Fence *fence;
    uint64_t fence_value;             /* the last value the queue was told to signal: everything waited for is at or below it */
    HANDLE event;
    ID3D12DescriptorHeap *srv_heap, *rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, rtv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    UINT srv_inc, rtv_inc;
    ID3D12RootSignature *root;
    ID3D12PipelineState *pso;
    cui__d3d12_tex *tex;                /* by cui_texture - 1; the descriptor at the same index */
    int tex_count, tex_cap;
    int none;                         /* a black texel, for a command whose texture is gone */
    cui__d3d12_later *later; int later_count, later_cap;   /* released once their fence passes */
    IDXGISwapChain3 *swapchain;
    UINT flags;
    HANDLE waitable;
    ID3D12Resource *buffers[CUI__T12_BUFFERS];
    int w, h;
    /* damage tracking (cui_gpu_render_damaged) */
    ID3D12Resource *kept;             /* the window's picture, redrawn only where a frame changed */
    int kept_w, kept_h;
    int shown;                        /* frames presented since the back buffers were made: each holds a frame once it reaches their number */
    bool kept_stale;                  /* frames went straight into the window (cui_damage_direct): the kept image lacks them */
    cui_rect aged[CUI__T12_BUFFERS - 1][CUI_DAMAGE_BOXES];   /* what the frames before this one changed, newest first */
    int aged_count[CUI__T12_BUFFERS - 1];                    /* -1: all of it */
    bool lost;                        /* the device was removed (a driver update, a GPU reset): nothing more is drawn */
    /* surfaces (cui_gpu_surface_draw) */
    cui__dcomp *dcomp, *dcomp_target, *dcomp_root;
    bool dcomp_failed;                /* the system would not give them: the lists go into the window's picture */
    cui__d3d12_zone zone[CUI_SURFACES + 1];   /* (the last: the window's own list on a program's device) */
    cui_pool pool;                    /* the surfaces' swap chains, by slot */
    cui__d3d12_pic pic[CUI_POOL_SLOTS];
    double reap_due;                  /* seconds until a spare swap chain is due to go, <0: none */
    bool live_resize, left_out;
};

#define CUI__REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

/* ---- submissions ---- */

/* The device is gone (a driver update, a GPU reset, the adapter taken away) or a call it cannot
 * go on without failed. Nothing recreates it: the window keeps its last picture, and the calls
 * that would use the device are not made. */
static void cui__d3d12_lose(cui_gpu_renderer *r) {
    if (!r->lost) fprintf(stderr, "cui: Direct3D 12 stopped (the device was lost, or it ran out of memory); the window is not drawn any more\n");
    r->lost = true;
}

static void cui__d3d12_wait(cui_gpu_renderer *r, uint64_t value) {
    if (value == 0 || r->fence->lpVtbl->GetCompletedValue(r->fence) >= value) return;
    if (SUCCEEDED(r->fence->lpVtbl->SetEventOnCompletion(r->fence, value, r->event)) && WaitForSingleObject(r->event, INFINITE) == WAIT_OBJECT_0) return;
    /* the event was not set up: look at the fence itself (a removed device completes every value) */
    while (r->fence->lpVtbl->GetCompletedValue(r->fence) < value) Sleep(1);
}

/* the objects whose fence has passed (all of them when idle); their texture slots are free again */
static void cui__d3d12_collect(cui_gpu_renderer *r, bool all) {
    uint64_t done = r->fence->lpVtbl->GetCompletedValue(r->fence);
    int n = 0;
    for (int i = 0; i < r->later_count; i++) {
        cui__d3d12_later *l = &r->later[i];
        if (all || l->fence <= done) {
            if (l->obj) l->obj->lpVtbl->Release(l->obj);
            if (l->slot >= 0) r->tex[l->slot].bpp = 0;
        } else {
            r->later[n++] = *l;
        }
    }
    r->later_count = n;
}

/* releases obj (and frees the texture slot) after what is recorded so far has run */
static void cui__d3d12_release_later(cui_gpu_renderer *r, IUnknown *obj, int slot) {
    if (r->later_count == r->later_cap) {
        int cap = r->later_cap ? r->later_cap * 2 : 64;
        cui__d3d12_later *grown = realloc(r->later, (size_t)cap * sizeof *grown);
        if (!grown) return;   /* out of memory: commands recorded and not yet run may use it, so it is left alive */
        r->later = grown;
        r->later_cap = cap;
    }
    cui__d3d12_later l = { obj, r->fence_value + 1, slot };
    r->later[r->later_count++] = l;
}

/* The open command list, opened on the next allocator once its last submission is done. NULL
 * when the renderer has stopped or the list will not open: the caller records nothing. */
static ID3D12GraphicsCommandList *cui__d3d12_cmd(cui_gpu_renderer *r) {
    if (r->lost) return NULL;
    if (!r->open) {
        int f = r->frame;
        if (r->shared) {   /* the program's queue: its work is not waited for while there is another allocator */
            uint64_t done = r->fence->lpVtbl->GetCompletedValue(r->fence);
            int free_one = -1;
            for (int i = 0; i < r->frames && free_one < 0; i++) if (r->alloc_fence[(f + i) % r->frames] <= done) free_one = (f + i) % r->frames;
            if (free_one < 0 && r->frames < CUI__T12_FRAMES_MAX &&
                SUCCEEDED(r->device->lpVtbl->CreateCommandAllocator(r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&r->alloc[r->frames])))
                free_one = r->frames++;
            if (free_one >= 0) f = r->frame = free_one;
        }
        cui__d3d12_wait(r, r->alloc_fence[f]);
        if (FAILED(r->alloc[f]->lpVtbl->Reset(r->alloc[f])) || FAILED(r->list->lpVtbl->Reset(r->list, r->alloc[f], NULL))) {
            cui__d3d12_lose(r);
            return NULL;
        }
        r->upload[f].used = 0;
        r->open = true;
    }
    return r->list;
}

static void cui__d3d12_submit(cui_gpu_renderer *r) {
    if (!r->open) return;
    ID3D12GraphicsCommandList *list = r->list;
    if (r->lost || FAILED(list->lpVtbl->Close(list))) {   /* nothing of it runs, and nothing waits for it */
        cui__d3d12_lose(r);
        r->open = false;
        return;
    }
    r->queue->lpVtbl->ExecuteCommandLists(r->queue, 1, (ID3D12CommandList **)&list);
    r->open = false;
    if (FAILED(r->queue->lpVtbl->Signal(r->queue, r->fence, r->fence_value + 1))) {   /* a value the fence would never reach is not waited for */
        cui__d3d12_lose(r);
        return;
    }
    r->alloc_fence[r->frame] = ++r->fence_value;
    r->frame = (r->frame + 1) % r->frames;
    r->open = false;
    cui__d3d12_collect(r, false);
}

static void cui__d3d12_idle(cui_gpu_renderer *r) {
    cui__d3d12_submit(r);
    cui__d3d12_wait(r, r->fence_value);
}

/* Texture work is recorded into the open list and runs with the next frame. A window that
 * shows no frames (minimized) would let it pile up without end: past this much, it is run now.
 * Only between texture calls, never while a frame is recorded. */
static void cui__d3d12_flush_backlog(cui_gpu_renderer *r) {
    bool uploads = r->open && r->upload[r->frame].used > ((size_t)16 << 20);
    if (r->lost || (!uploads && r->later_count < 256)) return;
    cui__d3d12_cmd(r);
    cui__d3d12_submit(r);
}

static void cui__d3d12_barrier(cui_gpu_renderer *r, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList *list = cui__d3d12_cmd(r);
    if (list) list->lpVtbl->ResourceBarrier(list, 1, &b);
}

/* a mapped buffer of `cap` bytes in a heap of that type */
static bool cui__d3d12_buffer_make(cui_gpu_renderer *r, cui__d3d12_buffer *b, size_t cap, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap = { type };
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cap;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    cui__d3d12_buffer made = { 0 };
    if (FAILED(r->device->lpVtbl->CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL, &IID_ID3D12Resource, (void **)&made.res)))
        return false;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(made.res->lpVtbl->Map(made.res, 0, type == D3D12_HEAP_TYPE_READBACK ? NULL : &none, (void **)&made.ptr))) { CUI__REL(made.res); return false; }
    made.va = made.res->lpVtbl->GetGPUVirtualAddress(made.res);
    made.cap = cap;
    *b = made;
    return true;
}

/* room in the open list's upload buffer, valid until its submission is done */
static uint8_t *cui__d3d12_upload(cui_gpu_renderer *r, size_t bytes, size_t align, D3D12_GPU_VIRTUAL_ADDRESS *va, ID3D12Resource **res, size_t *offset) {
    if (!cui__d3d12_cmd(r)) return NULL;
    cui__d3d12_buffer *u = &r->upload[r->frame];
    size_t at = (u->used + align - 1) & ~(align - 1);
    if (!u->res || at + bytes > u->cap) {
        size_t cap = u->cap ? u->cap * 2 : (size_t)1 << 16;
        while (cap < bytes) cap *= 2;
        cui__d3d12_buffer grown;
        if (!cui__d3d12_buffer_make(r, &grown, cap, D3D12_HEAP_TYPE_UPLOAD)) return NULL;
        if (u->res) cui__d3d12_release_later(r, (IUnknown *)u->res, -1);   /* what the list already uses from it stays */
        *u = grown;
        at = 0;
    }
    u->used = at + bytes;
    if (va) *va = u->va + at;
    if (res) *res = u->res;
    if (offset) *offset = at;
    return u->ptr + at;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cui__d3d12_rtv_at(cui_gpu_renderer *r, int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = { r->rtv_cpu.ptr + (SIZE_T)i * r->rtv_inc }; return h; }

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static bool cui__d3d12_tex_upload(cui_gpu_renderer *r, cui__d3d12_tex *t, int x, int y, int w, int h, const void *pixels, int stride, bool fresh) {
    UINT pitch = (UINT)((w * t->bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
    ID3D12Resource *src = NULL;
    size_t offset = 0;
    uint8_t *p = cui__d3d12_upload(r, (size_t)pitch * (size_t)h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, NULL, &src, &offset);
    if (!p) return false;   /* no memory for the pixels */
    size_t row = (size_t)w * (size_t)t->bpp, from = stride ? (size_t)stride : row;
    for (int k = 0; k < h; k++) memcpy(p + (size_t)k * pitch, (const uint8_t *)pixels + (size_t)k * from, row);
    if (!fresh) cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = { 0 }, from_loc = { 0 };
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from_loc.pResource = src;
    from_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from_loc.PlacedFootprint.Offset = offset;
    from_loc.PlacedFootprint.Footprint.Format = t->fmt;
    from_loc.PlacedFootprint.Footprint.Width = (UINT)w;
    from_loc.PlacedFootprint.Footprint.Height = (UINT)h;
    from_loc.PlacedFootprint.Footprint.Depth = 1;
    from_loc.PlacedFootprint.Footprint.RowPitch = pitch;
    r->list->lpVtbl->CopyTextureRegion(r->list, &dst, (UINT)x, (UINT)y, 0, &from_loc, NULL);
    cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

static cui_texture cui__d3d12_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_gpu_renderer *r = user;
    if (w <= 0 || h <= 0 || r->lost) return 0;
    cui__d3d12_flush_backlog(r);
    int slot = -1;
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].bpp) { slot = i; break; }
    if (slot < 0) {
        if (r->tex_count == CUI__T12_TEX_CAP) return 0;
        if (r->tex_count == r->tex_cap) {
            int cap = r->tex_cap ? r->tex_cap * 2 : 16;
            void *n = realloc(r->tex, sizeof(*r->tex) * (size_t)cap);
            if (!n) return 0;
            r->tex = n;
            r->tex_cap = cap;
        }
        slot = r->tex_count++;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    }
    cui__d3d12_tex *t = &r->tex[slot];
    t->fmt = fmt == CUI_TEXTURE_R8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)w; desc.Height = (UINT)h;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = t->fmt;
    desc.SampleDesc.Count = 1;
    if (FAILED(r->device->lpVtbl->CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                          &IID_ID3D12Resource, (void **)&t->res))) { t->res = NULL; return 0; }
    t->bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
    D3D12_CPU_DESCRIPTOR_HANDLE at = { r->srv_cpu.ptr + (SIZE_T)slot * r->srv_inc };
    r->device->lpVtbl->CreateShaderResourceView(r->device, t->res, NULL, at);
    if (pixels && !cui__d3d12_tex_upload(r, t, 0, 0, w, h, pixels, 0, true)) {
        /* no texture without its pixels: the caller is told, and can make it again (no command uses it yet) */
        CUI__REL(t->res);
        t->bpp = 0;
        return 0;
    }
    if (!pixels && !cui__d3d12_cmd(r)) {   /* (stopped: no texture) */
        CUI__REL(t->res);
        t->bpp = 0;
        return 0;
    }
    if (!pixels) cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return (cui_texture)(slot + 1);
}

static void cui__d3d12_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res || w <= 0 || h <= 0 || !pixels || r->lost) return;
    cui__d3d12_flush_backlog(r);
    cui__d3d12_tex_upload(r, &r->tex[slot], x, y, w, h, pixels, stride, false);
}

static void cui__d3d12_tex_destroy(void *user, cui_texture tex) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res) return;
    cui__d3d12_flush_backlog(r);
    /* draws already recorded may sample it: it goes, and its slot is free, once they have run */
    cui__d3d12_release_later(r, (IUnknown *)r->tex[slot].res, slot);
    r->tex[slot].res = NULL;
}

/* ---- the device ---- */

static ID3DBlob *cui__d3d12_compile(const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *errors = NULL;
    if (FAILED(D3DCompile(cui__d3d12_hlsl, sizeof cui__d3d12_hlsl - 1, "cui_d3d12", NULL, NULL, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)) && errors)
        fprintf(stderr, "cui: %s\n", (const char *)errors->lpVtbl->GetBufferPointer(errors));
    CUI__REL(errors);
    return blob;
}

static bool cui__d3d12_pipeline(cui_gpu_renderer *r) {
    ID3D12Device *dev = r->device;
    /* b0 root constants (the viewport), t0 a table of one texture, s0 static (linear, clamp) */
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 2;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = { 0 };
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob = NULL, *errors = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))) {
        if (errors) fprintf(stderr, "cui: %s\n", (const char *)errors->lpVtbl->GetBufferPointer(errors));
        CUI__REL(errors);
        return false;
    }
    dev->lpVtbl->CreateRootSignature(dev, 0, blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&r->root);
    CUI__REL(blob);
    if (!r->root) return false;

    /* one element per group of cui_vertex fields, as the OpenGL renderer's attributes */
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, x),            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, u),            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, lx),           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, hw),           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(cui_vertex, r_tl),         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, border),       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, (UINT)offsetof(cui_vertex, color),        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,     0, (UINT)offsetof(cui_vertex, border_color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vs = cui__d3d12_compile("vs_main", "vs_5_0"), *ps = cui__d3d12_compile("ps_main", "ps_5_0");
    if (vs && ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
        memset(&d, 0, sizeof d);
        d.pRootSignature = r->root;
        d.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
        d.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        d.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
        d.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        d.SampleMask = 0xffffffff;
        d.InputLayout.pInputElementDescs = elements;
        d.InputLayout.NumElements = (UINT)(sizeof elements / sizeof elements[0]);
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        /* premultiplied source over */
        D3D12_RENDER_TARGET_BLEND_DESC *rt = &d.BlendState.RenderTarget[0];
        rt->BlendEnable = TRUE;
        rt->SrcBlend = D3D12_BLEND_ONE;
        rt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt->BlendOp = D3D12_BLEND_OP_ADD;
        rt->SrcBlendAlpha = D3D12_BLEND_ONE;
        rt->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt->LogicOp = D3D12_LOGIC_OP_NOOP;
        rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        dev->lpVtbl->CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&r->pso);
    }
    CUI__REL(vs);
    CUI__REL(ps);
    return r->pso != NULL;
}

/* whether an adapter's name (UTF-16) holds CUI_GPU's value, ignoring case */
static bool cui__d3d12_wanted(const WCHAR *name, const char *want) {
    char utf8[384];   /* DXGI names are at most 128 UTF-16 units */
    if (!want || !*want || !WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL)) return false;
    return cui__contains_ci(utf8, want);
}

static bool cui__d3d12_setup(cui_gpu_renderer *r);

static bool cui__d3d12_device(cui_gpu_renderer *r) {
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&r->factory))) return false;
    IDXGIFactory5 *f5 = NULL;
    if (SUCCEEDED(r->factory->lpVtbl->QueryInterface(r->factory, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL tear = FALSE;
        if (SUCCEEDED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear))) r->tearing = tear != FALSE;
        CUI__REL(f5);
    }
    /* the adapter CUI_GPU names, else the first hardware one that takes a device */
    IDXGIAdapter1 *a = NULL;
    const char *want = getenv("CUI_GPU");
    for (int pass = want && *want ? 0 : 1; pass < 2 && !r->device; pass++)
        for (UINT i = 0; !r->device && r->factory->lpVtbl->EnumAdapters1(r->factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->lpVtbl->GetDesc1(a, &d);
            bool fits = pass == 0 ? cui__d3d12_wanted(d.Description, want) : !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            if (fits) D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&r->device);
            if (fits && r->device && pass == 0) fprintf(stderr, "cui: GPU %ls\n", d.Description);   /* (as the Vulkan renderer says which it took) */
            CUI__REL(a);
        }
    if (!r->device) return false;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(r->device->lpVtbl->CreateCommandQueue(r->device, &qd, &IID_ID3D12CommandQueue, (void **)&r->queue))) return false;
    return cui__d3d12_setup(r);
}

/* What the renderer makes on its device, its own or the program's. */
static bool cui__d3d12_setup(cui_gpu_renderer *r) {
    ID3D12Device *dev = r->device;
    r->frames = CUI__T12_FRAMES;
    for (int i = 0; i <= CUI_SURFACES; i++) r->zone[i].at = -1;
    r->reap_due = -1;
    for (int i = 0; i < CUI__T12_FRAMES; i++)
        if (FAILED(dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&r->alloc[i]))) return false;
    if (FAILED(dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&r->list))) return false;
    r->list->lpVtbl->Close(r->list);
    if (FAILED(dev->lpVtbl->CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&r->fence))) return false;
    r->event = CreateEventA(NULL, FALSE, FALSE, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CUI__T12_TEX_CAP, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    /* the window's back buffers, the kept image, and each pool swap chain's back buffers */
    D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, CUI__T12_BUFFERS + 1 + CUI_POOL_SLOTS * CUI__T12_ZONE_BUFFERS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&r->srv_heap)) ||
        FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&r->rtv_heap))) return false;
    r->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(r->srv_heap, &r->srv_cpu);
    r->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(r->srv_heap, &r->srv_gpu);
    r->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(r->rtv_heap, &r->rtv_cpu);
    r->srv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r->rtv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!cui__d3d12_pipeline(r)) return false;
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    r->none = (int)cui__d3d12_tex_create(r, 1, 1, CUI_TEXTURE_RGBA8, black) - 1;
    cui__d3d12_idle(r);
    /* a renderer that stopped while starting, or whose device went away, is none: the app draws with OpenGL */
    return r->none >= 0 && !r->lost && SUCCEEDED(dev->lpVtbl->GetDeviceRemovedReason(dev));
}

/* ---- the window ---- */

static bool cui__d3d12_targets(cui_gpu_renderer *r) {
    for (int i = 0; i < CUI__T12_BUFFERS; i++) {
        if (FAILED(r->swapchain->lpVtbl->GetBuffer(r->swapchain, (UINT)i, &IID_ID3D12Resource, (void **)&r->buffers[i]))) { r->buffers[i] = NULL; return false; }
        r->device->lpVtbl->CreateRenderTargetView(r->device, r->buffers[i], NULL, cui__d3d12_rtv_at(r, i));
    }
    D3D12_RESOURCE_DESC d;
    r->buffers[0]->lpVtbl->GetDesc(r->buffers[0], &d);
    r->w = (int)d.Width; r->h = (int)d.Height;
    r->shown = 0;
    return true;
}



static bool cui__d3d12_swapchain(cui_gpu_renderer *r) {
    DXGI_SWAP_CHAIN_DESC1 desc = { 0 };
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = CUI__T12_BUFFERS;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   /* back buffers keep their pictures: cui_gpu_render_damaged copies in only what changed */
    desc.Flags = r->flags;
    IDXGISwapChain1 *sc1 = NULL;
    IDXGIFactory4 *f = r->factory;
    if (FAILED(f->lpVtbl->CreateSwapChainForHwnd(f, (IUnknown *)r->queue, r->hwnd, &desc, NULL, NULL, &sc1)) ||
        FAILED(sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&r->swapchain))) { CUI__REL(sc1); r->swapchain = NULL; return false; }
    CUI__REL(sc1);
    f->lpVtbl->MakeWindowAssociation(f, r->hwnd, DXGI_MWA_NO_ALT_ENTER);
    /* frames queued ahead of the display: 2 (1 misses refreshes at 240 Hz) */
    r->swapchain->lpVtbl->SetMaximumFrameLatency(r->swapchain, 2);
    r->waitable = r->swapchain->lpVtbl->GetFrameLatencyWaitableObject(r->swapchain);
    if (!cui__d3d12_targets(r)) return false;
    WaitForSingleObjectEx(r->waitable, 1000, TRUE);
    return true;
}

static void cui__d3d12_zones_free(cui_gpu_renderer *r) {
    for (int i = 0; i <= CUI_SURFACES; i++) {
        cui__d3d12_zone *z = &r->zone[i];
        if (z->visual && z->shown && r->dcomp_root) cui__dc_remove(r->dcomp_root, z->visual);
        cui__dc_release(&z->visual);
        memset(z, 0, sizeof *z);
        z->at = -1;
    }
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        for (int b = 0; b < CUI__T12_ZONE_BUFFERS; b++) CUI__REL(r->pic[i].buffers[b]);
        CUI__REL(r->pic[i].swapchain);
    }
    if (r->dcomp_target) cui__dc_set_root(r->dcomp_target, NULL);
    if (r->dcomp) cui__dc_commit(r->dcomp);
    cui__dc_release(&r->dcomp_root);
    cui__dc_release(&r->dcomp_target);
    cui__dc_release(&r->dcomp);
}

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    if (r->queue && r->fence) cui__d3d12_idle(r);
    /* Stopped on a device that is still there (before or during that idle): work may be running
     * that no fence value accounts for. What it uses is left alone; only the memory on this side
     * is given back. */
    if (r->lost && r->device && SUCCEEDED(r->device->lpVtbl->GetDeviceRemovedReason(r->device))) {
        free(r->later);
        free(r->tex);
        free(r);
        return;
    }
    if (r->queue && r->fence) cui__d3d12_collect(r, true);
    cui_expanded_free(&r->expanded);
    free(r->later);
    for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
    cui__d3d12_zones_free(r);
    CUI__REL(r->kept);
    if (r->waitable) CloseHandle(r->waitable);
    CUI__REL(r->swapchain);
    for (int i = 0; i < r->tex_count; i++) CUI__REL(r->tex[i].res);
    free(r->tex);
    CUI__REL(r->pso);
    CUI__REL(r->root);
    for (int i = 0; i < CUI__T12_FRAMES_MAX; i++) { CUI__REL(r->alloc[i]); CUI__REL(r->upload[i].res); }
    CUI__REL(r->list);
    CUI__REL(r->srv_heap); CUI__REL(r->rtv_heap);
    CUI__REL(r->fence);
    if (r->event) CloseHandle(r->event);
    CUI__REL(r->queue); CUI__REL(r->device); CUI__REL(r->factory);
    free(r);
}

cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    if (!window) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->hwnd = (HWND)window;
    r->vsync = vsync;
    if (!cui__d3d12_device(r)) { cui_gpu_destroy(r); return NULL; }
    r->flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (r->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (!cui__d3d12_swapchain(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

/* window: a cui_d3d12_host. The device, the queue and the composition tree are the program's. */
cui_gpu_renderer *cui_gpu_create_shared(void *window, bool vsync, const cui_gpu_device *device) {
    const cui_d3d12_host *host = window;
    if (!host || !host->composition || !host->visual || !device || !device->device || !device->queue) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->shared = true;
    r->vsync = vsync;
    r->device = (ID3D12Device *)device->device;
    r->queue = (ID3D12CommandQueue *)device->queue;
    r->device->lpVtbl->AddRef(r->device);
    r->queue->lpVtbl->AddRef(r->queue);
    r->dcomp = (cui__dcomp *)host->composition;
    r->dcomp_root = (cui__dcomp *)host->visual;
    cui__dc_retain(r->dcomp);
    cui__dc_retain(r->dcomp_root);
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&r->factory)) || !cui__d3d12_setup(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

/* native: an ID3D12Resource of the renderer's device, a 2D texture the pixel shader can read. */
cui_texture cui_gpu_texture_wrap(cui_gpu_renderer *r, void *native) {
    if (!r || !native || r->lost) return 0;
    int slot = -1;
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].bpp) { slot = i; break; }
    if (slot < 0) {
        if (r->tex_count == CUI__T12_TEX_CAP) return 0;
        if (r->tex_count == r->tex_cap) {
            int cap = r->tex_cap ? r->tex_cap * 2 : 16;
            void *n = realloc(r->tex, sizeof(*r->tex) * (size_t)cap);
            if (!n) return 0;
            r->tex = n;
            r->tex_cap = cap;
        }
        slot = r->tex_count++;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    }
    cui__d3d12_tex *t = &r->tex[slot];
    D3D12_RESOURCE_DESC d;
    t->res = (ID3D12Resource *)native;
    t->res->lpVtbl->AddRef(t->res);
    t->res->lpVtbl->GetDesc(t->res, &d);
    t->fmt = d.Format;
    t->bpp = 4;
    D3D12_CPU_DESCRIPTOR_HANDLE at = { r->srv_cpu.ptr + (SIZE_T)slot * r->srv_inc };
    r->device->lpVtbl->CreateShaderResourceView(r->device, t->res, NULL, at);
    return (cui_texture)(slot + 1);
}

void cui_gpu_texture_unwrap(cui_gpu_renderer *r, cui_texture texture) { if (r) cui__d3d12_tex_destroy(r, texture); }
void cui_gpu_flush(cui_gpu_renderer *r) { if (r && r->queue && r->fence) { cui__d3d12_idle(r); cui__d3d12_collect(r, false); } }

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__d3d12_tex_create;
    i.texture_update = cui__d3d12_tex_update;
    i.texture_destroy = cui__d3d12_tex_destroy;
    return i;
}

/* ---- frames ---- */

/* What one frame's phases share */
typedef struct cui__d3d12_frame {
    ID3D12GraphicsCommandList *cmd;
    int bb;                                   /* the back buffer drawn into */
    cui__d3d12_buffer readback;               /* the frame copied out, when it was asked for */
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;    /* its layout */
} cui__d3d12_frame;

/* A frame begun: the buffers follow the window's size once the GPU is done with them, and f->bb is the back buffer. */
static bool cui__d3d12_begin(cui_gpu_renderer *r, cui__d3d12_frame *f) {
    RECT rc;
    GetClientRect(r->hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;   /* minimized */
    if (cw != r->w || ch != r->h) {
        cui__d3d12_idle(r);
        for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
        if (FAILED(r->swapchain->lpVtbl->ResizeBuffers(r->swapchain, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, r->flags)) || !cui__d3d12_targets(r)) {
            for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
            r->w = r->h = 0;   /* (no back buffers: asked for again with the next frame, unless the device is gone) */
            if (FAILED(r->device->lpVtbl->GetDeviceRemovedReason(r->device))) cui__d3d12_lose(r);
            return false;
        }
    }
    f->cmd = cui__d3d12_cmd(r);
    if (!f->cmd) return false;
    f->bb = (int)r->swapchain->lpVtbl->GetCurrentBackBufferIndex(r->swapchain);
    return true;
}

/* The back buffer to draw into, cleared. */
static bool cui__d3d12_acquire(cui_gpu_renderer *r, cui__d3d12_frame *f, const cui_draw_data *dd) {
    if (!cui__d3d12_begin(r, f)) return false;
    cui__d3d12_barrier(r, r->buffers[f->bb], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cui__d3d12_rtv_at(r, f->bb);
    float clear[4] = { dd->clear_color.r / 255.0f, dd->clear_color.g / 255.0f, dd->clear_color.b / 255.0f, 1 };
    f->cmd->lpVtbl->OMSetRenderTargets(f->cmd, 1, &rtv, FALSE, NULL);
    f->cmd->lpVtbl->ClearRenderTargetView(f->cmd, rtv, clear, 0, NULL);
    return true;
}

/* The frame's vertices and indices, then a draw per command (cui_gl_render's counterpart). */
/* False when the frame's vertices found no room: only the cleared window is shown. */
static bool cui__d3d12_draw_at(cui_gpu_renderer *r, const cui__d3d12_frame *f, const cui_draw_data *dd, int W, int H,
                               const D3D12_RECT *only, int only_count, D3D12_CPU_DESCRIPTOR_HANDLE rtv);

static bool cui__d3d12_draw(cui_gpu_renderer *r, const cui__d3d12_frame *f, const cui_draw_data *dd) {
    D3D12_CPU_DESCRIPTOR_HANDLE none = { 0 };
    return cui__d3d12_draw_at(r, f, dd, dd->width_px, dd->height_px, NULL, 0, none);
}

/* Into a W by H target; with only, redrawn inside each rectangle alone, each cleared first. */
static bool cui__d3d12_draw_at(cui_gpu_renderer *r, const cui__d3d12_frame *f, const cui_draw_data *dd, int W, int H,
                               const D3D12_RECT *only, int only_count, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    ID3D12GraphicsCommandList *cmd = f->cmd;
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    D3D12_GPU_VIRTUAL_ADDRESS vva = 0, iva = 0;
    uint8_t *v = dd->index_count ? cui__d3d12_upload(r, vbytes, 16, &vva, NULL, NULL) : NULL;
    uint8_t *ix = v ? cui__d3d12_upload(r, ibytes, 16, &iva, NULL, NULL) : NULL;
    if (!dd->index_count) return true;
    if (!v || !ix) return false;
    memcpy(v, dd->vertices, vbytes);
    memcpy(ix, dd->indices, ibytes);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_VERTEX_BUFFER_VIEW vbv = { vva, (UINT)vbytes, sizeof(cui_vertex) };
    D3D12_INDEX_BUFFER_VIEW ibv = { iva, (UINT)ibytes, DXGI_FORMAT_R32_UINT };
    float viewport[2] = { (float)W, (float)H };
    cmd->lpVtbl->RSSetViewports(cmd, 1, &vp);
    cmd->lpVtbl->SetDescriptorHeaps(cmd, 1, &r->srv_heap);
    cmd->lpVtbl->SetGraphicsRootSignature(cmd, r->root);
    cmd->lpVtbl->SetPipelineState(cmd, r->pso);
    cmd->lpVtbl->IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->lpVtbl->IASetVertexBuffers(cmd, 0, 1, &vbv);
    cmd->lpVtbl->IASetIndexBuffer(cmd, &ibv);
    cmd->lpVtbl->SetGraphicsRoot32BitConstants(cmd, 0, 2, viewport, 0);
    static const float clear[4] = { 0, 0, 0, 0 };
    for (int pass = 0; pass < (only ? only_count : 1); pass++) {
        if (only) cmd->lpVtbl->ClearRenderTargetView(cmd, rtv, clear, 1, &only[pass]);
        int bound = -1;
        for (int i = 0; i < dd->command_count; i++) {
            const cui_draw_cmd *c = &dd->commands[i];
            if (!c->index_count) continue;
            int x0 = (int)floorf(c->clip.x), y0 = (int)floorf(c->clip.y);
            int x1 = (int)ceilf(c->clip.x + c->clip.w), y1 = (int)ceilf(c->clip.y + c->clip.h);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W) x1 = W;
            if (y1 > H) y1 = H;
            if (only) {
                if (x0 < only[pass].left) x0 = only[pass].left;
                if (y0 < only[pass].top) y0 = only[pass].top;
                if (x1 > only[pass].right) x1 = only[pass].right;
                if (y1 > only[pass].bottom) y1 = only[pass].bottom;
            }
            if (x1 <= x0 || y1 <= y0) continue;
            D3D12_RECT sc = { x0, y0, x1, y1 };
            cmd->lpVtbl->RSSetScissorRects(cmd, 1, &sc);
            cui_texture t = c->texture ? c->texture : dd->font_atlas;
            int slot = (int)t - 1;
            if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res) slot = r->none;
            if (slot != bound) {
                D3D12_GPU_DESCRIPTOR_HANDLE at = { r->srv_gpu.ptr + (UINT64)slot * r->srv_inc };
                cmd->lpVtbl->SetGraphicsRootDescriptorTable(cmd, 1, at);
                bound = slot;
            }
            cmd->lpVtbl->DrawIndexedInstanced(cmd, c->index_count, 1, c->index_offset, 0, 0);
        }
    }
    return true;
}

/* The frame copied out, if asked, and the back buffer made ready to present. */
static void cui__d3d12_copy_out(cui_gpu_renderer *r, cui__d3d12_frame *f, bool wanted) {
    ID3D12Resource *target = r->buffers[f->bb];
    memset(&f->fp, 0, sizeof f->fp);
    if (wanted) {
        D3D12_RESOURCE_DESC d;
        UINT64 size = 0;
        target->lpVtbl->GetDesc(target, &d);
        r->device->lpVtbl->GetCopyableFootprints(r->device, &d, 0, 1, 0, &f->fp, NULL, NULL, &size);
        if (cui__d3d12_buffer_make(r, &f->readback, (size_t)size, D3D12_HEAP_TYPE_READBACK)) {
            cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = { 0 }, src = { 0 };
            dst.pResource = f->readback.res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = f->fp;
            src.pResource = target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            f->cmd->lpVtbl->CopyTextureRegion(f->cmd, &dst, 0, 0, 0, &src, NULL);
            cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        }
    }
    if (!f->readback.res) cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
}

static void cui__d3d12_present(cui_gpu_renderer *r) {
    cui__d3d12_submit(r);
    if (r->lost) return;   /* (the frame was not submitted: there is nothing to present) */
    bool tear = !r->vsync && r->tearing;
    HRESULT shown = r->swapchain->lpVtbl->Present(r->swapchain, r->vsync ? 1 : 0, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (shown == DXGI_ERROR_DEVICE_REMOVED || shown == DXGI_ERROR_DEVICE_RESET) cui__d3d12_lose(r);
    /* so that idle waits for the present too (a resize releases the back buffers) */
    if (SUCCEEDED(r->queue->lpVtbl->Signal(r->queue, r->fence, r->fence_value + 1))) r->fence_value++;
    else cui__d3d12_lose(r);
}

/* Waits for the copy and writes it as RGBA; false if one was asked for and could not be made. */
static bool cui__d3d12_read_back(cui_gpu_renderer *r, cui__d3d12_frame *f, const cui_draw_data *dd, void *rgba) {
    bool ok = !rgba;
    if (f->readback.res && r->lost) {   /* the copy may still be writing it: it is neither read nor released */
        f->readback.res = NULL;
        return false;
    }
    if (f->readback.res) {
        cui__d3d12_wait(r, r->fence_value);
        if ((int)f->fp.Footprint.Width == dd->width_px && (int)f->fp.Footprint.Height == dd->height_px) {
            const uint8_t *src = f->readback.ptr + f->fp.Offset;
            uint8_t *dst = rgba;
            for (int y = 0; y < dd->height_px; y++, src += f->fp.Footprint.RowPitch)
                for (int x = 0; x < dd->width_px; x++, dst += 4) {
                    dst[0] = src[x * 4 + 2]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4]; dst[3] = 255;
                }
            ok = true;
        }
        CUI__REL(f->readback.res);
    }
    return ok;
}

static bool cui__d3d12_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd || r->lost) return false;
    dd = cui_expand_instances(&r->expanded, dd);
    if (!dd) return false;
    cui__d3d12_frame f = { 0 };
    if (!cui__d3d12_acquire(r, &f, dd)) return false;
    bool drawn = cui__d3d12_draw(r, &f, dd);
    cui__d3d12_copy_out(r, &f, rgba != NULL);
    cui__d3d12_present(r);
    if (r->shown < CUI__T12_BUFFERS) r->shown++;
    for (int k = 0; k < CUI__T12_BUFFERS - 1; k++) r->aged_count[k] = -1;   /* (should cui_gpu_render_damaged follow) */
    bool ok = cui__d3d12_read_back(r, &f, dd, rgba) && drawn && !r->lost;   /* (a capture of a frame that was not drawn, or not finished, is none) */
    /* wait until the swapchain can take another frame */
    WaitForSingleObjectEx(r->waitable, 1000, TRUE);
    return ok;
}

/* ---- damage: the frame kept as an image, redrawn and shown only where it changed ---- */

static D3D12_RECT cui__d3d12_rect(cui_rect b) { D3D12_RECT rc = { (LONG)b.x, (LONG)b.y, (LONG)(b.x + b.w), (LONG)(b.y + b.h) }; return rc; }

static bool cui__d3d12_keep(cui_gpu_renderer *r, int w, int h) {
    if (r->kept && r->kept_w == w && r->kept_h == h) return true;
    if (r->kept) { cui__d3d12_release_later(r, (IUnknown *)r->kept, -1); r->kept = NULL; }
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)w; desc.Height = (UINT)h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(r->device->lpVtbl->CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void **)&r->kept))) {
        r->kept = NULL;
        return false;
    }
    r->device->lpVtbl->CreateRenderTargetView(r->device, r->kept, NULL, cui__d3d12_rtv_at(r, CUI__T12_BUFFERS));
    r->kept_w = w; r->kept_h = h;
    return true;
}

/* The frame, or the part of it inside the boxes, drawn into the kept image. */
static bool cui__d3d12_draw_kept(cui_gpu_renderer *r, cui__d3d12_frame *f, const cui_draw_data *dd, const cui_rect *part, int parts) {
    f->cmd = cui__d3d12_cmd(r);
    if (!f->cmd) return false;
    cui__d3d12_barrier(r, r->kept, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cui__d3d12_rtv_at(r, CUI__T12_BUFFERS);
    float clear[4] = { dd->clear_color.r / 255.0f, dd->clear_color.g / 255.0f, dd->clear_color.b / 255.0f, 1 };
    D3D12_RECT where[CUI_DAMAGE_BOXES];
    if (!part) parts = 0;
    for (int i = 0; i < parts; i++) where[i] = cui__d3d12_rect(part[i]);
    f->cmd->lpVtbl->OMSetRenderTargets(f->cmd, 1, &rtv, FALSE, NULL);
    f->cmd->lpVtbl->ClearRenderTargetView(f->cmd, rtv, clear, (UINT)parts, parts ? where : NULL);
    const cui_draw_data *flat = cui_expand_instances(&r->expanded, dd);
    bool drawn = flat && cui__d3d12_draw(r, f, flat);
    cui__d3d12_barrier(r, r->kept, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return drawn;
}

static bool cui__d3d12_base(cui_gpu_renderer *r, const cui_draw_data *dd, const cui_rect *boxes, int n);

bool cui_gpu_render_damaged(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d, bool present_anyway) {
    if (!r || !dd || !d || r->lost) return false;
    if (r->shared) {   /* the list into a surface of the window's size below the others, when it changed */
        cui_rect changed_rect;
        bool all = false;
        if (dd->width_px <= 0 || dd->height_px <= 0) return false;
        if (!cui_damage_track(d, dd, &changed_rect, &all) && !present_anyway) return false;
        cui_rect boxes[CUI_DAMAGE_BOXES];
        int n = all || present_anyway ? -1 : cui_damage_boxes(d, boxes, CUI_DAMAGE_BOXES);
        return cui__d3d12_base(r, dd, boxes, n);
    }
    cui__d3d12_frame f = { 0 };
    if (!cui__d3d12_begin(r, &f)) return false;
    int W = dd->width_px, H = dd->height_px;
    if (W != r->w || H != r->h) return false;   /* the frame was laid out for another size: the next one fits */
    if (!r->kept || r->kept_w != W || r->kept_h != H) {
        if (!cui__d3d12_keep(r, W, H)) { cui__d3d12_render(r, dd, NULL); return true; }
        cui_damage_reset(d);   /* the new image holds nothing yet */
    }
    cui_rect rect;
    bool full = false;
    bool changed = cui_damage_track(d, dd, &rect, &full);
    if (!changed && !present_anyway) return false;
    if (cui_damage_direct(d)) {   /* much of the window keeps changing: straight into it, with no copy */
        r->kept_stale = true;
        cui__d3d12_render(r, dd, NULL);
        return !r->lost;
    }
    if (r->kept_stale) { changed = full = present_anyway = true; r->kept_stale = false; }   /* back to the kept image: all of it first */
    if (changed) {
        cui_rect part[CUI_DAMAGE_BOXES];
        int parts = 0;
        const cui_draw_data *cut = full ? NULL : cui_damage_clip_boxes(d, dd, part, &parts);
        if (!cut) full = true;
        cui__d3d12_draw_kept(r, &f, cut ? cut : dd, cut ? part : NULL, parts);
    }
    /* into the back buffer: what this frame changed, and what the frames it has not seen did */
    cui_rect boxes[CUI_DAMAGE_BOXES * CUI__T12_BUFFERS];
    int own = changed && !present_anyway ? cui_damage_boxes(d, boxes, CUI_DAMAGE_BOXES) : -1, n = own;
    bool part = own >= 0 && r->shown >= CUI__T12_BUFFERS;
    for (int k = 0; part && k < CUI__T12_BUFFERS - 1; k++) {
        if (r->aged_count[k] < 0) { part = false; break; }
        memcpy(boxes + n, r->aged[k], sizeof *boxes * (size_t)r->aged_count[k]);
        n += r->aged_count[k];
    }
    f.cmd = cui__d3d12_cmd(r);
    if (!f.cmd) return false;
    ID3D12Resource *target = r->buffers[f.bb];
    cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = { 0 }, src = { 0 };
    dst.pResource = target; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = r->kept; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (!part) f.cmd->lpVtbl->CopyTextureRegion(f.cmd, &dst, 0, 0, 0, &src, NULL);
    for (int i = 0; part && i < n; i++) {
        D3D12_RECT rc = cui__d3d12_rect(boxes[i]);
        D3D12_BOX box = { (UINT)rc.left, (UINT)rc.top, 0, (UINT)rc.right, (UINT)rc.bottom, 1 };
        if (rc.right > rc.left && rc.bottom > rc.top) f.cmd->lpVtbl->CopyTextureRegion(f.cmd, &dst, box.left, box.top, 0, &src, &box);
    }
    cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    cui__d3d12_present(r);
    if (r->shown < CUI__T12_BUFFERS) r->shown++;
    for (int k = CUI__T12_BUFFERS - 2; k > 0; k--) {
        r->aged_count[k] = r->aged_count[k - 1];
        if (r->aged_count[k] > 0) memcpy(r->aged[k], r->aged[k - 1], sizeof(cui_rect) * (size_t)r->aged_count[k]);
    }
    r->aged_count[0] = own;
    if (own > 0) memcpy(r->aged[0], boxes, sizeof(cui_rect) * (size_t)own);
    WaitForSingleObjectEx(r->waitable, 1000, TRUE);
    return !r->lost;
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) {
    if (r && dd && r->shared) { if (!r->lost) cui__d3d12_base(r, dd, NULL, -1); return; }
    cui__d3d12_render(r, dd, NULL);
}
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return r && rgba && !r->shared && cui__d3d12_render(r, dd, rgba); }

/* ---- surfaces: a swap chain each, shown by a visual the system lays over the window ---- */

bool cui_gpu_surfaces(cui_gpu_renderer *r) {
    if (!r || r->lost || r->dcomp_failed) return false;
    if (r->dcomp) return true;
    if (r->shared) return false;
    if (FAILED(DCompositionCreateDevice(NULL, &cui__IID_IDCompositionDevice, (void **)&r->dcomp)) || !r->dcomp ||
        FAILED(cui__dc_target(r->dcomp, r->hwnd, &r->dcomp_target)) || FAILED(cui__dc_visual(r->dcomp, &r->dcomp_root)) ||
        FAILED(cui__dc_set_root(r->dcomp_target, r->dcomp_root)) || FAILED(cui__dc_commit(r->dcomp))) {
        cui__dc_release(&r->dcomp_root);
        cui__dc_release(&r->dcomp_target);
        cui__dc_release(&r->dcomp);
        r->dcomp_failed = true;
        return false;
    }
    return true;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cui__d3d12_pic_rtv(cui_gpu_renderer *r, int at, int buffer) {
    return cui__d3d12_rtv_at(r, CUI__T12_BUFFERS + 1 + at * CUI__T12_ZONE_BUFFERS + buffer);
}

static bool cui__d3d12_pic_targets(cui_gpu_renderer *r, int at) {
    cui__d3d12_pic *p = &r->pic[at];
    for (int i = 0; i < CUI__T12_ZONE_BUFFERS; i++) {
        if (FAILED(p->swapchain->lpVtbl->GetBuffer(p->swapchain, (UINT)i, &IID_ID3D12Resource, (void **)&p->buffers[i]))) { p->buffers[i] = NULL; return false; }
        r->device->lpVtbl->CreateRenderTargetView(r->device, p->buffers[i], NULL, cui__d3d12_pic_rtv(r, at, i));
    }
    return true;
}

/* Where the visual lies, and over which of the others: each over those of lower order. */
static void cui__d3d12_zone_place(cui_gpu_renderer *r, int slot) {
    cui__d3d12_zone *z = &r->zone[slot];
    cui__dc_offset(z->visual, (float)z->x, (float)z->y);
    if (z->shown) cui__dc_remove(r->dcomp_root, z->visual);
    cui__d3d12_zone *below = NULL;
    for (int i = 0; i <= CUI_SURFACES; i++) {
        cui__d3d12_zone *o = &r->zone[i];
        if (i == slot || !o->shown || !o->visual || o->order > z->order) continue;
        if (!below || o->order > below->order) below = o;
    }
    /* over the one just below it; with none below, under all the others */
    if (below) cui__dc_add(r->dcomp_root, z->visual, TRUE, below->visual);
    else cui__dc_add(r->dcomp_root, z->visual, FALSE, NULL);
    z->shown = true;
    cui__dc_commit(r->dcomp);
}

/* A pool swap chain and its buffers are let go of once the work that draws into them has run. */
static void cui__d3d12_pic_free(cui_gpu_renderer *r, int at) {
    cui__d3d12_pic *p = &r->pic[at];
    for (int b = 0; b < CUI__T12_ZONE_BUFFERS; b++) if (p->buffers[b]) cui__d3d12_release_later(r, (IUnknown *)p->buffers[b], -1);
    if (p->swapchain) cui__d3d12_release_later(r, (IUnknown *)p->swapchain, -1);
    memset(p, 0, sizeof *p);
}

/* A new swap chain of cw by ch pixels in pool slot at, premultiplied: the window shows through. */
static bool cui__d3d12_pic_make(cui_gpu_renderer *r, int at, int cw, int ch) {
    cui__d3d12_pic *p = &r->pic[at];
    memset(p, 0, sizeof *p);
    DXGI_SWAP_CHAIN_DESC1 desc = { 0 };
    desc.Width = (UINT)cw; desc.Height = (UINT)ch;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = CUI__T12_ZONE_BUFFERS;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   /* back buffers keep their pictures: only what changed is drawn again */
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1 *sc1 = NULL;
    IDXGIFactory4 *f = r->factory;
    if (FAILED(f->lpVtbl->CreateSwapChainForComposition(f, (IUnknown *)r->queue, &desc, NULL, &sc1)) ||
        FAILED(sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&p->swapchain))) { CUI__REL(sc1); p->swapchain = NULL; return false; }
    CUI__REL(sc1);
    if (!cui__d3d12_pic_targets(r, at)) {
        for (int b = 0; b < CUI__T12_ZONE_BUFFERS; b++) CUI__REL(p->buffers[b]);
        CUI__REL(p->swapchain);
        return false;
    }
    p->cw = cw; p->ch = ch;
    p->aged_count = -1;
    return true;
}

/* A swap chain new to the zone's visual becomes its content only once it holds a picture: shown
 * before that it is black for a frame, which a surface made again while a window is dragged flashes. */
static void cui__d3d12_zone_show(cui_gpu_renderer *r, int slot) {
    cui__d3d12_zone *z = &r->zone[slot];
    if (z->reclip && !z->unshown && !--z->reclip) {   /* the surface took another size within its swap chain: cut to it once that is drawn */
        cui__dc_clip(z->visual, (float)z->w, (float)z->h);
        cui__dc_commit(r->dcomp);
    }
    if (!z->unshown) return;
    z->unshown = false;
    z->reclip = 0;
    DwmFlush();   /* the system has taken the first picture: the swap chain is not shown a frame before it */
    cui__dc_content(z->visual, (IUnknown *)r->pic[z->at].swapchain);
    cui__dc_clip(z->visual, (float)z->w, (float)z->h);
    cui__dc_commit(r->dcomp);
}

static double cui__d3d12_seconds(void) {
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    return (double)now.QuadPart / (double)hz.QuadPart;
}

static cui_id cui__d3d12_zone_id(const cui__d3d12_zone *z, int slot) { return z->element ? z->element : 0xFFFFFF00u + (cui_id)(slot + 1); }

/* A zone's swap chain becomes spare, as it is: the element that comes back finds it. */
static void cui__d3d12_give_back(cui_gpu_renderer *r, int slot, double since) {
    cui__d3d12_zone *z = &r->zone[slot];
    if (z->key) cui_pool_give_back(&r->pool, cui__d3d12_zone_id(z, slot), since);
    z->key = 0;
    z->at = -1;
}

/* The pool swap chain the zone's W by H pixels go into, its visual cut to them; -1: none (the frame
 * is left out). While the window is dragged one that fits is kept and one that does not gets a quarter more. */
static int cui__d3d12_take(cui_gpu_renderer *r, int slot, int W, int H) {
    cui__d3d12_zone *z = &r->zone[slot];
    int kw = W, kh = H;
    if (r->live_resize && z->key) {
        int cw = cui_pool_key_width(z->key), ch = cui_pool_key_height(z->key);
        kw = W <= cw ? cw : CUI_MIN(W + W / 4, 8192);
        kh = H <= ch ? ch : CUI_MIN(H + H / 4, 8192);
    }
    uint16_t key = cui_pool_key(kw, kh);
    double now = cui__d3d12_seconds();
    if (z->key && z->key != key) cui__d3d12_give_back(r, slot, now - CUI_POOL_SPARE_SECONDS);   /* the old size's go first */
    z->key = key;
    bool make = false;
    int evict = -1, at = cui_pool_take(&r->pool, cui__d3d12_zone_id(z, slot), kw, kh, -1, now, &make, &evict);
    if (evict >= 0) { cui__d3d12_pic_free(r, evict); r->left_out = true; }
    if (at < 0) { r->left_out = r->left_out || key; return -1; }
    if (make && !cui__d3d12_pic_make(r, at, cui_pool_key_width(key), cui_pool_key_height(key))) { cui_pool_drop(&r->pool, at); return -1; }
    if (!z->visual && FAILED(cui__dc_visual(r->dcomp, &z->visual))) { z->visual = NULL; return -1; }
    if (at != z->at) { z->at = at; z->unshown = true; z->w = W; z->h = H; }
    else if (z->w != W || z->h != H) { z->reclip = 1; z->w = W; z->h = H; r->pic[at].drawn = 0; }
    return at;
}

/* flat, already vertices, W by H pixels, drawn into the zone's next back buffer and shown at x, y.
 * boxes (n of them; n < 0: all of it) are what changed since the frame before. A back buffer holds
 * the frame of two presents ago, so it is drawn again inside these boxes and those of the frame before. */
static bool cui__d3d12_zone_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *flat, int x, int y, int W, int H, int order, const cui_rect *boxes, int n) {
    cui__d3d12_zone *z = &r->zone[slot];
    int at = cui__d3d12_take(r, slot, W, H);
    if (at < 0) {
        if (FAILED(r->device->lpVtbl->GetDeviceRemovedReason(r->device))) { cui__d3d12_lose(r); return false; }
        return r->left_out;   /* (all taken, or room is made: drawn next frame) */
    }
    cui__d3d12_pic *pic = &r->pic[at];
    bool shifted = pic->size_w != W || pic->size_h != H || r->pool.slot[at].missed_count < 0;
    pic->size_w = W; pic->size_h = H;
    bool some = n >= 0 && !shifted && pic->drawn >= CUI__T12_ZONE_BUFFERS && pic->aged_count >= 0;
    D3D12_RECT where[CUI_DAMAGE_BOXES * 2];
    cui_rect own[CUI_DAMAGE_BOXES];
    int where_count = 0, own_count = 0;
    for (int i = 0; n >= 0 && i < n + (some ? pic->aged_count : 0); i++) {
        cui_rect b = i < n ? boxes[i] : pic->aged[i - n];
        D3D12_RECT rc = { (LONG)floorf(b.x), (LONG)floorf(b.y), (LONG)ceilf(b.x + b.w), (LONG)ceilf(b.y + b.h) };
        if (rc.left < 0) rc.left = 0;
        if (rc.top < 0) rc.top = 0;
        if (rc.right > W) rc.right = W;
        if (rc.bottom > H) rc.bottom = H;
        if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
        where[where_count++] = rc;
        if (i >= n) continue;
        own[own_count++] = (cui_rect){ (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top) };
    }
    if (some && !own_count) {   /* (what changed lies outside the surface: its picture stands) */
        if (z->reclip && !z->unshown) {
            z->reclip = 0;
            cui__dc_clip(z->visual, (float)z->w, (float)z->h);
            cui__dc_commit(r->dcomp);
        }
        return !r->lost;
    }
    cui__d3d12_frame f = { 0 };
    f.cmd = cui__d3d12_cmd(r);
    if (!f.cmd) return false;
    int bb = (int)pic->swapchain->lpVtbl->GetCurrentBackBufferIndex(pic->swapchain);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cui__d3d12_pic_rtv(r, at, bb);
    float clear[4] = { 0, 0, 0, 0 };
    cui__d3d12_barrier(r, pic->buffers[bb], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    f.cmd->lpVtbl->OMSetRenderTargets(f.cmd, 1, &rtv, FALSE, NULL);
    if (!some) f.cmd->lpVtbl->ClearRenderTargetView(f.cmd, rtv, clear, 0, NULL);
    bool drawn = cui__d3d12_draw_at(r, &f, flat, W, H, some ? where : NULL, where_count, rtv);
    cui__d3d12_barrier(r, pic->buffers[bb], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cui__d3d12_submit(r);
    if (r->lost) return false;
    HRESULT shown = pic->swapchain->lpVtbl->Present(pic->swapchain, 0, 0);   /* the window's own present paces the frame */
    if (shown == DXGI_ERROR_DEVICE_REMOVED || shown == DXGI_ERROR_DEVICE_RESET) { cui__d3d12_lose(r); return false; }
    if (SUCCEEDED(r->queue->lpVtbl->Signal(r->queue, r->fence, r->fence_value + 1))) r->fence_value++;
    else cui__d3d12_lose(r);
    cui__d3d12_zone_show(r, slot);
    cui_pool_drawn(&r->pool, at, r->fence_value, NULL, 0);
    pic->aged_count = n >= 0 && !shifted ? own_count : -1;
    if (pic->aged_count > 0) memcpy(pic->aged, own, sizeof(cui_rect) * (size_t)own_count);
    if (pic->drawn < CUI__T12_ZONE_BUFFERS) pic->drawn++;
    bool moved = !z->shown || z->x != x || z->y != y || z->order != order;
    z->x = x; z->y = y; z->order = order;
    if (moved) cui__d3d12_zone_place(r, slot);
    return drawn && !r->lost;
}

bool cui_gpu_surface_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, const cui_damage *d, int x, int y, int order) {
    if (!dd || slot < 0 || slot >= CUI_SURFACES || dd->width_px <= 0 || dd->height_px <= 0 || !cui_gpu_surfaces(r)) return false;
    cui_rect boxes[CUI_DAMAGE_BOXES];
    int n = d ? cui_damage_boxes(d, boxes, CUI_DAMAGE_BOXES) : -1;
    const cui_draw_data *flat = cui_expand_instances(&r->expanded, dd);
    return flat && cui__d3d12_zone_draw(r, slot, flat, x, y, dd->width_px, dd->height_px, order, boxes, n);
}

/* A hidden surface's visual leaves the tree; its swap chain goes back to the pool. */
static void cui__d3d12_zone_hide(cui_gpu_renderer *r, int slot) {
    cui__d3d12_zone *z = &r->zone[slot];
    if (!r->dcomp || !z->visual) return;
    if (z->shown) {
        cui__dc_remove(r->dcomp_root, z->visual);
        z->shown = false;
        cui__dc_commit(r->dcomp);
    }
    if (z->at >= 0) {   /* its swap chain stays spare as it is */
        cui__dc_content(z->visual, NULL);
        cui__d3d12_give_back(r, slot, cui__d3d12_seconds());
        cui__dc_commit(r->dcomp);
    }
}

void cui_gpu_surfaces_end(cui_gpu_renderer *r) {
    if (!r || !r->dcomp) return;
    int gone = cui_pool_reap(&r->pool, cui__d3d12_seconds(), r->fence->lpVtbl->GetCompletedValue(r->fence), &r->reap_due);
    if (gone >= 0) cui__d3d12_pic_free(r, gone);
}

/* The window's own list on a program's device: a surface of the window's size, below every other. */
static bool cui__d3d12_base(cui_gpu_renderer *r, const cui_draw_data *dd, const cui_rect *boxes, int n) {
    const cui_draw_data *flat = cui_expand_instances(&r->expanded, dd);
    return flat && cui__d3d12_zone_draw(r, CUI__T12_BASE, flat, 0, 0, dd->width_px, dd->height_px, -1, boxes, n);
}

bool cui_gpu_left_out(cui_gpu_renderer *r) {
    bool was = r && r->left_out;
    if (r) r->left_out = false;
    return was;
}

cui_damage *cui_gpu_surface_damage(cui_gpu_renderer *r, int slot, uint32_t element) {
    if (!r || slot < 0 || slot >= CUI_SURFACES || !element || !cui_gpu_surfaces(r)) return NULL;
    cui__d3d12_zone *z = &r->zone[slot];
    if (z->element != element) {   /* another element's: spare until it is back */
        cui__d3d12_give_back(r, slot, cui__d3d12_seconds());
        z->element = element;
    }
    int owner = cui_pool_owner_of(&r->pool, element);
    return owner < 0 ? NULL : &r->pool.owner[owner].damage.head;
}

float cui_gpu_next_wakeup(cui_gpu_renderer *r) { return r ? (float)r->reap_due : -1; }

void cui_gpu_live_resize(cui_gpu_renderer *r, bool on) {
    if (!r || r->live_resize == on) return;
    r->live_resize = on;
    if (!on) r->left_out = true;   /* drawn again whole, into settled sizes */
}

void cui_gpu_surface_place(cui_gpu_renderer *r, int slot, int x, int y, int order) {
    if (!r || slot < 0 || slot >= CUI_SURFACES || !r->dcomp || !r->zone[slot].visual) return;
    cui__d3d12_zone *z = &r->zone[slot];
    z->x = x; z->y = y; z->order = order;
    cui__d3d12_zone_place(r, slot);
}

void cui_gpu_surface_window(cui_gpu_renderer *r, int slot, void *wl_surface) { (void)r; (void)slot; (void)wl_surface; }   /* (Wayland only) */

void cui_gpu_surface_hide(cui_gpu_renderer *r, int slot) {
    if (r && slot >= 0 && slot < CUI_SURFACES) cui__d3d12_zone_hide(r, slot);
}

#endif
