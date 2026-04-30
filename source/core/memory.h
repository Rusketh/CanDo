/*
 * memory.h -- Memory Controller and GC scaffolding for Cando.
 *
 * The Memory Controller (CandoMemCtrl) owns the registry of all live
 * heap-allocated CdoObjects (and CdoThreads, which share its layout
 * prefix).  Every cdo_obj_alloc / cdo_thread_new registers itself here
 * via cando_memctrl_track and is freed at VM teardown by walking the
 * registry.  This is Stage 1 of GC plumbing: it stops objects leaking
 * at process exit.  Stage 2 will use the same registry plus per-kind
 * tracers to implement mark-and-sweep collection mid-execution.
 *
 * Each tracked entry carries its own destructor function pointer so
 * different object kinds (CdoObject vs CdoThread, etc.) can dispose of
 * their internals correctly.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_MEMORY_H
#define CANDO_MEMORY_H

#include "common.h"
#include "lock.h"
#include "value.h"
#include "handle.h"

/* -----------------------------------------------------------------------
 * CandoTrackedObj -- one entry in the live registry
 * --------------------------------------------------------------------- */
typedef struct {
    void *obj;
    /* Called once when the object is collected.  Must release any
     * heap state owned by the object AND free the object pointer
     * itself.                                                          */
    void (*destroy)(void *obj);
    /* Mark-and-sweep: cleared at the start of each collection cycle,
     * set when the marker reaches the entry, swept (destroy + remove)
     * if still false at the end of the cycle.                          */
    bool marked;
} CandoTrackedObj;

/* -----------------------------------------------------------------------
 * CandoMemCtrl -- the Memory Controller
 * --------------------------------------------------------------------- */
typedef struct CandoMemCtrl {
    CandoHandleTable handles;       /* unused yet; reserved for Stage 2 */
    CandoLockHeader  gc_lock;       /* serialises track / collect       */
    CandoTrackedObj *live;          /* registry of live tracked objects */
    u32              live_count;
    u32              live_cap;
    /* Auto-collect threshold (Stage 3): when live_count reaches this
     * after an allocation, the VM runs a collection cycle and bumps
     * the threshold to 2 * live_count_after.  Setting to 0 disables
     * the automatic trigger (manual gc_collect still works).         */
    u32              next_collect_threshold;
} CandoMemCtrl;

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

void cando_memctrl_init(CandoMemCtrl *mc);

/* Destroy every still-tracked object via its registered destructor,
 * then release the registry itself.  Idempotent.                       */
void cando_memctrl_destroy(CandoMemCtrl *mc);

/* -----------------------------------------------------------------------
 * Object tracking
 * --------------------------------------------------------------------- */

/* Register `obj` as live with the given destructor.  Called by every
 * cdo_obj_alloc / cdo_thread_new on the active VM's memctrl.           */
void cando_memctrl_track(CandoMemCtrl *mc, void *obj,
                         void (*destroy)(void *));

/* Remove `obj` from the registry without destroying it.  Used when an
 * object is freed early via an explicit code path so the destroy-all
 * sweep at VM teardown does not double-free.                           */
void cando_memctrl_untrack(CandoMemCtrl *mc, void *obj);

/* -----------------------------------------------------------------------
 * Active memctrl (per OS thread)
 *
 * Allocators (cdo_obj_alloc / cdo_thread_new) call cando_gc_track /
 * cando_gc_untrack with the object they just produced; those helpers
 * forward to whichever memctrl was last installed via
 * cando_gc_set_active_memctrl on this OS thread.  vm.c installs the
 * VM's memctrl in cando_vm_init / cando_vm_init_child / the spawned-
 * thread trampoline so allocations on every VM-owned thread land in
 * the right registry.  When no memctrl is active (e.g. unit tests
 * exercising the lexer or object layer in isolation) the helpers are
 * a no-op and allocations are not tracked.
 * --------------------------------------------------------------------- */

void cando_gc_set_active_memctrl(CandoMemCtrl *mc);
CandoMemCtrl *cando_gc_active_memctrl(void);

void cando_gc_track(void *obj, void (*destroy)(void *));
void cando_gc_untrack(void *obj);

/* -----------------------------------------------------------------------
 * Mark-and-sweep collection
 *
 * The collector's mark phase is driven from the VM layer (which knows
 * the roots and the per-kind tracer functions).  These primitives are
 * the agnostic plumbing the VM calls into:
 *
 *   1.  cando_memctrl_clear_marks(mc)  -- reset every entry's marked.
 *   2.  cando_memctrl_mark(mc, obj)    -- set marked, returns true if
 *                                          the entry was unmarked
 *                                          before (so the caller knows
 *                                          whether to recurse into it).
 *   3.  cando_memctrl_sweep(mc, free_handle, vm)
 *                                       -- destroy every still-unmarked
 *                                          entry, optionally calling
 *                                          free_handle(vm, obj) before
 *                                          destruction so the VM can
 *                                          release the object's handle
 *                                          slot.
 *
 * Callers should hold the gc_lock during a full collection cycle if
 * other threads might be allocating concurrently.
 * --------------------------------------------------------------------- */

void cando_memctrl_clear_marks(CandoMemCtrl *mc);

/* Returns true if the entry transitioned from unmarked to marked (the
 * caller should then trace its children); false if `obj` is not in the
 * registry or was already marked.                                       */
bool cando_memctrl_mark(CandoMemCtrl *mc, void *obj);

/* Free every unmarked entry.  `free_handle` may be NULL; otherwise it
 * is called with `(handle_user, entry->obj)` immediately before destroy
 * so the caller can release any handle table slot the object owns.    */
typedef void (*CandoFreeHandleFn)(void *handle_user, void *obj);
void cando_memctrl_sweep(CandoMemCtrl *mc,
                         CandoFreeHandleFn free_handle,
                         void *handle_user);

#endif /* CANDO_MEMORY_H */
