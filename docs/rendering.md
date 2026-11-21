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
| `reserved` | uint8 | Zero |
| `color_top`, `color_bottom` | 2 × RGBA8 | The fill at the top and bottom edge (the same for a plain fill) |
| `border_color` | RGBA8 | |

The quad a record stands for is its shape grown on every side by a margin the mode decides: 1 pixel for RRECT and IMAGE, three times the blur sigma for SHADOW, none for TEXT. Its four corners feed the fragment shader exactly what four vertices would: `cui_instance_vertices` writes them out, and is the definition.

A renderer has two ways to take records:

- **Draw them as instances.** One four-vertex strip per record, with the corner from the vertex index. Vulkan reads a record as per-instance vertex input and picks a command's first record with the draw's first instance. OpenGL 3.3, OpenGL ES 3.0 and WebGL 2 have no first instance, so the OpenGL renderer uploads the records as a texture of 32-bit words, sets one integer uniform to the command's first record, and reads record `first + gl_InstanceID` with `texelFetch`.
- **Write them back out as vertices.** `cui_expand_instances` returns draw data with every record as four vertices and six indices, in the same order, for a renderer that draws vertices only. The software, Direct3D 12 and Metal renderers do this.

## Damage

With `cui_app_desc.damage_tracking` (OpenGL only), a frame draws just what changed. It needs the records above, and turns them on.

`cui_damage_track` compares a frame's records with the last frame's: from the front while they match, then from the back while they match. What is left in the middle of either frame is what changed, and the bounding box of those shapes is the damage. A shape that appears (a hover highlight) shifts every record after it without damaging any of them. Two changes far apart in the draw order damage what lies between them as well, so the rectangle is never too small, only sometimes larger than it could be. Vertices are not compared shape by shape: when they change at all, everything their commands can draw in is damaged. A texture whose pixels change under a record that does not change is not seen.

`cui_gl_render_damaged` keeps an image of the window, clears and redraws only the damaged rectangle in it (`cui_damage_clip` leaves the commands and records that touch it), and copies the image to the window. It returns false when nothing changed, and then there is nothing to swap: `cui_app` paces such frames by the clock. A rectangle over 60% of the window is drawn as the whole window.

`CUI_DAMAGE_CHECK=1` draws every partly drawn frame whole as well and reports any pixel that differs; `CUI_DAMAGE_STATS=1` prints how many frames were unchanged, partial and full.

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
