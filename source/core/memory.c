/*
 * memory.c -- Memory Controller and GC scaffolding.
 *
 * Stage 1: registry of every live CdoObject / CdoThread, swept on VM
 * teardown so allocations stop leaking at process exit.
 *
 * Stage 2 (later): full mark-and-sweep -- this same registry plus a
 * per-kind tracer table will let us collect mid-execution.
 */

#include "memory.h"
#include <string.h>

/* =========================================================================
 * Live-object registry helpers (gc_lock must be held by the caller)
 * ===================================================================== */

static void live_grow(CandoMemCtrl *mc) {
    if (mc->live_cap == 0) mc->live_cap = 64;
    else                   mc->live_cap *= 2;
    mc->live = (CandoTrackedObj *)cando_realloc(
                   mc->live, sizeof(CandoTrackedObj) * mc->live_cap);
}

/* Linear search + swap-with-last.  Stage 2 will switch to an index
 * stored on the object header so we can untrack in O(1).               */
static bool live_remove_by_obj(CandoMemCtrl *mc, void *obj) {
    for (u32 i = 0; i < mc->live_count; i++) {
        if (mc->live[i].obj == obj) {
            u32 new_count = mc->live_count - 1;
            mc->live[i] = mc->live[new_count];
            __atomic_store_n(&mc->live_count, new_count, __ATOMIC_RELAXED);
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Lifecycle
 * ===================================================================== */

void cando_memctrl_init(CandoMemCtrl *mc) {
    cando_handle_table_init(&mc->handles, 0);
    cando_lock_init(&mc->gc_lock);
    mc->live       = NULL;
    mc->live_count = 0;
    mc->live_cap   = 0;
    /* Initial auto-collect threshold: 256 tracked objects.  After each
     * collect this is reset to 2 * live_count so the trigger
     * self-tunes to the working set size.                             */
    mc->next_collect_threshold = 256;
}

void cando_memctrl_destroy(CandoMemCtrl *mc) {
    /* Walk the registry once, destroying each tracked object.  Note we
     * intentionally do NOT call cando_memctrl_untrack from inside the
     * destructors: the registry is being torn down wholesale, so any
     * recursive untrack would just be wasted work and the swap-with-last
     * pattern would visit the same indices we're already iterating.    */
    for (u32 i = 0; i < mc->live_count; i++) {
        CandoTrackedObj *e = &mc->live[i];
        if (e->obj && e->destroy) e->destroy(e->obj);
    }
    cando_free(mc->live);
    mc->live       = NULL;
    mc->live_count = 0;
    mc->live_cap   = 0;

    cando_handle_table_destroy(&mc->handles);
}

/* =========================================================================
 * Object tracking
 * ===================================================================== */

void cando_memctrl_track(CandoMemCtrl *mc, void *obj,
                         void (*destroy)(void *)) {
    if (!mc || !obj) return;
    cando_lock_write_acquire(&mc->gc_lock);
    if (mc->live_count == mc->live_cap) live_grow(mc);
    mc->live[mc->live_count].obj     = obj;
    mc->live[mc->live_count].destroy = destroy;
    mc->live[mc->live_count].marked  = false;
    /* Relaxed atomic store: the unsynchronised reader in
     * vm_gc_maybe_collect uses __atomic_load_n on the same field. */
    __atomic_store_n(&mc->live_count, mc->live_count + 1, __ATOMIC_RELAXED);
    cando_lock_write_release(&mc->gc_lock);
}

void cando_memctrl_untrack(CandoMemCtrl *mc, void *obj) {
    if (!mc || !obj) return;
    cando_lock_write_acquire(&mc->gc_lock);
    live_remove_by_obj(mc, obj);
    cando_lock_write_release(&mc->gc_lock);
}

/* =========================================================================
 * Mark-and-sweep
 *
 * The mark step uses a linear scan over the registry to locate the
 * entry for a given pointer.  At ~100k tracked objects this is still
 * sub-millisecond per mark; Stage 3 will switch to a hash-keyed lookup
 * if profiling shows it matters in practice.
 * ===================================================================== */

void cando_memctrl_clear_marks(CandoMemCtrl *mc) {
    if (!mc) return;
    cando_lock_write_acquire(&mc->gc_lock);
    for (u32 i = 0; i < mc->live_count; i++)
        mc->live[i].marked = false;
    cando_lock_write_release(&mc->gc_lock);
}

bool cando_memctrl_mark(CandoMemCtrl *mc, void *obj) {
    if (!mc || !obj) return false;
    /* No lock: the marker runs synchronously on the collecting thread
     * with allocations paused (callers hold gc_lock externally for the
     * duration of the collect cycle).                                  */
    for (u32 i = 0; i < mc->live_count; i++) {
        if (mc->live[i].obj == obj) {
            if (mc->live[i].marked) return false;  /* already seen */
            mc->live[i].marked = true;
            return true;                            /* fresh -- recurse */
        }
    }
    /* Not in registry: untracked legacy object; nothing to do.        */
    return false;
}

void cando_memctrl_sweep(CandoMemCtrl *mc,
                        CandoFreeHandleFn free_handle,
                        void *handle_user) {
    if (!mc) return;
    cando_lock_write_acquire(&mc->gc_lock);
    u32 i = 0;
    while (i < mc->live_count) {
        CandoTrackedObj *e = &mc->live[i];
        if (e->marked) {
            i++;
            continue;
        }
        /* Capture the destructor pair before swap-with-last potentially
         * overwrites this slot.                                        */
        void *obj = e->obj;
        void (*destroy)(void *) = e->destroy;
        /* Remove from the registry first so the destroy hook can safely
         * re-enter (e.g. a finalizer that allocates new objects).      */
        u32 new_count = mc->live_count - 1;
        mc->live[i] = mc->live[new_count];
        __atomic_store_n(&mc->live_count, new_count, __ATOMIC_RELAXED);
        if (free_handle) free_handle(handle_user, obj);
        if (destroy)     destroy(obj);
        /* Don't increment i -- a different entry now sits at this slot. */
    }
    cando_lock_write_release(&mc->gc_lock);
}

/* =========================================================================
 * Active memctrl (per OS thread)
 * ===================================================================== */

static _Thread_local CandoMemCtrl *tl_active_memctrl = NULL;

void cando_gc_set_active_memctrl(CandoMemCtrl *mc) {
    tl_active_memctrl = mc;
}

CandoMemCtrl *cando_gc_active_memctrl(void) {
    return tl_active_memctrl;
}

void cando_gc_track(void *obj, void (*destroy)(void *)) {
    cando_memctrl_track(tl_active_memctrl, obj, destroy);
}

void cando_gc_untrack(void *obj) {
    cando_memctrl_untrack(tl_active_memctrl, obj);
}
