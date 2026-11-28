/* Damage tracking: what one frame's draw data changes against the frame before, so a
 * renderer redraws that rectangle only.
 *
 * Shapes are in draw order, and between two frames most of that order is the same. The
 * cui_instance records are compared from the front while they match, then from the back
 * while they match; what is left in the middle of either frame is what changed, and its
 * bounding box is the damage. An insertion (a hover highlight) shifts everything after it
 * without damaging it. Two changes far apart in the order damage what lies between them too:
 * the rectangle is never too small, only sometimes larger than it could be.
 *
 * Vertices (lines, polygons, meshes) are not compared shape by shape: when they or their
 * commands change at all, the clip rectangles of the vertex commands of both frames are
 * damaged. A texture whose pixels change under a record that does not is not seen. */
#include "../cui_internal.h"
#include "cui/cui_render.h"
#include "cui_damage.h"
#include <limits.h>

#define CUI__DMG_FULL_SHARE 0.6f   /* a rectangle over this share of the window: draw the window */
#define CUI__DMG_DIRECT_SHARE 0.4f /* frames that change this share of the window, CUI__DMG_DIRECT_RUN of them in a row: draw straight into the window */
#define CUI__DMG_DIRECT_RUN 3
#define CUI__DMG_KEEP_SHARE 0.25f  /* a frame that changes less than this: back to the kept picture */
#define CUI__DMG_RUN_GAP 4         /* records this close to each other are drawn as one run */

static const cui__dmg_box cui__dmg_none = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };

static bool cui__dmg_empty(cui__dmg_box b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

static cui__dmg_box cui__dmg_join(cui__dmg_box a, cui__dmg_box b) {
    if (cui__dmg_empty(b)) return a;
    if (cui__dmg_empty(a)) return b;
    cui__dmg_box r = { CUI_MIN(a.x0, b.x0), CUI_MIN(a.y0, b.y0), CUI_MAX(a.x1, b.x1), CUI_MAX(a.y1, b.y1) };
    return r;
}

static cui__dmg_box cui__dmg_cut(cui__dmg_box a, cui__dmg_box b) {
    cui__dmg_box r = { CUI_MAX(a.x0, b.x0), CUI_MAX(a.y0, b.y0), CUI_MIN(a.x1, b.x1), CUI_MIN(a.y1, b.y1) };
    return r;
}

static cui__dmg_box cui__dmg_of_rect(cui_rect r) {
    cui__dmg_box b = { (int)floorf(r.x), (int)floorf(r.y), (int)ceilf(r.x + r.w), (int)ceilf(r.y + r.h) };
    return b;
}

/* The pixels a record's quad can touch (a pixel more on each side, for its soft edge), inside its clip. */
static cui__dmg_box cui__dmg_of_instance(const cui_instance *in, cui__dmg_box clip) {
    float param = in->param * 0.25f;
    float pad = in->mode == CUI_SHADE_SHADOW ? param * 3.0f : in->mode == CUI_SHADE_TEXT ? 0.0f : 1.0f;
    if (in->mode == CUI_SHADE_LINE) {   /* the two ends, and half the thickness and the soft edge round them */
        float reach = CUI_MAX(param * 0.5f, 0.5f) + 2.0f;
        cui__dmg_box l = { (int)floorf(CUI_MIN(in->x0, in->x1) * 0.25f - reach) - 1, (int)floorf(CUI_MIN(in->y0, in->y1) * 0.25f - reach) - 1,
                           (int)ceilf(CUI_MAX(in->x0, in->x1) * 0.25f + reach) + 1, (int)ceilf(CUI_MAX(in->y0, in->y1) * 0.25f + reach) + 1 };
        return cui__dmg_cut(l, clip);
    }
    /* a sheared quad leans by up to its height (with the margin) times the slant, either way */
    float lean = fabsf(in->shear / 128.0f) * ((in->y1 - in->y0) * 0.25f + 2.0f * pad);
    cui__dmg_box b = { (int)floorf(in->x0 * 0.25f - pad - lean) - 1, (int)floorf(in->y0 * 0.25f - pad) - 1,
                       (int)ceilf(in->x1 * 0.25f + pad + lean) + 1, (int)ceilf(in->y1 * 0.25f + pad) + 1 };
    return cui__dmg_cut(b, clip);
}

static uint64_t cui__dmg_hash(const void *bytes, size_t n, uint64_t h) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (size_t i = n & ~(size_t)7; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static bool cui__dmg_same(const cui_instance *a, const cui_draw_cmd *ca, const cui_instance *b, const cui_draw_cmd *cb) {
    return memcmp(a, b, sizeof *a) == 0 && ca->texture == cb->texture && memcmp(&ca->clip, &cb->clip, sizeof ca->clip) == 0;
}

static cui__dmg_box cui__dmg_span(const cui_instance *inst, const uint16_t *cmd_of, const cui_draw_cmd *cmd, int first, int end) {
    cui__dmg_box b = cui__dmg_none;
    for (int i = first; i < end; i++) {
        b = cui__dmg_join(b, cui__dmg_of_instance(&inst[i], cui__dmg_of_rect(cmd[cmd_of[i]].clip)));
    }
    return b;
}

static bool cui__dmg_reserve(cui_damage *d, int instances, int commands) {
    cui__dmg_store *g = &d->grown;
    if (!d->heap) return instances <= CUI_DAMAGE_RECORDS && commands <= CUI_DAMAGE_COMMANDS;
    if (instances > g->inst_cap) {
        int cap = CUI_MAX(instances * 2, 256);
        cui_instance *a = realloc(g->inst, sizeof *a * (size_t)cap);
        if (a) g->inst = a;
        uint16_t *b = realloc(g->cmd_of[0], sizeof *b * (size_t)cap);
        if (b) g->cmd_of[0] = b;
        uint16_t *c = realloc(g->cmd_of[1], sizeof *c * (size_t)cap);
        if (c) g->cmd_of[1] = c;
        if (!a || !b || !c) return false;
        g->inst_cap = cap;
    }
    if (commands > g->cmd_cap) {
        int cap = CUI_MAX(commands * 2, 64);
        cui_draw_cmd *a = realloc(g->cmd, sizeof *a * (size_t)cap);
        if (!a) return false;
        g->cmd = a;
        g->cmd_cap = cap;
    }
    return true;
}

cui_damage *cui_damage_create(void) {
    cui_damage *d = calloc(1, sizeof *d);
    if (d) d->heap = true;
    return d;
}

void cui_damage_destroy(cui_damage *d) {
    if (!d) return;
    free(d->grown.inst); free(d->grown.cmd); free(d->grown.cmd_of[0]); free(d->grown.cmd_of[1]); free(d->grown.clipped);
    free(d);
}

void cui__damage_init(cui_damage_kept *k) { memset(&k->head, 0, sizeof k->head); }

void cui_damage_reset(cui_damage *d) { if (d) { d->have = false; d->placed = false; } }

cui_damage_stats cui_damage_get_stats(const cui_damage *d) {
    cui_damage_stats none = { 0 };
    return d ? d->stats : none;
}

static bool cui__dmg_track(cui_damage *d, const cui_draw_data *dd, cui_rect *rect, bool *full);

bool cui_damage_track(cui_damage *d, const cui_draw_data *dd, cui_rect *rect, bool *full) {
    uint64_t before = d->stats.pixels;
    if (!cui__dmg_track(d, dd, rect, full)) { d->direct = false; return false; }   /* (a list that is as it was has nothing to draw straight anywhere) */
    float window = (float)dd->width_px * (float)dd->height_px, share = window > 0 ? (float)(d->stats.pixels - before) / window : 1;
    d->large_run = share >= CUI__DMG_DIRECT_SHARE ? d->large_run + 1 : 0;
    if (d->large_run >= CUI__DMG_DIRECT_RUN) d->direct = true;
    else if (share < CUI__DMG_KEEP_SHARE) d->direct = false;
    return true;
}

bool cui_damage_direct(const cui_damage *d) { return d && d->direct; }

static bool cui__dmg_track(cui_damage *d, const cui_draw_data *dd, cui_rect *rect, bool *full) {
    int W = dd->width_px, H = dd->height_px;
    int ni = dd->instance_count > 0 ? dd->instance_count : 0, nc = dd->command_count;
    cui_rect whole = { 0, 0, (float)W, (float)H };
    *rect = whole;
    *full = true;
    d->stats.frames++;
    d->box_count = -1;
    /* (a record's command is kept in 16 bits) */
    if (nc > 65535 || !cui__dmg_reserve(d, ni, nc)) { d->have = false; d->stats.full++; return true; }
    cui_damage_kept *k = (cui_damage_kept *)d;   /* read only when !heap */
    cui_instance *inst = d->heap ? d->grown.inst : k->inst;
    cui_draw_cmd *cmd = d->heap ? d->grown.cmd : k->cmd;
    uint16_t *cmd_of = d->heap ? d->grown.cmd_of[d->of] : k->cmd_of[d->of], *cmd_of_new = d->heap ? d->grown.cmd_of[!d->of] : k->cmd_of[!d->of];

    uint64_t vh = cui__dmg_hash(dd->vertices, sizeof(cui_vertex) * (size_t)dd->vertex_count, 1469598103934665603ull);
    vh = cui__dmg_hash(dd->indices, sizeof(uint32_t) * (size_t)dd->index_count, vh);
    cui__dmg_box vbox = cui__dmg_none;
    for (int i = 0; i < nc; i++) {
        const cui_draw_cmd *c = &dd->commands[i];
        for (uint32_t k = c->instance_offset; k < c->instance_offset + c->instance_count; k++) cmd_of_new[k] = (uint16_t)i;
        if (!c->index_count) continue;
        vh = cui__dmg_hash(c, sizeof *c, vh);
        vbox = cui__dmg_join(vbox, cui__dmg_of_rect(c->clip));
    }

    bool known = d->have && d->width == W && d->height == H && memcmp(&d->clear, &dd->clear_color, sizeof d->clear) == 0;
    cui__dmg_box box = known && vh != d->vertex_hash ? cui__dmg_join(d->vertex_box, vbox) : cui__dmg_none;
    cui__dmg_box window = { 0, 0, W, H };
    if (known) d->box_count = 0;
    bool same = known && ni == d->inst_count && nc == d->cmd_count &&
                memcmp(dd->instances, inst, sizeof(cui_instance) * (size_t)ni) == 0 &&
                memcmp(dd->commands, cmd, sizeof(cui_draw_cmd) * (size_t)nc) == 0;
    if (!same) {
        if (known) {
            int shared = CUI_MIN(ni, d->inst_count), head = 0, tail = 0;
            while (head < shared && cui__dmg_same(&dd->instances[head], &dd->commands[cmd_of_new[head]], &inst[head], &cmd[cmd_of[head]])) head++;
            while (tail < shared - head &&
                   cui__dmg_same(&dd->instances[ni - 1 - tail], &dd->commands[cmd_of_new[ni - 1 - tail]],
                                 &inst[d->inst_count - 1 - tail], &cmd[cmd_of[d->inst_count - 1 - tail]])) tail++;
            if (ni == d->inst_count) {   /* none added or removed: what is between is compared one by one */
                for (int k = head; k < ni - tail; k++) {
                    if (cui__dmg_same(&dd->instances[k], &dd->commands[cmd_of_new[k]], &inst[k], &cmd[cmd_of[k]])) continue;
                    box = cui__dmg_join(box, cui__dmg_span(inst, cmd_of, cmd, k, k + 1));
                    box = cui__dmg_join(box, cui__dmg_span(dd->instances, cmd_of_new, dd->commands, k, k + 1));
                }
            } else {
                box = cui__dmg_join(box, cui__dmg_span(inst, cmd_of, cmd, head, d->inst_count - tail));
                box = cui__dmg_join(box, cui__dmg_span(dd->instances, cmd_of_new, dd->commands, head, ni - tail));
            }
        }
        d->of = !d->of;
        if (ni) memcpy(inst, dd->instances, sizeof(cui_instance) * (size_t)ni);
        if (nc) memcpy(cmd, dd->commands, sizeof(cui_draw_cmd) * (size_t)nc);
        d->inst_count = ni; d->cmd_count = nc;
    }
    d->vertex_hash = vh; d->vertex_box = vbox;
    d->width = W; d->height = H; d->clear = dd->clear_color;
    d->have = true;
    if (!known) { d->stats.full++; d->stats.pixels += (uint64_t)W * (uint64_t)H; return true; }

    box = cui__dmg_cut(box, window);
    if (cui__dmg_empty(box)) { *full = false; rect->w = rect->h = 0; d->stats.unchanged++; return false; }
    d->bound = box;
    d->box_count = 1;
    float area = (float)(box.x1 - box.x0) * (float)(box.y1 - box.y0);
    if (area > CUI__DMG_FULL_SHARE * (float)W * (float)H) { d->stats.full++; d->stats.pixels += (uint64_t)W * (uint64_t)H; return true; }
    *full = false;
    rect->x = (float)box.x0; rect->y = (float)box.y0; rect->w = (float)(box.x1 - box.x0); rect->h = (float)(box.y1 - box.y0);
    d->stats.partial++;
    d->stats.pixels += (uint64_t)area;
    return true;
}

int cui_damage_boxes(const cui_damage *d, cui_rect *out, int cap) {
    if (!d || d->box_count < 0 || d->box_count > cap) return -1;
    cui__dmg_box b = d->bound;
    if (d->box_count) out[0] = (cui_rect){ (float)b.x0, (float)b.y0, (float)(b.x1 - b.x0), (float)(b.y1 - b.y0) };
    return d->box_count;
}

/* Adds to out, from `kept` on, the commands and records of dd that touch box, clips cut to it. */
static int cui__dmg_clip_into(cui_draw_cmd *out, const cui_draw_data *dd, cui__dmg_box box, int kept) {
    for (int i = 0; i < dd->command_count; i++) {
        cui_draw_cmd c = dd->commands[i];
        cui__dmg_box clip = cui__dmg_cut(cui__dmg_of_rect(c.clip), box);
        if (cui__dmg_empty(clip)) continue;
        cui_rect cut = { (float)clip.x0, (float)clip.y0, (float)(clip.x1 - clip.x0), (float)(clip.y1 - clip.y0) };
        c.clip = cut;
        if (!c.instance_count) { out[kept++] = c; continue; }
        /* only the runs of records that touch the box are drawn */
        uint32_t end = c.instance_offset + c.instance_count, first = end, last = end;
        for (uint32_t k = c.instance_offset; k < end; k++) {
            if (cui__dmg_empty(cui__dmg_of_instance(&dd->instances[k], clip))) continue;
            if (first != end && k - last > CUI__DMG_RUN_GAP) {
                c.instance_offset = first; c.instance_count = last - first + 1;
                out[kept++] = c;
                first = end;
            }
            if (first == end) first = k;
            last = k;
        }
        if (first == end) continue;
        c.instance_offset = first; c.instance_count = last - first + 1;
        out[kept++] = c;
    }
    return kept;
}

static bool cui__dmg_clip_room(cui_damage *d, const cui_draw_data *dd, int boxes) {
    int need = cui__damage_clip_need(dd, boxes);
    if (need <= d->grown.clipped_cap) return true;
    if (!d->heap) return false;   /* a pool tracker owns no memory: its renderer clips */
    cui_draw_cmd *a = realloc(d->grown.clipped, sizeof *a * (size_t)(need * 2));
    if (!a) return false;
    d->grown.clipped = a;
    d->grown.clipped_cap = need * 2;
    return true;
}

int cui__damage_clip_need(const cui_draw_data *dd, int boxes) {
    return (dd->command_count + (dd->instance_count > 0 ? dd->instance_count : 0)) * boxes;
}

int cui__damage_clip_to(const cui_draw_data *dd, const cui_rect *rects, int count, cui_draw_cmd *out) {
    int kept = 0;
    for (int i = 0; i < count; i++) kept = cui__dmg_clip_into(out, dd, cui__dmg_of_rect(rects[i]), kept);
    return kept;
}

const cui_draw_data *cui_damage_clip(cui_damage *d, const cui_draw_data *dd, cui_rect rect) {
    if (!cui__dmg_clip_room(d, dd, 1)) return dd;   /* drawn whole, cut by the caller's scissor only where commands allow: still right */
    d->clipped_data = *dd;
    d->clipped_data.commands = d->grown.clipped;
    d->clipped_data.command_count = cui__dmg_clip_into(d->grown.clipped, dd, cui__dmg_of_rect(rect), 0);
    return &d->clipped_data;
}

const cui_draw_data *cui_damage_clip_boxes(cui_damage *d, const cui_draw_data *dd, cui_rect *out, int *count) {
    cui__dmg_box b = d->bound;
    if (!cui__dmg_clip_room(d, dd, 1)) return NULL;
    out[0] = (cui_rect){ (float)b.x0, (float)b.y0, (float)(b.x1 - b.x0), (float)(b.y1 - b.y0) };
    *count = 1;
    d->clipped_data = *dd;
    d->clipped_data.commands = d->grown.clipped;
    d->clipped_data.command_count = cui__dmg_clip_into(d->grown.clipped, dd, b, 0);
    return &d->clipped_data;
}

const cui_draw_data *cui_damage_clip_rects(cui_damage *d, const cui_draw_data *dd, const cui_rect *rects, int count) {
    if (!d || !dd || count < 1 || !cui__dmg_clip_room(d, dd, count)) return NULL;
    d->clipped_data = *dd;
    d->clipped_data.commands = d->grown.clipped;
    d->clipped_data.command_count = cui__damage_clip_to(dd, rects, count, d->grown.clipped);
    return &d->clipped_data;
}

int cui_damage_track_lists(const cui_render_list *lists, int count, cui_rect *boxes, int cap) {
    if (!lists || count < 1 || !lists[0].draw || !lists[0].damage) return -1;
    bool whole = false;
    int n = 0;
    for (int i = 0; i < count; i++) {
        cui_damage *d = lists[i].damage;
        const cui_draw_data *dd = lists[i].draw;
        cui_rect rect, own[CUI_DAMAGE_BOXES];
        bool full = false, changed = cui_damage_track(d, dd, &rect, &full);
        int x = lists[i].x, y = lists[i].y, w = dd->width_px, h = dd->height_px;
        bool moved = !d->placed || d->at_x != x || d->at_y != y || d->at_w != w || d->at_h != h;
        cui_rect was = { (float)d->at_x, (float)d->at_y, (float)d->at_w, (float)d->at_h }, is = { (float)x, (float)y, (float)w, (float)h };
        bool had = d->placed;
        d->placed = true; d->at_x = x; d->at_y = y; d->at_w = w; d->at_h = h;
        if (i == 0 && (moved || (changed && full))) { whole = true; continue; }   /* the first list is the target's own */
        int k = changed && !full && !moved ? cui_damage_boxes(d, own, CUI_DAMAGE_BOXES) : 0;
        if (k < 0 || n + k + 2 > cap) { whole = true; continue; }
        if (moved && had) boxes[n++] = was;          /* where it lay: what was under it shows again */
        if (moved || (changed && full)) boxes[n++] = is;
        for (int b = 0; b < k; b++) { own[b].x += (float)x; own[b].y += (float)y; boxes[n++] = own[b]; }
    }
    if (whole) return -1;
    /* one box where two meet: what lies in both would be drawn twice */
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            cui_rect p = boxes[a], q = boxes[b];
            if (p.x >= q.x + q.w || q.x >= p.x + p.w || p.y >= q.y + q.h || q.y >= p.y + p.h) continue;
            float x0 = p.x < q.x ? p.x : q.x, y0 = p.y < q.y ? p.y : q.y;
            float x1 = p.x + p.w > q.x + q.w ? p.x + p.w : q.x + q.w, y1 = p.y + p.h > q.y + q.h ? p.y + p.h : q.y + q.h;
            boxes[a] = (cui_rect){ x0, y0, x1 - x0, y1 - y0 };
            boxes[b] = boxes[--n];
            a = -1;   /* the larger box may now meet one it did not before */
            break;
        }
    }
    float area = 0;
    for (int i = 0; i < n; i++) area += boxes[i].w * boxes[i].h;
    if (area > CUI__DMG_FULL_SHARE * (float)lists[0].draw->width_px * (float)lists[0].draw->height_px) return -1;
    return n;
}

#if defined(CUI_VULKAN) || defined(CUI_D3D12) || defined(CUI_METAL)
typedef struct cui__presented { cui_damage *damage; cui_id id; int x, y, w, h, order; bool shown; } cui__presented;
#define CUI__PRESENT_MAX_DELTA 0.1f   /* a frame after a long rest advances animations by no more */
struct cui_presenter {
    cui_damage *damage;             /* the window's own list's tracker */
    bool owned;                     /* damage is the presenter's to destroy */
    cui_presenter_desc desc;
    cui__presented zone[CUI_SURFACES];
    double last, wake;              /* when the last frame began; when the next was asked for, 0: not */
    float w, h, scale;              /* the last frame's */
    bool dirty, whole;              /* a frame is asked for; the next is shown whole */
};

cui_presenter *cui_presenter_create(const cui_presenter_desc *desc) {
    cui_presenter *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (desc) p->desc = *desc;
    p->damage = p->desc.window;
    if (!p->damage) {
        p->owned = true;
        if (!(p->damage = cui_damage_create())) { free(p); return NULL; }
    }
    p->dirty = p->whole = true;
    return p;
}

void cui_presenter_destroy(cui_presenter *p) {
    if (!p) return;
    for (int i = 0; i < CUI_SURFACES; i++) cui_damage_destroy(p->zone[i].damage);
    if (p->owned) cui_damage_destroy(p->damage);
    free(p);
}

static void cui__presenter_hide(cui_presenter *p, cui_gpu_renderer *r, int slot) {
    cui__presented *z = &p->zone[slot];
    cui_gpu_surface_hide(r, slot);
    if (p->desc.hidden) p->desc.hidden(p->desc.user, slot);
    cui_damage_reset(z->damage);
    z->shown = false;
}

/* A shown slot's tracker: the pool's, kept with the element's pictures, else the presenter's. */
static cui_damage *cui__presenter_tracker(cui_presenter *p, cui_gpu_renderer *r, int slot) {
    cui_damage *pooled = r && p->zone[slot].shown ? cui_gpu_surface_damage(r, slot, p->zone[slot].id) : NULL;
    return pooled ? pooled : p->zone[slot].damage;
}

int cui_presenter_show(cui_presenter *p, cui_gpu_renderer *r, cui_ctx *ui, bool force) {
    if (!p || !r || !ui) return 0;
    int shown = cui_gpu_render_damaged(r, cui_get_draw_data(ui), p->damage, force) ? CUI_PRESENTED_WINDOW : 0;
    for (int slot = 0; slot < CUI_SURFACES; slot++) {
        cui__presented *z = &p->zone[slot];
        cui_surface sf;
        if (!cui_get_surface(ui, slot, &sf)) {
            if (z->shown) { cui__presenter_hide(p, r, slot); shown |= CUI_PRESENTED_SURFACES; }
            continue;
        }
        cui_damage *pooled = cui_gpu_surface_damage(r, slot, sf.id), *d = pooled ? pooled : z->damage;   /* pooled: outlives a hide */
        if (!d && !(d = z->damage = cui_damage_create())) continue;
        cui_rect rect;
        bool full = false;
        bool fresh = !z->shown || (pooled && z->id != sf.id);
        bool moved = fresh || z->x != sf.x || z->y != sf.y || z->order != sf.order;
        bool tracked = cui_damage_track(d, sf.draw, &rect, &full);
        bool whole = full || force || (!pooled && (!z->shown || z->w != sf.w || z->h != sf.h));
        bool changed = tracked || whole || fresh;
        if (p->desc.surface && (changed || moved)) cui_gpu_surface_window(r, slot, p->desc.surface(p->desc.user, slot, sf.x, sf.y, sf.w, sf.h, sf.order));
        if (changed && !(z->shown = cui_gpu_surface_draw(r, slot, sf.draw, whole ? NULL : d, sf.x, sf.y, sf.order))) {
            cui_damage_reset(d);   /* tracked and not drawn */
            shown |= CUI_PRESENTED_LOST;
        }
        else if (!changed && moved) cui_gpu_surface_place(r, slot, sf.x, sf.y, sf.order);
        if (changed || moved) shown |= CUI_PRESENTED_SURFACES;
        z->id = sf.id; z->x = sf.x; z->y = sf.y; z->w = sf.w; z->h = sf.h; z->order = sf.order;
    }
    cui_gpu_surfaces_end(r);
    if (cui_gpu_left_out(r)) {
        cui_damage_reset(p->damage);
        for (int i = 0; i < CUI_SURFACES; i++) cui_damage_reset(cui__presenter_tracker(p, r, i));
        cui_request_redraw(ui);
    }
    float spare = cui_gpu_next_wakeup(r);   /* when the pool frees a spare picture */
    if (spare >= 0) cui_request_wakeup(ui, spare);
    return shown;
}

void cui_presenter_reset(cui_presenter *p, cui_gpu_renderer *r) {
    if (!p) return;
    for (int slot = 0; slot < CUI_SURFACES; slot++) {
        cui_damage_reset(cui__presenter_tracker(p, r, slot));
        if (p->zone[slot].shown && r) cui__presenter_hide(p, r, slot);
    }
    cui_damage_reset(p->damage);
    p->whole = true;
}

void cui_presenter_invalidate(cui_presenter *p, bool whole) {
    if (!p) return;
    p->dirty = true;
    p->whole = p->whole || whole;
}

bool cui_presenter_due(const cui_presenter *p, cui_ctx *ui, float w, float h, float scale, double now) {
    if (!p || !ui || w <= 0 || h <= 0) return false;
    bool sized = w != p->w || h != p->h || scale != p->scale;
    bool woke = p->wake > 0 && now >= p->wake;
    return p->dirty || sized || woke || cui_needs_redraw(ui);
}

bool cui_presenter_begin(cui_presenter *p, cui_ctx *ui, float w, float h, float scale, double now) {
    if (!cui_presenter_due(p, ui, w, h, scale, now)) return false;
    float delta = p->last > 0 ? (float)(now - p->last) : 0;
    if (delta > CUI__PRESENT_MAX_DELTA) delta = CUI__PRESENT_MAX_DELTA;
    if (w != p->w || h != p->h || scale != p->scale) p->whole = true;
    p->w = w; p->h = h; p->scale = scale;
    p->last = now;
    p->dirty = false;
    cui_set_viewport(ui, w, h, scale);
    cui_begin_frame(ui, delta);
    return true;
}

bool cui_presenter_end(cui_presenter *p, cui_gpu_renderer *r, cui_ctx *ui, double now) {
    if (!p || !ui) return false;
    cui_end_frame(ui);
    int shown = cui_presenter_show(p, r, ui, p->whole);
    float next = cui_next_wakeup(ui);   /* after the show: the pool may have asked for one */
    p->wake = next >= 0 ? now + next : 0;
    if (!shown) return false;
    p->whole = false;
    return true;
}
#endif
