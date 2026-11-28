/* The buffer pool's bookkeeping: see cui_pool.h. */
#include "../cui_internal.h"
#include "cui_pool.h"

#if defined(CUI_VULKAN) || defined(CUI_D3D12) || defined(CUI_METAL) || defined(CUI_POOL_ALONE)

#define CUI__POOL_SPARE 2   /* key bit 1 */

uint16_t cui_pool_key(int w, int h) {   /* w steps bits 9..15, h steps bits 2..8, bit 0 valid */
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    return (uint16_t)((((w - 1) / 64) << 9) | (((h - 1) / 64) << 2) | 1);
}

int cui_pool_key_width(uint16_t key) { return key ? ((key >> 9) + 1) * 64 : 0; }
int cui_pool_key_height(uint16_t key) { return key ? (((key >> 2) & 127) + 1) * 64 : 0; }

int cui_pool_owner_of(cui_pool *p, cui_id element) {
    int open = -1, idle = -1;
    if (!element) return -1;
    for (int i = 0; i < CUI_POOL_OWNERS; i++) {
        if (p->owner_id[i] == element) return i;
        if (!p->owner_id[i] && open < 0) open = i;
    }
    for (int i = 0; i < CUI_POOL_OWNERS && open < 0 && idle < 0; i++) if (!p->owner[i].buffers) idle = i;   /* asked for, never drawn */
    if (open < 0) open = idle;
    if (open < 0) return -1;
    p->owner_id[open] = element;
    p->owner[open].buffers = 0;
    cui__damage_init(&p->owner[open].damage);
    return open;
}

static void cui__pool_unown(cui_pool *p, int owner) {
    if (p->owner[owner].buffers && --p->owner[owner].buffers) return;
    cui_damage_reset(&p->owner[owner].damage.head);
    p->owner_id[owner] = 0;
}

static void cui__pool_free(cui_pool *p, int i) {
    cui__pool_unown(p, p->slot[i].owner);
    p->used &= ~(1ull << i);
    p->key[i] = 0;
    p->spare_since[i] = 0;
    p->slot[i].flags = 0;
}

int cui_pool_take(cui_pool *p, cui_id element, int w, int h, int front, double now, bool *make, int *evict) {
    uint16_t k = cui_pool_key(w, h);
    int o = k ? cui_pool_owner_of(p, element) : -1, own = 0, back = -1, other = -1;
    *make = false;
    *evict = -1;
    if (o < 0) return -1;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        if (p->key[i] != k) continue;
        const cui_pool_slot *s = &p->slot[i];
        if (s->owner != o) continue;
        if (!(s->flags & CUI_POOL_HELD) && i != front) return i;
        own++;
    }
    if (own >= CUI_POOL_PER_SURFACE) return -1;
    for (int i = 0; i < CUI_POOL_SLOTS && back < 0; i++) {
        if (p->key[i] != (k | CUI__POOL_SPARE)) continue;
        const cui_pool_slot *s = &p->slot[i];
        if (s->flags & CUI_POOL_HELD) continue;
        if (s->owner == o) back = i;
        else if (other < 0 || p->spare_since[i] < p->spare_since[other]) other = i;
    }
    int at = back >= 0 ? back : other;
    if (at >= 0) {
        cui_pool_slot *s = &p->slot[at];
        if (at == other) {   /* another element's: its pixels mean nothing here */
            cui__pool_unown(p, s->owner);
            s->owner = (uint8_t)o;
            s->missed_count = -1;
            p->owner[o].buffers++;
        }
        s->flags |= CUI_POOL_TAKEN;
        p->key[at] = k;
        p->spare_since[at] = 0;
        return at;
    }
    for (int i = 0; i < CUI_POOL_SLOTS && at < 0; i++) if (!(p->used >> i & 1)) at = i;
    if (at < 0) {   /* a full table: the oldest spare goes now, the new picture is made next tick */
        for (int i = 0; i < CUI_POOL_SLOTS; i++) {
            if (!(p->key[i] & CUI__POOL_SPARE) || (p->slot[i].flags & CUI_POOL_HELD)) continue;
            if (at < 0 || p->spare_since[i] < p->spare_since[at]) at = i;
        }
        if (at >= 0 && !p->made) { cui__pool_free(p, at); *evict = at; }
        return -1;
    }
    cui_pool_slot *s = &p->slot[at];
    p->used |= 1ull << at;
    p->key[at] = k;
    p->spare_since[at] = 0;
    s->gpu_value = 0;
    s->owner = (uint8_t)o;
    s->flags = CUI_POOL_TAKEN;
    s->missed_count = -1;
    p->owner[o].buffers++;
    *make = p->made = true;
    return at;
}

void cui_pool_give_back(cui_pool *p, cui_id element, double now) {
    int o = -1;
    for (int i = 0; i < CUI_POOL_OWNERS && o < 0; i++) if (p->owner_id[i] == element) o = i;
    if (o < 0 || !element) return;
    if (!p->owner[o].buffers) { p->owner_id[o] = 0; return; }   /* asked for, never drawn */
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        if (!p->key[i] || (p->key[i] & CUI__POOL_SPARE) || p->slot[i].owner != o) continue;
        p->slot[i].flags &= (uint8_t)~CUI_POOL_TAKEN;
        p->key[i] |= CUI__POOL_SPARE;
        p->spare_since[i] = now > 0 ? now : 1e-9;
    }
}

void cui_pool_held(cui_pool *p, int slot, bool held) {
    if (held) p->slot[slot].flags |= CUI_POOL_HELD;
    else p->slot[slot].flags &= (uint8_t)~CUI_POOL_HELD;
}

void cui_pool_drawn(cui_pool *p, int slot, uint64_t gpu_value, const cui_rect *boxes, int count) {
    cui_pool_slot *into = &p->slot[slot];
    into->gpu_value = gpu_value;
    into->flags |= CUI_POOL_DRAWN;
    into->missed_count = 0;
    if (p->owner[into->owner].buffers < 2) return;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        cui_pool_slot *s = &p->slot[i];
        if (i == slot || !p->key[i] || s->owner != into->owner || s->missed_count < 0) continue;
        if (count < 0 || s->missed_count + count > CUI_POOL_MISSED) { s->missed_count = -1; continue; }
        if (!count) continue;
        memcpy(s->missed + s->missed_count, boxes, sizeof(cui_rect) * (size_t)count);
        s->missed_count = (int8_t)(s->missed_count + count);
    }
}

void cui_pool_cleared(cui_pool *p, int slot, bool remade) {
    p->slot[slot].missed_count = -1;
    if (remade) { p->slot[slot].flags &= (uint8_t)~CUI_POOL_DRAWN; p->slot[slot].gpu_value = 0; }
}

void cui_pool_drop(cui_pool *p, int slot) {
    if (p->used >> slot & 1) cui__pool_free(p, slot);
}

int cui_pool_reap(cui_pool *p, double now, uint64_t gpu_done, double *due) {
    int oldest = -1, spares = 0;
    bool made = p->made;
    p->made = false;
    p->tick++;
    *due = -1;
    for (int i = 0; i < CUI_POOL_SLOTS; i++) {
        if (p->spare_since[i] <= 0 || (p->slot[i].flags & CUI_POOL_HELD)) continue;   /* (held: no wakeup asks after it) */
        spares++;
        if (oldest < 0 || p->spare_since[i] < p->spare_since[oldest]) oldest = i;
    }
    if (!spares) return -1;
    double wait = 0;
    if (spares <= CUI_POOL_SPARES / 2) wait = p->spare_since[oldest] + CUI_POOL_SPARE_SECONDS - now;
    else if (spares <= CUI_POOL_SPARES && p->tick - p->reaped_tick < CUI_POOL_FAST_TICKS)   /* or, idle, their time */
        wait = p->reaped_at + CUI_POOL_FAST_TICKS * CUI_POOL_TICK_SECONDS - now;
    if (made || p->slot[oldest].gpu_value > gpu_done) wait = wait > CUI_POOL_TICK_SECONDS ? wait : CUI_POOL_TICK_SECONDS;
    if (wait > 0) { *due = wait; return -1; }
    cui__pool_free(p, oldest);
    p->reaped_tick = p->tick;
    p->reaped_at = now;
    if (spares > 1) *due = CUI_POOL_TICK_SECONDS;
    return oldest;
}

#endif
