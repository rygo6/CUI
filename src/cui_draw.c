/* Draw list: every shape becomes vertices of one format. Rounded rectangles
 * and shadows are signed distance fields evaluated per pixel, so corners
 * stay smooth at any size and DPI. Paths use feathered geometry. */
#include "cui_internal.h"
#include "cui/cui_render.h"

static cui_draw_cmd *cui__cmd_of(cui_ctx *ui, bool instanced) {
    cui_draw_list *dl = &ui->dl;
    cui_rect clip = dl->clip_stack[dl->clip_depth - 1];
    if (dl->cmd_count > 0) {
        cui_draw_cmd *c = &dl->cmd[dl->cmd_count - 1];
        bool same_kind = instanced ? c->index_count == 0 : c->instance_count == 0;
        if (same_kind && c->texture == dl->cur_tex && memcmp(&c->clip, &clip, sizeof clip) == 0) return c;
        if (c->index_count == 0 && c->instance_count == 0) { c->texture = dl->cur_tex; c->clip = clip; return c; }
    }
    CUI__GROW(ui, dl->cmd, dl->cmd_count, dl->cmd_cap, 1);
    cui_draw_cmd *c = &dl->cmd[dl->cmd_count++];
    c->clip = clip;
    c->texture = dl->cur_tex;
    c->index_offset = (uint32_t)dl->idx_count;
    c->index_count = 0;
    c->instance_offset = (uint32_t)dl->inst_count;
    c->instance_count = 0;
    return c;
}

static cui_draw_cmd *cui__cmd(cui_ctx *ui) { return cui__cmd_of(ui, false); }

/* ---- surfaces: zones of the window with a draw list of their own ---- */

static cui_draw_buf cui__buf_of(const cui_draw_list *dl) {
    cui_draw_buf b = { dl->vtx, dl->vtx_count, dl->vtx_cap, dl->idx, dl->idx_count, dl->idx_cap,
                       dl->cmd, dl->cmd_count, dl->cmd_cap, dl->inst, dl->inst_count, dl->inst_cap };
    return b;
}

/* Makes buffer `to` (0: the window, 1 + slot: a surface) the one shapes go into. */
static void cui__use_buf(cui_ctx *ui, int to) {
    if (to == ui->dbuf) return;
    cui_draw_list *dl = &ui->dl;
    ui->dbufs[ui->dbuf] = cui__buf_of(dl);
    cui_draw_buf b = ui->dbufs[to];
    dl->vtx = b.vtx; dl->vtx_count = b.vtx_count; dl->vtx_cap = b.vtx_cap;
    dl->idx = b.idx; dl->idx_count = b.idx_count; dl->idx_cap = b.idx_cap;
    dl->cmd = b.cmd; dl->cmd_count = b.cmd_count; dl->cmd_cap = b.cmd_cap;
    dl->inst = b.inst; dl->inst_count = b.inst_count; dl->inst_cap = b.inst_cap;
    ui->dbuf = to;
}

void cui__surface_push(cui_ctx *ui, cui_id id, cui_rect r) {
    if (ui->dbuf_depth >= CUI_COUNTOF(ui->dbuf_stack)) return;
    /* whole points, so that a system that places surfaces in points puts it where it was drawn */
    float x0 = floorf(r.x), y0 = floorf(r.y), x1 = ceilf(r.x + r.w), y1 = ceilf(r.y + r.h);
    x0 = CUI_MAX(x0, 0); y0 = CUI_MAX(y0, 0); x1 = CUI_MIN(x1, ceilf(ui->vw)); y1 = CUI_MIN(y1, ceilf(ui->vh));
    int slot = -1, same = 0;
    for (int i = 0; i < CUI_SURFACES; i++) same += ui->surf[i].used && ui->surf[i].base == id;
    cui_id base = id;
    if (same && id) { uint32_t key[2] = { id, (uint32_t)same }; id = cui__hash(key, sizeof key, 0x5f3759df); }   /* the same id again this frame: another surface, the same one every frame */
    for (int i = 0; i < CUI_SURFACES && id; i++) if (ui->surf[i].id == id) { slot = i; break; }
    for (int i = 0; i < CUI_SURFACES && slot < 0 && id; i++) if (!ui->surf[i].id) slot = i;
    if (x1 <= x0 || y1 <= y0) slot = -1;
    ui->dbuf_stack[ui->dbuf_depth++] = ui->dbuf;
    cui_rect zone = { x0, y0, x1 - x0, y1 - y0 };
    cui_push_clip(ui, slot < 0 ? r : zone);
    if (slot < 0) return;   /* no room for another, or nothing of it in the window: drawn where it is */
    cui__surface *sf = &ui->surf[slot];
    float s = ui->scale;
    sf->id = id; sf->base = base; sf->used = true; sf->order = ui->surf_order++;
    sf->x = (int)(x0 * s + 0.5f); sf->y = (int)(y0 * s + 0.5f);
    sf->w = (int)(x1 * s + 0.5f) - sf->x; sf->h = (int)(y1 * s + 0.5f) - sf->y;
    cui__use_buf(ui, 1 + slot);
}

void cui__surface_pop(cui_ctx *ui) {
    if (ui->dbuf_depth <= 0) return;
    cui_pop_clip(ui);
    cui__use_buf(ui, ui->dbuf_stack[--ui->dbuf_depth]);
}

void cui_surface_begin(cui_ctx *ui, const char *id, cui_rect rect) { cui__surface_push(ui, cui__hash_str(id, 0x5f3759df), rect); }
void cui_surface_end(cui_ctx *ui) { cui__surface_pop(ui); }

bool cui_get_surface(cui_ctx *ui, int slot, cui_surface *out) {
    if (slot < 0 || slot >= CUI_SURFACES || !ui->surf[slot].used) return false;
    const cui__surface *sf = &ui->surf[slot];
    if (out) { out->id = sf->id; out->x = sf->x; out->y = sf->y; out->w = sf->w; out->h = sf->h; out->order = sf->order; out->draw = &ui->surf_data[slot]; }
    return true;
}

const cui_draw_data *cui_get_draw_data(cui_ctx *ui) { return &ui->draw_data; }

static void cui__data_of(cui_draw_data *dd, const cui_draw_buf *b) {
    dd->vertices = b->vtx; dd->vertex_count = b->vtx_count;
    dd->indices = b->idx; dd->index_count = b->idx_count;
    dd->commands = b->cmd; dd->command_count = b->cmd_count;
    dd->instances = b->inst; dd->instance_count = b->inst_count;
}

void cui__draw_finish(cui_ctx *ui) {
    cui__use_buf(ui, 0);
    ui->dbuf_depth = 0;
    ui->dbufs[0] = cui__buf_of(&ui->dl);
    cui_draw_data base;
    memset(&base, 0, sizeof base);
    base.width_px = (int)(ui->vw * ui->scale + 0.5f);
    base.height_px = (int)(ui->vh * ui->scale + 0.5f);
    base.clear_color = ui->theme.colors[CUI_TOKEN_BACKGROUND];
    base.font_atlas = ui->atlas_tex;
    ui->draw_data = base;
    cui__data_of(&ui->draw_data, &ui->dbufs[0]);
    /* the surfaces in the order they were begun in: the order they lie over the window in */
    int order[CUI_SURFACES], count = 0;
    for (int i = 0; i < CUI_SURFACES; i++) {
        memset(&ui->surf_data[i], 0, sizeof ui->surf_data[i]);
        if (!ui->surf[i].used) { ui->surf[i].id = 0; continue; }   /* a surface that was not drawn into gives its slot up */
        int k = count++;
        while (k > 0 && ui->surf[order[k - 1]].order > ui->surf[i].order) { order[k] = order[k - 1]; k--; }
        order[k] = i;
    }
    ui->flat_stale = count > 0;
    ui->merged_data = ui->draw_data;
    /* each surface's own list, from its own top left corner: what is drawn into a target of its size */
    for (int n = 0; n < count; n++) {
        const cui__surface *sf = &ui->surf[order[n]];
        cui_draw_buf *b = &ui->dbufs[1 + order[n]];
        float ox = (float)sf->x, oy = (float)sf->y;
        for (int k = 0; k < b->vtx_count; k++) { b->vtx[k].x -= ox; b->vtx[k].y -= oy; }
        for (int k = 0; k < b->inst_count; k++) {
            cui_instance *in = &b->inst[k];
            in->x0 = (int16_t)(in->x0 - sf->x * 4); in->x1 = (int16_t)(in->x1 - sf->x * 4);
            in->y0 = (int16_t)(in->y0 - sf->y * 4); in->y1 = (int16_t)(in->y1 - sf->y * 4);
        }
        cui_rect whole = { 0, 0, (float)sf->w, (float)sf->h };
        for (int k = 0; k < b->cmd_count; k++) {
            b->cmd[k].clip.x -= ox; b->cmd[k].clip.y -= oy;
            b->cmd[k].clip = cui__rect_intersect(b->cmd[k].clip, whole);
        }
        cui_draw_data *dd = &ui->surf_data[order[n]];
        *dd = base;
        dd->width_px = sf->w; dd->height_px = sf->h;
        cui_color none = { 0, 0, 0, 0 };
        dd->clear_color = none;   /* clear where nothing is drawn: the window shows through */
        cui__data_of(dd, b);
    }
}

/* The window's list and every surface's as one list for one target: each surface's shapes put back
 * where the surface lies, in the order the surfaces lie over the window in. A copy of the frame:
 * for a renderer that has not learnt to take the lists one after another (cui_gl_render_lists). */
const cui_draw_data *cui__draw_data_merged(cui_ctx *ui) {
    if (!ui->flat_stale) return &ui->merged_data;
    ui->flat_stale = false;
    int order[CUI_SURFACES], count = 0, nv = ui->dbufs[0].vtx_count, ni = ui->dbufs[0].idx_count, nc = ui->dbufs[0].cmd_count, nr = ui->dbufs[0].inst_count;
    for (int i = 0; i < CUI_SURFACES; i++) {
        if (!ui->surf[i].used) continue;
        int k = count++;
        while (k > 0 && ui->surf[order[k - 1]].order > ui->surf[i].order) { order[k] = order[k - 1]; k--; }
        order[k] = i;
        const cui_draw_buf *b = &ui->dbufs[1 + i];
        nv += b->vtx_count; ni += b->idx_count; nc += b->cmd_count; nr += b->inst_count;
    }
    cui_draw_buf *f = &ui->flat;
    f->vtx_count = f->idx_count = f->cmd_count = f->inst_count = 0;
    CUI__GROW(ui, f->vtx, f->vtx_count, f->vtx_cap, nv);
    CUI__GROW(ui, f->idx, f->idx_count, f->idx_cap, ni);
    CUI__GROW(ui, f->cmd, f->cmd_count, f->cmd_cap, nc);
    CUI__GROW(ui, f->inst, f->inst_count, f->inst_cap, nr);
    for (int n = 0; n <= count; n++) {
        const cui_draw_buf *b = &ui->dbufs[n ? 1 + order[n - 1] : 0];
        int sx = n ? ui->surf[order[n - 1]].x : 0, sy = n ? ui->surf[order[n - 1]].y : 0;
        if (b->vtx_count) memcpy(f->vtx + f->vtx_count, b->vtx, sizeof *b->vtx * (size_t)b->vtx_count);
        if (b->inst_count) memcpy(f->inst + f->inst_count, b->inst, sizeof *b->inst * (size_t)b->inst_count);
        for (int k = 0; (sx || sy) && k < b->vtx_count; k++) { f->vtx[f->vtx_count + k].x += (float)sx; f->vtx[f->vtx_count + k].y += (float)sy; }
        for (int k = 0; (sx || sy) && k < b->inst_count; k++) {
            cui_instance *in = &f->inst[f->inst_count + k];
            in->x0 = (int16_t)(in->x0 + sx * 4); in->x1 = (int16_t)(in->x1 + sx * 4);
            in->y0 = (int16_t)(in->y0 + sy * 4); in->y1 = (int16_t)(in->y1 + sy * 4);
        }
        for (int k = 0; k < b->idx_count; k++) f->idx[f->idx_count + k] = b->idx[k] + (uint32_t)f->vtx_count;
        for (int k = 0; k < b->cmd_count; k++) {
            cui_draw_cmd c = b->cmd[k];
            c.clip.x += (float)sx; c.clip.y += (float)sy;
            c.index_offset += (uint32_t)f->idx_count;
            c.instance_offset += (uint32_t)f->inst_count;
            f->cmd[f->cmd_count + k] = c;
        }
        f->vtx_count += b->vtx_count; f->idx_count += b->idx_count; f->cmd_count += b->cmd_count; f->inst_count += b->inst_count;
    }
    cui__data_of(&ui->merged_data, f);
    return &ui->merged_data;
}

void cui__draw_free(cui_ctx *ui) {
    cui__use_buf(ui, 0);
    ui->dbufs[0] = cui__buf_of(&ui->dl);
    for (int i = 0; i < 1 + CUI_SURFACES; i++) {
        cui__free(ui, ui->dbufs[i].vtx); cui__free(ui, ui->dbufs[i].idx);
        cui__free(ui, ui->dbufs[i].cmd); cui__free(ui, ui->dbufs[i].inst);
    }
    cui__free(ui, ui->flat.vtx); cui__free(ui, ui->flat.idx); cui__free(ui, ui->flat.cmd); cui__free(ui, ui->flat.inst);
}

void cui__draw_reset(cui_ctx *ui) {
    cui_draw_list *dl = &ui->dl;
    cui__use_buf(ui, 0);
    ui->dbuf_depth = 0;
    ui->surf_order = 0;
    for (int i = 0; i < CUI_SURFACES; i++) {
        ui->surf[i].used = false;
        ui->dbufs[1 + i].vtx_count = ui->dbufs[1 + i].idx_count = ui->dbufs[1 + i].cmd_count = ui->dbufs[1 + i].inst_count = 0;
    }
    dl->vtx_count = dl->idx_count = dl->cmd_count = dl->inst_count = 0;
    dl->clip_depth = 1;
    cui_rect full = { 0, 0, ui->vw * ui->scale, ui->vh * ui->scale };
    dl->clip_stack[0] = full;
    dl->xform_depth = 1;
    cui_xform id = { 1, 0, 0, 1, 0, 0 };
    dl->xform_stack[0] = id;
    dl->cur_tex = 0;
}

/* Composite transform: p' = p * scale + (tx, ty) (stored in ox/oy), alpha. */
void cui__push_xform(cui_ctx *ui, float s, float ox, float oy, float alpha, float tx, float ty) {
    cui_draw_list *dl = &ui->dl;
    if (dl->xform_depth >= 32) return;
    cui_xform p = dl->xform_stack[dl->xform_depth - 1];
    float sc = ui->scale;
    ox *= sc; oy *= sc; tx *= sc; ty *= sc;
    float bx = ox - ox * s + tx, by = oy - oy * s + ty;
    cui_xform n;
    n.scale = s * p.scale;
    n.ox = bx * p.scale + p.ox;
    n.oy = by * p.scale + p.oy;
    n.alpha = alpha * p.alpha;
    n.tx = n.ty = 0;
    dl->xform_stack[dl->xform_depth++] = n;
}

void cui__pop_xform(cui_ctx *ui) { if (ui->dl.xform_depth > 1) ui->dl.xform_depth--; }

static inline cui_xform cui__xf(cui_ctx *ui) { return ui->dl.xform_stack[ui->dl.xform_depth - 1]; }
static inline bool cui__xf_identity(cui_xform x) { return x.scale == 1 && x.ox == 0 && x.oy == 0 && x.alpha == 1; }

static inline cui_color cui__xa(cui_color c, float a) {
    if (a >= 1) return c;
    c.a = (uint8_t)(c.a * a + 0.5f);
    return c;
}

void cui_push_clip(cui_ctx *ui, cui_rect r) {
    cui_draw_list *dl = &ui->dl;
    cui_xform x = cui__xf(ui);
    float s = ui->scale;
    cui_rect px = { r.x * s * x.scale + x.ox, r.y * s * x.scale + x.oy, r.w * s * x.scale, r.h * s * x.scale };
    /* snap to whole pixels outward */
    float x0 = floorf(px.x), y0 = floorf(px.y), x1 = ceilf(px.x + px.w), y1 = ceilf(px.y + px.h);
    px.x = x0; px.y = y0; px.w = x1 - x0; px.h = y1 - y0;
    cui_rect cur = dl->clip_stack[dl->clip_depth - 1];
    if (dl->clip_depth < 64) dl->clip_stack[dl->clip_depth++] = cui__rect_intersect(cur, px);
}

void cui_pop_clip(cui_ctx *ui) { if (ui->dl.clip_depth > 1) ui->dl.clip_depth--; }

static void cui__set_texture(cui_ctx *ui, cui_texture t) { ui->dl.cur_tex = t; }

/* Reserve vertices/indices in the current command. */
static cui_vertex *cui__reserve(cui_ctx *ui, int nv, int ni, uint32_t *base) {
    cui_draw_list *dl = &ui->dl;
    cui_draw_cmd *c = cui__cmd(ui);
    CUI__GROW(ui, dl->vtx, dl->vtx_count, dl->vtx_cap, nv);
    CUI__GROW(ui, dl->idx, dl->idx_count, dl->idx_cap, ni);
    *base = (uint32_t)dl->vtx_count;
    cui_vertex *v = &dl->vtx[dl->vtx_count];
    memset(v, 0, sizeof(cui_vertex) * (size_t)nv);
    dl->vtx_count += nv;
    c->index_count += (uint32_t)ni;
    return v;
}

static uint32_t *cui__idx(cui_ctx *ui, int ni) {
    cui_draw_list *dl = &ui->dl;
    uint32_t *p = &dl->idx[dl->idx_count];
    dl->idx_count += ni;
    return p;
}

static bool cui__clip_rejects(cui_ctx *ui, float x0, float y0, float x1, float y1) {
    cui_rect c = ui->dl.clip_stack[ui->dl.clip_depth - 1];
    return x1 < c.x || y1 < c.y || x0 > c.x + c.w || y0 > c.y + c.h || c.w <= 0 || c.h <= 0;
}

/* ---- instances ---- */

typedef char cui__instance_is_40_bytes[sizeof(cui_instance) == 40 ? 1 : -1];

static bool cui__quarter(float v, float lo, float hi, int *out) {
    float q = v * 4.0f;
    q = q < 0 ? q - 0.5f : q + 0.5f;
    if (!(q >= lo && q <= hi)) return false;
    *out = (int)q;
    return true;
}

static bool cui__unit16(float v, int *out) {
    if (!(v >= 0.0f && v <= 1.0f)) return false;
    *out = (int)(v * 65535.0f + 0.5f);
    return true;
}

static bool cui__same_color(cui_color a, cui_color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

static cui_instance *cui__reserve_instance(cui_ctx *ui) {
    cui_draw_list *dl = &ui->dl;
    cui_draw_cmd *c = cui__cmd_of(ui, true);
    CUI__GROW(ui, dl->inst, dl->inst_count, dl->inst_cap, 1);
    cui_instance *in = &dl->inst[dl->inst_count++];
    memset(in, 0, sizeof *in);
    c->instance_count++;
    return in;
}

/* The values of one shape as quarter pixels and texture fractions; false when one does not fit. */
typedef struct { int x0, y0, x1, y1, u0, v0, u1, v1, r[4], param; } cui__packed;

static bool cui__pack(cui__packed *p, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      const float r[4], float param) {
    bool ok = cui__quarter(x0, -32768, 32767, &p->x0) && cui__quarter(y0, -32768, 32767, &p->y0);
    ok = ok && cui__quarter(x1, -32768, 32767, &p->x1) && cui__quarter(y1, -32768, 32767, &p->y1);
    ok = ok && cui__unit16(u0, &p->u0) && cui__unit16(v0, &p->v0) && cui__unit16(u1, &p->u1) && cui__unit16(v1, &p->v1);
    for (int i = 0; i < 4; i++) ok = ok && cui__quarter(r[i], 0, 65535, &p->r[i]);
    return ok && cui__quarter(param, 0, 65535, &p->param) && p->x1 > p->x0 && p->y1 > p->y0;
}

/* The lean of a quad whose bottom edge is at y, while the draw list shears (cui_draw_list.shear):
 * the record's shear in 128ths, and how far its bottom edge moves. 0 and no move otherwise. */
static int cui__shear_of(cui_ctx *ui, float bottom, float *dx) {
    int q = (int)lroundf(ui->dl.shear * 128.0f);
    q = CUI_CLAMP(q, -127, 127);
    *dx = (float)q / 128.0f * (ui->dl.shear_base - bottom);
    return q;
}

static void cui__emit_instance(cui_ctx *ui, const cui__packed *p, int mode, cui_color top, cui_color bottom, cui_color border) {
    cui_instance *in = cui__reserve_instance(ui);
    in->x0 = (int16_t)p->x0; in->y0 = (int16_t)p->y0; in->x1 = (int16_t)p->x1; in->y1 = (int16_t)p->y1;
    in->u0 = (uint16_t)p->u0; in->v0 = (uint16_t)p->v0; in->u1 = (uint16_t)p->u1; in->v1 = (uint16_t)p->v1;
    in->r_tl = (uint16_t)p->r[0]; in->r_tr = (uint16_t)p->r[1]; in->r_br = (uint16_t)p->r[2]; in->r_bl = (uint16_t)p->r[3];
    in->param = (uint16_t)p->param;
    in->mode = (uint8_t)mode;
    in->color_top = top; in->color_bottom = bottom; in->border_color = border;
}

void cui_instance_vertices(const cui_instance *in, cui_vertex v[4]) {
    float x0 = in->x0 * 0.25f, y0 = in->y0 * 0.25f, x1 = in->x1 * 0.25f, y1 = in->y1 * 0.25f;
    if (in->mode == CUI_SHADE_LINE) {
        /* a box along the stroke: its length by its thickness, a pixel wider all round for the soft edge */
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len > 0) { dx /= len; dy /= len; } else { dx = 1; dy = 0; }
        float r = in->r_tl * 0.25f;   /* round ends reach past the two points by their radius */
        float hl = len * 0.5f + r, ht = CUI_MAX(in->param * 0.125f, 0.5f), mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        memset(v, 0, sizeof(cui_vertex) * 4);
        for (int i = 0; i < 4; i++) {
            float lx = (i == 1 || i == 2 ? 1.0f : -1.0f) * (hl + 1.0f), ly = (i >= 2 ? 1.0f : -1.0f) * (ht + 1.0f);
            v[i].x = mx + dx * lx - dy * ly; v[i].y = my + dy * lx + dx * ly;
            v[i].lx = lx; v[i].ly = ly; v[i].hw = hl; v[i].hh = ht;
            v[i].r_tl = v[i].r_tr = v[i].r_br = v[i].r_bl = r;
            v[i].mode = (float)CUI_SHADE_RRECT;
            v[i].color = in->color_top;
            v[i].border_color = in->border_color;
        }
        return;
    }
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    float param = in->param * 0.25f;
    float pad = in->mode == CUI_SHADE_SHADOW ? param * 3.0f : (in->mode == CUI_SHADE_RRECT || in->mode == CUI_SHADE_IMAGE) ? 1.0f : 0.0f;
    float u0 = in->u0 / 65535.0f, v0 = in->v0 / 65535.0f, u1 = in->u1 / 65535.0f, v1 = in->v1 / 65535.0f;
    float px[4] = { x0 - pad, x1 + pad, x1 + pad, x0 - pad }, py[4] = { y0 - pad, y0 - pad, y1 + pad, y1 + pad };
    memset(v, 0, sizeof(cui_vertex) * 4);
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i] + in->shear / 128.0f * (y1 - py[i]); v[i].y = py[i];
        v[i].lx = px[i] - cx; v[i].ly = py[i] - cy;
        v[i].hw = hw; v[i].hh = hh;
        v[i].u = u0 + (v[i].lx + hw) / (2 * hw) * (u1 - u0);
        v[i].v = v0 + (v[i].ly + hh) / (2 * hh) * (v1 - v0);
        v[i].r_tl = in->r_tl * 0.25f; v[i].r_tr = in->r_tr * 0.25f; v[i].r_br = in->r_br * 0.25f; v[i].r_bl = in->r_bl * 0.25f;
        v[i].border = param;
        v[i].mode = (float)in->mode;
        v[i].color = i < 2 ? in->color_top : in->color_bottom;
        v[i].border_color = in->border_color;
    }
}

const cui_draw_data *cui_expand_instances(cui_expanded *e, const cui_draw_data *dd) {
    if (!dd || dd->instance_count <= 0) return dd;
    uint32_t drawn = 0;   /* a list cut to several boxes (cui_damage_clip_boxes) draws a record once for each box it touches */
    for (int i = 0; i < dd->command_count; i++) drawn += dd->commands[i].instance_count;
    int nv = dd->vertex_count + (int)drawn * 4, ni = dd->index_count + (int)drawn * 6;
    if (nv > e->vertex_cap) { cui_vertex *p = realloc(e->vertices, sizeof *p * (size_t)nv); if (!p) return NULL; e->vertices = p; e->vertex_cap = nv; }
    if (ni > e->index_cap) { uint32_t *p = realloc(e->indices, sizeof *p * (size_t)ni); if (!p) return NULL; e->indices = p; e->index_cap = ni; }
    if (dd->command_count > e->command_cap) {
        cui_draw_cmd *p = realloc(e->commands, sizeof *p * (size_t)dd->command_count);
        if (!p) return NULL;
        e->commands = p; e->command_cap = dd->command_count;
    }
    if (dd->vertex_count) memcpy(e->vertices, dd->vertices, sizeof(cui_vertex) * (size_t)dd->vertex_count);
    if (dd->index_count) memcpy(e->indices, dd->indices, sizeof(uint32_t) * (size_t)dd->index_count);
    int v = dd->vertex_count, x = dd->index_count;
    for (int i = 0; i < dd->command_count; i++) {
        cui_draw_cmd c = dd->commands[i];
        if (c.instance_count) {
            c.index_offset = (uint32_t)x;
            c.index_count = c.instance_count * 6;
            for (uint32_t k = 0; k < c.instance_count; k++, v += 4, x += 6) {
                cui_instance_vertices(&dd->instances[c.instance_offset + k], &e->vertices[v]);
                uint32_t b = (uint32_t)v;
                uint32_t *ix = &e->indices[x];
                ix[0] = b; ix[1] = b + 1; ix[2] = b + 2; ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
            }
            c.instance_offset = c.instance_count = 0;
        }
        e->commands[i] = c;
    }
    e->data = *dd;
    e->data.vertices = e->vertices; e->data.vertex_count = nv;
    e->data.indices = e->indices; e->data.index_count = ni;
    e->data.commands = e->commands;
    e->data.instances = NULL; e->data.instance_count = 0;
    return &e->data;
}

void cui_expanded_free(cui_expanded *e) {
    if (!e) return;
    free(e->vertices); free(e->indices); free(e->commands);
    memset(e, 0, sizeof *e);
}

/* Emits one SDF quad in pixel space (before transform). */
static void cui__sdf_quad(cui_ctx *ui, float cx, float cy, float hw, float hh, float expand,
                          cui_corners rad, float border, int mode,
                          cui_color c_tl, cui_color c_tr, cui_color c_br, cui_color c_bl, cui_color bc,
                          cui_texture tex, float u0, float v0, float u1, float v1) {
    cui_xform x = cui__xf(ui);
    float s = x.scale;
    float ncx = cx * s + x.ox, ncy = cy * s + x.oy;
    float nhw = hw * s, nhh = hh * s, ne = expand * s;
    if (mode == CUI_SHADE_RRECT || mode == CUI_SHADE_IMAGE) ne = CUI_MAX(ne, 1.0f);
    float x0 = ncx - nhw - ne, y0 = ncy - nhh - ne, x1 = ncx + nhw + ne, y1 = ncy + nhh + ne;
    if (cui__clip_rejects(ui, x0, y0, x1, y1)) return;
    float a = x.alpha;
    cui__set_texture(ui, tex);
    float maxr = CUI_MIN(nhw, nhh);
    if (ui->dl.instanced && cui__same_color(c_tl, c_tr) && cui__same_color(c_bl, c_br)) {
        bool shadow = mode == CUI_SHADE_SHADOW;
        float r[4] = { rad.tl * s, rad.tr * s, rad.br * s, rad.bl * s };
        for (int i = 0; i < 4 && !shadow; i++) r[i] = CUI_MIN(r[i], maxr);
        cui__packed p;
        float lean;
        int shear = cui__shear_of(ui, ncy + nhh, &lean);
        if (cui__pack(&p, ncx - nhw + lean, ncy - nhh, ncx + nhw + lean, ncy + nhh, u0, v0, u1, v1, r, border * s)) {
            cui__emit_instance(ui, &p, mode, cui__xa(c_tl, a), cui__xa(c_bl, a), cui__xa(bc, a));
            ui->dl.inst[ui->dl.inst_count - 1].shear = (int8_t)shear;
            return;
        }
    }
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, 4, 6, &base);
    float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 };
    float uu[4] = { u0, u1, u1, u0 }, vv[4] = { v0, v0, v1, v1 };
    cui_color cc[4] = { c_tl, c_tr, c_br, c_bl };
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i]; v[i].y = py[i];
        v[i].lx = px[i] - ncx; v[i].ly = py[i] - ncy;
        v[i].hw = nhw; v[i].hh = nhh;
        v[i].r_tl = CUI_MIN(rad.tl * s, maxr); v[i].r_tr = CUI_MIN(rad.tr * s, maxr);
        v[i].r_br = CUI_MIN(rad.br * s, maxr); v[i].r_bl = CUI_MIN(rad.bl * s, maxr);
        if (mode == CUI_SHADE_SHADOW) { v[i].r_tl = rad.tl * s; v[i].r_tr = rad.tr * s; v[i].r_br = rad.br * s; v[i].r_bl = rad.bl * s; }
        v[i].border = border * s;
        v[i].mode = (float)mode;
        v[i].color = cui__xa(cc[i], a);
        v[i].border_color = cui__xa(bc, a);
        v[i].u = uu[i]; v[i].v = vv[i];
    }
    /* the uvs above map to the expanded quad; correct them for images */
    if (mode == CUI_SHADE_IMAGE && ne > 0) {
        float du = (u1 - u0) / (2 * nhw), dv = (v1 - v0) / (2 * nhh);
        for (int i = 0; i < 4; i++) {
            v[i].u = u0 + (v[i].lx + nhw) * du;
            v[i].v = v0 + (v[i].ly + nhh) * dv;
        }
    }
    uint32_t *ix = cui__idx(ui, 6);
    ix[0] = base; ix[1] = base + 1; ix[2] = base + 2; ix[3] = base; ix[4] = base + 2; ix[5] = base + 3;
}

static inline cui_corners cui__scale_corners(cui_corners c, float s) { c.tl *= s; c.tr *= s; c.br *= s; c.bl *= s; return c; }

void cui_draw_rect(cui_ctx *ui, cui_rect r, cui_corners rad, cui_color fill) {
    fill = cui_color_of(ui, fill);
    if (fill.a == 0 || r.w <= 0 || r.h <= 0) return;
    float s = ui->scale;
    cui__sdf_quad(ui, (r.x + r.w * 0.5f) * s, (r.y + r.h * 0.5f) * s, r.w * 0.5f * s, r.h * 0.5f * s, 1,
                  cui__scale_corners(rad, s), 0, CUI_SHADE_RRECT, fill, fill, fill, fill, fill, 0, 0, 0, 0, 0);
}

void cui_draw_rect_border(cui_ctx *ui, cui_rect r, cui_corners rad, cui_color fill, cui_color border, float bw) {
    fill = cui_color_of(ui, fill);
    border = cui_color_of(ui, border);
    if (r.w <= 0 || r.h <= 0) return;
    if (bw <= 0 || border.a == 0) { cui_draw_rect(ui, r, rad, fill); return; }
    float s = ui->scale;
    /* hairlines stay at least one device pixel wide */
    float bpx = CUI_MAX(bw * s, 1.0f);
    cui__sdf_quad(ui, (r.x + r.w * 0.5f) * s, (r.y + r.h * 0.5f) * s, r.w * 0.5f * s, r.h * 0.5f * s, 1,
                  cui__scale_corners(rad, s), bpx, CUI_SHADE_RRECT, fill, fill, fill, fill, border, 0, 0, 0, 0, 0);
}

void cui_draw_rect_gradient(cui_ctx *ui, cui_rect r, cui_corners rad, cui_color top, cui_color bottom) {
    top = cui_color_of(ui, top);
    bottom = cui_color_of(ui, bottom);
    if (r.w <= 0 || r.h <= 0) return;
    float s = ui->scale;
    /* vertex colors interpolate over the expanded quad; expansion is 1px so
     * the error is invisible */
    cui__sdf_quad(ui, (r.x + r.w * 0.5f) * s, (r.y + r.h * 0.5f) * s, r.w * 0.5f * s, r.h * 0.5f * s, 1,
                  cui__scale_corners(rad, s), 0, CUI_SHADE_RRECT, top, top, bottom, bottom, top, 0, 0, 0, 0, 0);
}

void cui_draw_shadow(cui_ctx *ui, cui_rect r, cui_corners rad, float blur, float spread, cui_vec2 off, cui_color color) {
    color = cui_color_of(ui, color);
    if (color.a == 0) return;
    float s = ui->scale;
    float sigma = CUI_MAX(blur * 0.5f, 0.5f) * s;
    float hw = (r.w * 0.5f + spread) * s, hh = (r.h * 0.5f + spread) * s;
    if (hw <= 0 || hh <= 0) return;
    cui_corners rr = cui__scale_corners(rad, s);
    rr.tl += spread * s; rr.tr += spread * s; rr.br += spread * s; rr.bl += spread * s;
    float cx = (r.x + r.w * 0.5f + off.x) * s, cy = (r.y + r.h * 0.5f + off.y) * s;
    cui__sdf_quad(ui, cx, cy, hw, hh, sigma * 3.0f, rr, sigma, CUI_SHADE_SHADOW, color, color, color, color, color, 0, 0, 0, 0, 0);
}

void cui_draw_circle(cui_ctx *ui, cui_vec2 c, float radius, cui_color color) {
    cui_rect r = { c.x - radius, c.y - radius, radius * 2, radius * 2 };
    cui_draw_rect(ui, r, CUI_CORNERS(radius), color);
}

void cui_draw_image(cui_ctx *ui, cui_texture tex, cui_rect r, cui_rect uv, cui_corners rad, cui_color tint) {
    tint = cui_color_of(ui, tint);
    if (!tint.a && !cui__is_token(tint)) tint = cui__rgba(255, 255, 255, 255);
    if (!tex || r.w <= 0 || r.h <= 0) return;
    float s = ui->scale;
    cui__sdf_quad(ui, (r.x + r.w * 0.5f) * s, (r.y + r.h * 0.5f) * s, r.w * 0.5f * s, r.h * 0.5f * s, 0,
                  cui__scale_corners(rad, s), 0, CUI_SHADE_IMAGE, tint, tint, tint, tint, tint, tex,
                  uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
}

void cui__draw_glyph_quad(cui_ctx *ui, float x0, float y0, float x1, float y1,
                          float u0, float v0, float u1, float v1, cui_color c) {
    /* coordinates are already in pixels */
    cui_xform x = cui__xf(ui);
    if (!cui__xf_identity(x)) {
        x0 = x0 * x.scale + x.ox; y0 = y0 * x.scale + x.oy;
        x1 = x1 * x.scale + x.ox; y1 = y1 * x.scale + x.oy;
        c = cui__xa(c, x.alpha);
    }
    if (cui__clip_rejects(ui, x0, y0, x1, y1)) return;
    cui__set_texture(ui, 0);
    if (ui->dl.instanced) {
        static const float square[4] = { 0, 0, 0, 0 };
        cui__packed p;
        float lean;
        int shear = cui__shear_of(ui, y1, &lean);
        /* the size is kept exact, so a glyph is not resampled when its origin lands between quarter pixels */
        if (cui__pack(&p, x0 + lean, y0, x1 + lean, y1, u0, v0, u1, v1, square, 0) && cui__quarter(x1 - x0, 1, 65535, &p.x1) && cui__quarter(y1 - y0, 1, 65535, &p.y1)) {
            p.x1 += p.x0; p.y1 += p.y0;
            if (p.x1 <= 32767 && p.y1 <= 32767) {
                cui__emit_instance(ui, &p, CUI_SHADE_TEXT, c, c, c);
                ui->dl.inst[ui->dl.inst_count - 1].shear = (int8_t)shear;
                return;
            }
        }
    }
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, 4, 6, &base);
    float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 };
    float uu[4] = { u0, u1, u1, u0 }, vv[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i]; v[i].y = py[i]; v[i].u = uu[i]; v[i].v = vv[i];
        v[i].mode = CUI_SHADE_TEXT; v[i].color = c;
    }
    uint32_t *ix = cui__idx(ui, 6);
    ix[0] = base; ix[1] = base + 1; ix[2] = base + 2; ix[3] = base; ix[4] = base + 2; ix[5] = base + 3;
}

/* ---- paths (pixel space after transform) ---- */

static void cui__solid_vertex(cui_vertex *v, float x, float y, cui_color c) {
    v->x = x; v->y = y; v->mode = CUI_SHADE_SOLID; v->color = c;
}

static void cui__polyline_px(cui_ctx *ui, const cui_vec2 *pts, int n, float thick, cui_color col, bool closed) {
    if (n < 2) return;
    cui__set_texture(ui, 0);
    int segs = closed ? n : n - 1;
    float aa = 1.0f;
    float hw = thick * 0.5f;
    bool thin = hw < 0.5f;
    if (thin) { col.a = (uint8_t)(col.a * (thick > 0 ? thick : 1)); hw = 0.5f; }
    /* (the vertices below fade over a pixel outside the stroke; a record's edge is in the middle of that pixel)
     * As records: one stroke a segment. Two points make one with square ends, as the vertices
     * below do. More points take round ends, which fill the joints, and so only at full opacity:
     * a translucent stroke would be twice as dense where two of them overlap. */
    if (ui->dl.instanced && ((n == 2 && !closed) || col.a == 255)) {
        int first = ui->dl.inst_count;
        bool ok = true;
        for (int i = 0; i < segs && ok; i++) {
            int j = (i + 1) % n, ax, ay, bx, by, t, r;
            ok = cui__quarter(pts[i].x, -32768, 32767, &ax) && cui__quarter(pts[i].y, -32768, 32767, &ay) &&
                 cui__quarter(pts[j].x, -32768, 32767, &bx) && cui__quarter(pts[j].y, -32768, 32767, &by) &&
                 cui__quarter(hw * 2.0f + aa, 1, 65535, &t) && cui__quarter(n == 2 ? 0.0f : hw + aa * 0.5f, 0, 65535, &r);
            if (!ok || (ax == bx && ay == by)) continue;
            cui_instance *in = cui__reserve_instance(ui);
            in->x0 = (int16_t)ax; in->y0 = (int16_t)ay; in->x1 = (int16_t)bx; in->y1 = (int16_t)by;
            in->param = (uint16_t)t;
            in->r_tl = (uint16_t)r;
            in->mode = CUI_SHADE_LINE;
            in->color_top = in->color_bottom = col;
        }
        if (ok) return;
        /* one end did not fit: the whole line goes as vertices */
        for (int c = 0; c < ui->dl.cmd_count; c++) {
            cui_draw_cmd *dc = &ui->dl.cmd[c];
            uint32_t end = dc->instance_offset + dc->instance_count;
            if (end > (uint32_t)first) dc->instance_count = dc->instance_offset >= (uint32_t)first ? 0 : (uint32_t)first - dc->instance_offset;
        }
        ui->dl.inst_count = first;
    }
    cui_color trans = col; trans.a = 0;
    /* normals per point */
    cui_vec2 *nrm = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)n);
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        float dx = pts[j].x - pts[i].x, dy = pts[j].y - pts[i].y;
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 0) { dx /= l; dy /= l; }
        nrm[i].x = dy; nrm[i].y = -dx;
    }
    if (!closed) nrm[n - 1] = nrm[n - 2];
    uint32_t base;
    int caps = closed ? 0 : 2;   /* open lines get a soft fringe across each end too, so cut ends do not stair-step */
    cui_vertex *v = cui__reserve(ui, n * 4 + caps * 4, segs * 18 + caps * 18, &base);
    for (int i = 0; i < n; i++) {
        cui_vec2 na = (i == 0 && !closed) ? nrm[0] : nrm[(i - 1 + n) % n];
        cui_vec2 nb = nrm[i];
        if (!closed && i == n - 1) nb = na;
        float mx = (na.x + nb.x) * 0.5f, my = (na.y + nb.y) * 0.5f;
        float d2 = mx * mx + my * my;
        if (d2 > 0.000001f) { float inv = 1.0f / d2; if (inv > 25.0f) inv = 25.0f; mx *= inv; my *= inv; }
        float p = hw, o = hw + aa;
        cui__solid_vertex(&v[i * 4 + 0], pts[i].x + mx * o, pts[i].y + my * o, trans);
        cui__solid_vertex(&v[i * 4 + 1], pts[i].x + mx * p, pts[i].y + my * p, col);
        cui__solid_vertex(&v[i * 4 + 2], pts[i].x - mx * p, pts[i].y - my * p, col);
        cui__solid_vertex(&v[i * 4 + 3], pts[i].x - mx * o, pts[i].y - my * o, trans);
    }
    uint32_t *ix = cui__idx(ui, segs * 18 + caps * 18);
    for (int i = 0; i < segs; i++) {
        uint32_t a = base + (uint32_t)i * 4, b = base + (uint32_t)((i + 1) % n) * 4;
        uint32_t q[18] = { a + 0, b + 0, b + 1, a + 0, b + 1, a + 1,
                           a + 1, b + 1, b + 2, a + 1, b + 2, a + 2,
                           a + 2, b + 2, b + 3, a + 2, b + 3, a + 3 };
        memcpy(ix + i * 18, q, sizeof q);
    }
    if (caps) {
        for (int e = 0; e < 2; e++) {
            int end = e == 0 ? 0 : n - 1;
            /* the outward tangent at this end */
            cui_vec2 tn = e == 0 ? (cui_vec2){ -nrm[0].y, nrm[0].x } : (cui_vec2){ nrm[n - 2].y, -nrm[n - 2].x };
            cui_vec2 t = { -tn.x * aa, -tn.y * aa };
            uint32_t a = base + (uint32_t)end * 4, c = base + (uint32_t)(n * 4 + e * 4);
            for (int k = 0; k < 4; k++) {
                cui_vertex *src = &v[end * 4 + k];
                cui__solid_vertex(&v[n * 4 + e * 4 + k], src->x + t.x, src->y + t.y, trans);
            }
            uint32_t q[18] = { a + 0, c + 0, c + 1, a + 0, c + 1, a + 1,
                               a + 1, c + 1, c + 2, a + 1, c + 2, a + 2,
                               a + 2, c + 2, c + 3, a + 2, c + 3, a + 3 };
            memcpy(ix + segs * 18 + e * 18, q, sizeof q);
        }
    }
}

static cui_vec2 *cui__xf_points(cui_ctx *ui, const cui_vec2 *pts, int n) {
    cui_xform x = cui__xf(ui);
    float s = ui->scale * x.scale;
    cui_vec2 *o = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)n);
    for (int i = 0; i < n; i++) { o[i].x = pts[i].x * s + x.ox; o[i].y = pts[i].y * s + x.oy; }
    return o;
}

void cui_draw_polyline(cui_ctx *ui, const cui_vec2 *pts, int n, float thick, cui_color col, bool closed) {
    col = cui_color_of(ui, col);
    if (n < 2 || col.a == 0) return;
    cui_xform x = cui__xf(ui);
    col = cui__xa(col, x.alpha);
    cui__polyline_px(ui, cui__xf_points(ui, pts, n), n, thick * ui->scale * x.scale, col, closed);
}

void cui_draw_line(cui_ctx *ui, cui_vec2 a, cui_vec2 b, float thick, cui_color col) {
    cui_vec2 p[2] = { a, b };
    cui_draw_polyline(ui, p, 2, thick, col, false);
}

void cui_draw_polygon(cui_ctx *ui, const cui_vec2 *pts, int n, cui_color col) {
    col = cui_color_of(ui, col);
    if (n < 3 || col.a == 0) return;
    cui_xform x = cui__xf(ui);
    col = cui__xa(col, x.alpha);
    cui_vec2 *p = cui__xf_points(ui, pts, n);
    cui__set_texture(ui, 0);
    cui_color trans = col; trans.a = 0;
    /* orientation so that normals point outward */
    float area = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; area += p[i].x * p[j].y - p[j].x * p[i].y; }
    float sign = area < 0 ? -1.0f : 1.0f;
    cui_vec2 *nrm = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 0) { dx /= l; dy /= l; }
        nrm[i].x = dy * sign; nrm[i].y = -dx * sign;
    }
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, n * 2, (n - 2) * 3 + n * 6, &base);
    for (int i = 0; i < n; i++) {
        cui_vec2 na = nrm[(i - 1 + n) % n], nb = nrm[i];
        float mx = (na.x + nb.x) * 0.5f, my = (na.y + nb.y) * 0.5f;
        float d2 = mx * mx + my * my;
        if (d2 > 0.000001f) { float inv = 1.0f / d2; if (inv > 25.0f) inv = 25.0f; mx *= inv; my *= inv; }
        cui__solid_vertex(&v[i * 2 + 0], p[i].x - mx * 0.5f, p[i].y - my * 0.5f, col);
        cui__solid_vertex(&v[i * 2 + 1], p[i].x + mx * 0.5f, p[i].y + my * 0.5f, trans);
    }
    uint32_t *ix = cui__idx(ui, (n - 2) * 3 + n * 6);
    int k = 0;
    for (int i = 2; i < n; i++) { ix[k++] = base; ix[k++] = base + (uint32_t)(i - 1) * 2; ix[k++] = base + (uint32_t)i * 2; }
    for (int i = 0; i < n; i++) {
        uint32_t a = base + (uint32_t)i * 2, b = base + (uint32_t)((i + 1) % n) * 2;
        ix[k++] = a; ix[k++] = b; ix[k++] = b + 1;
        ix[k++] = a; ix[k++] = b + 1; ix[k++] = a + 1;
    }
}

void cui_draw_arc(cui_ctx *ui, cui_vec2 c, float r, float a0, float a1, float thick, cui_color col) {
    float span = fabsf(a1 - a0);
    if (span <= 0.0001f) return;
    int seg = (int)CUI_CLAMP(span * r * ui->scale / 3.0f, 8, 256);
    cui_vec2 *p = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)(seg + 1));
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)seg;
        p[i].x = c.x + cosf(a) * r;
        p[i].y = c.y + sinf(a) * r;
    }
    bool closed = span >= 2 * CUI_PI - 0.001f;
    cui_draw_polyline(ui, p, closed ? seg : seg + 1, thick, col, closed);
}

/* ---- composite helpers used by components ---- */

/* A lit top edge: dark surfaces read as raised when their upper rim catches
 * a little light. Fades out down the sides. */
void cui__draw_top_light(cui_ctx *ui, cui_rect r, cui_corners rad, float alpha) {
    if (r.w < 4 || r.h < 4 || alpha <= 0) return;
    /* ten nested bands, so the light thins out smoothly instead of in visible steps */
    enum { B = 10 };
    float reach = CUI_MIN(r.h * 0.5f, 60.0f);
    for (int k = 1; k <= B; k++) {
        cui_push_clip(ui, (cui_rect){ r.x - 1, r.y - 1, r.w + 2, reach * (float)k / B + 1 });
        cui_draw_rect_border(ui, r, rad, cui__rgba(0, 0, 0, 0), cui__rgba(255, 255, 255, (int)(255 * alpha / B + 0.5f)), 1);
        cui_pop_clip(ui);
    }
}

/* Sheen for solid controls: a lit upper rim that fades down the sides. */
void cui__draw_sheen(cui_ctx *ui, cui_rect r, cui_corners rad, float k) {
    if (r.w < 2 || r.h < 2 || k <= 0) return;
    /* the lit rim fades out down the sides instead of stopping at a line */
    enum { B = 8 };
    for (int i = 1; i <= B; i++) {
        cui_push_clip(ui, (cui_rect){ r.x - 1, r.y - 1, r.w + 2, r.h * 0.42f * (float)i / B + 1 });
        cui_draw_rect_border(ui, r, rad, cui__rgba(0, 0, 0, 0), cui__rgba(255, 255, 255, (int)(255 * 0.16f * k / B + 0.5f)), 1);
        cui_pop_clip(ui);
    }
}


void cui__draw_check(cui_ctx *ui, cui_rect r, float t, float thick, cui_color c) {
    if (t <= 0.001f) return;
    cui_vec2 a = { r.x + r.w * 0.22f, r.y + r.h * 0.53f };
    cui_vec2 b = { r.x + r.w * 0.42f, r.y + r.h * 0.72f };
    cui_vec2 d = { r.x + r.w * 0.78f, r.y + r.h * 0.32f };
    float l1 = hypotf(b.x - a.x, b.y - a.y), l2 = hypotf(d.x - b.x, d.y - b.y);
    float len = (l1 + l2) * cui__clampf(t, 0, 1);
    cui_vec2 pts[3] = { a, b, d };
    int n = 3;
    if (len < l1) {
        float k = len / l1;
        pts[1].x = a.x + (b.x - a.x) * k; pts[1].y = a.y + (b.y - a.y) * k;
        n = 2;
    } else {
        float k = (len - l1) / l2;
        pts[2].x = b.x + (d.x - b.x) * k; pts[2].y = b.y + (d.y - b.y) * k;
    }
    cui_draw_polyline(ui, pts, n, thick, c, false);
}

/** Paint the arc spinner and request frames only while it is moving. */
void cui__draw_spinner(cui_ctx *ui, cui_vec2 c, float radius, float thick, cui_color col) {
    bool motion = ui->theme.motion > 0;
    float t = motion ? (float)ui->time : 0;
    col = cui_color_of(ui, col);
    cui_color track = cui_alpha(col, 0.18f);
    cui_draw_arc(ui, c, radius, 0, 2 * CUI_PI, thick, track);
    float head = t * 5.2f;
    float len = 1.2f + 0.9f * sinf(t * 2.4f);
    cui_draw_arc(ui, c, radius, head, head + len, thick, col);
    if (motion) ui->animating = true;
}

void cui__draw_focus_ring(cui_ctx *ui, cui_rect r, cui_corners rad, float alpha) {
    if (alpha <= 0.01f) return;
    float o = 3;
    cui_rect rr = { r.x - o, r.y - o, r.w + 2 * o, r.h + 2 * o };
    cui_corners cr = { rad.tl + o, rad.tr + o, rad.br + o, rad.bl + o };
    cui_color c = cui_alpha(CUI_C(ui, FOCUS_RING), alpha);
    cui_draw_rect_border(ui, rr, cr, cui__rgba(0, 0, 0, 0), c, 2);
}

void cui__draw_elevation(cui_ctx *ui, cui_rect r, cui_corners rad, cui_elevation e, float alpha) {
    if (e == CUI_ELEVATION_NONE || alpha <= 0) return;
    cui_theme *t = &ui->theme;
    const cui_shadow *sh = e == CUI_ELEVATION_RESTING ? t->shadow_resting : e == CUI_ELEVATION_RAISED ? t->shadow_raised : t->shadow_floating;
    for (int i = 1; i >= 0; i--) {
        if (sh[i].alpha <= 0) continue;
        cui_vec2 off = { 0, sh[i].y };
        cui_draw_shadow(ui, r, rad, sh[i].blur, sh[i].spread, off, cui__rgba(0, 0, 0, (int)(255 * sh[i].alpha * alpha)));
    }
}

void cui__draw_triangles(cui_ctx *ui, const cui_cvert *src, int nv, const uint32_t *idx, int ni) {
    if (nv <= 0 || ni <= 0) return;
    cui_xform x = cui__xf(ui);
    float s = ui->scale * x.scale;
    cui__set_texture(ui, 0);
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, nv, ni, &base);
    for (int i = 0; i < nv; i++) {
        v[i].x = src[i].x * s + x.ox;
        v[i].y = src[i].y * s + x.oy;
        v[i].mode = CUI_SHADE_SOLID;
        v[i].color = cui__xa(cui_color_of(ui, src[i].c), x.alpha);
    }
    uint32_t *ix = cui__idx(ui, ni);
    for (int i = 0; i < ni; i++) ix[i] = base + idx[i];
}

/* ---- ordered dither ---- */

/* what every dither vertex shares: the grid in pixels, the cut, the dot color and the floor */
typedef struct { float cell, gx, gy, phx, phy, cx, cy, hw, hh, fl; cui_corners rr; cui_color c; } cui__dither_px;

static bool cui__dither_setup(cui_ctx *ui, const cui__dither *d, cui__dither_px *g, cui_xform *x) {
    g->c = cui_color_of(ui, d->color);
    if (g->c.a == 0) return false;
    *x = cui__xf(ui);
    float s = ui->scale * x->scale;
    /* whole pixel cells on a whole pixel origin: a pixel center is never on a cell edge, so
     * both renderers pick the same cell for every pixel */
    g->cell = CUI_MAX(1.0f, roundf((d->cell > 0 ? d->cell : 2.0f) * s));
    g->gx = roundf(d->origin.x * s + x->ox); g->gy = roundf(d->origin.y * s + x->oy);
    g->phx = roundf(d->phase.x); g->phy = roundf(d->phase.y);
    g->cx = 0; g->cy = 0; g->hw = 1e6f; g->hh = 1e6f;
    g->rr = (cui_corners){ 0, 0, 0, 0 };
    if (d->shape.w > 0 && d->shape.h > 0) {
        g->hw = d->shape.w * 0.5f * s; g->hh = d->shape.h * 0.5f * s;
        g->cx = (d->shape.x + d->shape.w * 0.5f) * s + x->ox; g->cy = (d->shape.y + d->shape.h * 0.5f) * s + x->oy;
        float m = CUI_MIN(g->hw, g->hh);
        g->rr.tl = CUI_MIN(d->radius.tl * s, m); g->rr.tr = CUI_MIN(d->radius.tr * s, m);
        g->rr.br = CUI_MIN(d->radius.br * s, m); g->rr.bl = CUI_MIN(d->radius.bl * s, m);
    }
    g->c = cui__xa(g->c, x->alpha);
    g->fl = cui__clampf(d->floor, 0, 1);
    cui__set_texture(ui, 0);
    return true;
}

/* one vertex at pixel (px, py); `density` is the byte that the renderers compare thresholds with */
static void cui__dither_vert(cui_vertex *v, const cui__dither_px *g, float px, float py, int density) {
    v->x = px; v->y = py;
    v->u = (px - g->gx) / g->cell + g->phx; v->v = (py - g->gy) / g->cell + g->phy;
    v->lx = px - g->cx; v->ly = py - g->cy; v->hw = g->hw; v->hh = g->hh;
    v->r_tl = g->rr.tl; v->r_tr = g->rr.tr; v->r_br = g->rr.br; v->r_bl = g->rr.bl;
    v->border = g->fl;
    v->mode = CUI_SHADE_DITHER;
    v->color = cui__rgba(255, 255, 255, density);
    v->border_color = g->c;
}

static int cui__dither_byte(float density) { return (int)(cui__clampf(density, 0, 1) * 255.0f + 0.5f); }

void cui__draw_dither_mesh(cui_ctx *ui, const cui__dither *d, const cui__dvert *src, int nv, const uint32_t *idx, int ni) {
    cui__dither_px g;
    cui_xform x;
    if (nv <= 0 || ni <= 0 || !cui__dither_setup(ui, d, &g, &x)) return;
    float s = ui->scale * x.scale;
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, nv, ni, &base);
    for (int i = 0; i < nv; i++) cui__dither_vert(&v[i], &g, src[i].x * s + x.ox, src[i].y * s + x.oy, cui__dither_byte(src[i].density));
    uint32_t *ix = cui__idx(ui, ni);
    for (int i = 0; i < ni; i++) ix[i] = base + idx[i];
}

void cui__draw_dither_cells(cui_ctx *ui, const cui__dither *d, int i0, int j0, int cols, int rows, const float *density) {
    cui__dither_px g;
    cui_xform x;
    if (cols <= 0 || rows <= 0 || !cui__dither_setup(ui, d, &g, &x)) return;
    /* a run of cells with the same density in a row is one quad: count them, then emit them */
    int quads = 0;
    for (int j = 0; j < rows; j++)
        for (int i = 0; i < cols;) {
            int a = cui__dither_byte(density[j * cols + i]), e = i + 1;
            while (e < cols && cui__dither_byte(density[j * cols + e]) == a) e++;
            quads += a > 0;
            i = e;
        }
    if (!quads) return;
    uint32_t base;
    cui_vertex *v = cui__reserve(ui, quads * 4, quads * 6, &base);
    uint32_t *ix = cui__idx(ui, quads * 6);
    int q = 0;
    for (int j = 0; j < rows; j++) {
        float y0 = g.gy + (float)(j0 + j) * g.cell, y1 = y0 + g.cell;
        for (int i = 0; i < cols;) {
            int a = cui__dither_byte(density[j * cols + i]), e = i + 1;
            while (e < cols && cui__dither_byte(density[j * cols + e]) == a) e++;
            if (a > 0) {
                float x0 = g.gx + (float)(i0 + i) * g.cell, x1 = g.gx + (float)(i0 + e) * g.cell;
                cui__dither_vert(&v[q * 4 + 0], &g, x0, y0, a);
                cui__dither_vert(&v[q * 4 + 1], &g, x1, y0, a);
                cui__dither_vert(&v[q * 4 + 2], &g, x1, y1, a);
                cui__dither_vert(&v[q * 4 + 3], &g, x0, y1, a);
                uint32_t b = base + (uint32_t)(q * 4);
                ix[q * 6 + 0] = b; ix[q * 6 + 1] = b + 1; ix[q * 6 + 2] = b + 2;
                ix[q * 6 + 3] = b; ix[q * 6 + 4] = b + 2; ix[q * 6 + 5] = b + 3;
                q++;
            }
            i = e;
        }
    }
}

void cui__draw_dither_rect(cui_ctx *ui, const cui__dither *d, cui_rect r, float d0, float d1, bool vertical) {
    if (r.w <= 0 || r.h <= 0 || (d0 <= 0 && d1 <= 0)) return;
    cui__dvert v[4] = { { r.x, r.y, d0 }, { r.x + r.w, r.y, vertical ? d0 : d1 },
                        { r.x + r.w, r.y + r.h, d1 }, { r.x, r.y + r.h, vertical ? d1 : d0 } };
    static const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    cui__draw_dither_mesh(ui, d, v, 4, idx, 6);
}

void cui__draw_text_at(cui_ctx *ui, cui_font font, float size, float x, float y, cui__text_anchor anchor, cui_color color, const char *s, bool tab) {
    float w = cui__text_width(ui, font, size, s, -1, tab);
    float fh = cui__font_line_height(ui, font, size);
    float tx = anchor == CUI__AT_CENTER ? x - w * 0.5f : anchor == CUI__AT_RIGHT ? x - w : x;
    cui__draw_text_line(ui, font, size, tx, y - fh * 0.5f, color, s, -1, tab);
}
