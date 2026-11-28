/* Metal renderer (cui_gpu_*, built with CUI_METAL, macOS): the draw data
 * the OpenGL renderer takes (cui_draw_data's vertices, indices and commands), drawn the same
 * way. One pipeline; its fragment function is cui_render_gl.c's fragment shader in the Metal
 * Shading Language, branching on the vertex's mode. Keep the two in step.
 *
 * - A frame's vertices and indices are copied into a shared-storage buffer (one of three in
 *   turn, reused once the frame that read it is done), then a draw per command with its clip
 *   rectangle as the scissor and its texture.
 * - Drawn straight into a BGRA8 UNORM drawable of a CAMetalLayer, which becomes the layer of
 *   the window's content view, premultiplied source over, as the OpenGL renderer blends.
 *   Paced by the layer handing out drawables (with vsync, displaySyncEnabled).
 * - A surface (cui_gpu_surface_*) is a plain CALayer whose contents is an IOSurface the renderer
 *   drew into through a texture made over it. Each keeps a picture shown and one to draw the next
 *   into (a third only while the window server still holds both), and the layer's contents and
 *   place change in one transaction.
 *   CUI_MTL_IOSURFACES=0: a CAMetalLayer instead, whose drawables the system holds and which keep
 *   what was drawn into them, so each is drawn again only where it missed a change; it is
 *   presented in the transaction that places it (presentsWithTransaction).
 * - On a program's own device (cui_gpu_create_shared) there is no layer of the window's size: the
 *   surfaces go under a layer the program names, and the window's own list into a picture fitted
 *   to what it draws, none at all when it draws nothing.
 * Compile with -fobjc-arc. */
#include "cui/cui.h"
#include "cui/cui_render.h"
#include "cui_pool.h"

#if defined(CUI_METAL) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <IOSurface/IOSurface.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUI__TMTL_SLOTS 3           /* vertex memory, reused once its last frame is done */
#define CUI__MTL_ZONE_SLOTS 6       /* a surface's vertex memories at most */
#define CUI__MTL_BASE (-1)          /* the slot of the window's own list on a program's device */

static const char cui__mtl_msl[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VI {\n"
"  float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float2 local [[attribute(2)]]; float2 half_ [[attribute(3)]];\n"
"  float4 radii [[attribute(4)]]; float2 params [[attribute(5)]]; float4 color [[attribute(6)]]; float4 bcolor [[attribute(7)]];\n"
"};\n"
"struct VO {\n"
"  float4 pos [[position]]; float2 uv; float2 local;\n"
"  float2 half_ [[flat]]; float4 radii [[flat]]; float2 params [[flat]]; float4 color; float4 bcolor [[flat]];\n"
"};\n"
"vertex VO vs_main(VI i [[stage_in]], constant float2 &viewport [[buffer(1)]]) {\n"
"  VO o;\n"
"  o.uv = i.uv; o.local = i.local; o.half_ = i.half_; o.radii = i.radii;\n"
"  o.params = i.params; o.color = i.color; o.bcolor = i.bcolor;\n"
"  o.pos = float4(i.pos.x / viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / viewport.y * 2.0, 0.0, 1.0);\n"
"  return o;\n"
"}\n"
"static float sd_rrect(float2 p, float2 b, float4 r) {\n"
"  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
"  rr = min(rr, min(b.x, b.y));\n"
"  float2 q = abs(p) - b + rr;\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - rr;\n"
"}\n"
"static float erf_(float x) {\n"
"  float s = sign(x); x = abs(x);\n"
"  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
"  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);\n"
"  return s * y;\n"
"}\n"
"static float bayer8(float2 p) {\n"
"  float2 f = floor(p);\n"
"  int2 q = int2(f - 8.0 * floor(f / 8.0));\n"
"  int a = q.x ^ q.y, y = q.y;\n"
"  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);\n"
"  return (float(m) + 0.5) / 64.0;\n"
"}\n"
"fragment float4 ps_main(VO i [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
"  int mode = int(i.params.y + 0.5);\n"
"  float4 c = i.color;\n"
"  if (mode == 4) return float4(c.rgb * c.a, c.a);\n"
"  float4 t = tex.sample(smp, i.uv, level(0.0));\n"   /* (textures have one level) */
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

@interface CUIMetalSlot : NSObject
@property (nonatomic, strong) id<MTLBuffer> vertices, indices;
@property (nonatomic, strong) id<MTLCommandBuffer> reader;   /* the frame that last read it */
@end
@implementation CUIMetalSlot
@end

/* A surface of the UI as a layer of its own: its contents is the picture drawn last. */
@interface CUIMetalZone : NSObject
@property (nonatomic, strong) CALayer *layer;
@property (nonatomic, strong) NSMutableArray<CUIMetalSlot *> *slots;
@property (nonatomic, strong) id<MTLCommandBuffer> last;   /* the commands that drew the picture shown */
@property (nonatomic, assign) int front, x, y, w, h, order;   /* front: the pool slot shown, -1: none */
@property (nonatomic, assign) uint32_t element;               /* whose pictures the pool gives it; 0: the slot's own */
@property (nonatomic, assign) uint16_t key;                   /* the pool bucket of its pictures */
@property (nonatomic, assign) BOOL shown, solid;
@end
@implementation CUIMetalZone
@end

@interface CUIMetalWindow : NSObject
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) CAMetalLayer *layer;
@property (nonatomic, strong) CALayer *host;               /* a program's own device: the layer its surfaces go under, and no layer of the window's size */
@property (nonatomic, assign) const cui_metal_host *place; /* (the program's: the scale and which way up) */
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pso;
@property (nonatomic, strong) id<MTLRenderPipelineState> pso_clear;   /* the same without blending: writes a part of a picture back to nothing */
@property (nonatomic, strong) id<MTLSamplerState> sampler;
@property (nonatomic, strong) id<MTLTexture> none;   /* a black texel, for a command with no texture */
@property (nonatomic, strong) NSArray<CUIMetalSlot *> *slots;
/* The textures alive, by their cui_texture (the object's address). A handle that is not here
 * (destroyed, never made) draws the black texel instead of being used as an object. */
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, id<MTLTexture>> *textures;
@property (nonatomic, assign) int slot;
@property (nonatomic, strong) id<MTLCommandBuffer> last;   /* the frame committed last */
@property (nonatomic, strong) id<MTLTexture> kept;         /* damage tracking: the window's picture, redrawn only where a frame changed */
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, CUIMetalZone *> *zones;   /* the surfaces that have a layer, by slot */
@end
@implementation CUIMetalWindow
@end

/* A pool picture: an IOSurface (BGRA, sRGB) and the texture over it, and the size last drawn into it. */
typedef struct cui__mtl_pic {
    IOSurfaceRef surface;
    void *texture;                      /* id<MTLTexture>, retained */
    int size_w, size_h;
} cui__mtl_pic;

static void cui__mtl_pic_free(cui__mtl_pic *p);

struct cui_gpu_renderer {
    cui_expanded expanded;              /* instances written out as vertices: this renderer draws vertices only */
    void *win;                          /* CUIMetalWindow, retained */
    bool readable;                      /* a frame was read back: the drawables stay readable from then on */
    bool kept_stale;                    /* frames went straight into the window (cui_damage_direct): the kept image lacks them */
    bool in_frame;                      /* a program's device: the transaction of this frame's layer changes is open (cui_gpu_surfaces_end) */
    bool left_out;                      /* a surface's frame was left out since cui_gpu_left_out was asked */
    cui_pool pool;                      /* the surfaces' pictures, by slot (IOSurfaces) */
    cui__mtl_pic pic[CUI_POOL_SLOTS];
    uint64_t submitted, completed;      /* surface frames committed, and done on the GPU (written by Metal's thread) */
    double reap_due;                    /* seconds until a spare picture is due to go, <0: none */
    bool live_resize;
    cui_draw_cmd *clip_cmds;            /* a list cut to what changed */
    int clip_cap;
    cui_draw_data clip_data;
};

static CUIMetalWindow *cui__mtl_win(const cui_gpu_renderer *r) { return (__bridge CUIMetalWindow *)r->win; }

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static id<MTLTexture> cui__mtl_tex_make(id<MTLDevice> device, int w, int h, cui_texture_format fmt, const void *pixels) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt == CUI_TEXTURE_R8 ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm
                                                                                 width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    if (t && pixels) {
        NSUInteger bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w * bpp];
    }
    return t;
}

static cui_texture cui__mtl_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    if (w <= 0 || h <= 0) return 0;
    @autoreleasepool {
        CUIMetalWindow *win = cui__mtl_win(user);
        id<MTLTexture> t = cui__mtl_tex_make(win.device, w, h, fmt, pixels);
        if (!t) return 0;
        cui_texture handle = (cui_texture)(uintptr_t)(__bridge void *)t;
        win.textures[@((uintptr_t)handle)] = t;   /* the table keeps it alive */
        return handle;
    }
}

static void cui__mtl_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    if (!tex || w <= 0 || h <= 0 || !pixels) return;
    @autoreleasepool {
        CUIMetalWindow *win = cui__mtl_win(user);
        id<MTLTexture> t = win.textures[@((uintptr_t)tex)];
        if (!t) return;
        [win.last waitUntilCompleted];   /* replaceRegion is not ordered after frames that may still sample it */
        NSUInteger bpp = t.pixelFormat == MTLPixelFormatR8Unorm ? 1 : 4;
        [t replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
               withBytes:pixels bytesPerRow:stride ? (NSUInteger)stride : (NSUInteger)w * bpp];
    }
}

static void cui__mtl_tex_destroy(void *user, cui_texture tex) {
    if (!tex) return;
    /* frames in flight that sample it keep it alive until they are done */
    @autoreleasepool { [cui__mtl_win(user).textures removeObjectForKey:@((uintptr_t)tex)]; }
}

/* ---- the device ---- */

static bool cui__mtl_pipeline(CUIMetalWindow *w) {
    /* one attribute per group of cui_vertex fields, as the OpenGL renderer's */
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    static const struct { MTLVertexFormat format; NSUInteger offset; } attrs[8] = {
        { MTLVertexFormatFloat2, offsetof(cui_vertex, x) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, u) },
        { MTLVertexFormatFloat2, offsetof(cui_vertex, lx) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, hw) },
        { MTLVertexFormatFloat4, offsetof(cui_vertex, r_tl) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, border) },
        { MTLVertexFormatUChar4Normalized, offsetof(cui_vertex, color) }, { MTLVertexFormatUChar4Normalized, offsetof(cui_vertex, border_color) },
    };
    for (NSUInteger i = 0; i < 8; i++) {
        vd.attributes[i].format = attrs[i].format;
        vd.attributes[i].offset = attrs[i].offset;
        vd.attributes[i].bufferIndex = 0;
    }
    vd.layouts[0].stride = sizeof(cui_vertex);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    NSError *error = nil;
    id<MTLLibrary> lib = [w.device newLibraryWithSource:[NSString stringWithUTF8String:cui__mtl_msl] options:nil error:&error];
    if (!lib) { fprintf(stderr, "cui: metal: %s\n", error.localizedDescription.UTF8String); return false; }
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [lib newFunctionWithName:@"ps_main"];
    d.vertexDescriptor = vd;
    /* premultiplied source over */
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatBGRA8Unorm;
    c.blendingEnabled = YES;
    c.sourceRGBBlendFactor = MTLBlendFactorOne;
    c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    c.sourceAlphaBlendFactor = MTLBlendFactorOne;
    c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    w.pso = [w.device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!w.pso) { fprintf(stderr, "cui: metal: %s\n", error.localizedDescription.UTF8String); return false; }
    c.blendingEnabled = NO;
    w.pso_clear = [w.device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!w.pso_clear) { fprintf(stderr, "cui: metal: %s\n", error.localizedDescription.UTF8String); return false; }
    return true;
}

static bool cui__mtl_states(CUIMetalWindow *w);

static bool cui__mtl_device(CUIMetalWindow *w) {
    /* the device CUI_GPU names, else the system's default */
    const char *want = getenv("CUI_GPU");
    if (want && *want) {
        NSString *part = [NSString stringWithUTF8String:want];
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if (!w.device && part && [d.name rangeOfString:part options:NSCaseInsensitiveSearch].location != NSNotFound) w.device = d;
    }
    if (!w.device) w.device = MTLCreateSystemDefaultDevice();
    if (!w.device) return false;
    w.queue = [w.device newCommandQueue];
    return cui__mtl_states(w);
}

/* What a device and a queue need before they draw: the pipeline, the sampler, the black texel. */
static bool cui__mtl_states(CUIMetalWindow *w) {
    if (!w.queue || !cui__mtl_pipeline(w)) return false;
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    w.sampler = [w.device newSamplerStateWithDescriptor:sd];
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    w.none = cui__mtl_tex_make(w.device, 1, 1, CUI_TEXTURE_RGBA8, black);
    return w.sampler && w.none;
}

/* ---- the window ---- */

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    cui_expanded_free(&r->expanded);
    free(r->clip_cmds);
    @autoreleasepool {
        CUIMetalWindow *w = (__bridge_transfer CUIMetalWindow *)r->win;
        for (CUIMetalSlot *s in w.slots) [s.reader waitUntilCompleted];
        for (CUIMetalZone *z in w.zones.allValues) {
            for (CUIMetalSlot *s in z.slots) [s.reader waitUntilCompleted];
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            z.layer.contents = nil;
            [z.layer removeFromSuperlayer];
            [CATransaction commit];
        }
        [w.zones removeAllObjects];
        [w.last waitUntilCompleted];
        for (int i = 0; i < CUI_POOL_SLOTS; i++) cui__mtl_pic_free(&r->pic[i]);
        w = nil;
    }
    free(r);
}

/* window: the NSWindow; its content view shows the Metal layer */
cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    @autoreleasepool {
        NSWindow *nsw = (__bridge NSWindow *)window;
        if (!nsw || ![nsw isKindOfClass:[NSWindow class]] || !nsw.contentView) return NULL;
        CUIMetalWindow *w = [[CUIMetalWindow alloc] init];
        if (!cui__mtl_device(w)) return NULL;
        cui_gpu_renderer *r = calloc(1, sizeof *r);
        if (!r) return NULL;
        w.window = nsw;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = w.device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        layer.maximumDrawableCount = 3;
        layer.displaySyncEnabled = vsync;
        layer.allowsNextDrawableTimeout = YES;
        /* CUI's colors are sRGB: the system converts to the display's (P3 on a Mac's own) */
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        layer.contentsScale = nsw.backingScaleFactor;
        /* a resize shows the last frame at the top left until the next is drawn, not stretched */
        layer.contentsGravity = kCAGravityTopLeft;
        layer.actions = @{ @"contents" : [NSNull null], @"bounds" : [NSNull null], @"position" : [NSNull null] };
        nsw.contentView.layer = layer;      /* layer first: the view hosts it, AppKit does not draw into it */
        nsw.contentView.wantsLayer = YES;
        w.layer = layer;
        w.textures = [NSMutableDictionary dictionary];
        w.zones = [NSMutableDictionary dictionary];
        w.slots = @[ [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init] ];
        r->win = (__bridge_retained void *)w;
        r->reap_due = -1;
        return r;
    }
}

/* window: a cui_metal_host. The device and the queue are the program's. */
cui_gpu_renderer *cui_gpu_create_shared(void *window, bool vsync, const cui_gpu_device *device) {
    (void)vsync;
    const cui_metal_host *host = window;
    if (!host || !host->layer || !device || !device->device || !device->queue) return NULL;
    @autoreleasepool {
        CUIMetalWindow *w = [[CUIMetalWindow alloc] init];
        w.device = (__bridge id<MTLDevice>)device->device;
        w.queue = (__bridge id<MTLCommandQueue>)device->queue;
        if (!cui__mtl_states(w)) return NULL;
        cui_gpu_renderer *r = calloc(1, sizeof *r);
        if (!r) return NULL;
        w.host = (__bridge CALayer *)host->layer;
        w.place = host;
        w.textures = [NSMutableDictionary dictionary];
        w.zones = [NSMutableDictionary dictionary];
        w.slots = @[ [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init] ];
        r->win = (__bridge_retained void *)w;
        r->reap_due = -1;
        return r;
    }
}

/* native: an id<MTLTexture> of the renderer's device, which the table keeps alive. */
cui_texture cui_gpu_texture_wrap(cui_gpu_renderer *r, void *native) {
    if (!r || !native) return 0;
    @autoreleasepool {
        id<MTLTexture> t = (__bridge id<MTLTexture>)native;
        cui_texture handle = (cui_texture)(uintptr_t)native;
        cui__mtl_win(r).textures[@((uintptr_t)handle)] = t;
        return handle;
    }
}

void cui_gpu_texture_unwrap(cui_gpu_renderer *r, cui_texture texture) {
    if (!r || !texture) return;
    @autoreleasepool { [cui__mtl_win(r).textures removeObjectForKey:@((uintptr_t)texture)]; }
}

void cui_gpu_flush(cui_gpu_renderer *r) {
    if (!r) return;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        [w.last waitUntilCompleted];
        for (CUIMetalZone *z in w.zones.allValues) [z.last waitUntilCompleted];
    }
}

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__mtl_tex_create;
    i.texture_update = cui__mtl_tex_update;
    i.texture_destroy = cui__mtl_tex_destroy;
    return i;
}

/* ---- frames ---- */

/* a shared-storage buffer of at least `bytes` */
static id<MTLBuffer> cui__mtl_room(id<MTLDevice> device, id<MTLBuffer> have, size_t bytes) {
    if (have.length >= bytes) return have;
    size_t cap = have.length * 2 > 65536 ? have.length * 2 : 65536;
    while (cap < bytes) cap *= 2;
    return [device newBufferWithLength:cap options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
}

/* The drawable follows the frame's size in pixels. */
static void cui__mtl_layer(cui_gpu_renderer *r, CUIMetalWindow *w, int fw, int fh, bool capture) {
    CAMetalLayer *layer = w.layer;
    CGSize size = layer.drawableSize;
    if ((int)size.width != fw || (int)size.height != fh) layer.drawableSize = CGSizeMake(fw, fh);
    CGFloat scale = w.window.backingScaleFactor;
    if (layer.contentsScale != scale) layer.contentsScale = scale;
    /* a frame read back is blitted from; once one was, the drawables stay readable */
    if (capture) r->readable = true;
    if (layer.framebufferOnly != !r->readable) layer.framebufferOnly = !r->readable;
    /* while the window is dragged to a new size, the frame joins the Core Animation transaction
     * that resizes the window: drawn for the new size and shown with it, not a refresh later */
    BOOL sync = w.window.inLiveResize;
    if (layer.presentsWithTransaction != sync) layer.presentsWithTransaction = sync;
}

/* The next slot of vertex and index buffers, once its last reader is done. */
static CUIMetalSlot *cui__mtl_slot(CUIMetalWindow *w) {
    w.slot = (w.slot + 1) % CUI__TMTL_SLOTS;
    CUIMetalSlot *slot = w.slots[(NSUInteger)w.slot];
    if (slot.reader) { [slot.reader waitUntilCompleted]; slot.reader = nil; }
    return slot;
}

/* The frame's vertices and indices into the slot; false when there is nothing to draw. */
static bool cui__mtl_upload(CUIMetalWindow *w, CUIMetalSlot *slot, const cui_draw_data *dd) {
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    if (dd->index_count <= 0 || dd->vertex_count <= 0) return false;
    slot.vertices = cui__mtl_room(w.device, slot.vertices, vbytes);
    slot.indices = cui__mtl_room(w.device, slot.indices, ibytes);
    if (!slot.vertices || !slot.indices) return false;
    memcpy(slot.vertices.contents, dd->vertices, vbytes);
    memcpy(slot.indices.contents, dd->indices, ibytes);
    return true;
}

/* The pass into the drawable, cleared. */
static id<MTLRenderCommandEncoder> cui__mtl_encoder(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, const cui_draw_data *dd) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(dd->clear_color.r / 255.0, dd->clear_color.g / 255.0, dd->clear_color.b / 255.0, 1);
    return [cmd renderCommandEncoderWithDescriptor:pass];
}

/* A draw per command (cui_gl_render's counterpart); tw and th are the drawable's size. */
static void cui__mtl_draw(CUIMetalWindow *w, id<MTLRenderCommandEncoder> enc, CUIMetalSlot *slot, const cui_draw_data *dd, int tw, int th) {
    int fw = dd->width_px, fh = dd->height_px;
    float viewport[2] = { (float)fw, (float)fh };
    MTLViewport vp = { 0, 0, fw, fh, 0, 1 };
    [enc setViewport:vp];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:w.pso];
    [enc setVertexBuffer:slot.vertices offset:0 atIndex:0];
    [enc setVertexBytes:viewport length:sizeof viewport atIndex:1];
    [enc setFragmentSamplerState:w.sampler atIndex:0];
    void *bound = NULL;
    bool first = true;
    for (int i = 0; i < dd->command_count; i++) {
        const cui_draw_cmd *c = &dd->commands[i];
        if (!c->index_count) continue;
        int x0 = (int)floorf(c->clip.x), y0 = (int)floorf(c->clip.y);
        int x1 = (int)ceilf(c->clip.x + c->clip.w), y1 = (int)ceilf(c->clip.y + c->clip.h);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > tw) x1 = tw;
        if (y1 > th) y1 = th;
        if (x1 <= x0 || y1 <= y0) continue;
        MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
        [enc setScissorRect:sc];
        void *tex = (void *)(uintptr_t)(c->texture ? c->texture : dd->font_atlas);
        if (first || tex != bound) {
            id<MTLTexture> live = tex ? w.textures[@((uintptr_t)tex)] : nil;
            [enc setFragmentTexture:live ? live : w.none atIndex:0];
            bound = tex;
            first = false;
        }
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:c->index_count indexType:MTLIndexTypeUInt32
                       indexBuffer:slot.indices indexBufferOffset:(NSUInteger)c->index_offset * sizeof(uint32_t)];
    }
}

/* The frame copied out of the drawable into a buffer (nil if there is none to be had). */
static id<MTLBuffer> cui__mtl_copy_out(CUIMetalWindow *w, id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, int fw, int fh) {
    NSUInteger pitch = (NSUInteger)fw * 4;
    id<MTLBuffer> readback = [w.device newBufferWithLength:pitch * (NSUInteger)fh options:MTLResourceStorageModeShared];
    if (readback) {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        if (!blit) return nil;   /* (no copy: no capture) */
        [blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)fw, (NSUInteger)fh, 1)
                     toBuffer:readback destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch * (NSUInteger)fh];
        [blit endEncoding];
    }
    return readback;
}

/* The copy as RGBA, once the frame has run. */
static void cui__mtl_read_back(id<MTLBuffer> readback, int fw, int fh, void *rgba) {
    NSUInteger pitch = (NSUInteger)fw * 4;
    const uint8_t *src = readback.contents;
    uint8_t *dst = rgba;
    for (int y = 0; y < fh; y++, src += pitch)
        for (int x = 0; x < fw; x++, dst += 4) {
            dst[0] = src[x * 4 + 2]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4]; dst[3] = 255;
        }
}

static bool cui__mtl_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd) return false;
    dd = cui_expand_instances(&r->expanded, dd);
    if (!dd) return false;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        int fw = dd->width_px, fh = dd->height_px;
        if (fw <= 0 || fh <= 0) return false;
        cui__mtl_layer(r, w, fw, fh, rgba != NULL);
        CUIMetalSlot *slot = cui__mtl_slot(w);
        bool any = cui__mtl_upload(w, slot, dd);
        bool drawn = any || dd->index_count <= 0 || dd->vertex_count <= 0;   /* false: the vertices found no room */

        id<CAMetalDrawable> drawable = [w.layer nextDrawable];   /* waits for one to be free: the pacing */
        if (!drawable) return false;
        int tw = (int)drawable.texture.width, th = (int)drawable.texture.height;
        id<MTLCommandBuffer> cmd = [w.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = cui__mtl_encoder(cmd, drawable, dd);
        if (!enc) return false;   /* (no pass: nothing drawn, nothing to show or capture) */
        if (any) cui__mtl_draw(w, enc, slot, dd, tw, th);
        [enc endEncoding];

        id<MTLBuffer> readback = nil;
        if (rgba && tw == fw && th == fh) readback = cui__mtl_copy_out(w, cmd, drawable, fw, fh);
        if (w.layer.presentsWithTransaction) {
            /* the drawable must be scheduled on the GPU before it is presented in the transaction */
            [cmd commit];
            [cmd waitUntilScheduled];
            [drawable present];
        } else {
            [cmd presentDrawable:drawable];
            [cmd commit];
        }
        slot.reader = cmd;
        w.last = cmd;
        if (!readback) return !rgba;
        [cmd waitUntilCompleted];
        /* (a capture of a frame that was not drawn, or that the GPU did not finish, is none) */
        if (!drawn || cmd.status != MTLCommandBufferStatusCompleted) return false;
        cui__mtl_read_back(readback, fw, fh, rgba);
        return true;
    }
}

/* ---- damage: the frame kept as an image, redrawn only where it changed ---- */

/* The frame, or the part of it inside the boxes, drawn into the kept image. A pass cannot clear a
 * part of its target: each box is filled with the clear color by a quad, larger than it so that its
 * soft edge falls outside the scissor. */
static bool cui__mtl_draw_kept(cui_gpu_renderer *r, CUIMetalWindow *w, id<MTLCommandBuffer> cmd, const cui_draw_data *dd, const cui_rect *part, int parts) {
    const cui_draw_data *flat = cui_expand_instances(&r->expanded, dd);
    if (!flat) return false;
    CUIMetalSlot *slot = cui__mtl_slot(w);
    size_t vbytes = sizeof(cui_vertex) * (size_t)flat->vertex_count, ibytes = sizeof(uint32_t) * (size_t)flat->index_count;
    slot.vertices = cui__mtl_room(w.device, slot.vertices, vbytes + CUI_DAMAGE_BOXES * 4 * sizeof(cui_vertex));
    slot.indices = cui__mtl_room(w.device, slot.indices, ibytes + CUI_DAMAGE_BOXES * 6 * sizeof(uint32_t));
    if (!slot.vertices || !slot.indices) return false;
    if (vbytes) memcpy(slot.vertices.contents, flat->vertices, vbytes);
    if (ibytes) memcpy(slot.indices.contents, flat->indices, ibytes);
    int W = (int)w.kept.width, H = (int)w.kept.height;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = w.kept;
    pass.colorAttachments[0].loadAction = part ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(dd->clear_color.r / 255.0, dd->clear_color.g / 255.0, dd->clear_color.b / 255.0, 1);
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    if (!enc) return false;
    for (int i = 0; part && i < parts; i++) {
        const cui_rect *rect = &part[i];
        int x0 = (int)rect->x, y0 = (int)rect->y, x1 = (int)(rect->x + rect->w), y1 = (int)(rect->y + rect->h);
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > W ? W : x1; y1 = y1 > H ? H : y1;
        if (x1 > x0 && y1 > y0) {
            cui_instance in = { 0 };
            cui_vertex quad[4];
            in.x0 = (int16_t)((x0 - 4) * 4); in.y0 = (int16_t)((y0 - 4) * 4); in.x1 = (int16_t)((x1 + 4) * 4); in.y1 = (int16_t)((y1 + 4) * 4);
            in.mode = CUI_SHADE_RRECT;
            in.color_top = in.color_bottom = (cui_color){ dd->clear_color.r, dd->clear_color.g, dd->clear_color.b, 255 };
            cui_instance_vertices(&in, quad);
            uint32_t base = (uint32_t)flat->vertex_count + 4 * (uint32_t)i, six[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
            memcpy((uint8_t *)slot.vertices.contents + vbytes + (size_t)i * sizeof quad, quad, sizeof quad);
            memcpy((uint8_t *)slot.indices.contents + ibytes + (size_t)i * sizeof six, six, sizeof six);
            float viewport[2] = { (float)dd->width_px, (float)dd->height_px };
            MTLViewport vp = { 0, 0, dd->width_px, dd->height_px, 0, 1 };
            MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
            [enc setViewport:vp];
            [enc setCullMode:MTLCullModeNone];
            [enc setRenderPipelineState:w.pso];
            [enc setVertexBuffer:slot.vertices offset:0 atIndex:0];
            [enc setVertexBytes:viewport length:sizeof viewport atIndex:1];
            [enc setFragmentSamplerState:w.sampler atIndex:0];
            [enc setFragmentTexture:w.none atIndex:0];
            [enc setScissorRect:sc];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt32 indexBuffer:slot.indices indexBufferOffset:ibytes + (NSUInteger)i * 6 * sizeof(uint32_t)];
        }
    }
    if (flat->index_count > 0 && flat->vertex_count > 0) cui__mtl_draw(w, enc, slot, flat, W, H);
    [enc endEncoding];
    slot.reader = cmd;
    return true;
}

static bool cui__mtl_base(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d, const cui_rect *change);

bool cui_gpu_render_damaged(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d, bool present_anyway) {
    if (!r || !dd || !d) return false;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        int fw = dd->width_px, fh = dd->height_px;
        if (fw <= 0 || fh <= 0) return false;
        if (w.host) {   /* the list into a surface of the window's size below the others, when it changed */
            cui_rect rect;
            bool full = false;
            bool changed = cui_damage_track(d, dd, &rect, &full);
            if (!changed && !present_anyway) return false;
            bool whole = full || !changed || present_anyway;
            if (cui__mtl_base(r, dd, whole ? NULL : d, whole ? NULL : &rect)) return true;
            cui_damage_reset(d);   /* (not drawn: all of it the next time) */
            return false;
        }
        cui__mtl_layer(r, w, fw, fh, true);   /* (the drawable is copied into) */
        if (!w.kept || (int)w.kept.width != fw || (int)w.kept.height != fh) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)fw height:(NSUInteger)fh mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            w.kept = [w.device newTextureWithDescriptor:td];
            if (!w.kept) { cui__mtl_render(r, dd, NULL); return true; }
            cui_damage_reset(d);   /* the new image holds nothing yet */
        }
        cui_rect rect;
        bool full = false;
        bool changed = cui_damage_track(d, dd, &rect, &full);
        if (!changed && !present_anyway) return false;
        if (cui_damage_direct(d)) {   /* much of the window keeps changing: straight into it, with no copy */
            r->kept_stale = true;
            return cui__mtl_render(r, dd, NULL);
        }
        if (r->kept_stale) { changed = full = true; r->kept_stale = false; }   /* back to the kept image: all of it first */
        id<MTLCommandBuffer> cmd = [w.queue commandBuffer];
        if (changed) {
            cui_rect box[CUI_DAMAGE_BOXES];
            int boxes = 0;
            const cui_draw_data *cut = full ? NULL : cui_damage_clip_boxes(d, dd, box, &boxes);
            if (!cut) full = true;
            cui__mtl_draw_kept(r, w, cmd, cut ? cut : dd, cut ? box : NULL, boxes);
        }
        /* a drawable holds nothing of the frames before it, and the system takes no list of what changed: all of the image is copied in */
        id<CAMetalDrawable> drawable = [w.layer nextDrawable];
        id<MTLBlitCommandEncoder> blit = drawable ? [cmd blitCommandEncoder] : nil;
        if (blit) {
            NSUInteger cw = MIN(w.kept.width, drawable.texture.width), ch = MIN(w.kept.height, drawable.texture.height);
            [blit copyFromTexture:w.kept sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(cw, ch, 1)
                        toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cmd presentDrawable:drawable];
        }
        [cmd commit];
        w.last = cmd;
        return blit != nil;
    }
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) {
    if (r && dd && cui__mtl_win(r).host) cui__mtl_base(r, dd, NULL, NULL);
    else cui__mtl_render(r, dd, NULL);
}

/* (on a program's own device there is no picture of the whole window) */
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return r && rgba && !cui__mtl_win(r).host && cui__mtl_render(r, dd, rgba); }

/* ---- surfaces: a layer each, whose contents is an IOSurface of the surface's size ---- */

bool cui_gpu_surfaces(cui_gpu_renderer *r) { return r != NULL; }

static CALayer *cui__mtl_parent(CUIMetalWindow *w) { return w.host ? w.host : w.layer; }

/* Layer changes go to the system in a transaction with no implicit animations: on a program's
 * device one for the whole frame, opened by the first change and committed by cui_gpu_surfaces_end;
 * otherwise one for each change. */
static void cui__mtl_change(cui_gpu_renderer *r) {
    if (r->in_frame) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (cui__mtl_win(r).host) r->in_frame = true;
}

static void cui__mtl_changed(cui_gpu_renderer *r) {
    if (r->in_frame) return;
    [CATransaction commit];
}

static void cui__mtl_pic_free(cui__mtl_pic *p) {
    if (p->texture) CFRelease(p->texture);   /* (a frame in flight and the layer showing it keep their own references) */
    if (p->surface) CFRelease(p->surface);
    memset(p, 0, sizeof *p);
}

/* At most one spare picture goes a frame (cui_pool_reap); none while the window server holds it. */
static void cui__mtl_reap(cui_gpu_renderer *r) {
    for (int i = 0; i < CUI_POOL_SLOTS; i++)
        if (r->pool.spare_since[i] > 0 && r->pic[i].surface) cui_pool_held(&r->pool, i, IOSurfaceIsInUse(r->pic[i].surface));
    int gone = cui_pool_reap(&r->pool, CACurrentMediaTime(), __atomic_load_n(&r->completed, __ATOMIC_ACQUIRE), &r->reap_due);
    if (gone >= 0) cui__mtl_pic_free(&r->pic[gone]);
}

void cui_gpu_surfaces_end(cui_gpu_renderer *r) {
    if (!r) return;
    @autoreleasepool { cui__mtl_reap(r); }
    if (!r->in_frame) return;
    r->in_frame = false;
    [CATransaction commit];
}

static CGFloat cui__mtl_scale(CUIMetalWindow *w) {
    CGFloat scale = w.host ? (CGFloat)w.place->scale : w.window.backingScaleFactor;
    return scale > 0 ? scale : 1;
}

/* Where the zone's layer lies, in points of the layer above it. Inside the caller's transaction. */
static void cui__mtl_zone_place(CUIMetalWindow *w, CUIMetalZone *z) {
    CALayer *parent = cui__mtl_parent(w);
    CGFloat scale = cui__mtl_scale(w);
    bool top_left = w.host ? w.place->top_left : parent.geometryFlipped;
    CGRect frame = CGRectMake((CGFloat)z.x / scale, top_left ? (CGFloat)z.y / scale : parent.bounds.size.height - (CGFloat)(z.y + z.h) / scale,
                              (CGFloat)z.w / scale, (CGFloat)z.h / scale);
    CGFloat depth = (CGFloat)(2 + z.order);
    if (z.layer.contentsScale != scale) z.layer.contentsScale = scale;
    if (!CGRectEqualToRect(z.layer.frame, frame)) z.layer.frame = frame;
    if (z.layer.zPosition != depth) z.layer.zPosition = depth;
    if (z.layer.hidden) z.layer.hidden = NO;
}

static CUIMetalZone *cui__mtl_zone(cui_gpu_renderer *r, CUIMetalWindow *w, int slot) {
    CUIMetalZone *z = w.zones[@(slot)];
    if (z) return z;
    z = [[CUIMetalZone alloc] init];
    CALayer *layer = [CALayer layer];
    layer.opaque = NO;   /* clear where the surface draws nothing: what lies below shows through */
    layer.masksToBounds = YES;   /* a picture is as large as the surface or a step larger: its top left part is the surface */
    layer.anchorPoint = CGPointZero;
    layer.allowsEdgeAntialiasing = NO;
    layer.actions = @{ @"contents" : [NSNull null], @"bounds" : [NSNull null], @"position" : [NSNull null], @"hidden" : [NSNull null],
                       @"zPosition" : [NSNull null], @"contentsScale" : [NSNull null], @"backgroundColor" : [NSNull null], @"opaque" : [NSNull null],
                       @"onOrderIn" : [NSNull null], @"onOrderOut" : [NSNull null] };
    layer.hidden = YES;
    bool top_left = w.host ? w.place->top_left : cui__mtl_parent(w).geometryFlipped;
    layer.autoresizingMask = top_left ? kCALayerMaxYMargin : kCALayerMinYMargin;   /* (it keeps its distance from the top until it is placed again) */
    layer.contentsGravity = top_left ? kCAGravityBottomLeft : kCAGravityTopLeft;   /* (under a flipped layer its bottom is the top) */
    cui__mtl_change(r);
    [cui__mtl_parent(w) addSublayer:layer];
    cui__mtl_changed(r);
    z.layer = layer;
    z.slots = [NSMutableArray array];
    z.front = -1;
    w.zones[@(slot)] = z;
    return z;
}

/* A picture of fw by fh pixels: an IOSurface (BGRA, sRGB) and a texture over it. */
static bool cui__mtl_pic_make(CUIMetalWindow *w, cui__mtl_pic *p, int fw, int fh) {
    NSDictionary *props = @{ (id)kIOSurfaceWidth : @(fw), (id)kIOSurfaceHeight : @(fh), (id)kIOSurfaceBytesPerElement : @4,
                             (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA') };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surface) return false;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFPropertyListRef space = srgb ? CGColorSpaceCopyPropertyList(srgb) : NULL;
    if (space) { IOSurfaceSetValue(surface, kIOSurfaceColorSpace, space); CFRelease(space); }
    CGColorSpaceRelease(srgb);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)fw height:(NSUInteger)fh mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = w.device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [w.device newTextureWithDescriptor:td iosurface:surface plane:0];
    if (!texture) { CFRelease(surface); return false; }
    memset(p, 0, sizeof *p);
    p->surface = surface;
    p->texture = (__bridge_retained void *)texture;
    return true;
}

static cui_id cui__mtl_zone_id(CUIMetalZone *z, int slot) { return z.element ? z.element : 0xFFFFFF00u + (cui_id)(slot + 1); }

/* The pool slot to draw the zone's next frame into, or -1: none free (the frame is left out). While
 * the window is dragged a picture that fits is kept and one that does not gets a quarter more. */
static int cui__mtl_take(cui_gpu_renderer *r, CUIMetalWindow *w, CUIMetalZone *z, int slot, int fw, int fh) {
    cui_pool *pool = &r->pool;
    cui_id id = cui__mtl_zone_id(z, slot);
    int kw = fw, kh = fh;
    if (r->live_resize && z.key) {
        int cw = cui_pool_key_width(z.key), ch = cui_pool_key_height(z.key);
        kw = fw <= cw ? cw : MIN(fw + fw / 4, 8192);
        kh = fh <= ch ? ch : MIN(fh + fh / 4, 8192);
    }
    uint16_t key = cui_pool_key(kw, kh);
    double now = CACurrentMediaTime();
    if (z.key && z.key != key) {   /* another bucket: the old size's spares are first to go */
        cui_pool_give_back(pool, id, now - CUI_POOL_SPARE_SECONDS);
        z.front = -1;
    }
    z.key = key;
    for (int i = 0; i < CUI_POOL_SLOTS; i++)
        if (r->pic[i].surface && (pool->used >> i & 1)) cui_pool_held(pool, i, IOSurfaceIsInUse(r->pic[i].surface));
    bool make = false;
    int evict = -1, at = cui_pool_take(pool, id, kw, kh, z.front, now, &make, &evict);
    if (evict >= 0) cui__mtl_pic_free(&r->pic[evict]);
    if (at < 0) return -1;
    if (make && !cui__mtl_pic_make(w, &r->pic[at], cui_pool_key_width(key), cui_pool_key_height(key))) { cui_pool_drop(pool, at); return -1; }
    return at;
}

/* A zone's pictures become spare, as they are: the element that comes back finds them. */
static void cui__mtl_give_back(cui_gpu_renderer *r, CUIMetalZone *z, int slot) {
    if (z.key) cui_pool_give_back(&r->pool, cui__mtl_zone_id(z, slot), CACurrentMediaTime());
    z.key = 0;
    z.front = -1;
}

/* Vertex memory no frame still reads; one more when all are read, up to CUI__MTL_ZONE_SLOTS, and
 * none beyond: the frame is then left out, to be drawn again when the GPU has caught up. */
static CUIMetalSlot *cui__mtl_zone_slot(CUIMetalZone *z) {
    for (CUIMetalSlot *s in z.slots) {
        if (!s.reader || s.reader.status >= MTLCommandBufferStatusCompleted) { s.reader = nil; return s; }
    }
    if (z.slots.count >= CUI__MTL_ZONE_SLOTS) return nil;
    CUIMetalSlot *s = [[CUIMetalSlot alloc] init];
    [z.slots addObject:s];
    return s;
}

/* A list that is one square shape of one color over all of its fw by fh pixels. */
static bool cui__mtl_one_color(const cui_draw_data *dd, int fw, int fh, cui_color *color) {
    if (dd->command_count < 1 || dd->instance_count < 1 || !dd->commands[0].instance_count) return false;
    const cui_draw_cmd *c = &dd->commands[0];
    const cui_instance *in = &dd->instances[c->instance_offset];
    if (in->mode != CUI_SHADE_RRECT || in->r_tl || in->r_tr || in->r_br || in->r_bl || in->param || in->shear) return false;
    if (memcmp(&in->color_top, &in->color_bottom, sizeof in->color_top)) return false;
    if (in->x0 > 0 || in->y0 > 0 || in->x1 < fw * 4 || in->y1 < fh * 4) return false;
    if (c->clip.x > 0 || c->clip.y > 0 || c->clip.x + c->clip.w < (float)fw || c->clip.y + c->clip.h < (float)fh) return false;
    *color = in->color_top;
    return true;
}

static void cui__mtl_zone_release(cui_gpu_renderer *r, CUIMetalZone *z, int slot) {
    [z.slots removeAllObjects];
    cui__mtl_give_back(r, z, slot);
    z.last = nil;
}

/* A surface that is one color all over needs no picture: the layer's own background is it. */
static bool cui__mtl_zone_fill(cui_gpu_renderer *r, CUIMetalWindow *w, CUIMetalZone *z, int slot, int x, int y, int fw, int fh, int order, cui_color color) {
    z.x = x; z.y = y; z.w = fw; z.h = fh; z.order = order;
    CGColorRef fill = CGColorCreateSRGB(color.r / 255.0, color.g / 255.0, color.b / 255.0, color.a / 255.0);
    cui__mtl_change(r);
    z.layer.contents = nil;
    z.layer.backgroundColor = fill;
    z.layer.opaque = color.a == 255;
    cui__mtl_zone_place(w, z);
    cui__mtl_changed(r);
    CGColorRelease(fill);
    cui__mtl_zone_release(r, z, slot);
    z.solid = YES;
    z.shown = YES;
    return true;
}

/* The part of the picture inside x0..y1 (the picture's pixels) written back to nothing. */
static void cui__mtl_zone_wipe(CUIMetalWindow *w, id<MTLRenderCommandEncoder> enc, CUIMetalSlot *mem, size_t vbytes, size_t ibytes, uint32_t base, int x0, int y0, int x1, int y1, int tw, int th) {
    cui_instance in = { 0 };
    cui_vertex quad[4];
    in.x0 = (int16_t)((x0 - 4) * 4); in.y0 = (int16_t)((y0 - 4) * 4); in.x1 = (int16_t)((x1 + 4) * 4); in.y1 = (int16_t)((y1 + 4) * 4);
    in.mode = CUI_SHADE_SOLID;
    cui_instance_vertices(&in, quad);
    uint32_t six[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
    memcpy((uint8_t *)mem.vertices.contents + vbytes, quad, sizeof quad);
    memcpy((uint8_t *)mem.indices.contents + ibytes, six, sizeof six);
    float viewport[2] = { (float)tw, (float)th };
    MTLViewport vp = { 0, 0, tw, th, 0, 1 };
    MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
    [enc setViewport:vp];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:w.pso_clear];
    [enc setVertexBuffer:mem.vertices offset:0 atIndex:0];
    [enc setVertexBytes:viewport length:sizeof viewport atIndex:1];
    [enc setFragmentSamplerState:w.sampler atIndex:0];
    [enc setFragmentTexture:w.none atIndex:0];
    [enc setScissorRect:sc];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt32 indexBuffer:mem.indices indexBufferOffset:ibytes];
}

/* dd cut to rect into the renderer's own commands (a pool tracker has no room of its own); NULL: no memory. */
static const cui_draw_data *cui__mtl_cut(cui_gpu_renderer *r, const cui_draw_data *dd, cui_rect rect) {
    int need = cui__damage_clip_need(dd, 1);
    if (need > r->clip_cap) {
        cui_draw_cmd *grown = realloc(r->clip_cmds, sizeof *grown * (size_t)need * 2);
        if (!grown) return NULL;
        r->clip_cmds = grown;
        r->clip_cap = need * 2;
    }
    r->clip_data = *dd;
    r->clip_data.commands = r->clip_cmds;
    r->clip_data.command_count = cui__damage_clip_to(dd, &rect, 1, r->clip_cmds);
    return &r->clip_data;
}

/* Metal's thread: a surface frame is done on the GPU (frames on one queue end in order, mostly). */
static void cui__mtl_done(cui_gpu_renderer *r, uint64_t value) {
    uint64_t was = __atomic_load_n(&r->completed, __ATOMIC_ACQUIRE);
    while (was < value && !__atomic_compare_exchange_n(&r->completed, &was, value, true, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {}
}

/* dd drawn into the zone's next picture and shown at x, y. With d, only what changed since that
 * picture was drawn last is drawn again: change is what this frame changed, and the picture adds
 * the changes of the frames it sat out. The picture and the layer's place change together. */
static bool cui__mtl_zone_draw(cui_gpu_renderer *r, CUIMetalWindow *w, CUIMetalZone *z, int slot, const cui_draw_data *dd, int x, int y, int fw, int fh, int order,
                               cui_damage *d, const cui_rect *change) {
    z.x = x; z.y = y; z.w = fw; z.h = fh; z.order = order;
    CUIMetalSlot *mem = cui__mtl_zone_slot(z);
    if (!mem) { r->left_out = true; return false; }
    int at = cui__mtl_take(r, w, z, slot, fw, fh);
    if (at < 0) { r->left_out = true; return true; }   /* (all held, or room is made: drawn next frame) */
    cui__mtl_pic *p = &r->pic[at];
    if (p->size_w != fw || p->size_h != fh) cui_pool_cleared(&r->pool, at, false);
    p->size_w = fw; p->size_h = fh;
    const cui_pool_slot *kept = &r->pool.slot[at];

    /* what this frame changed, in the picture's pixels, and with it what the picture missed */
    int x0 = 0, y0 = 0, x1 = fw, y1 = fh;
    if (d && change) {
        x0 = MAX(0, (int)floorf(change->x)); y0 = MAX(0, (int)floorf(change->y));
        x1 = MIN(fw, (int)ceilf(change->x + change->w)); y1 = MIN(fh, (int)ceilf(change->y + change->h));
    }
    bool all = !d || !change || kept->missed_count < 0 || !(kept->flags & CUI_POOL_DRAWN);
    for (int i = 0; !all && i < kept->missed_count; i++) {
        const cui_rect *m = &kept->missed[i];
        x0 = MIN(x0, MAX(0, (int)floorf(m->x))); y0 = MIN(y0, MAX(0, (int)floorf(m->y)));
        x1 = MAX(x1, MIN(fw, (int)ceilf(m->x + m->w))); y1 = MAX(y1, MIN(fh, (int)ceilf(m->y + m->h)));
    }
    if (!all && (x1 <= x0 || y1 <= y0 || (float)(x1 - x0) * (float)(y1 - y0) >= 0.6f * (float)fw * (float)fh)) all = true;
    const cui_draw_data *cut = all ? dd : cui__mtl_cut(r, dd, (cui_rect){ (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0) });
    if (!cut) { all = true; cut = dd; }
    if (all) { x0 = 0; y0 = 0; x1 = fw; y1 = fh; }

    const cui_draw_data *flat = cui_expand_instances(&r->expanded, cut);
    if (!flat) { cui_pool_cleared(&r->pool, at, false); return false; }
    size_t vbytes = sizeof(cui_vertex) * (size_t)flat->vertex_count, ibytes = sizeof(uint32_t) * (size_t)flat->index_count;
    mem.vertices = cui__mtl_room(w.device, mem.vertices, vbytes + 4 * sizeof(cui_vertex));
    mem.indices = cui__mtl_room(w.device, mem.indices, ibytes + 6 * sizeof(uint32_t));
    if (!mem.vertices || !mem.indices) { cui_pool_cleared(&r->pool, at, false); return false; }
    if (vbytes) memcpy(mem.vertices.contents, flat->vertices, vbytes);
    if (ibytes) memcpy(mem.indices.contents, flat->indices, ibytes);

    id<MTLCommandBuffer> cmd = [w.queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)p->texture;
    pass.colorAttachments[0].loadAction = all ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    if (!enc) { cui_pool_cleared(&r->pool, at, false); return false; }
    if (!all) cui__mtl_zone_wipe(w, enc, mem, vbytes, ibytes, (uint32_t)flat->vertex_count, x0, y0, x1, y1, fw, fh);
    if (vbytes && ibytes) cui__mtl_draw(w, enc, mem, flat, fw, fh);
    [enc endEncoding];
    uint64_t value = ++r->submitted;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; cui__mtl_done(r, value); }];
    [cmd commit];
    [cmd waitUntilScheduled];   /* on the GPU's list before the window server is given the picture: it reads after the draw */
    mem.reader = cmd;
    z.last = cmd;
    w.last = cmd;
    cui_pool_drawn(&r->pool, at, value, d && change ? change : NULL, d && change ? 1 : -1);

    cui__mtl_change(r);
    z.layer.contents = (__bridge id)p->surface;
    if (z.solid) { z.layer.backgroundColor = NULL; z.layer.opaque = NO; }   /* (a fill may have made it opaque) */
    cui__mtl_zone_place(w, z);   /* (always: the layer above may have another height since) */
    cui__mtl_changed(r);
    z.front = at;
    z.solid = NO;
    z.shown = YES;
    return true;
}

/* A surface off the screen holds nothing: its pictures and its vertex memory go, and its layer. */
static void cui__mtl_zone_hide(cui_gpu_renderer *r, CUIMetalWindow *w, int slot) {
    CUIMetalZone *z = w.zones[@(slot)];
    if (!z) return;
    cui__mtl_give_back(r, z, slot);
    cui__mtl_change(r);
    z.layer.contents = nil;
    [z.layer removeFromSuperlayer];
    cui__mtl_changed(r);
    [w.zones removeObjectForKey:@(slot)];
}

static bool cui__mtl_surface(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, int x, int y, int order, cui_damage *d, const cui_rect *change) {
    if (!r || !dd || slot < 0 || dd->width_px <= 0 || dd->height_px <= 0) return false;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        CUIMetalZone *z = cui__mtl_zone(r, w, slot);
        cui_color color;
        if (dd->instance_count == 1 && dd->vertex_count == 0 && cui__mtl_one_color(dd, dd->width_px, dd->height_px, &color))
            return cui__mtl_zone_fill(r, w, z, slot, x, y, dd->width_px, dd->height_px, order, color);
        return cui__mtl_zone_draw(r, w, z, slot, dd, x, y, dd->width_px, dd->height_px, order, d, change);
    }
}

bool cui_gpu_surface_draw(cui_gpu_renderer *r, int slot, const cui_draw_data *dd, const cui_damage *d, int x, int y, int order) {
    cui_rect box[CUI_DAMAGE_BOXES], change = { 0 };
    int boxes = d ? cui_damage_boxes(d, box, CUI_DAMAGE_BOXES) : -1;
    if (boxes <= 0) return cui__mtl_surface(r, slot, dd, x, y, order, NULL, NULL);   /* (all of it, or a tracker that found nothing: drawn whole) */
    float x0 = box[0].x, y0 = box[0].y, x1 = box[0].x + box[0].w, y1 = box[0].y + box[0].h;
    for (int i = 1; i < boxes; i++) {
        x0 = fminf(x0, box[i].x); y0 = fminf(y0, box[i].y); x1 = fmaxf(x1, box[i].x + box[i].w); y1 = fmaxf(y1, box[i].y + box[i].h);
    }
    change = (cui_rect){ x0, y0, x1 - x0, y1 - y0 };
    return cui__mtl_surface(r, slot, dd, x, y, order, (cui_damage *)(uintptr_t)d, &change);   /* (cut with the tracker's own scratch memory) */
}

void cui_gpu_surface_place(cui_gpu_renderer *r, int slot, int x, int y, int order) {
    if (!r) return;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        CUIMetalZone *z = w.zones[@(slot)];
        if (!z || !z.shown) return;
        z.x = x; z.y = y; z.order = order;
        cui__mtl_change(r);
        cui__mtl_zone_place(w, z);
        cui__mtl_changed(r);
    }
}

void cui_gpu_surface_window(cui_gpu_renderer *r, int slot, void *wl_surface) { (void)r; (void)slot; (void)wl_surface; }   /* (Wayland only) */

void cui_gpu_surface_hide(cui_gpu_renderer *r, int slot) {
    if (!r) return;
    @autoreleasepool { cui__mtl_zone_hide(r, cui__mtl_win(r), slot); }
}

bool cui_gpu_left_out(cui_gpu_renderer *r) {
    bool was = r && r->left_out;
    if (r) r->left_out = false;
    return was;
}

cui_damage *cui_gpu_surface_damage(cui_gpu_renderer *r, int slot, uint32_t element) {
    if (!r || slot < 0 || !element) return NULL;
    @autoreleasepool {
        CUIMetalZone *z = cui__mtl_zone(r, cui__mtl_win(r), slot);
        if (z.element != element) {   /* another element's: spare until it is back */
            cui__mtl_give_back(r, z, slot);
            z.element = element;
        }
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

/* The window's own list on a program's device: a surface of the window's size, below every other. */
static bool cui__mtl_base(cui_gpu_renderer *r, const cui_draw_data *dd, cui_damage *d, const cui_rect *change) {
    CUIMetalWindow *w = cui__mtl_win(r);
    CUIMetalZone *z = cui__mtl_zone(r, w, CUI__MTL_BASE);
    return cui__mtl_zone_draw(r, w, z, CUI__MTL_BASE, dd, 0, 0, dd->width_px, dd->height_px, -1, d, change);
}

#endif
