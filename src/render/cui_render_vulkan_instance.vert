#version 450
/* cui_render_vulkan.c's vertex shader for cui_instance records: one corner of the quad a record
 * stands for. A quad is one instance of a four vertex strip, and a draw's first instance picks
 * its first record. The outputs are cui_render_vulkan.vert's.
 * tools/compile_vk_shaders.py compiles it into generated/cui_render_vulkan_spirv.h. */
layout(location = 0) in ivec4 i_rect;      /* shape bounds, quarter pixels */
layout(location = 1) in vec4 i_uv;         /* texture rectangle, 0..1 */
layout(location = 2) in uvec4 i_radii;     /* quarter pixels */
layout(location = 3) in uint i_param;      /* border width or blur sigma, quarter pixels */
layout(location = 4) in uvec2 i_mode;      /* x: CUI_SHADE_*, y: shear in 128ths, a signed byte */
layout(location = 5) in vec4 i_top;
layout(location = 6) in vec4 i_bottom;
layout(location = 7) in vec4 i_bcolor;
layout(push_constant) uniform Globals { vec2 u_viewport; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_local;
layout(location = 2) flat out vec2 v_half;
layout(location = 3) flat out vec4 v_radii;
layout(location = 4) flat out vec2 v_params;
layout(location = 5) out vec4 v_color;
layout(location = 6) flat out vec4 v_bcolor;
void main() {
  vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  vec2 lo = vec2(i_rect.xy) * 0.25, hi = vec2(i_rect.zw) * 0.25;
  vec2 center = (lo + hi) * 0.5, half_size = (hi - lo) * 0.5;
  float param = float(i_param) * 0.25;
  int mode = int(i_mode.x);
  float pad = mode == 2 ? param * 3.0 : ((mode == 1 || mode == 3) ? 1.0 : 0.0);
  vec2 pos = mix(lo - pad, hi + pad, corner);
  v_local = pos - center;
  int lean = int(i_mode.y);
  pos.x += float(lean > 127 ? lean - 256 : lean) / 128.0 * (hi.y - pos.y);
  vec4 radii = vec4(i_radii) * 0.25;
  if (mode == 6) {   /* a stroke from lo to hi: a box along it, shaded as a rounded rectangle */
    vec2 along = hi - lo;
    float len = length(along);
    along = len > 0.0 ? along / len : vec2(1.0, 0.0);
    half_size = vec2(len * 0.5 + radii.x, max(param * 0.5, 0.5));
    v_local = (corner * 2.0 - 1.0) * (half_size + 1.0);
    pos = center + along * v_local.x + vec2(-along.y, along.x) * v_local.y;
    radii = vec4(radii.x);
    mode = 1; param = 0.0;
  }
  v_half = half_size;
  v_uv = i_uv.xy + (v_local + half_size) / (2.0 * half_size) * (i_uv.zw - i_uv.xy);
  v_radii = radii;
  v_params = vec2(param, float(mode));
  v_color = mix(i_top, i_bottom, corner.y);
  v_bcolor = i_bcolor;
  gl_Position = vec4(pos.x / u_viewport.x * 2.0 - 1.0, pos.y / u_viewport.y * 2.0 - 1.0, 0.0, 1.0);
}
