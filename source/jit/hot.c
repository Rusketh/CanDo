/*
 * jit/hot.c -- per-PC hot counter table.
 *
 * See hot.h for the surface and design notes.
 */

#include "hot.h"

#include <string.h>

/* -----------------------------------------------------------------------
 * Hash
 *
 * The PC pointer is at least 16-byte aligned in practice (chunk->code
 * is heap-allocated, instructions are byte-addressed but the
 * allocation alignment dominates the low bits).  Shift the pointer
 * down before masking so the low buckets aren't strongly preferred.
 * --------------------------------------------------------------------- */
static u32 hot_hash(const u8 *pc, u32 bucket_count) {
    uptr p = (uptr)pc;
    /* Multiplicative mix: spreads adjacent PCs (consecutive offsets in
     * the same chunk) across distinct buckets. */
    p ^= p >> 33;
    p *= 0xFF51AFD7ED558CCDull;
    p ^= p >> 33;
    return (u32)p & (bucket_count - 1);
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
void cando_hot_table_init(CandoHotTable *t, u32 threshold) {
    if (!t) return;
    t->bucket_count    = CANDO_HOT_INITIAL_BUCKETS;
    t->entry_count     = 0;
    t->blacklist_count = 0;
    t->threshold       = threshold ? threshold : CANDO_HOT_DEFAULT_THRESHOLD;
    t->buckets         = cando_alloc(sizeof(CandoHotEntry *) * t->bucket_count);
    memset(t->buckets, 0, sizeof(CandoHotEntry *) * t->bucket_count);
}

void cando_hot_table_destroy(CandoHotTable *t) {
    if (!t) return;
    for (u32 b = 0; b < t->bucket_count; b++) {
        CandoHotEntry *e = t->buckets[b];
        while (e) {
            CandoHotEntry *next = e->next;
            cando_free(e);
            e = next;
        }
    }
    cando_free(t->buckets);
    t->buckets         = NULL;
    t->bucket_count    = 0;
    t->entry_count     = 0;
    t->blacklist_count = 0;
}

/* -----------------------------------------------------------------------
 * Internal: lookup / insert
 * --------------------------------------------------------------------- */
static CandoHotEntry *hot_find(const CandoHotTable *t, const u8 *pc,
                               u32 *out_bucket) {
    u32 b = hot_hash(pc, t->bucket_count);
    if (out_bucket) *out_bucket = b;
    for (CandoHotEntry *e = t->buckets[b]; e; e = e->next)
        if (e->pc == pc) return e;
    return NULL;
}

static void hot_grow(CandoHotTable *t) {
    u32              old_count   = t->bucket_count;
    CandoHotEntry  **old_buckets = t->buckets;

    u32 new_count = old_count * 2;
    CandoHotEntry **new_buckets =
        cando_alloc(sizeof(CandoHotEntry *) * new_count);
    memset(new_buckets, 0, sizeof(CandoHotEntry *) * new_count);

    for (u32 b = 0; b < old_count; b++) {
        CandoHotEntry *e = old_buckets[b];
        while (e) {
            CandoHotEntry *next = e->next;
            u32 nb = hot_hash(e->pc, new_count);
            e->next = new_buckets[nb];
            new_buckets[nb] = e;
            e = next;
        }
    }
    cando_free(old_buckets);
    t->buckets      = new_buckets;
    t->bucket_count = new_count;
}

static CandoHotEntry *hot_intern(CandoHotTable *t, const u8 *pc) {
    /* Grow before insert so the bucket index we compute is valid post-grow. */
    if (t->entry_count * CANDO_HOT_LOAD_FACTOR_DEN >=
        t->bucket_count * CANDO_HOT_LOAD_FACTOR_NUM)
        hot_grow(t);

    u32 b;
    CandoHotEntry *e = hot_find(t, pc, &b);
    if (e) return e;

    e = cando_alloc(sizeof(CandoHotEntry));
    e->pc          = pc;
    e->count       = 0;
    e->blacklisted = 0;
    e->next        = t->buckets[b];
    t->buckets[b]  = e;
    t->entry_count++;
    return e;
}

/* -----------------------------------------------------------------------
 * Hits and queries
 * --------------------------------------------------------------------- */
bool cando_hot_hit(CandoHotTable *t, const u8 *pc) {
    if (!t || !pc) return false;
    CandoHotEntry *e = hot_intern(t, pc);
    if (e->blacklisted) {
        /* Still bump for diagnostic visibility; never trigger. */
        e->count++;
        return false;
    }
    e->count++;
    if (e->count >= t->threshold) {
        /* Auto-blacklist to prevent re-trigger before the recorder
         * decides what to do.  Phase 3.3+: the recorder un-blacklists
         * on successful trace completion, or extends the blacklist
         * with backoff on abort. */
        e->blacklisted = 1;
        t->blacklist_count++;
        return true;
    }
    return false;
}

void cando_hot_blacklist(CandoHotTable *t, const u8 *pc) {
    if (!t || !pc) return;
    CandoHotEntry *e = hot_intern(t, pc);
    if (!e->blacklisted) {
        e->blacklisted = 1;
        t->blacklist_count++;
    }
}

u32 cando_hot_count(const CandoHotTable *t, const u8 *pc) {
    if (!t || !pc) return 0;
    const CandoHotEntry *e = hot_find(t, pc, NULL);
    return e ? e->count : 0;
}

bool cando_hot_is_blacklisted(const CandoHotTable *t, const u8 *pc) {
    if (!t || !pc) return false;
    const CandoHotEntry *e = hot_find(t, pc, NULL);
    return e ? (e->blacklisted != 0) : false;
}

/* -----------------------------------------------------------------------
 * Tuning
 * --------------------------------------------------------------------- */
void cando_hot_set_threshold(CandoHotTable *t, u32 n) {
    if (!t) return;
    t->threshold = n ? n : CANDO_HOT_DEFAULT_THRESHOLD;
}

u32 cando_hot_get_threshold(const CandoHotTable *t) {
    return t ? t->threshold : 0;
}

u32 cando_hot_entry_count(const CandoHotTable *t) {
    return t ? t->entry_count : 0;
}

u32 cando_hot_blacklist_count(const CandoHotTable *t) {
    return t ? t->blacklist_count : 0;
}
