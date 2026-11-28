/* The damage tracker's layout (cui_damage.c), for what keeps trackers by value: cui_pool.h. */
#ifndef CUI_DAMAGE_H
#define CUI_DAMAGE_H

#include "cui/cui_render.h"

#define CUI_DAMAGE_RECORDS 1024    /* a kept tracker's last frame holds this many; more: no history */
#define CUI_DAMAGE_COMMANDS 128

typedef struct cui__dmg_box { int x0, y0, x1, y1; } cui__dmg_box;

typedef struct cui__dmg_store {     /* cui_damage_create's arrays: they grow */
    cui_instance *inst;
    cui_draw_cmd *cmd;
    uint16_t *cmd_of[2];
    cui_draw_cmd *clipped;          /* cui_damage_clip's commands */
    int inst_cap, cmd_cap, clipped_cap;
} cui__dmg_store;

struct cui_damage {
    int inst_count, cmd_count;      /* the last frame's records and commands */
    uint64_t vertex_hash;
    cui__dmg_box vertex_box;
    int width, height;
    cui_color clear;
    bool have;
    bool heap;                      /* cui_damage_create's: arrays in grown; else it heads a cui_damage_kept */
    uint8_t of;                     /* which cmd_of row is the last frame's */
    bool direct;                    /* see cui_damage_direct */
    bool placed;                    /* cui_damage_track_lists: where its list lay last frame */
    int box_count;                  /* -1: all of it; 0: nothing; 1: bound */
    cui__dmg_box bound;             /* what the last frame changed */
    int at_x, at_y, at_w, at_h;
    int large_run;                  /* frames in a row that changed a large share of the window */
    cui_damage_stats stats;
    cui_draw_data clipped_data;
    cui__dmg_store grown;
};

typedef struct cui_damage_kept {    /* a tracker by value: no memory of its own, fixed arrays */
    cui_damage head;
    uint16_t cmd_of[2][CUI_DAMAGE_RECORDS];   /* the command a record belongs to, last frame and this */
    cui_draw_cmd cmd[CUI_DAMAGE_COMMANDS];
    cui_instance inst[CUI_DAMAGE_RECORDS];
} cui_damage_kept;

void cui__damage_init(cui_damage_kept *k);
int cui__damage_clip_need(const cui_draw_data *dd, int boxes);   /* commands cui__damage_clip_to may write */
int cui__damage_clip_to(const cui_draw_data *dd, const cui_rect *rects, int count, cui_draw_cmd *out);

#endif /* CUI_DAMAGE_H */
