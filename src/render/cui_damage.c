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
#include <limits.h>

#define CUI__DMG_FULL_SHARE 0.6f   /* a rectangle over this share of the window: draw the window */
#define CUI__DMG_RUN_GAP 4         /* records this close to each other are drawn as one run */

typedef struct { int x0, y0, x1, y1; } cui__dmg_box;

struct cui_damage {
    cui_instance *inst;             /* the last frame's records and commands */
    cui_draw_cmd *cmd;
    uint16_t *cmd_of, *cmd_of_new;  /* the command a record belongs to: last frame's, this frame's */
    int inst_cap, cmd_cap;
    int inst_count, cmd_count;
    cui_draw_cmd *clipped;          /* cui_damage_clip's commands */
    int clipped_cap;
    cui_draw_data clipped_data;
    uint64_t vertex_hash;
    cui__dmg_box vertex_box;
    int width, height;
    cui_color clear;
    bool have;
    cui_damage_stats stats;
};

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
    cui__dmg_box b = { (int)floorf(in->x0 * 0.25f - pad) - 1, (int)floorf(in->y0 * 0.25f - pad) - 1,
                       (int)ceilf(in->x1 * 0.25f + pad) + 1, (int)ceilf(in->y1 * 0.25f + pad) + 1 };
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
    for (int i = first; i < end; i++) b = cui__dmg_join(b, cui__dmg_of_instance(&inst[i], cui__dmg_of_rect(cmd[cmd_of[i]].clip)));
    return b;
}

static bool cui__dmg_reserve(cui_damage *d, int instances, int commands) {
    if (instances > d->inst_cap) {
        int cap = CUI_MAX(instances * 2, 256);
        cui_instance *a = realloc(d->inst, sizeof *a * (size_t)cap);
        if (a) d->inst = a;
        uint16_t *b = realloc(d->cmd_of, sizeof *b * (size_t)cap);
        if (b) d->cmd_of = b;
        uint16_t *c = realloc(d->cmd_of_new, sizeof *c * (size_t)cap);
        if (c) d->cmd_of_new = c;
        if (!a || !b || !c) return false;
        d->inst_cap = cap;
    }
    if (commands > d->cmd_cap) {
        int cap = CUI_MAX(commands * 2, 64);
        cui_draw_cmd *a = realloc(d->cmd, sizeof *a * (size_t)cap);
        if (!a) return false;
        d->cmd = a;
        d->cmd_cap = cap;
    }
    return true;
}

cui_damage *cui_damage_create(void) { return calloc(1, sizeof(cui_damage)); }

void cui_damage_destroy(cui_damage *d) {
    if (!d) return;
    free(d->inst); free(d->cmd); free(d->cmd_of); free(d->cmd_of_new); free(d->clipped);
    free(d);
}

void cui_damage_reset(cui_damage *d) { if (d) d->have = false; }

cui_damage_stats cui_damage_get_stats(const cui_damage *d) {
    cui_damage_stats none = { 0 };
    return d ? d->stats : none;
}

bool cui_damage_track(cui_damage *d, const cui_draw_data *dd, cui_rect *rect, bool *full) {
    int W = dd->width_px, H = dd->height_px;
    int ni = dd->instance_count > 0 ? dd->instance_count : 0, nc = dd->command_count;
    cui_rect whole = { 0, 0, (float)W, (float)H };
    *rect = whole;
    *full = true;
    d->stats.frames++;
    /* (a record's command is kept in 16 bits) */
    if (nc > 65535 || !cui__dmg_reserve(d, ni, nc)) { d->have = false; d->stats.full++; return true; }

    uint64_t vh = cui__dmg_hash(dd->vertices, sizeof(cui_vertex) * (size_t)dd->vertex_count, 1469598103934665603ull);
    vh = cui__dmg_hash(dd->indices, sizeof(uint32_t) * (size_t)dd->index_count, vh);
    cui__dmg_box vbox = cui__dmg_none;
    for (int i = 0; i < nc; i++) {
        const cui_draw_cmd *c = &dd->commands[i];
        for (uint32_t k = c->instance_offset; k < c->instance_offset + c->instance_count; k++) d->cmd_of_new[k] = (uint16_t)i;
        if (!c->index_count) continue;
        vh = cui__dmg_hash(c, sizeof *c, vh);
        vbox = cui__dmg_join(vbox, cui__dmg_of_rect(c->clip));
    }

    bool known = d->have && d->width == W && d->height == H && memcmp(&d->clear, &dd->clear_color, sizeof d->clear) == 0;
    cui__dmg_box box = known && vh != d->vertex_hash ? cui__dmg_join(d->vertex_box, vbox) : cui__dmg_none;
    bool same = known && ni == d->inst_count && nc == d->cmd_count &&
                memcmp(dd->instances, d->inst, sizeof(cui_instance) * (size_t)ni) == 0 &&
                memcmp(dd->commands, d->cmd, sizeof(cui_draw_cmd) * (size_t)nc) == 0;
    if (!same) {
        if (known) {
            int shared = CUI_MIN(ni, d->inst_count), head = 0, tail = 0;
            while (head < shared && cui__dmg_same(&dd->instances[head], &dd->commands[d->cmd_of_new[head]], &d->inst[head], &d->cmd[d->cmd_of[head]])) head++;
            while (tail < shared - head &&
                   cui__dmg_same(&dd->instances[ni - 1 - tail], &dd->commands[d->cmd_of_new[ni - 1 - tail]],
                                 &d->inst[d->inst_count - 1 - tail], &d->cmd[d->cmd_of[d->inst_count - 1 - tail]])) tail++;
            box = cui__dmg_join(box, cui__dmg_span(d->inst, d->cmd_of, d->cmd, head, d->inst_count - tail));
            box = cui__dmg_join(box, cui__dmg_span(dd->instances, d->cmd_of_new, dd->commands, head, ni - tail));
        }
        uint16_t *swap = d->cmd_of; d->cmd_of = d->cmd_of_new; d->cmd_of_new = swap;
        if (ni) memcpy(d->inst, dd->instances, sizeof(cui_instance) * (size_t)ni);
        if (nc) memcpy(d->cmd, dd->commands, sizeof(cui_draw_cmd) * (size_t)nc);
        d->inst_count = ni; d->cmd_count = nc;
    }
    d->vertex_hash = vh; d->vertex_box = vbox;
    d->width = W; d->height = H; d->clear = dd->clear_color;
    d->have = true;
    if (!known) { d->stats.full++; d->stats.pixels += (uint64_t)W * (uint64_t)H; return true; }

    cui__dmg_box window = { 0, 0, W, H };
    box = cui__dmg_cut(box, window);
    if (cui__dmg_empty(box)) { *full = false; rect->w = rect->h = 0; d->stats.unchanged++; return false; }
    float area = (float)(box.x1 - box.x0) * (float)(box.y1 - box.y0);
    if (area > CUI__DMG_FULL_SHARE * (float)W * (float)H) { d->stats.full++; d->stats.pixels += (uint64_t)W * (uint64_t)H; return true; }
    *full = false;
    rect->x = (float)box.x0; rect->y = (float)box.y0; rect->w = (float)(box.x1 - box.x0); rect->h = (float)(box.y1 - box.y0);
    d->stats.partial++;
    d->stats.pixels += (uint64_t)area;
    return true;
}

const cui_draw_data *cui_damage_clip(cui_damage *d, const cui_draw_data *dd, cui_rect rect) {
    int need = dd->command_count + (dd->instance_count > 0 ? dd->instance_count : 0);
    if (need > d->clipped_cap) {
        cui_draw_cmd *a = realloc(d->clipped, sizeof *a * (size_t)(need * 2));
        if (!a) return dd;   /* drawn whole, cut by the caller's scissor only where commands allow: still right */
        d->clipped = a;
        d->clipped_cap = need * 2;
    }
    cui__dmg_box box = cui__dmg_of_rect(rect);
    int kept = 0;
    for (int i = 0; i < dd->command_count; i++) {
        cui_draw_cmd c = dd->commands[i];
        cui__dmg_box clip = cui__dmg_cut(cui__dmg_of_rect(c.clip), box);
        if (cui__dmg_empty(clip)) continue;
        cui_rect cut = { (float)clip.x0, (float)clip.y0, (float)(clip.x1 - clip.x0), (float)(clip.y1 - clip.y0) };
        c.clip = cut;
        if (!c.instance_count) { d->clipped[kept++] = c; continue; }
        /* only the runs of records that touch the rectangle are drawn */
        uint32_t end = c.instance_offset + c.instance_count, first = end, last = end;
        for (uint32_t k = c.instance_offset; k < end; k++) {
            if (cui__dmg_empty(cui__dmg_of_instance(&dd->instances[k], clip))) continue;
            if (first != end && k - last > CUI__DMG_RUN_GAP) {
                c.instance_offset = first; c.instance_count = last - first + 1;
                d->clipped[kept++] = c;
                first = end;
            }
            if (first == end) first = k;
            last = k;
        }
        if (first == end) continue;
        c.instance_offset = first; c.instance_count = last - first + 1;
        d->clipped[kept++] = c;
    }
    d->clipped_data = *dd;
    d->clipped_data.commands = d->clipped;
    d->clipped_data.command_count = kept;
    return &d->clipped_data;
}
