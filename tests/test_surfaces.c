/* Tests for surfaces (cui_box.surface, cui_surface_begin): that a zone of the window gets a draw
 * list of its own, from its own corner and of its own size; that it keeps its slot from frame to
 * frame and gives it up when it is gone; that the frame as one list still holds everything; and
 * what the damage tracker makes of two changes far apart. Runs headless with the software renderer. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static const cui_color RED = { 255, 0, 0, 255 }, GREEN = { 0, 255, 0, 255 }, BLUE = { 0, 0, 255, 255 };
static bool g_zoned, g_second;
static int g_many;

/* red in the window; green in a surface over part of it; then blue in the window over part of the green: the surface still shows over it */
static void paint(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    cui_draw_rect(ui, (cui_rect){ r.x + 10, r.y + 10, 100, 100 }, CUI_CORNERS(0), RED);
    cui_rect zone = { r.x + 60, r.y + 20, 100, 80 };
    if (g_zoned) cui_surface_begin(ui, "green", zone);
    cui_draw_rect(ui, (cui_rect){ r.x + 40, r.y + 10, 200, 100 }, CUI_CORNERS(0), GREEN);   /* wider than its zone: cut to it */
    if (g_zoned) cui_surface_end(ui);
    cui_draw_rect(ui, (cui_rect){ r.x + 110, r.y + 10, 100, 100 }, CUI_CORNERS(0), BLUE);
    if (g_second) {
        cui_surface_begin(ui, "second", (cui_rect){ r.x + 200, r.y + 150, 40, 40 });
        cui_draw_rect(ui, (cui_rect){ r.x + 200, r.y + 150, 40, 40 }, CUI_CORNERS(0), RED);
        cui_surface_end(ui);
    }
    for (int i = 0; i < g_many; i++) {
        char id[16];
        snprintf(id, sizeof id, "many%d", i);
        cui_rect one = { r.x + 4 + (float)i * 12, r.y + 200, 10, 10 };
        cui_surface_begin(ui, id, one);
        cui_draw_rect(ui, one, CUI_CORNERS(0), GREEN);
        cui_surface_end(ui);
    }
}

static void frame(cui_ctx *ui, bool box) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_canvas(ui, "c", CUI_FILL, box ? CUI_PX(200) : CUI_FILL, paint, NULL, 0);
    if (box) {
        cui_box zone = { .width = CUI_PX(120), .height = CUI_PX(24), .bg = BLUE, .surface = true };
        cui_column_begin(ui, &zone);
        cui_label(ui, "In a box");
        cui_box_end(ui);
    }
    cui_end_frame(ui);
}

static int shapes(const cui_draw_data *dd, bool instanced) { return instanced ? dd->instance_count : dd->vertex_count / 4; }
static int surfaces(cui_ctx *ui) { int n = 0; for (int i = 0; i < CUI_SURFACES; i++) n += cui_get_surface(ui, i, NULL); return n; }

static void pixel(cui_sw_renderer *sw, const cui_draw_data *dd, int x, int y, unsigned char out[4]) {
    unsigned char *px = malloc((size_t)dd->width_px * (size_t)dd->height_px * 4);
    cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    memcpy(out, px + ((size_t)y * (size_t)dd->width_px + (size_t)x) * 4, 4);
    free(px);
}

static void run(bool instanced, float scale) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK, .instanced_quads = instanced });
    cui_set_viewport(ui, 320, 240, scale);
    int s = (int)scale;
    unsigned char p[4];

    g_zoned = false;
    frame(ui, false);
    CHECK_EQ(surfaces(ui), 0);
    const cui_draw_data *all = cui__draw_data_merged(ui), *window = cui_get_draw_data(ui);
    CHECK(all->vertices == window->vertices && all->instances == window->instances);   /* nothing is copied when there is no surface */
    int plain = shapes(all, instanced);
    pixel(sw, all, 130 * s, 50 * s, p);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255);   /* no surface: the blue, drawn last, is on top */

    g_zoned = true;
    frame(ui, false);
    CHECK_EQ(surfaces(ui), 1);
    cui_surface z;
    int slot = -1;
    for (int i = 0; i < CUI_SURFACES; i++) if (cui_get_surface(ui, i, &z)) slot = i;
    CHECK(slot >= 0);
    CHECK_EQ(z.x, 60 * s); CHECK_EQ(z.y, 20 * s); CHECK_EQ(z.w, 100 * s); CHECK_EQ(z.h, 80 * s);
    CHECK_EQ(z.draw->width_px, 100 * s); CHECK_EQ(z.draw->height_px, 80 * s);
    CHECK_EQ(z.draw->clear_color.a, 0);   /* clear where it draws nothing */
    CHECK_EQ(shapes(z.draw, instanced), 1);
    all = cui__draw_data_merged(ui); window = cui_get_draw_data(ui);
    CHECK_EQ(shapes(window, instanced), plain - 1);
    CHECK_EQ(shapes(all, instanced), plain);
    for (int k = 0; k < z.draw->command_count; k++) {   /* its commands are cut to it, from its own corner */
        cui_rect c = z.draw->commands[k].clip;
        CHECK(c.x >= 0 && c.y >= 0 && c.x + c.w <= (float)z.w && c.y + c.h <= (float)z.h);
    }
    pixel(sw, all, 130 * s, 50 * s, p);
    CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0);   /* the lists in order: the surface over the blue drawn after it */
    pixel(sw, all, 130 * s, 15 * s, p);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255);   /* above the zone the green is cut away */
    pixel(sw, window, 130 * s, 50 * s, p);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255);   /* the window's own list: without the surface */
    pixel(sw, z.draw, 5 * s, 5 * s, p);
    CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);   /* the surface's own list, drawn into a picture of its size */

    g_second = true;
    frame(ui, false);
    CHECK_EQ(surfaces(ui), 2);
    cui_surface again, second;
    CHECK(cui_get_surface(ui, slot, &again) && again.id == z.id);   /* the same slot the frame after */
    int other = -1;
    for (int i = 0; i < CUI_SURFACES; i++) if (i != slot && cui_get_surface(ui, i, &second)) other = i;
    CHECK(other >= 0 && second.order > again.order);   /* begun later: over the first */
    g_second = false;
    frame(ui, false);
    CHECK_EQ(surfaces(ui), 1);
    CHECK(!cui_get_surface(ui, other, NULL));   /* gone: its slot is free */

    g_many = CUI_SURFACES + 4;   /* more than there are: the rest are drawn where they are */
    frame(ui, false);
    CHECK_EQ(surfaces(ui), CUI_SURFACES);
    CHECK_EQ(shapes(cui__draw_data_merged(ui), instanced), plain + g_many);
    g_many = 0;
    frame(ui, false);   /* their slots are free the frame after they are gone */

    frame(ui, true);   /* a box that is a surface: its background and what is in it */
    CHECK_EQ(surfaces(ui), 2);
    bool found = false;
    for (int i = 0; i < CUI_SURFACES; i++) {
        if (i == slot || !cui_get_surface(ui, i, &second)) continue;
        found = true;
        CHECK_EQ(second.w, 120 * s); CHECK_EQ(second.h, 24 * s);
        CHECK(shapes(second.draw, instanced) > 1);
        pixel(sw, second.draw, second.w - 2, 2, p);
        CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255);
    }
    CHECK(found);

    /* an unbalanced begin does not carry into the next frame */
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_surface_begin(ui, "left open", (cui_rect){ 0, 0, 10, 10 });
    cui_end_frame(ui);
    g_zoned = false;
    frame(ui, false);
    CHECK_EQ(surfaces(ui), 0);

    cui_destroy(ui);
    cui_sw_destroy(sw);
}

/* Two changes far apart are one rectangle round both, the one box to draw. */
static void boxes(void) {
    cui_instance a[3] = { 0 }, b[3];
    for (int i = 0; i < 3; i++) {
        a[i].x0 = (int16_t)(40 + i * 1200); a[i].y0 = 40; a[i].x1 = (int16_t)(a[i].x0 + 40); a[i].y1 = 80;   /* 10 pixel squares, 300 pixels apart */
        a[i].mode = CUI_SHADE_RRECT;
        a[i].color_top = a[i].color_bottom = (cui_color){ 255, 255, 255, 255 };
    }
    memcpy(b, a, sizeof a);
    cui_draw_cmd cmd = { .clip = { 0, 0, 800, 100 }, .instance_count = 3 };
    cui_draw_data dd = { .commands = &cmd, .command_count = 1, .width_px = 800, .height_px = 100, .clear_color = { 0, 0, 0, 255 }, .instances = a, .instance_count = 3 };
    cui_damage *d = cui_damage_create();
    cui_rect rect, out[CUI_DAMAGE_BOXES];
    bool full = false;
    CHECK(cui_damage_track(d, &dd, &rect, &full) && full);
    CHECK_EQ(cui_damage_boxes(d, out, CUI_DAMAGE_BOXES), -1);   /* the first frame: all of it */
    CHECK(!cui_damage_track(d, &dd, &rect, &full));
    CHECK_EQ(cui_damage_boxes(d, out, CUI_DAMAGE_BOXES), 0);
    b[0].color_top.a = 128; b[2].color_top.a = 128;
    dd.instances = b;
    CHECK(cui_damage_track(d, &dd, &rect, &full));
    CHECK(rect.w > 600);   /* the rectangle spans all three */
    CHECK_EQ(cui_damage_boxes(d, out, CUI_DAMAGE_BOXES), 1);
    CHECK(out[0].x == rect.x && out[0].y == rect.y && out[0].w == rect.w && out[0].h == rect.h);
    cui_rect part[CUI_DAMAGE_BOXES];
    int parts = 0;
    const cui_draw_data *cut = cui_damage_clip_boxes(d, &dd, part, &parts);
    CHECK(cut && parts == 1 && part[0].w == rect.w);
    CHECK_EQ(cui_damage_boxes(d, out, 0), -1);
    cui_damage_destroy(d);
}

int main(void) {
    boxes();
    run(false, 1);
    run(true, 1);
    run(true, 2);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
