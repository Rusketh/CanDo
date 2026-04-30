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
            mc->live[i] = mc->live[--mc->live_count];
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
    mc->live_count++;
    cando_lock_write_release(&mc->gc_lock);
}

void cando_memctrl_untrack(CandoMemCtrl *mc, void *obj) {
    if (!mc || !obj) return;
    cando_lock_write_acquire(&mc->gc_lock);
    live_remove_by_obj(mc, obj);
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
