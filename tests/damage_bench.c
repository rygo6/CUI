#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Damage bench: a window of ordinary chrome (document tabs, a row of buttons, a list of
 * switches) left alone, then hovered across, then with a tab dragged back and forth, to see
 * what a frame costs when little of the window changes.
 *
 *   cui_damage_bench [--renderer opengl|gpu|software] [--seconds S]
 *
 * Three phases of S seconds each (default 10) after 3 seconds of settling:
 *   idle   nothing moves
 *   hover  the pointer sweeps along the row of buttons, a full sweep every 3 seconds
 *   drag   a tab is held and dragged along the strip the same way
 * For each it prints the frames built, the frames presented and how (with CUI_DAMAGE=1), the
 * process's CPU time as a share of one core, and at the end its peak memory. Run it once as it
 * is and once with CUI_DAMAGE=1 (which turns CUI_INSTANCED on) to compare. The GPU's load is
 * not something a process can read about itself: sample it from outside. Not run by ctest: it
 * needs a window. */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

static double db_wall(void) {
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

static double db_cpu(void) {
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

static double db_peak_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? (double)pmc.PeakWorkingSetSize / 1048576.0 : 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return (double)ru.ru_maxrss / 1048576.0;
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
#endif
}

enum { DB_SETTLE = -1, DB_IDLE, DB_HOVER, DB_DRAG, DB_DONE };
static const char *const db_names[] = { "idle", "hover", "drag" };

int main(int argc, char **argv) {
    cui_app_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.title = "CUI damage bench";
    desc.width = 1280;
    desc.height = 720;
    desc.theme = CUI_THEME_DARK;
    desc.renderer = CUI_RENDERER_OPENGL;
    double span = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            const char *r = argv[++i];
            desc.renderer = !strcmp(r, "gpu") ? CUI_RENDERER_GPU : !strcmp(r, "software") ? CUI_RENDERER_SOFTWARE : CUI_RENDERER_OPENGL;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            span = atof(argv[++i]);
        }
    }
    cui_app *app = cui_app_create(&desc);
    if (!app) return 1;

    cui_doc_tab tabs[6];
    memset(tabs, 0, sizeof tabs);
    static const char *const titles[6] = { "World", "Dance 4.0 VR-MR", "Reduced Bind Rendering", "Settings", "WebXR Samples", "Drivers" };
    for (int i = 0; i < 6; i++) tabs[i].title = titles[i];
    static const char *const buttons[10] = { "Back", "Forward", "Reload", "Bookmarks", "Downloads", "History", "Studio", "Inspector", "Settings", "Menu" };
    static const char *const rows[12] = { "Appearance", "Default OpenXR runtime", "WebXR for immersive-web.github.io", "Folder access", "GPU",
                                          "Inspector server", "PSVR2", "Vive", "Desktop", "Naeb", "Passthrough", "Realtime queue" };
    bool on[12];
    for (int i = 0; i < 12; i++) on[i] = (i % 3) != 0;
    cui_doc_tabs_opts tab_opts;
    memset(&tab_opts, 0, sizeof tab_opts);
    tab_opts.no_close = true;

    const double settle = 3;
    int selected = 2, phase = DB_SETTLE, frames = 0, begin_frames = 0, hover_frames = 0;
    double start = db_wall(), begin_wall = start, begin_cpu = db_cpu();
    bool pressed = false;
    cui_damage_stats begin_stats, stats;
    memset(&begin_stats, 0, sizeof begin_stats);
    bool tracked = cui_app_damage_stats(app, &begin_stats);
    printf("renderer asked for: %s, damage tracking %s\n", desc.renderer == CUI_RENDERER_GPU ? "gpu" : desc.renderer == CUI_RENDERER_SOFTWARE ? "software" : "opengl",
           tracked ? "on" : "off");

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        double now = db_wall(), t = now - start - settle;
        int want = t < 0 ? DB_SETTLE : t < span ? DB_IDLE : t < 2 * span ? DB_HOVER : t < 3 * span ? DB_DRAG : DB_DONE;
        if (want != phase) {
            if (phase >= 0) {
                double wall = now - begin_wall;
                printf("%-5s %5d frames built (%.1f/s), cpu %.2f%% of one core", db_names[phase], frames - begin_frames, (frames - begin_frames) / wall,
                       100.0 * (db_cpu() - begin_cpu) / wall);
                if (cui_app_damage_stats(app, &stats)) {
                    unsigned long long partial = stats.partial - begin_stats.partial, full = stats.full - begin_stats.full;
                    unsigned long long drawn = partial + full;
                    cui_vec2 size = cui_app_size(app);
                    double window = (double)size.x * size.y * cui_dpi_scale(ui) * cui_dpi_scale(ui);
                    printf(", presented %llu (%llu partial, %llu full), %.1f%% of the window a drawn frame", drawn, partial, full,
                           drawn && window > 0 ? 100.0 * (double)(stats.pixels - begin_stats.pixels) / (double)drawn / window : 0.0);
                    begin_stats = stats;
                }
                printf("\n");
                fflush(stdout);
            }
            if (pressed) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); pressed = false; }
            phase = want;
            if (phase == DB_DONE) break;
            begin_wall = now; begin_cpu = db_cpu(); begin_frames = frames; hover_frames = 0;
            cui_app_damage_stats(app, &begin_stats);
        }

        float width = cui_viewport_size(ui).x;
        float sweep = width * (0.08f + 0.40f * (1.0f - (float)cos((now - begin_wall) / 3.0 * 2.0 * 3.14159265358979)));
        if (phase == DB_HOVER) {
            cui_input_mouse_move(ui, sweep, 84);
            cui_request_redraw(ui);
        } else if (phase == DB_DRAG) {
            /* the pointer rests on a tab for a few frames, presses, and only then moves: a control is under the pointer a frame after it gets there */
            if (!pressed) {
                cui_input_mouse_move(ui, width * 0.30f, 18);
                if (++hover_frames > 4) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); pressed = true; }
            } else {
                cui_input_mouse_move(ui, sweep, 18);
            }
            cui_request_redraw(ui);
        } else {
            double left = phase == DB_SETTLE ? -t : span - t;
            cui_request_wakeup(ui, (float)left + 0.01f);   /* one frame when the phase ends, none before */
        }

        cui_box page;
        memset(&page, 0, sizeof page);
        page.width = CUI_FILL;
        cui_column_begin(ui, &page);
        cui_doc_tabs_ex(ui, "tabs", tabs, 6, &selected, &tab_opts);
        cui_box bar;
        memset(&bar, 0, sizeof bar);
        bar.pad = 16; bar.gap = 8;
        cui_row_begin(ui, &bar);
        for (int i = 0; i < 10; i++) cui_button_ex(ui, buttons[i], NULL);
        cui_box_end(ui);
        cui_box list;
        memset(&list, 0, sizeof list);
        list.pad = 16; list.gap = 10; list.width = CUI_FILL;
        cui_column_begin(ui, &list);
        for (int i = 0; i < 12; i++) cui_switch_ex(ui, rows[i], &on[i], NULL);
        cui_box_end(ui);
        cui_box_end(ui);
        frames++;
    }
    printf("peak memory %.0f MB\n", db_peak_mb());
    cui_app_destroy(app);
    return 0;
}
