#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Dirty rectangle test and bench: one window, used the ways a window is used, to check that
 * damage tracking redraws the right pixels and to see what each use costs.
 *
 * The window is an app's: document tabs, a toolbar, a list with a light on every row, a long
 * text that scrolls with a row of pictures and a card of translucent panels in it, a panel of
 * switches with a chart and a moving picture, and a status bar. So one window holds what is
 * cheap to redraw and what is dear: flat panels, thousands of glyphs, pictures, and shadowed
 * panels blended over one another.
 *
 * It is then used, one thing after another (dr_acts). Most are what a user sees all day, and a
 * figure for ordinary use weighs each by how common it is; the last few are rare, or there
 * because they are the worst a tracker meets:
 *   idle     nothing                               caret    a caret blinks in the text
 *   typing   a line grows letter by letter         hover    the pointer sweeps the toolbar
 *   list     the pointer sweeps the list           scroll   the text scrolls
 *   spinner  a spinner turns in the toolbar        video    the picture changes every frame
 *   clock    a counter in the status bar counts    chart    the chart takes new values
 *   popover  a shadowed panel fades in and out over the text
 *   tooltip  a small panel follows the pointer     tabdrag  a tab is dragged along the strip
 *   far      a spinner at the top right and a progress bar at the bottom left: a rectangle round both is the window
 *   lights   the light of every list row blinks
 *   glass    one light blinks on the card of translucent panels: a small change over the dearest pixels
 *   sweep    a bar crosses the text, the pictures and the card
 *   insert   shapes are added and removed early in the list while the caret blinks late in it
 *   flash    the whole window changes shade
 *   busy     hover, spinner, lights, video and clock at once
 *
 *   cui_test_dirty_rect [-v]
 * Headless, with the software renderer (what ctest runs): every frame is drawn only where
 * cui_damage_track says it changed, from the list cui_damage_clip_boxes cut to those boxes, into
 * the picture kept from the frame before, and must then equal the frame drawn whole, pixel for
 * pixel, and so again with the window zoned into surfaces (cui_box.surface), whose own pictures
 * laid over the window's must then be that same frame. Where a use has one right answer that is checked too. -v prints the table.
 *
 *   cui_test_dirty_rect --window [options]
 * The same in a real window, timed (where CUI is built with cui_app):
 *     --renderer R   opengl (default), gpu or software
 *     --seconds S    how long each use is measured, default 3, after --settle seconds (default 0.5) left out
 *     --only TEXT    only the uses whose name holds TEXT
 *     --surfaces     the tab strip, the list, the chart, the picture, the spinner, the counter and what lies over the text are surfaces of their own
 *     --size WxH     the window in points; left out, the window is maximized, to fill as many pixels as the display has
 *     --uncapped     do not wait for the display (cui_app_desc.no_vsync)
 *     --watch PID    also the CPU and GPU time of that process, for the system compositor
 *     --stamps       end each row with the time it was measured from and to, in seconds since 1970, for a sampler that runs beside it
 * A row a use: frames built a second, CPU time as a share of one core, GPU time as a share of
 * the GPU (Linux, from the process's DRM fdinfo), the same two for the watched process, and the
 * frames presented in part and whole and how much of the window one drew. Then ordinary use, and
 * the dearest. Run it once a setting to compare them: --surfaces, --renderer gpu. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <math.h>
#if defined(CUI_TEST_WINDOW)
#include <cui/cui_app.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#endif
#endif

/* ---- the window and its uses ---- */

enum { DR_IDLE, DR_CARET, DR_TYPING, DR_HOVER, DR_LIST, DR_SCROLL, DR_SPINNER, DR_VIDEO, DR_CLOCK, DR_CHART, DR_POPOVER, DR_TOOLTIP, DR_TABDRAG,
       DR_FAR, DR_LIGHTS, DR_GLASS, DR_SWEEP, DR_INSERT, DR_FLASH, DR_BUSY, DR_ACTS };
static const struct { const char *name; int weight; } dr_acts[DR_ACTS] = {   /* weight: its share of an app's time, in percent */
    { "idle", 30 }, { "caret", 14 }, { "typing", 8 }, { "hover", 10 }, { "list", 6 }, { "scroll", 10 }, { "spinner", 4 }, { "video", 4 }, { "clock", 2 }, { "chart", 2 },
    { "popover", 1 }, { "tooltip", 1 }, { "tabdrag", 1 }, { "far", 1 }, { "lights", 1 }, { "glass", 1 }, { "sweep", 1 }, { "insert", 1 }, { "flash", 1 }, { "busy", 1 },
};

static struct {
    int act, tick;
    bool zoned;             /* the tab strip, the list, the picture and what lies over the text are surfaces of their own */
    /* tick: the frame of this use; the same tick builds the same shapes where no component animates by the clock */
    cui_texture picture;
    int tab, rest;
    bool pressed, on[6];
} dr;

#define DR_LIGHT 14.0f

static cui_texture dr_picture(cui_ctx *ui) {
    enum { N = 128 };
    unsigned char *px = malloc(N * N * 4);
    if (!px) return 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int dx = x - N / 2, dy = y - N / 2, ring = (dx * dx + dy * dy) / 96;
            unsigned char *p = px + (y * N + x) * 4;
            p[0] = (unsigned char)(60 + (ring * 37) % 180); p[1] = (unsigned char)(60 + (ring * 61) % 180); p[2] = (unsigned char)(60 + (ring * 89) % 180); p[3] = 255;
        }
    }
    cui_texture tex = cui_image_create(ui, N, N, px);
    free(px);
    return tex;
}

static void dr_start(cui_ctx *ui, int act, bool zoned) {
    cui_texture picture = dr.picture ? dr.picture : dr_picture(ui);
    memset(&dr, 0, sizeof dr);
    dr.act = act; dr.zoned = zoned; dr.picture = picture; dr.tab = 2;
    for (int i = 0; i < 6; i++) dr.on[i] = (i % 3) != 0;
}

static void dr_stop(cui_ctx *ui) {
    if (dr.pressed) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    dr.pressed = false;
    cui_input_mouse_leave(ui);
}

static void dr_light(cui_ctx *ui, float x, float y, int tick, int which) {
    int beat = (tick + which * 5) % 32;
    uint8_t level = (uint8_t)(90 + 5 * (beat < 16 ? beat : 32 - beat));
    cui_draw_rect(ui, (cui_rect){ x, y, DR_LIGHT, DR_LIGHT }, CUI_CORNERS(DR_LIGHT / 2), (cui_color){ 40, level, (uint8_t)(255 - level), 255 });
}

static void dr_paint_light(cui_ctx *ui, cui_rect r, void *user) {
    bool blink = dr.act == DR_LIGHTS || dr.act == DR_BUSY;
    dr_light(ui, r.x + (r.w - DR_LIGHT) / 2, r.y + (r.h - DR_LIGHT) / 2, blink ? dr.tick : 0, *(int *)user);
}

static void dr_paint_picture(cui_ctx *ui, cui_rect r, void *user) {
    cui_draw_image(ui, dr.picture, r, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(10), (cui_color){ 255, 255, 255, (uint8_t)(255 - 30 * *(int *)user) });
}

/* The card: shadowed, translucent panels over one another, so that a pixel of it is written many times. */
static void dr_paint_card(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    unsigned seed = 7;
    cui_draw_rect(ui, r, CUI_CORNERS(12), CUI_RGB(0x16181d));
    cui_push_clip(ui, r);
    for (int i = 0; i < 24; i++) {
        float f[4];
        uint8_t c[6];
        for (int k = 0; k < 4; k++) { seed = seed * 1664525u + 1013904223u; f[k] = (float)((seed >> 8) % 1000) / 1000.0f; }
        for (int k = 0; k < 6; k++) { seed = seed * 1664525u + 1013904223u; c[k] = (uint8_t)(seed >> 16); }
        float w = r.w * (0.25f + 0.35f * f[0]), h = r.h * (0.35f + 0.4f * f[1]);
        cui_rect p = { r.x + f[2] * (r.w - w), r.y + f[3] * (r.h - h), w, h };
        cui_draw_shadow(ui, p, CUI_CORNERS(14), 24, 2, (cui_vec2){ 0, 6 }, CUI_RGBA(0x00000050));
        cui_draw_rect_gradient(ui, p, CUI_CORNERS(14), (cui_color){ c[0], c[1], c[2], 60 }, (cui_color){ c[3], c[4], c[5], 60 });
    }
    cui_pop_clip(ui);
    if (dr.act == DR_GLASS) dr_light(ui, r.x + r.w / 2, r.y + r.h / 2, dr.tick, 0);
}

static void dr_paint_video(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    int t = dr.act == DR_VIDEO || dr.act == DR_BUSY ? dr.tick : 0;
    for (int i = 0; i < 12; i++) {
        cui_color top = { (uint8_t)((i * 21 + t * 5) % 256), (uint8_t)((i * 40 + t * 3) % 256), 160, 255 }, bottom = { 20, (uint8_t)((i * 13 + t * 7) % 256), (uint8_t)((t * 2) % 256), 255 };
        cui_draw_rect_gradient(ui, (cui_rect){ r.x + r.w * (float)i / 12, r.y, r.w / 12 + 1, r.h }, CUI_CORNERS(0), top, bottom);
    }
}

/* Shapes that come and go in the toolbar, early in the draw list. */
static void dr_paint_chips(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    int n = dr.act == DR_INSERT ? 1 + (dr.tick / 4) % 6 : 3;
    for (int i = 0; i < n; i++) cui_draw_rect(ui, (cui_rect){ r.x + (float)i * 16, r.y + (r.h - 12) / 2, 12, 12 }, CUI_CORNERS(3), CUI_RGB(0x4f8cff));
}

/* What lies over the text: the caret, the line being typed, the popover, the tooltip, the bar. Zoned, each is a
 * surface no larger than itself and its shadow, not one the size of the text with little in it. */
static void dr_paint_over(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    cui_rect zone = { 0, 0, 0, 0 };
    if (dr.act == DR_CARET || dr.act == DR_INSERT) {
        zone = (cui_rect){ r.x + floorf(r.w * 0.4f), r.y + r.h - 40, 2, 18 };
        if (dr.zoned) cui_surface_begin(ui, "dr-over", zone);
        if ((dr.tick / 8) % 2 == 0) cui_draw_rect(ui, zone, CUI_CORNERS(0), CUI_RGB(0xffffff));
    } else if (dr.act == DR_TYPING) {
        static const char typed[] = "A line that is typed one letter at a time, as far as it goes before it starts again.";
        int n = 1 + (dr.tick / 4) % (int)(sizeof typed - 1);
        zone = (cui_rect){ r.x + 24, r.y + r.h - 44, r.w - 48, 26 };
        if (dr.zoned) cui_surface_begin(ui, "dr-over", zone);
        cui_draw_rect(ui, zone, CUI_CORNERS(4), CUI_RGB(0x14161a));
        cui_draw_text(ui, CUI_FONT_BODY, 14, (cui_vec2){ zone.x + 6, zone.y + 4 }, CUI_RGB(0xe6e9ef), typed, n);
    } else if (dr.act == DR_POPOVER) {
        int beat = dr.tick % 64;
        float shown = (float)(beat < 32 ? beat : 64 - beat) / 32.0f;
        cui_rect panel = { r.x + floorf(r.w * 0.5f) - 180, r.y + floorf(r.h * 0.5f) - 120, 360, 240 };
        zone = (cui_rect){ panel.x - 130, panel.y - 110, panel.w + 260, panel.h + 260 };   /* with room for its shadow */
        if (dr.zoned) cui_surface_begin(ui, "dr-over", zone);
        cui_draw_shadow(ui, panel, CUI_CORNERS(14), 40, 4, (cui_vec2){ 0, 16 }, cui_alpha(CUI_RGBA(0x000000a0), shown));
        cui_draw_rect_border(ui, panel, CUI_CORNERS(14), cui_alpha(CUI_RGB(0x262a33), shown), cui_alpha(CUI_RGB(0x3a404d), shown), 1);
        cui_draw_text(ui, CUI_FONT_MEDIUM, 15, (cui_vec2){ panel.x + 20, panel.y + 18 }, cui_alpha(CUI_RGB(0xe6e9ef), shown), "Share this document", -1);
    } else if (dr.act == DR_TOOLTIP) {
        float turn = (float)dr.tick / 180.0f * 6.2831853f;
        cui_rect tip = { floorf(r.x + r.w * (0.5f + 0.35f * cosf(turn))) - 70, floorf(r.y + r.h * (0.5f + 0.3f * sinf(turn * 2))) - 14, 140, 28 };
        zone = (cui_rect){ tip.x - 44, tip.y - 40, tip.w + 88, tip.h + 88 };
        if (dr.zoned) cui_surface_begin(ui, "dr-over", zone);
        cui_draw_shadow(ui, tip, CUI_CORNERS(6), 14, 0, (cui_vec2){ 0, 4 }, CUI_RGBA(0x00000080));
        cui_draw_rect(ui, tip, CUI_CORNERS(6), CUI_RGB(0x2b303a));
        cui_draw_text(ui, CUI_FONT_BODY, 12, (cui_vec2){ tip.x + 10, tip.y + 7 }, CUI_RGB(0xe6e9ef), "Opens in a new tab", -1);
    } else if (dr.act == DR_SWEEP) {
        zone = (cui_rect){ r.x + (float)((dr.tick * 6) % (int)(r.w > 12 ? r.w - 6 : 6)), r.y, 6, r.h };
        if (dr.zoned) cui_surface_begin(ui, "dr-over", zone);
        cui_draw_rect(ui, zone, CUI_CORNERS(0), CUI_RGBA(0x4f8cffc0));
    }
    if (dr.zoned && zone.w > 0) cui_surface_end(ui);
}

/* One frame of the window, between cui_begin_frame and cui_end_frame. */
static void dr_build(cui_ctx *ui) {
    static const char *const titles[6] = { "Overview", "Quarterly report", "Meeting notes", "Settings", "Reading list", "Budget" };
    static const char *const buttons[8] = { "Back", "Forward", "Reload", "Bookmarks", "Downloads", "History", "Share", "Menu" };
    static const char *const rows[14] = { "Inbox", "Drafts", "Sent", "Archive", "Projects", "Design", "Research", "Invoices", "Travel", "Receipts", "Photos", "Notes", "Updates", "Trash" };
    static const char *const switches[6] = { "Notifications", "Spell check", "Word wrap", "Line numbers", "Autosave", "Reduced motion" };
    static const char paragraph[] = "A window that shows a long text has thousands of small shapes in it, and nearly all of them stay where they are from one frame to the "
                                    "next. What it costs to change one of them is what this measures: the caret, a light, a number that counts, or the whole text as it scrolls.";
    static const int ids[14] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    cui_vec2 view = cui_viewport_size(ui);
    float sweep = 0.5f - 0.5f * cosf((float)dr.tick / 180.0f * 6.2831853f);
    bool busy = dr.act == DR_BUSY;
    int shown = (int)((view.y - 170) / 38);   /* as many list rows as leave the status bar in the window */
    shown = shown > 14 ? 14 : shown < 1 ? 1 : shown;
    if (dr.act == DR_HOVER || busy) cui_input_mouse_move(ui, view.x * (0.05f + 0.5f * sweep), 70);
    else if (dr.act == DR_LIST) cui_input_mouse_move(ui, 110, 122 + (float)(shown - 1) * 38 * sweep);
    else if (dr.act == DR_TABDRAG) {
        if (!dr.pressed) {   /* the pointer rests on the tab a few frames before it presses: a control is under the pointer a frame after it gets there */
            cui_input_mouse_move(ui, view.x * 0.30f, 18);
            if (++dr.rest > 4) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); dr.pressed = true; }
        } else cui_input_mouse_move(ui, view.x * (0.08f + 0.6f * sweep), 18);
    }
    if (dr.act == DR_SCROLL) cui_scroll_to(ui, "dr-text", (float)(dr.tick * 3 % 1200));
    else if (dr.tick == 0) cui_scroll_to(ui, "dr-text", 0);

    cui_doc_tab tabs[6];
    memset(tabs, 0, sizeof tabs);
    for (int i = 0; i < 6; i++) tabs[i].title = titles[i];
    cui_doc_tabs_opts tab_opts;
    memset(&tab_opts, 0, sizeof tab_opts);
    tab_opts.no_close = true;
    cui_box page = { .width = CUI_FILL, .height = CUI_FILL };
    if (dr.act == DR_FLASH) { int beat = dr.tick % 32; page.bg = (cui_color){ 18, 20, (uint8_t)(24 + 2 * (beat < 16 ? beat : 32 - beat)), 255 }; }
    cui_column_begin(ui, &page);
    cui_box strip = { .width = CUI_FILL, .surface = dr.zoned };
    cui_column_begin(ui, &strip);
    cui_doc_tabs_ex(ui, "dr-tabs", tabs, 6, &dr.tab, &tab_opts);
    cui_box_end(ui);

    cui_box bar = { .pad = 16, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER };
    cui_row_begin(ui, &bar);
    for (int i = 0; i < 8; i++) cui_button_ex(ui, buttons[i], NULL);
    cui_canvas(ui, "dr-chips", CUI_PX(96), CUI_PX(20), dr_paint_chips, NULL, 0);
    cui_spacer(ui);
    if (dr.act == DR_SPINNER || dr.act == DR_FAR || busy) {
        cui_box turn = { .surface = dr.zoned };
        cui_column_begin(ui, &turn);
        cui_spinner_ex(ui, 18, NULL);
        cui_box_end(ui);
    }
    cui_box_end(ui);

    cui_box middle = { .width = CUI_FILL, .height = CUI_FILL, .gap = 12, .pad_x = 12 };
    cui_row_begin(ui, &middle);
    cui_box side = { .width = CUI_PX(220), .height = CUI_FILL, .gap = 2, .surface = dr.zoned };
    cui_column_begin(ui, &side);
    for (int i = 0; i < shown; i++) {
        char id[16];
        snprintf(id, sizeof id, "dr-light%d", i);
        cui_box row = { .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER };
        cui_row_begin(ui, &row);
        cui_canvas(ui, id, CUI_PX(18), CUI_PX(18), dr_paint_light, &ids[i], sizeof ids[i]);
        cui_button_ex(ui, rows[i], &(cui_button_opts){ .variant = CUI_BUTTON_GHOST });
        cui_box_end(ui);
    }
    cui_box_end(ui);

    cui_box text = { .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL };
    cui_box_begin(ui, &text);
    cui_scroll_opts scroll = { .id = "dr-text", .width = CUI_FILL, .height = CUI_FILL, .pad = 16, .gap = 10 };
    cui_scroll_begin(ui, &scroll);
    cui_heading(ui, "What a frame costs", 2);
    cui_canvas(ui, "dr-card", CUI_FILL, CUI_PX(130), dr_paint_card, NULL, 0);
    cui_text_ex(ui, paragraph, NULL);
    cui_box pictures = { .width = CUI_FILL, .gap = 10 };
    cui_row_begin(ui, &pictures);
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof id, "dr-picture%d", i);
        cui_canvas(ui, id, CUI_FILL, CUI_PX(90), dr_paint_picture, &ids[i], sizeof ids[i]);
    }
    cui_box_end(ui);
    for (int i = 0, n = 40 + (int)(view.y / 10); i < n; i++) cui_text_ex(ui, paragraph, NULL);   /* more than any window shows: there is always text to scroll */
    cui_scroll_end(ui);
    cui_canvas(ui, "dr-over", CUI_FILL, CUI_FILL, dr_paint_over, NULL, 0);
    cui_box_end(ui);

    cui_box panel = { .width = CUI_PX(260), .height = CUI_FILL, .gap = 12, .pad = 4 };
    cui_column_begin(ui, &panel);
    for (int i = 0; i < 6; i++) cui_switch_ex(ui, switches[i], &dr.on[i], NULL);
    float values[48];
    int moved = dr.act == DR_CHART ? dr.tick : 0;
    for (int i = 0; i < 48; i++) values[i] = 0.5f + 0.4f * sinf((float)(i + moved) * 0.35f) * cosf((float)(i + moved) * 0.11f);
    cui_box chart = { .width = CUI_FILL, .surface = dr.zoned };
    cui_column_begin(ui, &chart);
    cui_sparkline(ui, values, 48, CUI_FILL, 56, CUI_RGB(0x4f8cff));
    cui_box_end(ui);
    cui_box video = { .width = CUI_FILL, .height = CUI_FILL, .surface = dr.zoned };
    cui_column_begin(ui, &video);
    cui_canvas(ui, "dr-video", CUI_FILL, CUI_FILL, dr_paint_video, NULL, 0);
    cui_box_end(ui);
    cui_box_end(ui);
    cui_box_end(ui);

    cui_box status = { .pad = 10, .gap = 12, .width = CUI_FILL, .align = CUI_ALIGN_CENTER };
    cui_row_begin(ui, &status);
    cui_box meter = { .width = CUI_PX(200) };
    cui_column_begin(ui, &meter);
    cui_progress_ex(ui, dr.act == DR_FAR ? (float)(dr.tick % 240) / 240.0f : 0.4f, NULL);
    cui_box_end(ui);
    cui_spacer(ui);
    char count[48];
    snprintf(count, sizeof count, "%d frames", dr.act == DR_CLOCK || busy ? dr.tick : 0);
    cui_box counter = { .surface = dr.zoned };
    cui_column_begin(ui, &counter);
    cui_muted(ui, count);
    cui_box_end(ui);
    cui_box_end(ui);
    cui_box_end(ui);
}

/* ---- headless: every partly drawn frame against the frame drawn whole ---- */

static int g_failed, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { W = 800, H = 500, FRAMES = 12, SETTLE = 5, STEP = 7 };   /* STEP: ticks a frame, so that few frames see a caret blink and a pointer move */

static void check(bool zoned, bool verbose) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK, .instanced_quads = true });
    cui_set_viewport(ui, W, H, 1);
    cui_damage *d = cui_damage_create();
    size_t bytes = (size_t)W * H * 4;
    unsigned char *kept = malloc(bytes), *whole = malloc(bytes), *part = malloc(bytes);
    if (verbose) printf("-- %s\n", zoned ? "zoned into surfaces" : "one list");
    unsigned char *own = malloc(bytes);
    memset(&dr, 0, sizeof dr);

    for (int act = 0; act < DR_ACTS; act++) {
        const char *name = dr_acts[act].name;
        dr_start(ui, act, zoned);
        int unchanged = 0, most_zones = 0, apart = 0, partial = 0, full_frames = 0, most = 0, wrong = 0;
        double area = 0, around = 0;
        for (int frame = 0; frame < FRAMES; frame++) {
            dr.tick = frame * STEP;
            cui_begin_frame(ui, frame < SETTLE ? 0.25f : 1.0f / 60.0f);   /* long frames first: what the use before left moving comes to rest */
            dr_build(ui);
            cui_end_frame(ui);
            const cui_draw_data *dd = cui__draw_data_merged(ui);   /* the frame as one list: the window's, and each surface's where it lies */
            cui_sw_render(sw, dd, whole, W, H, W * 4, CUI_SW_RGBA);
            if (zoned) {
                /* the window's own list, and each surface's from its own corner laid over it where the surface is: the frame */
                cui_surface zone[CUI_SURFACES];
                int zones = 0;
                for (int k = 0; k < CUI_SURFACES; k++) {
                    cui_surface z;
                    if (!cui_get_surface(ui, k, &z)) continue;
                    int at = zones++;
                    while (at > 0 && zone[at - 1].order > z.order) { zone[at] = zone[at - 1]; at--; }
                    zone[at] = z;
                }
                if (zones > most_zones) most_zones = zones;
                cui_sw_render(sw, cui_get_draw_data(ui), part, W, H, W * 4, CUI_SW_RGBA);
                for (int k = 0; k < zones; k++) {
                    cui_surface z = zone[k];
                    CHECK(z.x >= 0 && z.y >= 0 && z.w > 0 && z.h > 0 && z.x + z.w <= W && z.y + z.h <= H && z.draw->width_px == z.w && z.draw->height_px == z.h, "%s: a surface at %d,%d of %dx%d", name, z.x, z.y, z.w, z.h);
                    cui_sw_render(sw, z.draw, own, z.w, z.h, z.w * 4, CUI_SW_RGBA);
                    for (int y = 0; y < z.h; y++) {
                        for (int x = 0; x < z.w; x++) {
                            const unsigned char *src = own + ((size_t)y * (size_t)z.w + (size_t)x) * 4;
                            unsigned char *dst = part + ((size_t)(z.y + y) * W + (size_t)(z.x + x)) * 4;
                            for (int c = 0; c < 3; c++) dst[c] = (unsigned char)CUI_MIN(255, src[c] + (dst[c] * (255 - src[3]) + 127) / 255);   /* (its colors are multiplied by its alpha already) */
                            dst[3] = 255;
                        }
                    }
                }
                int off = 0, worst = 0, bx0 = W, by0 = H, bx1 = 0, by1 = 0;
                for (size_t p = 0; p < bytes; p++) {
                    if ((p & 3) == 3) continue;
                    int delta = abs((int)part[p] - (int)whole[p]);
                    if (delta > worst) worst = delta;
                    if (delta <= 2) continue;   /* (a level or two: the same blend, done in two steps) */
                    int x = (int)(p / 4 % W), y = (int)(p / 4 / W);
                    bx0 = CUI_MIN(bx0, x); by0 = CUI_MIN(by0, y); bx1 = CUI_MAX(bx1, x + 1); by1 = CUI_MAX(by1, y + 1);
                    off++;
                }
                /* a pixel or two on a shape's edge may fall the other way: the same edge worked out from another corner */
                if (off > 24) apart++;
                if (off > 24 && apart == 1) printf("  FAIL %s frame %d: the surfaces laid over the window differ from the frame in %d bytes inside %d,%d to %d,%d, by up to %d\n", name, frame, off, bx0, by0, bx1, by1, worst);
            }
            cui_rect rect, box[CUI_DAMAGE_BOXES];
            bool full = false, changed = cui_damage_track(d, dd, &rect, &full);
            int boxes = 0;
            const cui_draw_data *cut = changed && !full ? cui_damage_clip_boxes(d, dd, box, &boxes) : NULL;
            if (changed && (full || !cut)) {
                memcpy(kept, whole, bytes);
            } else if (changed) {
                cui_sw_render(sw, cut, part, W, H, W * 4, CUI_SW_RGBA);
                for (int b = 0; b < boxes; b++) {
                    int x0 = (int)box[b].x, y0 = (int)box[b].y, x1 = (int)(box[b].x + box[b].w), y1 = (int)(box[b].y + box[b].h);
                    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > W ? W : x1; y1 = y1 > H ? H : y1;
                    for (int y = y0; y < y1 && x1 > x0; y++) memcpy(kept + ((size_t)y * W + (size_t)x0) * 4, part + ((size_t)y * W + (size_t)x0) * 4, (size_t)(x1 - x0) * 4);
                }
            }
            if (memcmp(kept, whole, bytes) != 0) {
                int first = -1, count = 0;
                for (int p = 0; p < W * H; p++) if (memcmp(kept + (size_t)p * 4, whole + (size_t)p * 4, 4) != 0) { if (first < 0) first = p; count++; }
                if (!wrong++) printf("  FAIL %s%s frame %d: %d pixels differ from the frame drawn whole, the first at %d,%d\n", name, zoned ? " (zoned)" : "", frame, count, first % W, first / W);
                memcpy(kept, whole, bytes);
            }
            if (frame < SETTLE) continue;
            if (!changed) { unchanged++; continue; }
            if (full || !cut) { full_frames++; continue; }
            partial++;
            if (boxes > most) most = boxes;
            for (int b = 0; b < boxes; b++) area += (double)box[b].w * box[b].h / ((double)W * H);
            around += (double)rect.w * rect.h / ((double)W * H);
        }
        dr_stop(ui);
        g_checks++;
        if (wrong) g_failed++;
        if (zoned) { g_checks++; if (apart) g_failed++; CHECK(most_zones >= 5, "%s: %d surfaces", name, most_zones); }

        double share = partial ? 100.0 * area / partial : 0, bound = partial ? 100.0 * around / partial : 0;
        int frames = FRAMES - SETTLE;
        if (verbose) printf("  %-8s unchanged %2d  partial %2d  whole %2d  boxes %d  drawn %5.1f%%  a rectangle round all %5.1f%%\n", name, unchanged, partial, full_frames, most, share, bound);
        switch (act) {
        case DR_IDLE: CHECK(partial + full_frames == 0, "%s drew %d frames", name, partial + full_frames); break;
        case DR_CARET: case DR_SPINNER: case DR_CLOCK: case DR_GLASS:   /* a caret is the same in some frames */
            CHECK(partial > 0 && full_frames == 0 && (act == DR_CARET || partial == frames) && share < 1, "%s: partial %d, whole %d of %d, %.2f%%", name, partial, full_frames, frames, share);
            break;
        case DR_FLASH: case DR_FAR:   /* far: the one rectangle round both is most of the window */
            CHECK(full_frames == frames, "%s: whole %d of %d", name, full_frames, frames);
            break;
        default: break;
        }
    }
    free(kept); free(whole); free(part); free(own);
    cui_damage_destroy(d);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    memset(&dr, 0, sizeof dr);
}

/* ---- in a window: what each use costs ---- */

#if defined(CUI_TEST_WINDOW)
static double w_wall(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

/* Seconds since 1970, the clock a sampler beside this reads too. */
static double w_epoch(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (double)(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) * 1e-7 - 11644473600.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static double w_cpu(void) {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (double)(k + u) * 1e-7;
#else
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

/* Seconds of GPU time a process has used (0 for this one), or -1 where that is not to be read. */
static double w_gpu(long pid) {
#if defined(__linux__)
    char dir[64], path[400], text[4096];
    if (pid) snprintf(dir, sizeof dir, "/proc/%ld/fdinfo", pid); else snprintf(dir, sizeof dir, "/proc/self/fdinfo");
    DIR *d = opendir(dir);
    if (!d) return -1;
    long seen[64];
    int clients = 0;
    double ns = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = 0;
        const char *id = strstr(text, "drm-client-id:");
        if (!id) continue;
        long client = strtol(id + 14, NULL, 10);   /* several descriptors of one client report the same time */
        bool known = false;
        for (int i = 0; i < clients; i++) known = known || seen[i] == client;
        if (known) continue;
        if (clients < 64) seen[clients++] = client;
        for (const char *p = text; (p = strstr(p, "drm-engine-")) != NULL; p += 11) {
            const char *colon = strchr(p, ':'), *end = strchr(p, '\n');
            if (colon && (!end || colon < end) && strncmp(p, "drm-engine-capacity", 19) != 0) ns += strtod(colon + 1, NULL);
        }
    }
    closedir(d);
    return clients ? ns * 1e-9 : -1;
#else
    (void)pid;
    return -1;
#endif
}

/* Seconds of CPU time another process has used, or -1. */
static double w_cpu_of(long pid) {
#if defined(__linux__)
    char path[64], text[1024];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const char *p = strrchr(text, ')');
    unsigned long long user = 0, kernel = 0;
    if (!p || sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user, &kernel) != 2) return -1;
    return (double)(user + kernel) / 100.0;
#else
    (void)pid;
    return -1;
#endif
}

typedef struct w_mark { double wall, cpu, gpu, watch_cpu, watch_gpu, epoch; int frames; bool tracked; cui_damage_stats stats; } w_mark;

static w_mark w_now(cui_app *app, long watch, int frames) {
    w_mark m;
    memset(&m, 0, sizeof m);
    m.epoch = w_epoch();
    m.wall = w_wall(); m.cpu = w_cpu(); m.gpu = w_gpu(0); m.frames = frames;
    m.watch_cpu = watch ? w_cpu_of(watch) : -1; m.watch_gpu = watch ? w_gpu(watch) : -1;
    m.tracked = cui_app_damage_stats(app, &m.stats);
    return m;
}

static int window(int argc, char **argv) {
    cui_app_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.title = "CUI dirty rectangle";
    desc.width = 1280;
    desc.height = 720;
    desc.theme = CUI_THEME_DARK;
    desc.renderer = CUI_RENDERER_OPENGL;
    cui_window_placement filled = { .width = 1280, .height = 720, .state = CUI_WINDOW_MAXIMIZED };
    desc.placement = &filled;
    double span = 3, settle = 0.5;
    const char *only = NULL;
    bool zoned = false;
    long watch = 0;
    bool stamps = false;
    for (int i = 2; i < argc; i++) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--renderer") && more) {
            const char *r = argv[++i];
            desc.renderer = !strcmp(r, "gpu") ? CUI_RENDERER_GPU : !strcmp(r, "software") ? CUI_RENDERER_SOFTWARE : CUI_RENDERER_OPENGL;
        } else if (!strcmp(argv[i], "--seconds") && more) span = atof(argv[++i]);
        else if (!strcmp(argv[i], "--settle") && more) settle = atof(argv[++i]);
        else if (!strcmp(argv[i], "--only") && more) only = argv[++i];
        else if (!strcmp(argv[i], "--surfaces")) zoned = true;
        else if (!strcmp(argv[i], "--watch") && more) watch = atol(argv[++i]);
        else if (!strcmp(argv[i], "--size") && more) { sscanf(argv[++i], "%dx%d", &desc.width, &desc.height); desc.placement = NULL; }
        else if (!strcmp(argv[i], "--uncapped")) desc.no_vsync = true;
        else if (!strcmp(argv[i], "--stamps")) stamps = true;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    cui_app *app = cui_app_create(&desc);
    if (!app) return 1;
    printf("renderer asked for: %s, %.1f s a use%s\n", desc.renderer == CUI_RENDERER_GPU ? "gpu" : desc.renderer == CUI_RENDERER_SOFTWARE ? "software" : "opengl", span, desc.no_vsync ? ", uncapped" : "");
    printf("%-13s %8s %7s %7s %10s %10s  %s\n", "use", "built/s", "cpu%", "gpu%", "watch cpu%", "watch gpu%", "presented (in part, whole), share of the window: with damage tracking");

    int act = -1, frames = 0, dearest = -1;
    bool measuring = false;
    double begun = 0, usual[4] = { 0 }, weight = 0, worst = -1;
    w_mark from;
    memset(&from, 0, sizeof from);
    memset(&dr, 0, sizeof dr);

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        double now = w_wall();
        if (act < 0 || now - begun >= settle + span) {
            if (act >= 0 && measuring) {
                w_mark to = w_now(app, watch, frames);
                double wall = to.wall - from.wall, cpu = 100 * (to.cpu - from.cpu) / wall;
                double gpu = to.gpu < 0 || from.gpu < 0 ? -1 : 100 * (to.gpu - from.gpu) / wall;
                double watch_cpu = to.watch_cpu < 0 ? -1 : 100 * (to.watch_cpu - from.watch_cpu) / wall, watch_gpu = to.watch_gpu < 0 || from.watch_gpu < 0 ? -1 : 100 * (to.watch_gpu - from.watch_gpu) / wall;
                printf("%-13s %8.1f %7.2f", dr_acts[act].name, (to.frames - from.frames) / wall, cpu);
                if (gpu >= 0) printf(" %7.2f", gpu); else printf(" %7s", "-");
                if (watch_cpu >= 0) printf(" %10.2f", watch_cpu); else printf(" %10s", "-");
                if (watch_gpu >= 0) printf(" %10.2f", watch_gpu); else printf(" %10s", "-");
                if (to.tracked && from.tracked) {
                    unsigned long long partial = to.stats.partial - from.stats.partial, full = to.stats.full - from.stats.full, drawn = partial + full;
                    cui_vec2 size = cui_app_size(app);
                    double pixels = (double)size.x * size.y * cui_dpi_scale(ui) * cui_dpi_scale(ui);
                    printf("  %5llu (%5llu, %5llu) %5.1f%%", drawn, partial, full, drawn && pixels > 0 ? 100.0 * (double)(to.stats.pixels - from.stats.pixels) / (double)drawn / pixels : 0.0);
                }
                if (stamps) printf("  @%.2f-%.2f", from.epoch, to.epoch);
                printf("\n");
                fflush(stdout);
                double w = dr_acts[act].weight, both = (gpu > 0 ? gpu : 0) + (watch_gpu > 0 ? watch_gpu : 0), cost = both > 0 ? both : cpu;
                usual[0] += w * cpu; usual[1] += w * (gpu > 0 ? gpu : 0); usual[2] += w * (watch_cpu > 0 ? watch_cpu : 0); usual[3] += w * (watch_gpu > 0 ? watch_gpu : 0);
                weight += w;
                if (cost > worst) { worst = cost; dearest = act; }
            }
            if (act >= 0) dr_stop(ui);
            do act++; while (act < DR_ACTS && only && !strstr(dr_acts[act].name, only));
            if (act >= DR_ACTS) break;
            dr_start(ui, act, zoned);
            begun = now;
            measuring = false;
        }
        if (!measuring && now - begun >= settle) { from = w_now(app, watch, frames); measuring = true; }

        if (dr.act == DR_IDLE) cui_request_wakeup(ui, (float)(measuring ? settle + span - (now - begun) : settle - (now - begun)) + 0.01f);   /* a frame when the measuring starts and ends, none between */
        else cui_request_redraw(ui);
        dr_build(ui);
        dr.tick++;
        frames++;
    }
    cui_vec2 at = cui_app_size(app);
    float scale = cui_dpi_scale(cui_app_ui(app));
    printf("window: %.0fx%.0f pixels\n", at.x * scale, at.y * scale);
    if (weight > 0) printf("%-13s %8s %7.2f %7.2f %10.2f %10.2f  every use weighed by how common it is\n", "ordinary use", "", usual[0] / weight, usual[1] / weight, usual[2] / weight, usual[3] / weight);
    if (dearest >= 0) printf("dearest: %s\n", dr_acts[dearest].name);
    cui_app_destroy(app);
    return 0;
}
#endif

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--window")) {
#if defined(CUI_TEST_WINDOW)
        return window(argc, argv);
#else
        fprintf(stderr, "built without cui_app: no window\n");
        return 2;
#endif
    }
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    check(false, verbose);
    check(true, verbose);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
