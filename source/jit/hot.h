/*
 * jit/hot.h -- per-PC hot counter table for the recorder trigger.
 *
 * Phase 3.2 of docs/jit-plan.md (§5).  When `vm->jit_enabled` is true,
 * the dispatch loop calls cando_hot_hit() on every backedge,
 * function-entry, and iterator NEXT to bump the counter for that
 * specific bytecode site.  When a counter crosses the threshold the
 * function returns true and the recorder is invited to start a trace
 * rooted at that PC.
 *
 * The key is the live IP pointer (chunk->code + offset).  Chunks are
 * immortal once compiled, so the pointer is stable for the script's
 * lifetime; we don't need a (chunk, offset) tuple.
 *
 * Storage: chained hash.  Buckets are a power-of-two count; growth
 * doubles when the load factor hits 0.75.  Overhead is dominated by
 * the buckets array (64 entries × pointer width = 512 bytes when off).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_HOT_H
#define CANDO_JIT_HOT_H

#include "../core/common.h"

/* -----------------------------------------------------------------------
 * Tuning knobs
 * --------------------------------------------------------------------- */

/* Default number of times a PC must fire before the recorder is
 * invoked.  Matches LuaJIT's default; higher values reduce false
 * positives on cold loops, lower values trace earlier. */
#define CANDO_HOT_DEFAULT_THRESHOLD  56u

#define CANDO_HOT_INITIAL_BUCKETS    64u   /* must be power of 2 */
#define CANDO_HOT_LOAD_FACTOR_NUM    3u    /* grow when entry/bucket > 3/4 */
#define CANDO_HOT_LOAD_FACTOR_DEN    4u

/* -----------------------------------------------------------------------
 * CandoHotEntry -- one (PC, count) record.
 *
 * `blacklisted` is set after the recorder aborts (Phase 3.3) so the
 * counter no longer triggers re-entry.  Counts continue to accumulate
 * for diagnostics but cando_hot_hit returns false on a blacklisted PC.
 * --------------------------------------------------------------------- */
typedef struct CandoHotEntry {
    const u8                *pc;
    u32                      count;
    u8                       blacklisted;
    u8                       _pad[3];
    struct CandoHotEntry    *next;
} CandoHotEntry;

/* -----------------------------------------------------------------------
 * CandoHotTable
 * --------------------------------------------------------------------- */
typedef struct CandoHotTable {
    CandoHotEntry **buckets;
    u32             bucket_count;
    u32             entry_count;
    u32             blacklist_count;
    u32             threshold;
} CandoHotTable;

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

/* cando_hot_table_init -- prepare an empty table.  threshold==0 falls
 * back to CANDO_HOT_DEFAULT_THRESHOLD. */
CANDO_API void cando_hot_table_init(CandoHotTable *t, u32 threshold);

/* cando_hot_table_destroy -- release all entries and the bucket array. */
CANDO_API void cando_hot_table_destroy(CandoHotTable *t);

/* -----------------------------------------------------------------------
 * Hits and queries
 * --------------------------------------------------------------------- */

/* cando_hot_hit -- bump the counter for `pc`, allocating a new entry
 * on first sight.  Returns true exactly once per PC: when the count
 * transitions to >= threshold AND the PC is not blacklisted.  After
 * that the entry is auto-blacklisted to prevent re-trigger; the
 * caller (Phase 3.3 recorder) is responsible for un-blacklisting on
 * successful trace completion. */
CANDO_API bool cando_hot_hit(CandoHotTable *t, const u8 *pc);

/* cando_hot_blacklist -- mark `pc` so cando_hot_hit returns false
 * unconditionally.  Idempotent.  Used by the recorder when a trace
 * aborts. */
CANDO_API void cando_hot_blacklist(CandoHotTable *t, const u8 *pc);

/* cando_hot_unblacklist -- clear the blacklist bit for `pc` and
 * reset its counter so a subsequent run can re-trigger recording.
 * No-op if `pc` is unknown.  Used by the trace cache when an LRU
 * entry is evicted -- the slot is gone, but the loop may still be
 * worth re-recording if it goes hot again. */
CANDO_API void cando_hot_unblacklist(CandoHotTable *t, const u8 *pc);

/* cando_hot_count -- read the current count for `pc` (0 if absent). */
CANDO_API u32  cando_hot_count(const CandoHotTable *t, const u8 *pc);

/* cando_hot_is_blacklisted -- read the blacklist bit (false if absent). */
CANDO_API bool cando_hot_is_blacklisted(const CandoHotTable *t,
                                        const u8 *pc);

/* -----------------------------------------------------------------------
 * Tuning
 * --------------------------------------------------------------------- */
CANDO_API void cando_hot_set_threshold(CandoHotTable *t, u32 n);
CANDO_API u32  cando_hot_get_threshold(const CandoHotTable *t);

/* cando_hot_entry_count       -- distinct PCs tracked.
 * cando_hot_blacklist_count   -- of those, how many are blacklisted. */
CANDO_API u32 cando_hot_entry_count(const CandoHotTable *t);
CANDO_API u32 cando_hot_blacklist_count(const CandoHotTable *t);

#endif /* CANDO_JIT_HOT_H */
