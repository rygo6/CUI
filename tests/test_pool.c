/* The buffer pool's bookkeeping (src/render/cui_pool.c), with no GPU and no window. */
#include "../src/cui_internal.h"
#include "../src/render/cui_pool.h"

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static cui_pool *g_pool;
static bool g_make;
static int g_evict;

static int take(cui_id element, int w, int h, int front, double now) { return cui_pool_take(g_pool, element, w, h, front, now, &g_make, &g_evict); }
static cui_damage *tracker(cui_id element) { return &g_pool->owner[cui_pool_owner_of(g_pool, element)].damage.head; }
static int owners(void) { int n = 0; for (int i = 0; i < CUI_POOL_OWNERS; i++) n += g_pool->owner_id[i] != 0; return n; }
static int slots(void) { int n = 0; for (int i = 0; i < CUI_POOL_SLOTS; i++) n += (int)(g_pool->used >> i & 1); return n; }

/* one record coloured by `shade`, tracked: a last frame for a tracker to keep or lose */
static bool track(cui_id element, int shade) {
    cui_instance one = { 0 };
    one.x1 = 400; one.y1 = 400;
    one.color_top.r = (uint8_t)shade;
    cui_draw_cmd cmd = { { 0, 0, 300, 200 }, 0, 0, 0, 0, 1 };
    cui_draw_data dd = { 0 };
    dd.instances = &one; dd.instance_count = 1;
    dd.commands = &cmd; dd.command_count = 1;
    dd.width_px = 300; dd.height_px = 200;
    cui_rect rect;
    bool full = false;
    return cui_damage_track(tracker(element), &dd, &rect, &full);
}

static void fresh(void) { memset(g_pool, 0, sizeof *g_pool); }

static void keys(void) {
    CHECK_EQ(cui_pool_key(0, 10), 0);
    CHECK_EQ(cui_pool_key(10, 8193), 0);
    CHECK_EQ(cui_pool_key(1, 1), 1);
    CHECK_EQ(cui_pool_key(64, 64), cui_pool_key(1, 1));
    CHECK(cui_pool_key(65, 64) != cui_pool_key(64, 64));
    CHECK_EQ(cui_pool_key_width(cui_pool_key(300, 200)), 320);
    CHECK_EQ(cui_pool_key_height(cui_pool_key(300, 200)), 256);
    CHECK_EQ(cui_pool_key_width(cui_pool_key(8192, 8192)), 8192);
    CHECK_EQ(cui_pool_key_height(cui_pool_key(8192, 8192)), 8192);
    CHECK(!(cui_pool_key(8192, 8192) & 2));   /* bit 1 is the pool's, for spare */
}

/* the same element reopening gets its slot back, missed boxes and tracker as they were */
static void reopen(void) {
    fresh();
    cui_rect box = { 10, 20, 30, 40 };
    CHECK(track(7, 1));
    int a = take(7, 300, 200, -1, 1.0);
    CHECK(a >= 0 && g_make && g_evict < 0);
    cui_pool_drawn(g_pool, a, 5, NULL, -1);
    cui_pool_held(g_pool, a, true);
    int b = take(7, 300, 200, a, 1.1);   /* the first is on screen: a second */
    CHECK(b >= 0 && b != a && g_make);
    cui_pool_drawn(g_pool, b, 6, &box, 1);
    CHECK_EQ(g_pool->slot[a].missed_count, 1);
    CHECK_EQ(g_pool->slot[b].missed_count, 0);
    cui_pool_held(g_pool, b, true);
    cui_pool_held(g_pool, a, false);
    cui_pool_give_back(g_pool, 7, 2.0);
    cui_pool_held(g_pool, b, false);
    CHECK_EQ(slots(), 2);
    CHECK_EQ(owners(), 1);
    CHECK(g_pool->spare_since[a] == 2.0 && g_pool->spare_since[b] == 2.0);
    CHECK(!track(7, 1));   /* the tracker still holds the last frame: nothing changed */
    int again = take(7, 300, 200, -1, 2.3);
    CHECK(again == a || again == b);
    CHECK(!g_make);
    CHECK_EQ(g_pool->slot[a].missed_count, 1);
    CHECK(g_pool->slot[a].missed[0].x == 10 && g_pool->slot[a].missed[0].h == 40);
    CHECK(g_pool->slot[again].flags & CUI_POOL_DRAWN);
    CHECK(g_pool->spare_since[again] == 0);
    CHECK_EQ(g_pool->owner[cui_pool_owner_of(g_pool, 7)].buffers, 2);
    /* a frame drawn while one of its pictures is spare: the spare one has missed it too */
    cui_pool_drawn(g_pool, again, 7, &box, 1);
    int other = again == a ? b : a;
    CHECK_EQ(g_pool->slot[other].missed_count, other == a ? 2 : 1);
    cui_pool_drawn(g_pool, again, 8, NULL, -1);
    CHECK_EQ(g_pool->slot[other].missed_count, -1);
}

/* another element of the size takes a spare slot: cleared; the old owner goes with its last */
static void recycle(void) {
    fresh();
    CHECK(track(7, 1));
    int a = take(7, 300, 200, -1, 1.0);
    cui_pool_drawn(g_pool, a, 5, NULL, -1);
    cui_pool_held(g_pool, a, true);
    int b = take(7, 300, 200, a, 1.1);
    cui_pool_drawn(g_pool, b, 6, NULL, 0);
    cui_pool_give_back(g_pool, 7, 2.0);
    cui_pool_held(g_pool, a, false);
    CHECK(track(9, 2));
    int c = take(9, 310, 250, -1, 2.1);   /* the same bucket */
    CHECK(c == a || c == b);
    CHECK(!g_make);
    CHECK_EQ(g_pool->slot[c].missed_count, -1);
    CHECK_EQ(g_pool->owner_id[g_pool->slot[c].owner], 9);
    CHECK_EQ(owners(), 2);   /* 7 still has a spare slot, and so its tracker */
    CHECK(!track(7, 1));
    cui_pool_drawn(g_pool, c, 7, NULL, -1);
    cui_pool_held(g_pool, c, true);
    int d = take(9, 300, 200, c, 2.2);
    CHECK(d == (c == a ? b : a));
    CHECK(!g_make);
    CHECK_EQ(owners(), 1);   /* 7's last slot went: its entry is free, its tracker has no history */
    CHECK(track(7, 1));
    cui_pool_give_back(g_pool, 7, 2.3);   /* asked for and never drawn: free again */
    CHECK_EQ(owners(), 1);
    CHECK_EQ(slots(), 2);
    int e = take(11, 500, 200, -1, 2.4);   /* another bucket: a slot of its own */
    CHECK(e >= 0 && g_make);
    CHECK_EQ(slots(), 3);
}

/* a run of popups of one size, one at a time: one slot serves them all */
static void run_of_popups(void) {
    fresh();
    int first = -1, made = 0;
    for (int i = 0; i < 20; i++) {
        cui_id element = 100 + (cui_id)i;
        int w = 240 + i % 3 * 5, at = take(element, w, 300, -1, 1.0 + i);
        CHECK(at >= 0);
        made += g_make;
        if (first < 0) first = at;
        CHECK_EQ(at, first);
        cui_pool_drawn(g_pool, at, (uint64_t)i + 1, NULL, -1);
        cui_pool_held(g_pool, at, true);
        cui_pool_give_back(g_pool, element, 1.5 + i);
        cui_pool_held(g_pool, at, false);   /* (hidden: the compositor lets go) */
        CHECK_EQ(owners(), 1);
    }
    CHECK_EQ(made, 1);
    CHECK_EQ(slots(), 1);
}

static void reap(void) {   /* half the spares or fewer: each goes once 5 s old, one a tick */
    fresh();
    double due = 0;
    int s[4];
    for (int i = 0; i < 4; i++) {
        s[i] = take(20 + (cui_id)i, 100 + 100 * i, 100, -1, 1.0);
        cui_pool_drawn(g_pool, s[i], (uint64_t)i + 1, NULL, -1);
    }
    CHECK_EQ(cui_pool_reap(g_pool, 5.0, 100, &due), -1);   /* all taken */
    CHECK(due < 0);
    cui_pool_give_back(g_pool, 20, 10.0);
    cui_pool_give_back(g_pool, 21, 10.5);
    cui_pool_give_back(g_pool, 22, 10.6);
    cui_pool_held(g_pool, s[1], true);
    CHECK_EQ(cui_pool_reap(g_pool, 14.9, 100, &due), -1);   /* never before 5 s */
    CHECK(due > 0.09 && due < 0.11);
    CHECK_EQ(cui_pool_reap(g_pool, 15.0, 0, &due), -1);     /* the GPU has not passed it */
    CHECK(due > 0 && due <= CUI_POOL_TICK_SECONDS + 1e-9);
    CHECK_EQ(cui_pool_reap(g_pool, 15.1, 100, &due), s[0]);
    CHECK(due > 0 && due <= CUI_POOL_TICK_SECONDS + 1e-9);
    CHECK_EQ(slots(), 3);
    CHECK_EQ(cui_pool_reap(g_pool, 15.2, 100, &due), -1);   /* by age, not by interval */
    CHECK(due > 0.39 && due < 0.41);
    CHECK_EQ(cui_pool_reap(g_pool, 15.6, 100, &due), s[2]);   /* never the held one, though it is older */
    CHECK(due < 0);   /* what is left is held: no wakeup for it */
    CHECK_EQ(cui_pool_reap(g_pool, 20.0, 100, &due), -1);
    cui_pool_held(g_pool, s[1], false);
    int made = take(30, 900, 100, -1, 20.05);
    CHECK(made >= 0 && g_make);
    CHECK_EQ(cui_pool_reap(g_pool, 20.1, 100, &due), -1);   /* none on a tick that made one */
    CHECK(due > 0);
    CHECK_EQ(cui_pool_reap(g_pool, 20.2, 100, &due), s[1]);
    CHECK(due < 0);
    CHECK_EQ(cui_pool_reap(g_pool, 30.0, 100, &due), -1);     /* never a taken one */
    CHECK_EQ(slots(), 2);
    CHECK_EQ(owners(), 2);
}

static int spares(void) {
    int n = 0;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) n += g_pool->spare_since[i] > 0;
    return n;
}

static void drain(void) {   /* above the cap one a tick, above half one each 10th tick or its time */
    fresh();
    double due = 0, t = 2.0;
    for (int i = 0; i < 10; i++) {
        int at = take(40 + (cui_id)i, 64 * (i + 1), 64, -1, 1.0);
        cui_pool_drawn(g_pool, at, 1, NULL, -1);
        cui_pool_give_back(g_pool, 40 + (cui_id)i, 1.0 + 0.01 * i);
    }
    CHECK_EQ(cui_pool_reap(g_pool, t, 100, &due), -1);   /* (the tick that made them) */
    CHECK(cui_pool_reap(g_pool, t += 0.001, 100, &due) >= 0);
    CHECK(cui_pool_reap(g_pool, t += 0.001, 100, &due) >= 0);
    CHECK_EQ(spares(), CUI_POOL_SPARES);
    int ticks = 0;
    while (cui_pool_reap(g_pool, t += 0.001, 100, &due) < 0) ticks++;
    CHECK_EQ(ticks, CUI_POOL_FAST_TICKS - 1);
    CHECK_EQ(spares(), CUI_POOL_SPARES - 1);
    CHECK_EQ(cui_pool_reap(g_pool, t += 0.001, 100, &due), -1);   /* idle: one wakeup at ten ticks' time */
    CHECK(due > CUI_POOL_FAST_TICKS * CUI_POOL_TICK_SECONDS - 0.01 && due < CUI_POOL_FAST_TICKS * CUI_POOL_TICK_SECONDS);
    CHECK(cui_pool_reap(g_pool, t += due, 100, &due) >= 0);
    while (spares() > CUI_POOL_SPARES / 2) CHECK(cui_pool_reap(g_pool, t += 0.2, 100, &due) >= 0);
    CHECK_EQ(cui_pool_reap(g_pool, t += 0.2, 100, &due), -1);   /* half: by age now */
    CHECK(due > 0 && due < CUI_POOL_SPARE_SECONDS);
}

static void full_table(void) {
    fresh();
    double due = 0;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        int at = take(1000 + (cui_id)i, 64 * (i + 1), 64, -1, 1.0);
        CHECK(at >= 0 && g_make);
        cui_pool_drawn(g_pool, at, 1, NULL, -1);
    }
    CHECK_EQ(slots(), CUI_POOL_SLOTS);
    cui_pool_reap(g_pool, 1.0, 0, &due);
    CHECK_EQ(take(5000, 64, 6400, -1, 2.0), -1);   /* none spare: no room */
    CHECK_EQ(g_evict, -1);
    cui_pool_give_back(g_pool, 5000, 2.0);
    cui_pool_give_back(g_pool, 1005, 3.0);
    cui_pool_give_back(g_pool, 1003, 2.5);
    cui_pool_give_back(g_pool, 1009, 2.7);
    int old = -1;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) if (g_pool->spare_since[i] == 2.5) old = i;
    CHECK_EQ(take(5000, 64, 6400, -1, 4.0), -1);   /* the oldest spare goes this tick */
    CHECK_EQ(g_evict, old);
    CHECK_EQ(slots(), CUI_POOL_SLOTS - 1);
    for (int i = 0; i < CUI_POOL_OWNERS; i++) CHECK(g_pool->owner_id[i] != 1003);
    cui_pool_reap(g_pool, 4.0, 0, &due);
    int at = take(5000, 64, 6400, -1, 4.02);   /* the new picture the next */
    CHECK(at == old && g_make && g_evict < 0);
    CHECK_EQ(take(5001, 128, 6400, -1, 4.03), -1);   /* full again, and one made this tick: none goes */
    CHECK_EQ(g_evict, -1);
}

static void all_held(void) {
    fresh();
    int front = -1;
    for (int i = 0; i < CUI_POOL_PER_SURFACE; i++) {
        int at = take(7, 300, 200, front, 1.0);
        CHECK(at >= 0 && g_make);
        cui_pool_drawn(g_pool, at, (uint64_t)i + 1, NULL, -1);
        cui_pool_held(g_pool, at, true);
        front = at;
    }
    CHECK_EQ(take(7, 300, 200, front, 1.0), -1);   /* the compositor holds them all: the frame waits */
    CHECK(!g_make);
    CHECK_EQ(slots(), CUI_POOL_PER_SURFACE);
    cui_pool_held(g_pool, front, false);
    CHECK_EQ(take(7, 300, 200, front, 1.0), -1);   /* (the one on screen is not drawn into either) */
    int first = 0;
    while (!(g_pool->used >> first & 1) || first == front) first++;
    cui_pool_held(g_pool, first, false);
    CHECK_EQ(take(7, 300, 200, front, 1.0), first);
}

int main(void) {
    g_pool = calloc(1, sizeof *g_pool);
    if (!g_pool) return 1;
    printf("sizeof(cui_pool) %zu, cui_pool_owner %zu, cui_pool_slot %zu, cui_damage_kept %zu\n", sizeof(cui_pool), sizeof(cui_pool_owner),
           sizeof(cui_pool_slot), sizeof(cui_damage_kept));
    keys();
    reopen();
    recycle();
    run_of_popups();
    reap();
    drain();
    full_table();
    all_held();
    free(g_pool);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
