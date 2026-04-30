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

#endif /* CANDO_MEMORY_H */
