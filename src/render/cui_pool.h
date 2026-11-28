/* One buffer pool for the GPU renderers: bookkeeping only, it returns slot indices and flags. */
#ifndef CUI_POOL_H
#define CUI_POOL_H

#include "cui_damage.h"

#define CUI_POOL_SLOTS 64           /* one bit each in cui_pool.used */
#define CUI_POOL_OWNERS (CUI_POOL_SLOTS + CUI_SURFACES + 1)   /* one a slot, one a surface not yet drawn */
#define CUI_POOL_MISSED 24          /* boxes a picture can be behind by before all of it is drawn */
#define CUI_POOL_PER_SURFACE 3      /* shown, drawn into, and one the compositor still reads */
#define CUI_POOL_SPARES 8           /* spares above this go one a tick, above half of it one each 10th */
#define CUI_POOL_SPARE_SECONDS 5.0  /* at half or fewer, a spare goes once it is this old */
#define CUI_POOL_FAST_TICKS 10
#define CUI_POOL_TICK_SECONDS (1.0 / 60)   /* while idle, a tick is this long */

enum { CUI_POOL_TAKEN = 1, CUI_POOL_HELD = 2, CUI_POOL_DRAWN = 4 };

typedef struct cui_pool_slot {      /* read together once a slot is found */
    uint64_t gpu_value;             /* last submission that drew into it */
    uint8_t  owner;                 /* index into the owner tables, also while spare */
    uint8_t  flags;                 /* CUI_POOL_TAKEN, _HELD, _DRAWN */
    int8_t   missed_count;          /* -1: all of it */
    cui_rect missed[CUI_POOL_MISSED];
} cui_pool_slot;

typedef struct cui_pool_owner {     /* read together once an element is found */
    uint8_t    buffers;             /* slots still pointing at it */
    cui_damage_kept damage;
} cui_pool_owner;

typedef struct cui_pool {
    uint64_t       used;                          /* one bit per slot */
    uint16_t       key[CUI_POOL_SLOTS];           /* scanned: find by size bucket */
    double         spare_since[CUI_POOL_SLOTS];   /* scanned by reap; 0 while taken */
    cui_pool_slot  slot[CUI_POOL_SLOTS];
    cui_id         owner_id[CUI_POOL_OWNERS];     /* scanned: find by element; 0: free */
    cui_pool_owner owner[CUI_POOL_OWNERS];
    uint32_t       tick;                          /* cui_pool_reap calls */
    uint32_t       reaped_tick;
    double         reaped_at;
    bool           made;                          /* a take this tick asked for a new picture */
} cui_pool;

uint16_t cui_pool_key(int w, int h);   /* 64 px steps; bit 0 valid, bit 1 spare; 0: not poolable */
int cui_pool_key_width(uint16_t key);
int cui_pool_key_height(uint16_t key);

int cui_pool_owner_of(cui_pool *p, cui_id element);   /* found or made; -1: the table is full */

/* The slot to draw into, or -1: all held or on screen (front), or *evict freed to make room next tick. */
int cui_pool_take(cui_pool *p, cui_id element, int w, int h, int front, double now, bool *make, int *evict);
void cui_pool_give_back(cui_pool *p, cui_id element, double now);   /* its slots go spare; nothing is cleared */
void cui_pool_held(cui_pool *p, int slot, bool held);
/* Drawn with submission gpu_value; the owner's other slots, spares too, missed boxes (-1: all). */
void cui_pool_drawn(cui_pool *p, int slot, uint64_t gpu_value, const cui_rect *boxes, int count);
void cui_pool_cleared(cui_pool *p, int slot, bool remade);   /* its pixels are of no use; remade: a new picture */
void cui_pool_drop(cui_pool *p, int slot);                   /* its picture is gone */
/* A tick: a spare to destroy or -1, none if one was made; *due: seconds to the next, <0: none */
int cui_pool_reap(cui_pool *p, double now, uint64_t gpu_done, double *due);

#endif /* CUI_POOL_H */
