# Rendering

CUI turns every shape into vertices of a single format and a few draw commands. A renderer only needs one shader.

## Draw data

```c
typedef struct cui_draw_data {
    const cui_vertex *vertices;  int vertex_count;
    const uint32_t *indices;     int index_count;     /* triangles */
    const cui_draw_cmd *commands; int command_count;
    int width_px, height_px;                         /* framebuffer size */
    cui_color clear_color;                           /* the theme background */
    cui_texture font_atlas;
} cui_draw_data;

typedef struct cui_draw_cmd {
    cui_rect clip;            /* scissor in pixels, top left origin */
    cui_texture texture;      /* 0 = font atlas */
    uint32_t index_offset, index_count;
} cui_draw_cmd;
```

Draw commands in order. Each command changes at most the scissor and the texture, so a typical frame is a handful of draw calls.

## Vertex format (64 bytes)

| Field | Type | Meaning |
|---|---|---|
| `x, y` | float | Position in pixels |
| `u, v` | float | Texture coordinate |
| `lx, ly` | float | Position relative to the shape's center, pixels |
| `hw, hh` | float | Half width and half height of the shape |
| `r_tl, r_tr, r_br, r_bl` | float | Corner radii |
| `border` | float | Border width (rounded rectangles) or blur sigma (shadows) |
| `mode` | float | Shading mode, see below |
| `color` | 4 × u8 | Fill color, straight alpha, interpolated |
| `border_color` | 4 × u8 | Border color, constant per shape |

All fields except `x, y, u, v, lx, ly, color` are the same for every vertex of a shape and can be `flat` in shading languages.

| Mode | Value | Shading |
|---|---|---|
| `CUI_SHADE_TEXT` | 0 | `color` × atlas red channel (coverage) |
| `CUI_SHADE_RRECT` | 1 | Signed distance rounded rectangle with optional border |
| `CUI_SHADE_SHADOW` | 2 | Gaussian blurred rounded rectangle |
| `CUI_SHADE_IMAGE` | 3 | RGBA texture × `color`, masked by the rounded rectangle |
| `CUI_SHADE_SOLID` | 4 | `color` (anti-aliasing is in the geometry) |
| `CUI_SHADE_DITHER` | 5 | Ordered dither: `border_color` where `border` ≤ the 8×8 Bayer threshold of cell `floor(u, v)` < `color.a`, masked by the rounded rectangle |

Dither vertices carry the cell grid in `u, v` (in cells, interpolated), the density in `color.a` (interpolated) and a floor in `border`. Each cell is either the dot color or nothing, so the dither components (the dither progress bar, the pattern meter, dither area fills and the veil) look the same in every renderer. A renderer that skips mode 5 still draws everything else.

## Instances (40 bytes)

With `cui_ctx_desc.instanced_quads` set, an axis-aligned shape (a rounded rectangle, a shadow, an image, a glyph) is one `cui_instance` in `cui_draw_data.instances` instead of four vertices. Lines, polygons, dither meshes and anything that does not fit a record stay vertices, in the same draw data. A draw command is one kind or the other: `index_count` or `instance_count`, never both, and the commands are still in draw order.

| Field | Type | Meaning |
|---|---|---|
| `x0, y0, x1, y1` | 4 × int16 | The shape's bounds in quarter pixels |
| `u0, v0, u1, v1` | 4 × uint16 | Its texture rectangle, 0..65535 across the texture |
| `r_tl, r_tr, r_br, r_bl` | 4 × uint16 | Corner radii in quarter pixels |
| `param` | uint16 | Border width (RRECT) or blur sigma (SHADOW), quarter pixels |
| `mode` | uint8 | `CUI_SHADE_TEXT`, `RRECT`, `SHADOW` or `IMAGE` |
| `shear` | int8 | Slant in 128ths: the quad leans right by `shear / 128` of its height above the bottom edge. Zero is upright |
| `color_top`, `color_bottom` | 2 × RGBA8 | The fill at the top and bottom edge (the same for a plain fill) |
| `border_color` | RGBA8 | |

The quad a record stands for is its shape grown on every side by a margin the mode decides: 1 pixel for RRECT and IMAGE, three times the blur sigma for SHADOW, none for TEXT. Its four corners feed the fragment shader exactly what four vertices would: `cui_instance_vertices` writes them out, and is the definition.

A renderer has two ways to take records:

- **Draw them as instances.** One four-vertex strip per record, with the corner from the vertex index. Vulkan reads a record as per-instance vertex input and picks a command's first record with the draw's first instance. OpenGL 3.3, OpenGL ES 3.0 and WebGL 2 have no first instance, so the OpenGL renderer uploads the records as a texture of 32-bit words, sets one integer uniform to the command's first record, and reads record `first + gl_InstanceID` with `texelFetch`.
- **Write them back out as vertices.** `cui_expand_instances` returns draw data with every record as four vertices and six indices, in the same order, for a renderer that draws vertices only. The software, Direct3D 12 and Metal renderers do this.

## Damage

With `cui_app_desc.damage_tracking` (OpenGL only), a frame draws just what changed. It needs the records above, and turns them on.

`cui_damage_track` compares a frame's records with the last frame's: from the front while they match, then from the back while they match. What is left in the middle of either frame is what changed, and the bounding box of those shapes is the damage. A shape that appears (a hover highlight) shifts every record after it without damaging any of them. Two changes far apart in the draw order damage what lies between them as well, so the rectangle is never too small, only sometimes larger than it could be. Vertices are not compared shape by shape: when they change at all, everything their commands can draw in is damaged. A texture whose pixels change under a record that does not change is not seen.

`cui_gl_render_damaged` keeps an image of the window, clears and redraws only what changed in it, and copies all of the image to the window, which `cui_app` then swaps. It returns false when nothing changed, and then there is nothing to swap: `cui_app` paces such frames by the clock.

What changed is redrawn as one box: `cui_damage_boxes` gives the last frame's change as one box, the rectangle round all of it (`CUI_DAMAGE_BOXES` is 1), and `cui_damage_clip_boxes` gives that box and the draw data cut to it, the commands and records that touch it with their clips cut to the box. The renderer clears the box and draws the data, whose commands carry the box as their scissor. A box over 60% of the window is drawn as the whole window. `cui_damage_clip` cuts draw data to any one rectangle.

A kept picture costs a copy of what was drawn into the window, so it loses where there is little to keep: while frame after frame changes much of the window (scrolling, a resize, a picture that fills it), drawing the frame and copying it costs about twice what drawing it straight into the window does. After three frames in a row that each change 40% of the window or more, the renderers therefore draw straight into the window and let the kept picture go stale (`cui_damage_direct`). The first frame that changes less than 25% is drawn whole into the kept picture again, and those after it in part.

### Vulkan, Direct3D 12 and Metal

`cui_gpu_render_damaged` is the same thing for the system's own renderer, and `cui_app` uses it when damage tracking is on. Each keeps the frame as an image, draws into it only the boxes that changed, and returns false when nothing did. Surfaces reach these renderers copied into the window's list.

| Renderer | Into the window |
|---|---|
| Direct3D 12 | The changed boxes, and those of the frames its back buffer missed (the swap chain is `FLIP_SEQUENTIAL`, so back buffers keep their pictures). |
| Direct3D 12, a surface (`cui_gpu_surface_draw`) | The changed boxes, and those of the frame before (a surface's swap chain is `FLIP_SEQUENTIAL` with two buffers: the back buffer missed exactly one frame), drawn straight into the back buffer with no kept picture; all of it for a surface that was hidden or resized. |
| Vulkan | The changed boxes, and those each swapchain image missed since it was last shown. Surfaces on a program's device are drawn straight into their swapchain images, with no kept image. |
| Metal | All of the image: a drawable holds nothing of earlier frames. |
| Metal, surfaces | Into the picture that is off the screen: the changed rectangle, joined with what that picture missed while the other was shown (`cui_gpu_surface_draw`). A layer whose contents changes is composited whole, so a surface is kept as small as what changes together. |

## A device of the program's own (Vulkan)

A program that already draws with Vulkan hands the renderer its device with `cui_gpu_create_shared(window, vsync, &device)` in place of `cui_gpu_create`. `cui_gpu_device` names the instance, the physical device, the device, a queue and its family. The instance needs `VK_KHR_surface` and the window system's surface extension; the device needs Vulkan 1.3 with `dynamicRendering` and `timelineSemaphore`, and `VK_KHR_swapchain`. The renderer submits and presents on that queue from the thread that calls it, so that thread is the only one to use the queue. `cui_gpu_destroy` leaves the device and the instance alone.

A picture the program drew on that device becomes a texture with `cui_gpu_texture_wrap(r, image_view)`: draw it with `cui_draw_image`. The program keeps the image in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` whenever a frame that draws it is rendered. To let go of it, call `cui_gpu_texture_unwrap`, then `cui_gpu_flush` (or wait until the frames already rendered have been shown) before destroying the view.

With `cui_x11_window.translucent` the window's picture has an alpha: a frame whose clear color has no alpha leaves what lies below the surface visible wherever it draws nothing. A Wayland subsurface over another window's content is drawn this way.

A program that has its own Wayland window sets `cui_x11_window.host` and gives `wl_surface` a surface of its own (a subsurface over its window's content, say) and `scale` the pixels a point. The renderer then behaves as it does with a `cui_metal_host` or a `cui_d3d12_host`, through the same calls:

- No swapchain of the window's size is made. Each [surface](#surfaces) is a subsurface the renderer makes under the program's surface, synchronized, with an empty input region, a `wp_viewport` and a swapchain of the surface's own pixel size. The window's own list (`cui_gpu_render`, `cui_gpu_render_damaged`) goes to one more, of the window's size, below the others.
- `cui_gpu_surface_draw` redraws this frame's boxes and those the swapchain image that came up missed since it was last drawn (each image keeps up to 24 such boxes, then is drawn whole). The swapchain's images are the only pictures: there is no kept image and nothing is copied.
- `cui_gpu_surface_place` moves or reorders a subsurface with nothing drawn, and `cui_gpu_surface_hide` takes it off the screen (no buffer) and lets its swapchain go. A surface is submitted and presented when it is drawn, and `cui_gpu_surfaces_end` then commits the program's surface once when any subsurface was drawn, placed or reordered, so a subsurface's place, size and picture change together.
- A swapchain is made when a surface first draws and again when its size changes, with the fewest images the surface allows and the FIFO present mode. The one it replaces is destroyed when the last submission that drew into it is done, after its successor was presented; the renderer waits for neither the device nor the queue. An image the compositor has not given back within 2 ms leaves the surface's frame out (`cui_gpu_left_out`), to be drawn with the next.
- A subsurface is placed and sized in whole points. A box of pixels that does not fall on whole points (a fractional scale) is grown until it does, so a picture is always shown one pixel to one.
- `cui_presenter_show(p, r, ui, force)` shows a finished frame in one call: the window's own list, each surface drawn, moved or hidden as its list and place changed, and `cui_gpu_surfaces_end`. The presenter (`cui_presenter_create`) holds the damage trackers. `cui_presenter_begin(p, ui, w, h, scale, now)` begins a frame only when one is due and `cui_presenter_end` ends and shows it, so a program's loop is those two calls.

What Wayland needs that the other two do not: the program's surface must be mapped (a subsurface shows nothing under a parent with no buffer), so the program gives it a buffer of its own, one clear pixel will do; and a program that makes its subsurfaces itself, as `cui_app` does, still hands each to the renderer with `cui_gpu_surface_window` and places it itself.

### Metal

`cui_gpu_create_shared(&host, vsync, &device)` takes a `cui_metal_host` in place of a window, and in `cui_gpu_device` an `id<MTLDevice>` as `device` and an `id<MTLCommandQueue>` of it as `queue` (the other fields are not read). A program that draws with Vulkan on MoltenVK exports its queue with `vkExportMetalObjectsEXT` and hands that over: the UI is then drawn on the device its Vulkan images live on. The renderer commits to the queue from the thread that calls it.

`cui_metal_host` names a `CALayer` of the program's (a view's layer, as a rule), the scale in pixels a point, and whether that layer's coordinates start at its top left. The renderer makes no layer of the window's size. Each [surface](#surfaces) is a layer under the host's. The window's own list (`cui_gpu_render`, `cui_gpu_render_damaged`) is drawn into one more layer, of the window's size, below the surfaces.

`cui_gpu_texture_wrap(r, texture)` takes an `id<MTLTexture>` of the device, which the renderer retains until `cui_gpu_texture_unwrap`. There is no picture of the whole window to read back: `cui_gpu_render_capture` returns false.

### Direct3D 12

`cui_gpu_create_shared(&host, vsync, &device)` takes a `cui_d3d12_host` in place of a window, and in `cui_gpu_device` an `ID3D12Device` as `device` and a direct `ID3D12CommandQueue` of it as `queue` (the other fields are not read). The renderer executes its command lists on that queue from the thread that calls it and signals a fence of its own there; it waits for nothing the program put on the queue.

`cui_d3d12_host` names the program's `IDCompositionDevice` and an `IDCompositionVisual` of its tree. The renderer makes no swap chain of the window's size and never sees the window. Each [surface](#surfaces) is a visual under the host's, with a flip-model composition swap chain (`CreateSwapChainForComposition`, premultiplied alpha) of the surface's own size, made when the surface first appears and again only when its pixel size changes; the old one is released once its last frame has run. The window's own list (`cui_gpu_render`, `cui_gpu_render_damaged`) is drawn into one more visual, of the window's size, below the surfaces. Placing, reordering, showing or hiding a visual is a property change and one `Commit`, with no draw.

A surface drawn with `cui_gpu_surface_draw(r, slot, list, damage, x, y, order)`, where `damage` is the `cui_damage` the program tracked that surface's list with, is drawn again only inside the boxes that changed and those of the frame before, which the other of its two back buffers lacks. There is no kept picture: the swap chain's two buffers are all a surface holds. A surface's swap chain has a premultiplied alpha. Its size is a multiple of 64 pixels each way and it is kept while the surface fits it and fills at least half of it or is within two 64 pixel steps of its size (the visual is cut to the surface), so an animated size makes few swap chains. A hidden surface releases its swap chain. Call `cui_gpu_surfaces_end(r)` after a frame's surfaces.

`cui_gpu_texture_wrap(r, resource)` takes an `ID3D12Resource` of the device, which the renderer retains until `cui_gpu_texture_unwrap`. As with Metal, `cui_gpu_render_capture` returns false.

## Surfaces

A surface is a zone of the window, a rectangle, with a draw list of its own. Set `surface` on a box and the box and all in it are one, the size of the box. In a paint callback, `cui_surface_begin(ui, id, rect)` and `cui_surface_end` do the same for any rectangle in points. What is drawn inside is cut to the zone and lies over what the window draws there, whatever the order of the calls; a surface begun later lies over one begun earlier.

The UI only says where the zones are. What becomes of the lists is the renderer's choice: each into a picture of its own, or all into the window's one picture, one after another. There is no list of the whole frame beside them, and there are no layers the size of the window laid over one another: a UI that zones nothing has one list for one picture.

Surfaces exist so that one part of a window can change without the rest being drawn again. A tab bar, a row with a status light, a spinner or a moving picture that has a picture of its own is redrawn and shown by itself, at its own size: what it costs is what it holds. Each has its own damage tracker, so shapes that come and go inside one cannot disturb the comparison of the rest of the window, and any number of surfaces can change in a frame. One that moves and is otherwise the same is not drawn at all: the system puts its picture somewhere else. The window can be redrawn under a surface that is left as it is, and a surface over a window that is.

After `cui_end_frame`, `cui_get_draw_data` is the window's list, and `cui_get_surface(ui, slot, &s)` gives each surface (`CUI_SURFACES` slots; one keeps its slot while it is drawn every frame): where it is in pixels, its order, and its shapes from its own top left corner as draw data of its own size, whose clear color has no alpha.

How the lists reach the screen:

| | |
| --- | --- |
| Wayland, OpenGL | Each surface is a subsurface over the window, of its own size, with pictures of its own. One that changed is drawn whole into it and shown; one that moved is placed again. The window's list is drawn into the window alone. |
| Other OpenGL windows | All into the window: its list, then each surface's at its place (`cui_gl_render_at`). With damage tracking each list has its own tracker and `cui_gl_render_lists` redraws the boxes that changed, each from every list that reaches into it, in order; a surface that moved is two boxes, where it was and where it is. |
| macOS, Metal | Each surface is a plain `CALayer` whose `contents` is an IOSurface of the surface's size, drawn into through a texture made over it (`cui_gpu_surface_draw`): no `CAMetalLayer`, no drawables, no copy. One that changed is drawn whole into the picture that is not on the screen and the two are swapped; one that moved has its layer moved, with nothing drawn. The pictures come from the picture pool (below), held while `IOSurfaceIsInUse`: a surface never draws into one the window server still reads, and leaves the frame out when it would have to. With a damage tracker only the changed rectangle is drawn into the picture, and what that picture missed. A surface that is one color all over is the layer's `backgroundColor` with no picture; one that leaves the frame gives its pictures back to the pool and removes its layer. The new contents and the layer's place are set in one transaction, with implicit animations off, after the commands are scheduled on the GPU. The window's list is drawn into the window's layer alone, or with a program's own device into a layer of the window's size below the surfaces. |
| Windows, Direct3D 12 | Each surface is a flip-model composition swap chain from the picture pool (below), one per surface, shown by a DirectComposition visual over the window and cut to the surface's size. One that changed is drawn again in its changed boxes and those its other back buffer missed, and presented; one that moved has its visual moved, with nothing drawn. Every swap chain has alpha. A hidden surface's swap chain goes back to the pool with its pictures, so the same element shown again carries on. |
| Wayland, Vulkan on a program's device | Each surface is a subsurface the renderer makes under the program's surface, with a swapchain of its own size. One that changed is redrawn in its changed boxes and presented; one that moved is placed again, with nothing drawn. The window's own list goes to one more, of the window's size. See [A device of the program's own](#a-device-of-the-programs-own-vulkan). |
| Vulkan, software | All into the window as well, for now from a copy of the lists made into one. |

### The picture pool

On Wayland the Vulkan renderer draws each surface into images of its own that the compositor imports as `wl_buffer`s (where the compositor cannot import them, each surface has a swapchain instead); Metal draws into IOSurfaces a layer shows; Direct3D 12 into composition swap chains, one slot per surface, since DXGI turns the back buffers inside one. Which picture a surface draws into is decided by one pool, `src/render/cui_pool.c`, that every surface of the renderer shares. The pool is bookkeeping only: fixed tables addressed by slot, no memory of its own and no calls into a graphics API. It answers with a slot and whether to make a picture there, and the renderer keeps its images in an array at the same index.

- **Size buckets.** A picture is found by its size rounded up to 64 pixels each way. On a subsurface the renderer made (`cui_x11_window.host`) the picture has the bucket's size and the viewport shows the surface's part of it. On a subsurface the program made, the picture has the surface's exact size, because that surface shows all of what is attached; one of another size in the same bucket is made again in its slot.
- **Taking one.** A surface asks for a picture each frame it draws. It gets, in this order: one of its own that the compositor has released and that is not the one on screen; a spare one that was last drawn for the same element, with its pixels; any other spare of the bucket, which is then drawn whole; a new one. A surface holds three at most. When the compositor holds them all, the frame is left out and drawn with the next (`cui_gpu_left_out`): a picture the compositor still reads is never drawn into. When the table of 64 pictures is full, the oldest spare is freed and the frame is left out; the new picture is made on the next frame, never on the one that freed.
- **Sizes.** A picture is its bucket's size, 64 pixels a step each way, and the surface's viewport crops it: on a renderer owned subsurface the renderer sets the crop, on one the program made (`cui_app`) the program does. While the window is dragged to a new size (`cui_gpu_live_resize`, from the `xdg_toplevel` resizing state in `cui_app`, which also ends a drag after 0.25 seconds without a configure: GNOME 50 sends none when the button is let go), a surface keeps its picture while it still fits, and a side that outgrows it gets a quarter more than it needs, rounded up to 64 pixels; when the drag ends every surface is drawn once more into its own bucket.
- **Giving back.** When a surface hides or its size leaves the bucket, its pictures become spare. Nothing is cleared: the pixels, the boxes each picture missed and the element's damage tracker all stay. An element that comes back within 5 seconds finds its picture as it left it and redraws only what changed since. A picture left behind by a size change counts as 5 seconds spare already, so it is the first to go.
- **The tracker lives with the pictures.** `cui_gpu_surface_damage(r, slot, id)` tells the renderer which element (`cui_surface.id`) a slot shows and returns the tracker the pool keeps for it; `cui_presenter` uses that one and does not reset it on hide. It returns NULL where there is no pool, and the caller then keeps a tracker of its own as before. A pool tracker holds the last frame in a fixed array of 1024 records and 128 commands; a surface with more has no history and is drawn whole every time it changes. The tracker goes when the last picture drawn for the element is reused or freed.
- **Freeing.** Each frame (`cui_gpu_surfaces_end`) frees at most one spare picture, and none on a frame that made one. With 4 spares or fewer, the oldest goes once it has been spare 5 seconds. With more than 4, one goes every 10th frame, or after 10 frames' time (1/6 s) when the program draws nothing. With more than 8, one goes every frame. A picture the compositor still holds or the GPU still draws into waits. `cui_gpu_next_wakeup` says in how many seconds the next one is due, and `cui_presenter` passes that to `cui_request_wakeup`, so an idle program wakes once for each picture it frees, with no timer.

## Reference shader (GLSL 330)

```glsl
// vertex
in vec2 a_pos, a_uv, a_local, a_half; in vec4 a_radii; in vec2 a_params;   // params = (border, mode)
in vec4 a_color, a_bcolor;                                                  // normalized u8
uniform vec2 u_viewport;                                                    // framebuffer size in pixels
out vec2 v_uv, v_local; flat out vec2 v_half; flat out vec4 v_radii; flat out vec2 v_params;
out vec4 v_color; flat out vec4 v_bcolor;
void main() {
    v_uv = a_uv; v_local = a_local; v_half = a_half; v_radii = a_radii; v_params = a_params;
    v_color = a_color; v_bcolor = a_bcolor;
    gl_Position = vec4(a_pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - a_pos.y / u_viewport.y * 2.0, 0.0, 1.0);
}

// fragment
uniform sampler2D u_tex;
float sd_rrect(vec2 p, vec2 b, vec4 r) {           // r = (tl, tr, br, bl), y down
    float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);
    rr = min(rr, min(b.x, b.y));
    vec2 q = abs(p) - b + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;
}
float erf_(float x) {                              // Abramowitz and Stegun 7.1.26
    float s = sign(x); x = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * x);
    return s * (1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x));
}
float bayer8(vec2 p) {                             // 8x8 Bayer threshold of cell p, from the bits of x ^ y and y
    ivec2 q = ivec2(mod(floor(p), 8.0));
    int a = q.x ^ q.y, y = q.y;
    int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);
    return (float(m) + 0.5) / 64.0;
}
void main() {
    int mode = int(v_params.y + 0.5);
    vec4 c = v_color;
    if (mode == 4) { frag = vec4(c.rgb * c.a, c.a); return; }
    if (mode == 0) { float a = c.a * texture(u_tex, v_uv).r; frag = vec4(c.rgb * a, a); return; }
    float d = sd_rrect(v_local, v_half, v_radii);
    if (mode == 1) {
        float outer = clamp(0.5 - d, 0.0, 1.0);
        float inner = v_params.x > 0.0 ? clamp(0.5 - (d + v_params.x), 0.0, 1.0) : outer;
        float fa = c.a * inner, ba = v_bcolor.a * (outer - inner);
        frag = vec4(c.rgb * fa + v_bcolor.rgb * ba, fa + ba);
    } else if (mode == 2) {
        float a = c.a * 0.5 * (1.0 - erf_(d / (max(v_params.x, 0.01) * 1.41421356)));
        frag = vec4(c.rgb * a, a);
    } else if (mode == 5) {
        float t = bayer8(v_uv);
        float k = t >= v_params.x && t < c.a ? v_bcolor.a * clamp(0.5 - d, 0.0, 1.0) : 0.0;
        frag = vec4(v_bcolor.rgb * k, k);
    } else {
        float k = c.a * clamp(0.5 - d, 0.0, 1.0);
        vec4 t = texture(u_tex, v_uv);
        frag = vec4(t.rgb * t.a * c.rgb * k, t.a * k);
    }
}
```

The output is premultiplied. Blend with `ONE, ONE_MINUS_SRC_ALPHA`. Sample textures with linear filtering and clamp to edge. The software renderer in `src/render/cui_render_sw.c` implements the same math in C and is a good second reference.
