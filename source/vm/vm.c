/*
 * vm.c -- Cando bytecode interpreter implementation.
 *
 * The dispatch loop uses a labelled-goto table (computed gotos) when
 * compiling with GCC/Clang (__GNUC__); otherwise falls back to a switch
 * statement.  Both paths are covered by the same macro shell.
 *
 * Must compile with gcc -std=c11.
 */

#include "vm.h"
#include "bridge.h"
#include "../core/value.h"
#include "../core/thread_platform.h"
#include "../object/function.h"
#include "../object/thread.h"
#include "../jit/jit.h"
#include "../jit/hot.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#if !defined(_WIN32) && !defined(_WIN64)
#  include <dlfcn.h>   /* dlclose() for binary module cleanup */
#endif

/* =========================================================================
 * Thread-local: current CdoThread for the running OS thread (NULL = main)
 * ===================================================================== */
_Thread_local static CdoThread *tl_current_thread = NULL;

/* Thread-local: VM currently owning the calling OS thread.  Set by
 * cando_vm_init / cando_vm_init_child (and the thread trampoline) so
 * allocators -- which run synchronously on whichever OS thread is
 * compiling, evaluating, or executing the script -- can find the
 * active memctrl to register their allocations against.  May be NULL
 * for callers that never enter a VM (e.g. unit tests that exercise the
 * lexer in isolation), in which case allocations are not tracked and
 * leak as before.                                                      */
_Thread_local static CandoVM *tl_current_vm = NULL;

CdoThread *cando_current_thread(void) {
    return tl_current_thread;
}

CandoVM *cando_current_vm(void) {
    return tl_current_vm;
}

void cando_vm_wait_all_threads(CandoVM *vm) {
    CandoThreadRegistry *reg = vm->thread_registry;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    while (reg->count > 0)
        cando_os_cond_wait(&reg->cond, &reg->mutex);
    cando_os_mutex_unlock(&reg->mutex);
}

void cando_vm_wait_all_lifelines(CandoVM *vm) {
    /* Currently aliased to wait_all_threads -- the registry counts both
     * `thread { }` blocks and native lifelines via the same counter. */
    cando_vm_wait_all_threads(vm);
}

void cando_vm_lifeline_acquire(CandoVM *vm, const char *kind) {
    (void)kind;  /* reserved for diagnostics in a later commit */
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    reg->count++;
    cando_os_mutex_unlock(&reg->mutex);
}

void cando_vm_lifeline_release(CandoVM *vm) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    if (reg->count > 0) reg->count--;
    cando_os_cond_broadcast(&reg->cond);
    cando_os_mutex_unlock(&reg->mutex);
}

void cando_vm_request_quit(CandoVM *vm, int exit_code) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    reg->quit_requested = 1;
    reg->exit_code      = exit_code;
    cando_os_cond_broadcast(&reg->cond);
    cando_os_mutex_unlock(&reg->mutex);
}

bool cando_vm_quit_requested(CandoVM *vm) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return false;
    cando_os_mutex_lock(&reg->mutex);
    bool q = reg->quit_requested != 0;
    cando_os_mutex_unlock(&reg->mutex);
    return q;
}

int cando_vm_get_exit_code(CandoVM *vm) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return 0;
    cando_os_mutex_lock(&reg->mutex);
    int c = reg->exit_code;
    cando_os_mutex_unlock(&reg->mutex);
    return c;
}

void cando_vm_set_exit_code(CandoVM *vm, int exit_code) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    reg->exit_code = exit_code;
    cando_os_mutex_unlock(&reg->mutex);
}

u32 cando_vm_lifeline_count(CandoVM *vm) {
    CandoThreadRegistry *reg = vm ? vm->thread_registry : NULL;
    if (!reg) return 0;
    cando_os_mutex_lock(&reg->mutex);
    u32 c = reg->count;
    cando_os_mutex_unlock(&reg->mutex);
    return c;
}

/* =========================================================================
 * Internal forward declarations
 * ===================================================================== */
static CandoVMResult  vm_run(CandoVM *vm);
static void           vm_runtime_error(CandoVM *vm, const char *fmt, ...);
static bool           vm_call(CandoVM *vm, CandoClosure *closure, u32 arg_count);
static bool           vm_push_frame(CandoVM *vm, CandoClosure *closure,
                                    u8 *ip, u32 arg_count, bool is_fluent);
static CandoUpvalue  *vm_capture_upvalue(CandoVM *vm, CandoValue *local);
static void           vm_close_upvalues(CandoVM *vm, CandoValue *last);
static void           vm_upvalue_release(CandoUpvalue *uv);
static void           vm_closure_trace_adapter(void *bytecode,
                                               CdoMarkFn mark, void *ud);
static bool           vm_is_truthy(CandoValue v);
static bool           vm_is_truthy_meta(CandoVM *vm, CandoValue v, bool *ok);
static u32            vm_global_hash(const char *str, u32 len);
static void           vm_call_closure_with_args(CandoVM *vm, CdoObject *fn_obj,
                                                 CandoValue *args, u32 argc);
static int            vm_native_class_default_call(CandoVM *vm, int argc,
                                                    CandoValue *args);

/* =========================================================================
 * VM lifecycle
 * ===================================================================== */

void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem) {
    /* Make the new VM the active one for this OS thread BEFORE any
     * downstream code allocates a CdoObject (the object/native libs
     * register heap-allocated metatables during cando_openlibs etc.,
     * and those allocations need to land in the right registry).      */
    tl_current_vm = vm;
    cando_gc_set_active_memctrl(mem);

    vm->stack_top      = vm->stack;
    vm->frame_count    = 0;
    vm->try_depth      = 0;
    vm->loop_depth     = 0;
    vm->if_depth       = 0;
    vm->open_upvalues  = NULL;
    /* Native registry: lazily grown on first cando_vm_register_native(). */
    vm->native_fns     = NULL;
    vm->native_count   = 0;
    vm->native_cap     = 0;
    /* JIT fast-native registry: lazily allocated on first
     * cando_vm_register_fast_native_f1; safe to leave NULL. */
    vm->fast_natives_f1     = NULL;
    vm->fast_natives_f1_cap = 0;
    vm->mem            = mem;
    vm->has_error       = false;
    vm->error_val_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) vm->error_vals[i] = cando_null();
    vm->error_msg[0]    = '\0';
    vm->last_ret_count  = 0;
    vm->spread_extra    = 0;
    vm->array_extra     = 0;
    vm->eval_stop_frame = ~0u;
    vm->eval_results     = NULL;
    vm->eval_result_count = 0;
    vm->eval_result_cap   = 0;
    vm->thread_stop_frame  = ~0u;  /* ~0u = "not set"; 0 is a valid stop boundary */
    vm->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) vm->thread_results[i] = cando_null();
    vm->string_proto    = cando_null();
    vm->array_proto     = cando_null();
    vm->thread_proto    = cando_null();
    vm->default_class_call = cando_null();

    /* Module cache — initially empty. */
    vm->module_cache       = NULL;
    vm->module_cache_count = 0;
    vm->module_cache_cap   = 0;

    /* Thread registry — root VM owns it. */
    vm->thread_registry_owned = (CandoThreadRegistry *)cando_alloc(sizeof(CandoThreadRegistry));
    cando_os_mutex_init(&vm->thread_registry_owned->mutex);
    cando_os_cond_init(&vm->thread_registry_owned->cond);
    vm->thread_registry_owned->count          = 0;
    vm->thread_registry_owned->quit_requested = 0;
    vm->thread_registry_owned->exit_code      = 0;
    vm->thread_registry = vm->thread_registry_owned;

    /* Initialise object layer and handle table (root VM owns both). */
    cdo_object_init();
    vm->handles_owned = (CandoHandleTable *)cando_alloc(sizeof(CandoHandleTable));
    cando_handle_table_init(vm->handles_owned, 0);
    vm->handles = vm->handles_owned;

    /* Initialise global hash table with a small prime-ish capacity. */
    vm->globals_owned = (CandoGlobalEnv *)cando_alloc(sizeof(CandoGlobalEnv));
    cando_lock_init(&vm->globals_owned->lock);
    vm->globals_owned->capacity = 64;
    vm->globals_owned->count    = 0;
    vm->globals_owned->version  = 1;   /* Phase 8.7: starts at 1 so 0 means "uninitialised" */
    vm->globals_owned->entries  = (CandoGlobalEntry *)cando_alloc(
                                       64 * sizeof(CandoGlobalEntry));
    memset(vm->globals_owned->entries, 0, 64 * sizeof(CandoGlobalEntry));
    vm->globals = vm->globals_owned;

    /* Register the default class __call wrapper as an unnamed native. */
    vm->default_class_call =
        cando_vm_add_native(vm, vm_native_class_default_call);

    /* JIT profiling: off by default, counters zeroed.  --jit / CANDO_JIT
     * / jit.on() flip the flag at runtime. */
    vm->jit_enabled = false;
    vm->jit_stats   = (CandoJitStats){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    vm->jit         = NULL;   /* lazy-allocated by cando_jit_enable     */
}

void cando_vm_init_child(CandoVM *child, const CandoVM *parent) {
    /* The child VM runs on a different OS thread; pin the thread-local
     * to it so allocations in this thread land in the shared memctrl
     * via the child rather than (incorrectly) the parent's pointer.   */
    tl_current_vm = child;
    cando_gc_set_active_memctrl(parent->mem);

    /* Start with a clean state. */
    child->stack_top      = child->stack;
    child->frame_count    = 0;
    child->try_depth      = 0;
    child->loop_depth     = 0;
    child->if_depth       = 0;
    child->open_upvalues  = NULL;
    child->mem            = parent->mem;
    child->has_error       = false;
    child->error_val_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) child->error_vals[i] = cando_null();
    child->error_msg[0]    = '\0';
    child->last_ret_count  = 0;
    child->spread_extra    = 0;
    child->array_extra     = 0;
    child->eval_stop_frame = ~0u;
    child->eval_results     = NULL;
    child->eval_result_count = 0;
    child->eval_result_cap   = 0;
    child->thread_stop_frame   = ~0u;  /* ~0u = "not set" */
    child->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) child->thread_results[i] = cando_null();
    child->string_proto    = cando_value_copy(parent->string_proto);
    child->array_proto     = cando_value_copy(parent->array_proto);
    child->thread_proto    = cando_value_copy(parent->thread_proto);
    child->default_class_call = parent->default_class_call;

    /* Module cache: child VMs do not cache modules independently. */
    child->module_cache       = NULL;
    child->module_cache_count = 0;
    child->module_cache_cap   = 0;

    /* Share parent's thread registry — child does not own it. */
    child->thread_registry_owned = NULL;
    child->thread_registry       = parent->thread_registry;

    /* Share parent's handle table and globals — no owned copies. */
    child->handles_owned = NULL;
    child->handles       = parent->handles;   /* shared, thread-safe */

    child->globals_owned = NULL;
    child->globals       = parent->globals;   /* shared, locked on access */

    /* JIT profiling: child inherits the parent's enabled flag.  Stats
     * are per-VM (each child counts its own work) so they start zeroed
     * and the parent's --jit-stats dump only reflects the parent's run.
     * Aggregating across child VMs is left for Phase 4 when traces are
     * cross-thread shareable.                                           */
    child->jit_enabled = parent->jit_enabled;
    child->jit_stats   = (CandoJitStats){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    /* Child gets its own hot-counter / recorder state if the JIT is
     * on; cross-thread sharing of trace metadata is a Phase 4 problem. */
    child->jit         = parent->jit_enabled ? cando_jit_create() : NULL;

    /* Copy native function registry (read-only after init; child gets its
     * own buffer so subsequent registrations on either VM cannot disturb
     * the other). */
    child->native_count = parent->native_count;
    child->native_cap   = parent->native_count;
    if (parent->native_count > 0) {
        child->native_fns = (CandoNativeFn *)cando_alloc(
            parent->native_count * sizeof(CandoNativeFn));
        for (u32 i = 0; i < parent->native_count; i++)
            child->native_fns[i] = parent->native_fns[i];
    } else {
        child->native_fns = NULL;
    }

    /* Mirror parent's fast-native registry into the child so threads
     * see the same JIT fast paths.  Read-only after init like
     * native_fns; child gets its own buffer for symmetry. */
    child->fast_natives_f1_cap = parent->fast_natives_f1_cap;
    if (parent->fast_natives_f1_cap > 0) {
        child->fast_natives_f1 = (CandoFastFn1 *)cando_alloc(
            parent->fast_natives_f1_cap * sizeof(CandoFastFn1));
        for (u32 i = 0; i < parent->fast_natives_f1_cap; i++)
            child->fast_natives_f1[i] = parent->fast_natives_f1[i];
    } else {
        child->fast_natives_f1 = NULL;
    }

    /* NOTE: cdo_object_init() is intentionally NOT called here.
     * The parent VM's cando_vm_init() already initialized the global
     * object subsystem (intern table + meta-key globals) before any
     * child threads were spawned.  Calling it again from child threads
     * would race on the g_meta_* globals and the string retain counts. */
}

void cando_vm_destroy(CandoVM *vm) {
    /* Tear down handle table (only if this VM owns it). */
    if (vm->handles_owned) {
        cando_handle_table_destroy(vm->handles_owned);
        cando_free(vm->handles_owned);
        vm->handles_owned = NULL;
        cdo_object_destroy_globals();
    }
    vm->handles = NULL;

    /* Release global values (only if this VM owns the globals table). */
    if (vm->globals_owned) {
        for (u32 i = 0; i < vm->globals_owned->capacity; i++) {
            CandoGlobalEntry *e = &vm->globals_owned->entries[i];
            if (e->key) {
                cando_string_release(e->key);
                cando_value_release(e->value);
            }
        }
        cando_free(vm->globals_owned->entries);
        cando_free(vm->globals_owned);
        vm->globals_owned = NULL;
    }
    vm->globals = NULL;

    /* Release any remaining stack values. */
    while (vm->stack_top > vm->stack) {
        cando_value_release(*--vm->stack_top);
    }

    /* Drop the open-list reference on every still-open upvalue.  Any
     * upvalue still referenced by a live closure survives until that
     * closure is released; orphans are freed here.                     */
    while (vm->open_upvalues) {
        CandoUpvalue *uv = vm->open_upvalues;
        vm->open_upvalues = uv->next;
        vm_upvalue_release(uv);
    }

    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) cando_value_release(vm->error_vals[i]);

    /* Release the native registry.  Each VM owns its own buffer (see the
     * fork path in cando_vm_init_child) so freeing here is unconditional. */
    cando_free(vm->native_fns);
    vm->native_fns   = NULL;
    vm->native_count = 0;
    vm->native_cap   = 0;

    /* Free the JIT fast-native registry; each VM owns its own buffer. */
    cando_free(vm->fast_natives_f1);
    vm->fast_natives_f1     = NULL;
    vm->fast_natives_f1_cap = 0;

    /* Release eval results. */
    for (u32 i = 0; i < vm->eval_result_count; i++) {
        cando_value_release(vm->eval_results[i]);
    }
    cando_free(vm->eval_results);

    /* Release module cache.
     *
     * Binary modules may export an optional `cando_module_shutdown` symbol
     * that lets them stop manager threads, release OS handles, etc. before
     * the shared library is unloaded.  Unloading the .so while a thread it
     * spawned is still running unmaps the thread's instruction pointer and
     * deadlocks the dlclose call -- so we always invoke shutdown first
     * when the symbol is exported. */
    typedef void (*CandoModuleShutdownFn)(void);
    for (u32 i = 0; i < vm->module_cache_count; i++) {
        CandoModuleEntry *e = &vm->module_cache[i];
        cando_free(e->path);
        for (u32 j = 0; j < e->value_count; j++) {
            cando_value_release(e->values[j]);
        }
        cando_free(e->values);
        cando_closure_free(e->closure); /* NULL-safe; frees script-module closures */
        cando_chunk_free(e->chunk);     /* NULL-safe; chunk owned by module entry  */
        if (e->dl_handle) {
#if defined(_WIN32) || defined(_WIN64)
            CandoModuleShutdownFn shutdown_fn = (CandoModuleShutdownFn)(void *)
                GetProcAddress((HMODULE)e->dl_handle, "cando_module_shutdown");
            if (shutdown_fn) shutdown_fn();
            FreeLibrary((HMODULE)e->dl_handle);
#else
            CandoModuleShutdownFn shutdown_fn = (CandoModuleShutdownFn)(uintptr_t)
                dlsym(e->dl_handle, "cando_module_shutdown");
            if (shutdown_fn) shutdown_fn();
            dlclose(e->dl_handle);
#endif
        }
    }
    cando_free(vm->module_cache);

    /* Destroy owned thread registry. */
    if (vm->thread_registry_owned) {
        cando_os_cond_destroy(&vm->thread_registry_owned->cond);
        cando_os_mutex_destroy(&vm->thread_registry_owned->mutex);
        cando_free(vm->thread_registry_owned);
        vm->thread_registry_owned = NULL;
    }
    vm->thread_registry = NULL;

    /* Destroy the JIT state (per-VM hot table + recorder).  Always
     * owned by this VM; child VMs allocate their own in
     * cando_vm_init_child. */
    if (vm->jit) {
        cando_jit_destroy(vm->jit);
        vm->jit = NULL;
    }
}

/* =========================================================================
 * Closure helpers
 * ===================================================================== */

CandoClosure *cando_closure_new(CandoChunk *chunk) {
    CandoClosure *c = (CandoClosure *)cando_alloc(sizeof(CandoClosure));
    c->chunk         = chunk;
    c->upvalue_count = chunk->upval_count;
    if (c->upvalue_count > 0) {
        c->upvalues = (CandoUpvalue **)cando_alloc(
                          c->upvalue_count * sizeof(CandoUpvalue *));
        memset(c->upvalues, 0, c->upvalue_count * sizeof(CandoUpvalue *));
    } else {
        c->upvalues = NULL;
    }
    return c;
}

void cando_closure_free(CandoClosure *closure) {
    if (!closure) return;
    /* Drop our reference on each captured upvalue.  An upvalue may be
     * shared with sibling closures and/or the open list; vm_upvalue_release
     * frees and cleans up the captured value only once the last owner
     * lets go.                                                          */
    for (u32 i = 0; i < closure->upvalue_count; i++)
        vm_upvalue_release(closure->upvalues[i]);
    cando_free(closure->upvalues);
    cando_free(closure);
}

void cando_closure_trace(CandoClosure *closure, CandoVM *vm,
                         CandoClosureMarkFn mark, void *ud) {
    if (!closure || !vm || !mark) return;
    for (u32 i = 0; i < closure->upvalue_count; i++) {
        CandoUpvalue *uv = closure->upvalues[i];
        if (!uv || !uv->location) continue;
        CandoValue v = *uv->location;
        if (!cando_is_object(v)) continue;
        void *obj = cando_handle_get(vm->handles, cando_as_handle(v));
        if (obj) mark(obj, ud);
    }
}

/* =========================================================================
 * GC root walker + collect cycle (Stage 2)
 *
 * The collector runs as a single sweep on the VM's memctrl, with the
 * VM stack quiesced (the dispatch loop is paused if invoked from a
 * native function -- safe because the caller's args are roots).
 * ===================================================================== */

/* Trace context threaded through every mark callback so the per-kind
 * tracers can resolve handles via the active VM without breaking the
 * object-layer's no-vm.h-dependency rule.                              */
typedef struct {
    CandoVM      *vm;
    CandoMemCtrl *mem;
} TraceCtx;

/* Mark `obj` and recursively trace its children if it transitioned
 * from unmarked to marked (i.e. was just discovered).                  */
static bool gc_mark_obj(void *obj, void *ud);

/* Resolve a CandoValue's heap target to a tracked-object pointer, or
 * NULL if the value is not a heap reference.                           */
static void *gc_resolve_cv(CandoVM *vm, CandoValue v) {
    if (!cando_is_object(v)) return NULL;
    return cando_handle_get(vm->handles, cando_as_handle(v));
}

static void gc_mark_cv(CandoValue v, TraceCtx *ctx) {
    void *obj = gc_resolve_cv(ctx->vm, v);
    if (obj) gc_mark_obj(obj, ctx);
}

/* Adapter so the object-layer tracer's CdoThread{Resolve,Mark}Fn
 * signature can drive into our gc_mark_obj.                            */
static void *gc_thread_resolve(CandoValue v, void *ud) {
    return gc_resolve_cv(((TraceCtx *)ud)->vm, v);
}
static bool gc_thread_mark(void *obj, void *ud) {
    return gc_mark_obj(obj, ud);
}

/* Adapter so OBJ_FUNCTION's bytecode_trace (untyped (void *bytecode,
 * CdoMarkFn, void *)) can drive cando_closure_trace.                  */
static void vm_closure_trace_adapter(void *bytecode,
                                     CdoMarkFn mark, void *ud) {
    TraceCtx *ctx = (TraceCtx *)ud;
    cando_closure_trace((CandoClosure *)bytecode, ctx->vm,
                        (CandoClosureMarkFn)mark, ud);
}

static bool gc_mark_obj(void *obj, void *ud) {
    if (!obj) return false;
    TraceCtx *ctx = (TraceCtx *)ud;
    if (!cando_memctrl_mark(ctx->mem, obj))
        return false;   /* already marked or not in registry */
    /* Dispatch by kind for the trace.  CdoObject and CdoThread share
     * the kind byte at offset 16.                                    */
    u8 kind = *((u8 *)obj + 16);
    if (kind == OBJ_THREAD) {
        cdo_thread_trace((CdoThread *)obj,
                         gc_thread_resolve, gc_thread_mark, ctx);
    } else {
        cdo_object_trace((CdoObject *)obj, gc_mark_obj, ctx);
    }
    return true;
}

/* Walk every root the VM holds and mark each one (recursively traced
 * via gc_mark_obj's per-kind dispatch).                                */
static void gc_mark_roots(TraceCtx *ctx) {
    CandoVM *vm = ctx->vm;

    /* Active value stack.  Frame->slots pointers index into this
     * region so they don't need separate enumeration.                  */
    for (CandoValue *p = vm->stack; p < vm->stack_top; p++)
        gc_mark_cv(*p, ctx);

    /* Globals. */
    if (vm->globals) {
        for (u32 i = 0; i < vm->globals->capacity; i++) {
            CandoGlobalEntry *e = &vm->globals->entries[i];
            if (e->key) gc_mark_cv(e->value, ctx);
        }
    }

    /* Frame closures' upvalue closed-state.  Each frame holds a
     * CandoClosure pointer; the closure isn't itself a tracked object,
     * but the values its upvalues point at may be.                     */
    for (u32 fi = 0; fi < vm->frame_count; fi++) {
        CandoClosure *c = vm->frames[fi].closure;
        if (!c) continue;
        cando_closure_trace(c, vm, gc_mark_obj, ctx);
    }

    /* Open upvalues: their `location` aliases a stack slot which is
     * already a root, so the value itself is covered above.  Nothing
     * extra to do here for Stage 2.                                   */

    /* Pending error and thread-result buffers. */
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++)
        gc_mark_cv(vm->error_vals[i], ctx);
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++)
        gc_mark_cv(vm->thread_results[i], ctx);

    /* Eval-mode result buffer (set when an embedded eval is in
     * progress; lifted out by the caller).                            */
    if (vm->eval_results) {
        for (u32 i = 0; i < vm->eval_result_count; i++)
            gc_mark_cv(vm->eval_results[i], ctx);
    }

    /* Cached prototypes / class-call dispatcher. */
    gc_mark_cv(vm->string_proto,       ctx);
    gc_mark_cv(vm->array_proto,        ctx);
    gc_mark_cv(vm->thread_proto,       ctx);
    gc_mark_cv(vm->default_class_call, ctx);
}

/* Free the handle slot owned by `obj` so the table doesn't grow
 * unbounded across collections.  Called for every entry the sweep is
 * about to destroy.                                                   */
static void gc_free_handle(void *handle_user, void *obj) {
    CandoVM *vm = (CandoVM *)handle_user;
    if (!vm || !vm->handles) return;
    HandleIndex h;
    u8 kind = *((u8 *)obj + 16);
    if (kind == OBJ_THREAD) {
        h = ((CdoThread *)obj)->handle_idx;
    } else {
        h = ((CdoObject *)obj)->handle_idx;
    }
    if (h != CANDO_INVALID_HANDLE) cando_handle_free(vm->handles, h);
}

/* =========================================================================
 * JIT profiling API -- see docs/jit-plan.md §5.
 *
 * Phase 2 introduced jit_enabled + the aggregate counters; Phase 3.2
 * lazy-allocates a CandoJit (per-PC hot table + recorder stub) on
 * first enable and tears it down on disable.  Phase 3.3+ keeps the
 * per-VM JIT state alive across enable/disable transitions.
 * ===================================================================== */

void cando_jit_enable(CandoVM *vm) {
    if (!vm) return;
    if (!vm->jit) vm->jit = cando_jit_create();
    vm->jit_enabled = true;
}

void cando_jit_disable(CandoVM *vm) {
    if (!vm) return;
    vm->jit_enabled = false;
    /* Keep the CandoJit allocated; re-enabling shouldn't lose the
     * accumulated hot counts.  vm_destroy frees it. */
}

bool cando_jit_is_enabled(const CandoVM *vm) {
    return vm ? vm->jit_enabled : false;
}

CandoJitStats cando_jit_get_stats(const CandoVM *vm) {
    if (!vm) return (CandoJitStats){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    /* The aggregate counters live directly on CandoVM; the recorder /
     * hot-table counters live inside the CandoJit object (which may
     * not exist yet).  Snapshot both into the returned struct. */
    CandoJitStats st = vm->jit_stats;
    if (vm->jit) {
        st.trace_starts    = vm->jit->recorder.trace_starts;
        st.trace_aborts    = vm->jit->recorder.trace_aborts;
        st.traces_compiled = vm->jit->recorder.traces_compiled;
        st.hot_pcs         = cando_hot_entry_count(&vm->jit->hot);
        st.blacklisted_pcs = cando_hot_blacklist_count(&vm->jit->hot);
        st.traces_evicted  = vm->jit->traces_evicted;
        /* trace_iters / trace_exits live on vm->jit_stats already
         * (incremented from the dispatch loop), so they're carried
         * by the `st = vm->jit_stats` copy above. */
    }
    return st;
}

const char *cando_jit_last_abort(const CandoVM *vm) {
    if (!vm || !vm->jit) return NULL;
    return vm->jit->recorder.last_abort;
}

void cando_jit_dump_traces(const CandoVM *vm, FILE *out) {
    if (!vm || !vm->jit || !out) return;
    for (u32 i = 0; i < vm->jit->trace_count; i++) {
        const CandoTrace *t = &vm->jit->traces[i];
        fprintf(out, "trace %u  start_pc=%p\n", t->id, (const void *)t->start_pc);
        cando_ir_dump(&t->ir, out);
        /* Phase 8: hex-dump the codegen'd mcode body when present.
         * 16 bytes per line, addressed from the mcode base so the
         * output can be cross-referenced with `objdump -D --target
         * binary -m i386:x86-64` after extraction. */
        if (t->mcode.base && t->mcode.written > 0) {
            fprintf(out, "==== trace mcode (%u bytes @ %p) ====\n",
                    t->mcode.written, (const void *)t->mcode.base);
            const u8 *p = t->mcode.base;
            for (u32 off = 0; off < t->mcode.written; off += 16) {
                fprintf(out, "  %04x ", off);
                u32 row = (t->mcode.written - off > 16) ? 16
                                                       : (t->mcode.written - off);
                for (u32 c = 0; c < row; c++)
                    fprintf(out, "%02x ", p[off + c]);
                fputc('\n', out);
            }
        }
        if (t->sink_rec_count > 0) {
            fprintf(out, "==== trace sink_recs (%u) ====\n", t->sink_rec_count);
            for (u32 s = 0; s < t->sink_rec_count; s++) {
                const CandoSinkRec *r = &t->sink_recs[s];
                fprintf(out, "  [%u] slot=%u stack_off=%d cap=%u %s\n",
                        s, r->slot, r->stack_off, r->capacity,
                        r->is_array ? "array" : "object");
            }
        }
    }
}

void cando_jit_reset_stats(CandoVM *vm) {
    if (!vm) return;
    vm->jit_stats = (CandoJitStats){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (vm->jit) {
        vm->jit->recorder.trace_starts    = 0;
        vm->jit->recorder.trace_aborts    = 0;
        vm->jit->recorder.traces_compiled = 0;
        vm->jit->recorder.last_abort[0]   = '\0';
        /* Hot-table state is intentionally NOT reset -- the per-PC
         * counts accumulate across reset_stats so that the recorder
         * trigger stays warm.  Use cando_hot_table_destroy/init via
         * a future API to wipe completely. */
    }
}

u32 cando_vm_gc_collect(CandoVM *vm) {
    if (!vm || !vm->mem) return 0;
    u32 before = vm->mem->live_count;

    TraceCtx ctx = { vm, vm->mem };
    cando_memctrl_clear_marks(vm->mem);
    gc_mark_roots(&ctx);
    cando_memctrl_sweep(vm->mem, gc_free_handle, vm);

    u32 after = vm->mem->live_count;
    /* Self-tuning threshold: collect again after the live set doubles.
     * Floor at 256 so very small programs don't ping-pong.            */
    u32 next = after * 2;
    if (next < 256) next = 256;
    /* Relaxed atomic store -- pairs with the unsynchronised relaxed
     * load in vm_gc_maybe_collect's hot path. */
    __atomic_store_n(&vm->mem->next_collect_threshold, next,
                     __ATOMIC_RELAXED);
    return before - after;
}

/* Called from the dispatch loop after every allocating instruction.
 * Cheap fast-path (one comparison) when the threshold isn't reached.
 * Safe call site: between instructions, when the freshly-allocated
 * object has already been pushed onto the value stack so it counts as
 * a root.                                                              */
static inline void vm_gc_maybe_collect(CandoVM *vm) {
    if (CANDO_UNLIKELY(vm->mem)) {
        /* Use relaxed atomic loads here; the matching writes in
         * memory.c are made under gc_lock, but this hot path
         * intentionally reads without taking the lock and the values
         * are only used as a heuristic to decide whether to run a
         * full collect.  Stale reads are acceptable -- a true torn
         * read is not, hence the explicit atomic load. */
        u32 threshold = __atomic_load_n(&vm->mem->next_collect_threshold,
                                        __ATOMIC_RELAXED);
        u32 live      = __atomic_load_n(&vm->mem->live_count,
                                        __ATOMIC_RELAXED);
        if (CANDO_UNLIKELY(threshold > 0 && live >= threshold))
            cando_vm_gc_collect(vm);
    }
}

/* =========================================================================
 * Stack helpers
 * ===================================================================== */

void cando_vm_push(CandoVM *vm, CandoValue val) {
    CANDO_ASSERT_MSG(vm->stack_top - vm->stack < CANDO_STACK_MAX,
                     "value stack overflow");
    *vm->stack_top++ = val;
}

CandoValue cando_vm_pop(CandoVM *vm) {
    CANDO_ASSERT_MSG(vm->stack_top > vm->stack, "value stack underflow");
    return *--vm->stack_top;
}

CandoValue cando_vm_peek(const CandoVM *vm, u32 dist) {
    CANDO_ASSERT_MSG(vm->stack_top - vm->stack > (ptrdiff_t)dist,
                     "peek out of range");
    return vm->stack_top[-1 - (ptrdiff_t)dist];
}

u32 cando_vm_stack_depth(const CandoVM *vm) {
    return (u32)(vm->stack_top - vm->stack);
}

/* =========================================================================
 * Global variable hash table (open addressing, FNV-1a)
 * ===================================================================== */

static u32 vm_global_hash(const char *str, u32 len) {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < len; i++) {
        hash ^= (u8)str[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Resize the global table to new_cap (must be power of two >= count*2). */
static void vm_globals_resize(CandoGlobalEnv *g, u32 new_cap) {
    CandoGlobalEntry *old    = g->entries;
    u32               old_cap = g->capacity;
    g->entries  = (CandoGlobalEntry *)cando_alloc(new_cap * sizeof(CandoGlobalEntry));
    g->capacity = new_cap;
    g->count    = 0;
    memset(g->entries, 0, new_cap * sizeof(CandoGlobalEntry));
    for (u32 i = 0; i < old_cap; i++) {
        if (!old[i].key) continue;
        u32 idx = vm_global_hash(old[i].key->data, old[i].key->length)
                  & (new_cap - 1);
        while (g->entries[idx].key) idx = (idx + 1) & (new_cap - 1);
        g->entries[idx] = old[i];
        g->count++;
    }
    cando_free(old);
    /* Phase 8.7: invalidate JIT trace caches that refer to old
     * entry pointers.  Any trace with cached_entry_ptrs will see
     * a version mismatch on next entry and bail to bytecode. */
    g->version++;
}

/* Phase 8.7: look up the entry pointer for a global by name.  The
 * JIT caches this pointer at codegen time and dereferences it
 * directly in mcode (skips the per-iter hash lookup).  Validity
 * is enforced via globals->version: the trace stores the version
 * at codegen time and aborts if it differs at trace entry. */
CandoGlobalEntry *cando_vm_get_global_entry(CandoVM *vm, const char *name) {
    CandoGlobalEnv *g   = vm->globals;
    u32             len = (u32)strlen(name);
    u32             h   = vm_global_hash(name, len);
    u32             idx = h & (g->capacity - 1);
    while (g->entries[idx].key) {
        CandoString *k = g->entries[idx].key;
        if (k->length == len && memcmp(k->data, name, len) == 0)
            return &g->entries[idx];
        idx = (idx + 1) & (g->capacity - 1);
    }
    return NULL;
}

bool cando_vm_set_global(CandoVM *vm, const char *name, CandoValue val,
                          bool is_const) {
    CandoGlobalEnv *g   = vm->globals;
    u32             len = (u32)strlen(name);
    u32             h   = vm_global_hash(name, len);

    /* Look for existing entry. */
    u32 idx = h & (g->capacity - 1);
    while (g->entries[idx].key) {
        CandoString *k = g->entries[idx].key;
        if (k->length == len && memcmp(k->data, name, len) == 0) {
            if (g->entries[idx].is_const) return false; /* write-protected */
            cando_value_release(g->entries[idx].value);
            g->entries[idx].value    = val;
            g->entries[idx].is_const = is_const;
            return true;
        }
        idx = (idx + 1) & (g->capacity - 1);
    }

    /* New entry — maybe grow first. */
    if (g->count + 1 > g->capacity * 3 / 4) {
        vm_globals_resize(g, g->capacity * 2);
        idx = h & (g->capacity - 1);
        while (g->entries[idx].key) idx = (idx + 1) & (g->capacity - 1);
    }

    g->entries[idx].key      = cando_string_new(name, len);
    g->entries[idx].value    = val;
    g->entries[idx].is_const = is_const;
    g->count++;
    return true;
}

bool cando_vm_get_global(const CandoVM *vm, const char *name,
                          CandoValue *out) {
    const CandoGlobalEnv *g   = vm->globals;
    u32                   len = (u32)strlen(name);
    u32                   h   = vm_global_hash(name, len);
    u32                   idx = h & (g->capacity - 1);

    while (g->entries[idx].key) {
        CandoString *k = g->entries[idx].key;
        if (k->length == len && memcmp(k->data, name, len) == 0) {
            if (out) *out = g->entries[idx].value;
            return true;
        }
        idx = (idx + 1) & (g->capacity - 1);
    }
    return false;
}

/* Variant that takes a CandoString* key (no copy). */
static bool vm_get_global_str(CandoVM *vm, CandoString *key, CandoValue *out) {
    CandoGlobalEnv *g   = vm->globals;
    u32             h   = vm_global_hash(key->data, key->length);
    u32             idx = h & (g->capacity - 1);
    while (g->entries[idx].key) {
        CandoString *k = g->entries[idx].key;
        if (k->length == key->length &&
            memcmp(k->data, key->data, key->length) == 0) {
            if (out) *out = g->entries[idx].value;
            return true;
        }
        idx = (idx + 1) & (g->capacity - 1);
    }
    return false;
}

/* Write to global by CandoString key; returns false if const-protected. */
static bool vm_set_global_str(CandoVM *vm, CandoString *key,
                               CandoValue val, bool is_const) {
    return cando_vm_set_global(vm, key->data, val, is_const);
}

/* =========================================================================
 * Native function registration
 * ===================================================================== */

/* Ensure vm->native_fns has room for at least one more entry.  Doubles the
 * capacity (starting at 16) so amortised registration is O(1).  Aborts on
 * allocation failure via cando_realloc, matching the rest of the codebase. */
static void vm_native_reserve_one(CandoVM *vm) {
    if (vm->native_count < vm->native_cap) return;
    u32 new_cap = vm->native_cap ? vm->native_cap * 2 : 16;
    vm->native_fns = (CandoNativeFn *)cando_realloc(
        vm->native_fns, new_cap * sizeof(CandoNativeFn));
    vm->native_cap = new_cap;
}

bool cando_vm_register_native(CandoVM *vm, const char *name,
                               CandoNativeFn fn) {
    vm_native_reserve_one(vm);
    u32 idx = vm->native_count++;
    vm->native_fns[idx] = fn;
    /* Sentinel value: native #idx → -(idx+1) */
    CandoValue sentinel = cando_number(-(f64)(idx + 1));
    cando_vm_set_global(vm, name, sentinel, true);
    return true;
}

void cando_vm_register_fast_native_f1(CandoVM *vm, CandoNativeFn slow,
                                       CandoFastFn1 fast) {
    if (!vm || !slow || !fast) return;
    /* Find slow's index in vm->native_fns. */
    u32 idx = vm->native_count;
    for (u32 i = 0; i < vm->native_count; i++) {
        if (vm->native_fns[i] == slow) { idx = i; break; }
    }
    if (idx == vm->native_count) return;  /* unknown native; silently skip */

    /* Grow fast_natives_f1 to cover idx; new cells are NULL. */
    if (idx >= vm->fast_natives_f1_cap) {
        u32 nc = vm->fast_natives_f1_cap ? vm->fast_natives_f1_cap * 2 : 16;
        while (idx >= nc) nc *= 2;
        vm->fast_natives_f1 = (CandoFastFn1 *)cando_realloc(
            vm->fast_natives_f1, nc * sizeof(CandoFastFn1));
        for (u32 i = vm->fast_natives_f1_cap; i < nc; i++)
            vm->fast_natives_f1[i] = NULL;
        vm->fast_natives_f1_cap = nc;
    }
    vm->fast_natives_f1[idx] = fast;
}

CandoValue cando_vm_add_native(CandoVM *vm, CandoNativeFn fn) {
    vm_native_reserve_one(vm);
    u32 idx = vm->native_count++;
    vm->native_fns[idx] = fn;
    return cando_number(-(f64)(idx + 1));
}

/* =========================================================================
 * Default class __call wrapper
 *
 * Bound as the __call metamethod of every CLASS object.  When the class is
 * invoked --  Vector(1, 2, 3)  --  OP_CALL routes the call here with:
 *     args[0]      = the class object itself
 *     args[1..N-1] = the user-supplied constructor arguments
 *
 * Behaviour:
 *   1. Allocate a fresh instance object.
 *   2. Set instance.__index = class so methods on the class are reachable
 *      via the prototype chain.
 *   3. If the class has a __constructor field, invoke it with
 *      (instance, args[1..N-1]).  Discard its return values.
 *   4. Push the instance onto the value stack and return 1.
 * ===================================================================== */
static int vm_native_class_default_call(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        vm_runtime_error(vm, "class __call: missing class receiver");
        return -1;
    }

    CandoValue cls_val  = args[0];
    CdoObject *cls_obj  = cando_bridge_resolve(vm, cando_as_handle(cls_val));
    if (!cls_obj) {
        vm_runtime_error(vm, "class __call: invalid class handle");
        return -1;
    }

    /* 1. Allocate the instance. */
    CandoValue inst_val = cando_bridge_new_object(vm);
    CdoObject *inst_obj = cando_bridge_resolve(vm, cando_as_handle(inst_val));

    /* 2. instance.__index = class. */
    if (g_meta_index) {
        CdoValue cls_cdo = cando_bridge_to_cdo(vm, cls_val);
        cdo_object_rawset(inst_obj, g_meta_index, cls_cdo, FIELD_NONE);
        cdo_value_release(cls_cdo);
    }

    /* 3. Look up __constructor on the class (raw, no proto traversal: a
     * subclass declares its own constructor and we don't want to silently
     * borrow the parent's body when none is given). */
    CdoValue ctor_cdo = cdo_null();
    bool has_ctor = false;
    if (g_meta_constructor) {
        has_ctor = cdo_object_rawget(cls_obj, g_meta_constructor, &ctor_cdo);
    }

    if (has_ctor) {
        CandoValue ctor_val = cando_bridge_to_cando(vm, ctor_cdo);
        cdo_value_release(ctor_cdo);

        /* Build the constructor argument list:
         *   ctor_args[0]      = the new instance
         *   ctor_args[1..]    = the original user args
         */
        u32 ctor_argc = (u32)argc;  /* same total: replace cls with instance */
        CandoValue *ctor_args = (CandoValue *)cando_alloc(
            ctor_argc * sizeof(CandoValue));
        ctor_args[0] = inst_val;
        for (int i = 1; i < argc; i++) ctor_args[i] = args[i];

        int ret = cando_vm_call_value(vm, ctor_val, ctor_args, ctor_argc);
        cando_value_release(ctor_val);
        cando_free(ctor_args);

        if (vm->has_error) {
            cando_value_release(inst_val);
            return -1;
        }
        /* Pop and discard any return values from the constructor; the
         * instance, not the body's return value, is what __call yields. */
        for (int i = 0; i < ret; i++)
            cando_value_release(cando_vm_pop(vm));
    }

    /* 4. Push the instance as the result of the call. */
    cando_vm_push(vm, inst_val);
    return 1;
}

/* =========================================================================
 * Upvalue management
 * ===================================================================== */

/* vm_capture_upvalue -- find or create an open upvalue aliasing the
 * given live stack slot.  The open list is sorted by `location`
 * descending so close-on-return can pop the head while the head's
 * location is still inside the closing frame.  On dedup hit the
 * existing upvalue is returned with its refcount unchanged; on miss a
 * fresh upvalue is created with refcount = 1 (for the open-list slot).
 * Callers (OP_CLOSURE) bump the refcount once more when they store the
 * pointer into a CandoClosure.                                        */
static CandoUpvalue *vm_capture_upvalue(CandoVM *vm, CandoValue *local) {
    CandoUpvalue *prev = NULL;
    CandoUpvalue *cur  = vm->open_upvalues;
    while (cur && cur->location > local) {
        prev = cur;
        cur  = cur->next;
    }
    if (cur && cur->location == local) return cur;

    CandoUpvalue *uv = (CandoUpvalue *)cando_alloc(sizeof(CandoUpvalue));
    cando_lock_init(&uv->lock);
    uv->location = local;
    uv->closed   = cando_null();
    uv->next     = cur;
    atomic_store_explicit(&uv->refcount, 1u, memory_order_relaxed);
    if (prev) prev->next       = uv;
    else      vm->open_upvalues = uv;
    return uv;
}

/* Drop a reference held by either the open list or a closure.  Frees
 * and releases the closed value once the last owner goes away.        */
static void vm_upvalue_release(CandoUpvalue *uv) {
    if (!uv) return;
    if (atomic_fetch_sub_explicit(&uv->refcount, 1u, memory_order_acq_rel)
        == 1u) {
        /* If we owned the last ref, the upvalue must be closed (the
         * open-list slot would otherwise still hold one ref).         */
        if (uv->location == &uv->closed)
            cando_value_release(uv->closed);
        cando_free(uv);
    }
}

/* Close every open upvalue whose location lies within the frame slab
 * being torn down.  Each upvalue retains its own copy of the live value
 * (so the stack-slot release that follows in OP_RETURN doesn't dangle
 * the closure's view) and drops its open-list reference.              */
static void vm_close_upvalues(CandoVM *vm, CandoValue *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        CandoUpvalue *uv = vm->open_upvalues;
        cando_lock_write_acquire(&uv->lock);
        uv->closed   = cando_value_copy(*uv->location);
        uv->location = &uv->closed;
        cando_lock_write_release(&uv->lock);
        vm->open_upvalues = uv->next;
        vm_upvalue_release(uv);
    }
}

/* =========================================================================
 * Meta-method dispatch helper
 * ===================================================================== */

bool cando_vm_dispatch_callable(CandoVM *vm, const struct CdoValue *raw_p,
                                 CandoValue *args, u32 argc) {
    if (!raw_p) return false;
    CdoValue raw = *raw_p;
    CandoValue callee = cando_bridge_to_cando(vm, raw);

    /* Native sentinel: dispatch via vm->native_fns table. */
    if (IS_NATIVE_FN(callee)) {
        u32 ni = NATIVE_INDEX(callee);
        if (ni >= vm->native_count) {
            vm_runtime_error(vm, "meta-method: invalid native index %u", ni);
            return false;
        }
        u32 stack_before = (u32)(vm->stack_top - vm->stack);
        int ret = vm->native_fns[ni](vm, (int)argc, args);
        if (vm->has_error) return false;
        if (ret <= 0) {
            if ((u32)(vm->stack_top - vm->stack) <= stack_before)
                cando_vm_push(vm, cando_null());
        }
        while ((u32)(vm->stack_top - vm->stack) > stack_before + 1) {
            CandoValue extra = cando_vm_pop(vm);
            cando_value_release(extra);
        }
        return true;
    }

    /* Number: PC-offset inline function in the current chunk. */
    if (cando_is_number(callee)) {
        u32 pc = (u32)cando_as_number(callee);
        CandoCallFrame *cur_frame = &vm->frames[vm->frame_count - 1];

        u32 saved_stop = vm->thread_stop_frame;
        vm->thread_stop_frame   = vm->frame_count;
        vm->thread_result_count = 0;
        for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
            cando_value_release(vm->thread_results[i]);
            vm->thread_results[i] = cando_null();
        }

        /* Stack the sentinel + args first so vm_push_frame's slot-vs-arg
         * arithmetic matches the regular call path.                    */
        u32 stack_before = (u32)(vm->stack_top - vm->stack);
        cando_vm_push(vm, cando_null());
        for (u32 i = 0; i < argc; i++)
            cando_vm_push(vm, cando_value_copy(args[i]));

        if (!vm_push_frame(vm, cur_frame->closure,
                           cur_frame->closure->chunk->code + pc,
                           argc, false)) {
            while ((u32)(vm->stack_top - vm->stack) > stack_before)
                cando_value_release(cando_vm_pop(vm));
            vm->thread_stop_frame = saved_stop;
            return false;
        }

        vm_run(vm);
        vm->thread_stop_frame = saved_stop;
        if (vm->has_error) return false;

        if (vm->thread_result_count > 0)
            cando_vm_push(vm, cando_value_copy(vm->thread_results[0]));
        else
            cando_vm_push(vm, cando_null());
        return true;
    }

    /* OBJ_NATIVE or OBJ_FUNCTION: dispatch via cando_vm_call_value. */
    if (cando_is_object(callee)) {
        CdoObject *fn_obj = cando_bridge_resolve(vm, cando_as_handle(callee));

        /* OBJ_NATIVE: call via fn.native.fn with CdoValue args. */
        if (fn_obj && fn_obj->kind == OBJ_NATIVE && fn_obj->fn.native.fn) {
            CdoValue *cdo_args = (CdoValue *)cando_alloc(
                                     argc * sizeof(CdoValue));
            for (u32 i = 0; i < argc; i++)
                cdo_args[i] = cando_bridge_to_cdo(vm, args[i]);
            CdoValue result = fn_obj->fn.native.fn(NULL, cdo_args, argc);
            for (u32 i = 0; i < argc; i++)
                cdo_value_release(cdo_args[i]);
            cando_free(cdo_args);
            cando_vm_push(vm, cando_bridge_to_cando(vm, result));
            cdo_value_release(result);
            cando_value_release(callee);
            return true;
        }

        /* OBJ_FUNCTION: script closure -- reentrant VM call. */
        if (fn_obj && fn_obj->kind == OBJ_FUNCTION &&
            fn_obj->fn.script.bytecode) {
            int ret = cando_vm_call_value(vm, callee, args, argc);
            cando_value_release(callee);
            if (vm->has_error) return false;
            if (ret > 1) {
                CandoValue top = cando_vm_pop(vm);
                for (int i = 1; i < ret; i++) {
                    CandoValue extra = cando_vm_pop(vm);
                    cando_value_release(extra);
                }
                cando_vm_push(vm, top);
            } else if (ret == 0) {
                cando_vm_push(vm, cando_null());
            }
            return true;
        }

        cando_value_release(callee);
    }

    vm_runtime_error(vm, "meta-method is not callable");
    return false;
}

bool cando_vm_call_meta(CandoVM *vm, HandleIndex h,
                         struct CdoString *meta_key,
                         CandoValue *args, u32 argc) {
    CdoObject *obj = cando_bridge_resolve(vm, h);
    if (!obj || !meta_key) return false;

    CdoValue raw;
    if (!cdo_object_get(obj, (CdoString *)meta_key, &raw))
        return false;

    return cando_vm_dispatch_callable(vm, &raw, args, argc);
}

/* =========================================================================
 * Truthiness
 *
 * Falsy values: NULL, FALSE, and the number 0 (zero).
 * Objects may override truthiness via the __is metamethod, which receives
 * the object as its single argument and whose return value is itself
 * tested for truthiness via the rules above.
 * ===================================================================== */

static bool vm_is_truthy(CandoValue v) {
    if (cando_is_null(v))   return false;
    if (cando_is_bool(v))   return cando_as_bool(v);
    if (cando_is_number(v)) return cando_as_number(v) != 0.0;
    return true;
}

static bool vm_is_truthy_meta(CandoVM *vm, CandoValue v, bool *ok) {
    *ok = true;
    if (cando_is_object(v) && g_meta_is) {
        CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
        if (obj) {
            CdoValue raw;
            if (cdo_object_get(obj, g_meta_is, &raw)) {
                /* __is may be a literal TRUE/FALSE (used directly) or a
                 * callable (invoked as __is(self); its return value is
                 * tested for truthiness).  Numbers can't be used as a
                 * literal here -- they share the CDO_NUMBER tag with
                 * inline-function PC offsets and so are always treated
                 * as callable. */
                if (raw.tag == CDO_BOOL) {
                    return raw.as.boolean;
                }
                CandoValue arg = v;
                if (cando_vm_dispatch_callable(vm, &raw, &arg, 1)) {
                    CandoValue r = cando_vm_pop(vm);
                    bool t = vm_is_truthy(r);
                    cando_value_release(r);
                    return t;
                }
                if (vm->has_error) { *ok = false; return false; }
            }
        }
    }
    return vm_is_truthy(v);
}


/* =========================================================================
 * Error reporting
 * ===================================================================== */

/* vm_error_commit -- shared tail: set has_error and populate error_vals[0]. */
static void vm_error_commit(CandoVM *vm) {
    vm->has_error = true;
    cando_value_release(vm->error_vals[0]);
    CandoString *s = cando_string_new(vm->error_msg,
                                      (u32)strlen(vm->error_msg));
    vm->error_vals[0]   = cando_string_value(s);
    vm->error_val_count = 1;
}

/* cando_vm_error -- public; for use by native functions.
 * Does NOT attach source location (no frame IP access needed).           */
void cando_vm_error(CandoVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm_error_commit(vm);
}

/* cando_vm_log_uncaught -- print vm's current error to stderr and clear it.
 * Used by callback dispatchers where there is no surrounding TRY/CATCH and
 * no caller to propagate the error to.  No-op when vm has no error.        */
void cando_vm_log_uncaught(CandoVM *vm, const char *context) {
    if (!vm || !vm->has_error) return;
    fprintf(stderr, "cando: uncaught error in %s: %s\n",
            context && *context ? context : "callback",
            vm->error_msg[0] ? vm->error_msg : "<no message>");
    fflush(stderr);

    /* Clear the error so the calling thread can continue dispatching. */
    vm->has_error       = false;
    vm->error_msg[0]    = '\0';
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
        cando_value_release(vm->error_vals[i]);
        vm->error_vals[i] = cando_null();
    }
    vm->error_val_count = 0;
}

/* vm_append_stack_trace -- append a multi-line "  at <file>:<line>" trace
 * to vm->error_msg covering every active call frame, innermost first.
 * Truncation is bounded by the fixed error_msg buffer; if a frame would
 * not fit, append a "  ... N more frame(s)" summary and stop.            */
static void vm_append_stack_trace(CandoVM *vm) {
    if (vm->frame_count == 0) return;

    size_t cap = sizeof(vm->error_msg);
    size_t used = strlen(vm->error_msg);

    for (int i = (int)vm->frame_count - 1; i >= 0; i--) {
        CandoCallFrame *f     = &vm->frames[i];
        CandoChunk     *chunk = f->closure->chunk;
        u32 line = 0;
        /* The innermost frame has just executed (or attempted) one byte
         * past the failing instruction; deeper frames record the IP as
         * "the byte after the call site that pushed this frame". In both
         * cases stepping back one byte lands on the relevant opcode.    */
        u32 offset = (u32)(f->ip - chunk->code);
        if (offset > 0) offset--;
        if (offset < chunk->code_len) line = chunk->lines[offset];

        char entry[256];
        int n = snprintf(entry, sizeof(entry),
                         "\n  at %s:%u",
                         chunk->name ? chunk->name : "<anonymous>",
                         line);
        if (n < 0) break;
        if (used + (size_t)n + 1 >= cap) {
            /* Not enough room for this frame; summarise remainder. */
            char tail[64];
            int tn = snprintf(tail, sizeof(tail),
                              "\n  ... %d more frame(s)", i + 1);
            if (tn > 0 && used + (size_t)tn + 1 < cap) {
                memcpy(vm->error_msg + used, tail, (size_t)tn + 1);
            }
            break;
        }
        memcpy(vm->error_msg + used, entry, (size_t)n + 1);
        used += (size_t)n;
    }
}

/* vm_runtime_error -- internal; like cando_vm_error but appends a stack
 * trace covering every active call frame.                                */
static void vm_runtime_error(CandoVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);

    vm_append_stack_trace(vm);
    vm_error_commit(vm);
}

/* =========================================================================
 * Function call
 * ===================================================================== */

/* Push a new call frame for `closure`, starting at `ip` (a byte address
 * inside closure->chunk->code).  The caller is responsible for arranging
 * the value stack so the function-value sentinel sits at
 *      vm->stack_top - arg_count - 1
 * with the `arg_count` arguments occupying the slots above it.  After
 * this returns, dispatch loop callers must reload their local frame/ip
 * via LOAD_FRAME().  Returns false (and raises a runtime error) on
 * frame_count overflow.                                                 */
static bool vm_push_frame(CandoVM *vm, CandoClosure *closure, u8 *ip,
                          u32 arg_count, bool is_fluent) {
    if (vm->frame_count >= CANDO_FRAMES_MAX) {
        vm_runtime_error(vm, "call stack overflow");
        return false;
    }
    CandoChunk     *chunk = closure->chunk;
    CandoCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure   = closure;
    frame->ip        = ip;
    frame->slots     = vm->stack_top - arg_count - 1;
    frame->ret_count = 0;
    frame->loop_save = vm->loop_depth;
    frame->if_save   = vm->if_depth;
    frame->is_fluent = is_fluent;
    /* Pre-allocate null slots for the function's remaining locals so
     * expression evaluation never overwrites them; n_present = slot-0
     * sentinel + the args already on the stack.                        */
    u32 n_present = arg_count + 1;
    if (chunk->local_count > n_present) {
        u32 n_extra = chunk->local_count - n_present;
        for (u32 i = 0; i < n_extra; i++)
            cando_vm_push(vm, cando_null());
    }
    if (CANDO_UNLIKELY(vm->jit_enabled))
        vm->jit_stats.func_entry_hits++;
    return true;
}

static bool vm_call(CandoVM *vm, CandoClosure *closure, u32 arg_count) {
    CandoChunk *chunk = closure->chunk;
    if (!chunk->has_vararg && arg_count != chunk->arity) {
        vm_runtime_error(vm,
            "function '%s' expects %u argument(s) but got %u",
            chunk->name, chunk->arity, arg_count);
        return false;
    }
    return vm_push_frame(vm, closure, chunk->code, arg_count, false);
}

/* =========================================================================
 * Main dispatch loop
 * ===================================================================== */

/*
 * Macro helpers used inside vm_run.
 * ip and frame are local; they alias vm->frames[vm->frame_count-1].ip
 * and the frame pointer itself.  We update vm's frame ip before any
 * call that might recurse.
 */

#define READ_BYTE()    (*ip++)
#define READ_U16()     (ip += 2, cando_read_u16(ip - 2))
#define READ_I16()     ((i16)READ_U16())
#define READ_CONST()   (frame->closure->chunk->constants[READ_U16()])

/* Hot-path stack ops: raw pointer arithmetic instead of going through
 * the public cando_vm_push/pop/peek helpers (which add a function call
 * and a bounds assertion).  The dispatch loop visits these macros
 * millions of times per second; the parser already guarantees per-op
 * arity so the assertions are redundant inside vm_run.  The public
 * helpers remain for native-function callers.
 *
 * PUSH evaluates its argument into a temporary first to avoid sequence-
 * point UB when the argument also reads vm->stack_top (e.g. PEEK).    */
#define PUSH(v)        do {                                              \
    CandoValue _pv = (v);                                                \
    *vm->stack_top++ = _pv;                                              \
} while (0)
#define POP()          (*--vm->stack_top)
#define DROP()         ((void)(--vm->stack_top))   /* discard top, no read */
#define PEEK(d)        (vm->stack_top[-1 - (ptrdiff_t)(d)])

/* Saved frame-local ip back to the frame struct before any call. */
#define SYNC_IP()  (frame->ip = ip)

/* Reload ip from the current (possibly new) top frame. */
#define LOAD_FRAME()  do {                         \
    frame = &vm->frames[vm->frame_count - 1];       \
    ip    = frame->ip;                              \
} while (0)

CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk) {
    /* Wrap the top-level chunk in a throwaway closure and push a sentinel
     * value for slot 0 (the "function" position at frame base).          */
    CandoClosure *top_closure = cando_closure_new(chunk);
    PUSH(cando_null()); /* slot 0 placeholder */
    if (!vm_call(vm, top_closure, 0)) {
        cando_closure_free(top_closure);
        return VM_RUNTIME_ERR;
    }
    CandoVMResult result = vm_run(vm);
    /* Wait for all spawned threads before freeing the closure.
     * Thread functions share top_closure via fn_obj->fn.script.bytecode,
     * so it must not be freed while any thread is still executing.        */
    cando_vm_wait_all_threads(vm);
    cando_closure_free(top_closure);
    return result;
}

CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue **results_out, u32 *count_out) {
    /* Save outer eval state so nested evals don't interfere. */
    u32         saved_stop   = vm->eval_stop_frame;
    CandoValue *saved_results = vm->eval_results;
    u32         saved_count   = vm->eval_result_count;
    u32         saved_cap     = vm->eval_result_cap;

    /* The stop marker is the current frame depth.  When OP_RETURN brings
     * frame_count back to this value, VM_EVAL_DONE is returned.          */
    vm->eval_stop_frame = vm->frame_count;
    vm->eval_results     = NULL;
    vm->eval_result_count = 0;
    vm->eval_result_cap   = 0;

    CandoClosure *closure = cando_closure_new(chunk);
    cando_vm_push(vm, cando_null()); /* slot 0 placeholder */
    if (!vm_call(vm, closure, 0)) {
        cando_vm_pop(vm);            /* remove the placeholder */
        cando_closure_free(closure);
        vm->eval_stop_frame = saved_stop;
        vm->eval_results     = saved_results;
        vm->eval_result_count = saved_count;
        vm->eval_result_cap   = saved_cap;
        return VM_RUNTIME_ERR;
    }

    CandoVMResult res = vm_run(vm);
    cando_closure_free(closure);

    if (results_out && count_out) {
        *results_out = vm->eval_results;
        *count_out   = vm->eval_result_count;
    } else {
        for (u32 i = 0; i < vm->eval_result_count; i++)
            cando_value_release(vm->eval_results[i]);
        cando_free(vm->eval_results);
    }

    /* Restore outer eval state. */
    vm->eval_results     = saved_results;
    vm->eval_result_count = saved_count;
    vm->eval_result_cap   = saved_cap;
    vm->eval_stop_frame = saved_stop;
    return res;
}

CandoVMResult cando_vm_exec_eval_module(CandoVM *vm, CandoChunk *chunk,
                                         CandoValue **results_out, u32 *count_out,
                                         CandoClosure **closure_out) {
    /* Identical to cando_vm_exec_eval, but transfers the closure to the
     * caller instead of freeing it.  The caller is responsible for calling
     * cando_closure_free() when the closure is no longer needed.           */
    u32         saved_stop   = vm->eval_stop_frame;
    CandoValue *saved_results = vm->eval_results;
    u32         saved_count   = vm->eval_result_count;
    u32         saved_cap     = vm->eval_result_cap;

    vm->eval_stop_frame = vm->frame_count;
    vm->eval_results     = NULL;
    vm->eval_result_count = 0;
    vm->eval_result_cap   = 0;

    CandoClosure *closure = cando_closure_new(chunk);
    cando_vm_push(vm, cando_null()); /* slot 0 placeholder */
    if (!vm_call(vm, closure, 0)) {
        cando_vm_pop(vm);
        cando_closure_free(closure);
        vm->eval_stop_frame = saved_stop;
        vm->eval_results     = saved_results;
        vm->eval_result_count = saved_count;
        vm->eval_result_cap   = saved_cap;
        if (closure_out) *closure_out = NULL;
        return VM_RUNTIME_ERR;
    }

    CandoVMResult res = vm_run(vm);
    /* Do NOT free closure — transfer ownership to caller. */

    if (closure_out) {
        *closure_out = closure;
    } else {
        cando_closure_free(closure); /* caller opted out */
    }

    if (results_out && count_out) {
        *results_out = vm->eval_results;
        *count_out   = vm->eval_result_count;
    } else {
        for (u32 i = 0; i < vm->eval_result_count; i++)
            cando_value_release(vm->eval_results[i]);
        cando_free(vm->eval_results);
    }

    vm->eval_results     = saved_results;
    vm->eval_result_count = saved_count;
    vm->eval_result_cap   = saved_cap;
    vm->eval_stop_frame = saved_stop;
    return res;
}

/* =========================================================================
 * cando_vm_exec_closure -- run a closure on this VM, capturing results.
 * ===================================================================== */
CandoVMResult cando_vm_exec_closure(CandoVM *vm, CandoClosure *closure,
                                     u32 fn_pc) {
    /* Save outer thread-capture state. */
    u32 saved_stop  = vm->thread_stop_frame;
    u32 saved_count = vm->thread_result_count;

    vm->thread_stop_frame   = vm->frame_count;
    vm->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++)
        cando_value_release(vm->thread_results[i]);
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++)
        vm->thread_results[i] = cando_null();

    /* Push slot-0 sentinel and create a call frame at fn_pc.  No args:
     * vm_push_frame handles local-null fill from arg_count = 0.        */
    cando_vm_push(vm, cando_null());
    if (!vm_push_frame(vm, closure, closure->chunk->code + fn_pc, 0, false)) {
        cando_value_release(cando_vm_pop(vm));
        vm->thread_stop_frame   = saved_stop;
        vm->thread_result_count = saved_count;
        return VM_RUNTIME_ERR;
    }

    CandoVMResult res = vm_run(vm);

    vm->thread_stop_frame = saved_stop;
    (void)saved_count; /* thread_result_count is set by OP_RETURN */
    return res;
}

/* =========================================================================
 * vm_call_closure_with_args -- call an OBJ_FUNCTION with pre-loaded args.
 *
 * Pushes slot-0 sentinel, then args[0..argc-1] for param slots, then nulls
 * for any remaining locals.  Creates a call frame and runs vm_run.  Return
 * values are captured in vm->thread_results via the thread_stop_frame
 * mechanism (results are NOT pushed onto vm->stack by this helper).
 * ===================================================================== */
static void vm_call_closure_with_args(CandoVM *vm, CdoObject *fn_obj,
                                       CandoValue *args, u32 argc) {
    if (!fn_obj || fn_obj->kind != OBJ_FUNCTION || !fn_obj->fn.script.bytecode)
        return;

    CandoClosure *closure = (CandoClosure *)fn_obj->fn.script.bytecode;
    u32           fn_pc   = fn_obj->fn.script.param_count;
    /* Save and reset thread-capture state. */
    u32 saved_stop  = vm->thread_stop_frame;
    vm->thread_stop_frame   = vm->frame_count;
    vm->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
        cando_value_release(vm->thread_results[i]);
        vm->thread_results[i] = cando_null();
    }

    /* Push slot-0 sentinel + the passed args, then build the frame.
     * Uses argc (not closure->chunk->arity, which is the top-level
     * script's arity = 0) because all nested functions share the flat
     * chunk.                                                           */
    u32 stack_before = (u32)(vm->stack_top - vm->stack);
    cando_vm_push(vm, cando_null());
    for (u32 i = 0; i < argc; i++)
        cando_vm_push(vm, cando_value_copy(args[i]));

    if (!vm_push_frame(vm, closure, closure->chunk->code + fn_pc, argc, false)) {
        while ((u32)(vm->stack_top - vm->stack) > stack_before)
            cando_value_release(cando_vm_pop(vm));
        vm->thread_stop_frame = saved_stop;
        return;
    }

    vm_run(vm);

    vm->thread_stop_frame = saved_stop;
}

/* =========================================================================
 * cando_vm_call_value -- public API: call a Cando function value with args.
 *
 * Return values are pushed onto vm->stack.  Returns the count pushed.
 * ===================================================================== */
int cando_vm_call_value(CandoVM *vm, CandoValue fn_val,
                         CandoValue *args, u32 argc) {
    /* Support native functions as well. */
    if (IS_NATIVE_FN(fn_val)) {
        u32 ni = NATIVE_INDEX(fn_val);
        if (ni >= vm->native_count) return 0;
        int ret = vm->native_fns[ni](vm, (int)argc, args);
        if (vm->has_error) return 0;
        if (ret < 0) return 0;
        /* Values are already pushed on stack by native. */
        return ret;
    }

    if (!cando_is_object(fn_val)) return 0;
    CdoObject *fn_obj = cando_bridge_resolve(vm, cando_as_handle(fn_val));
    if (!fn_obj || fn_obj->kind != OBJ_FUNCTION) return 0;

    vm_call_closure_with_args(vm, fn_obj, args, argc);

    /* Push captured return values onto vm->stack for the caller. */
    u32 ret = vm->thread_result_count;
    for (u32 i = 0; i < ret; i++)
        cando_vm_push(vm, cando_value_copy(vm->thread_results[i]));
    return (int)ret;
}

/* =========================================================================
 * Thread trampoline -- entry point for spawned OS threads.
 * ===================================================================== */

typedef struct {
    CandoVM    *parent_vm;
    CandoValue  fn_val;           /* retained OBJ_FUNCTION CandoValue */
    CdoThread  *thread;           /* the thread object to report results into */
    CandoValue  thread_handle_val; /* CandoValue handle for this thread       */
} ThreadArg;

static CANDO_THREAD_RETURN vm_thread_trampoline(void *raw_arg) {
    ThreadArg *ta = (ThreadArg *)raw_arg;

    /* Publish this thread's CdoThread* for thread.current(). */
    tl_current_thread = ta->thread;

    /* Mark as running. */
    atomic_store(&ta->thread->state, CDO_THREAD_RUNNING);

    /* Create a child VM sharing the parent's handles + globals. */
    CandoVM child;
    cando_vm_init_child(&child, ta->parent_vm);

    /* Resolve the OBJ_FUNCTION and run it. */
    if (!cando_is_object(ta->fn_val)) {
        CandoValue err = cando_string_value(
            cando_string_new("thread: not a function", 22));
        cdo_thread_set_error(ta->thread, err);
        cando_value_release(err);
        goto cleanup;
    }

    {
        CdoObject *fn_obj = cando_bridge_resolve(&child, cando_as_handle(ta->fn_val));
        if (!fn_obj || fn_obj->kind != OBJ_FUNCTION ||
            !fn_obj->fn.script.bytecode) {
            CandoValue err = cando_string_value(
                cando_string_new("thread: invalid function object", 31));
            cdo_thread_set_error(ta->thread, err);
            cando_value_release(err);
            goto cleanup;
        }

        CandoClosure *fn_closure = (CandoClosure *)fn_obj->fn.script.bytecode;
        u32           fn_pc      = fn_obj->fn.script.param_count;

        CandoVMResult res = cando_vm_exec_closure(&child, fn_closure, fn_pc);

        if (res == VM_RUNTIME_ERR || child.has_error) {
            /* Propagate the error to the thread object. */
            CandoValue err = (child.error_val_count > 0)
                             ? child.error_vals[0]
                             : cando_string_value(
                                   cando_string_new(child.error_msg,
                                                    (u32)strlen(child.error_msg)));
            cdo_thread_set_error(ta->thread, err);
            if (child.error_val_count == 0) cando_value_release(err);
        } else {
            cdo_thread_set_results(ta->thread,
                                   child.thread_results,
                                   child.thread_result_count);
        }
    }

    /* Fire then/catch callback if one was registered.
     *
     * We read the callback under done_mutex so we don't race with
     * thread.then / thread.catch storing a callback after our set_results /
     * set_error call but before we check here.  By the time set_results /
     * set_error returned it had already broadcast; thread.then checks the
     * state under the same lock, so only one side will ever invoke the cb.  */
    {
        CdoThreadState final_state = atomic_load(&ta->thread->state);
        CandoValue cb = cando_null();
        bool       cb_is_catch = false;

        cando_os_mutex_lock(&ta->thread->done_mutex);
        if (final_state == CDO_THREAD_DONE && cando_is_object(ta->thread->then_fn)) {
            cb = cando_value_copy(ta->thread->then_fn);
        } else if (final_state == CDO_THREAD_ERROR &&
                   cando_is_object(ta->thread->catch_fn)) {
            cb = cando_value_copy(ta->thread->catch_fn);
            /* The error is being delivered to user code -- mark it
             * observed so the destroy hook doesn't also print it. */
            ta->thread->error_observed = true;
            cb_is_catch                = true;
        }
        cando_os_mutex_unlock(&ta->thread->done_mutex);

        if (cando_is_object(cb)) {
            CdoObject *cb_obj = cando_bridge_resolve(&child, cando_as_handle(cb));
            if (cb_obj && cb_obj->kind == OBJ_FUNCTION) {
                /* Reset child VM stack/frame state for the callback call. */
                child.stack_top     = child.stack;
                child.frame_count   = 0;
                child.has_error     = false;
                child.error_val_count = 0;

                if (final_state == CDO_THREAD_DONE) {
                    vm_call_closure_with_args(&child, cb_obj,
                                              ta->thread->results,
                                              ta->thread->result_count);
                } else {
                    vm_call_closure_with_args(&child, cb_obj,
                                              &ta->thread->error, 1);
                }

                /* If the then/catch callback itself threw, surface that
                 * on stderr -- there is no further caller to receive
                 * it.  Helpful for diagnosing bugs in completion logic. */
                if (child.has_error) {
                    cando_vm_log_uncaught(&child,
                        cb_is_catch ? "thread.catch callback"
                                    : "thread.then callback");
                }
            }
            cando_value_release(cb);
        }
    }

cleanup:
    tl_current_thread = NULL;
    cando_vm_destroy(&child);
    tl_current_vm     = NULL;
    cando_gc_set_active_memctrl(NULL);
    cando_value_release(ta->fn_val);
    /* Capture the registry pointer before freeing ta. */
    CandoThreadRegistry *reg = ta->parent_vm->thread_registry;
    cando_free(ta);
    /* Deregister from the registry last — this may wake the main thread,
     * so no shared resources may be accessed after the broadcast.        */
    if (reg) {
        cando_os_mutex_lock(&reg->mutex);
        if (reg->count > 0) reg->count--;
        cando_os_cond_broadcast(&reg->cond);
        cando_os_mutex_unlock(&reg->mutex);
    }
    return CANDO_THREAD_RETURN_VAL;
}

static CandoVMResult vm_run(CandoVM *vm) {
    CandoCallFrame *frame = &vm->frames[vm->frame_count - 1];
    u8             *ip    = frame->ip;

/* ── Computed-goto dispatch (GCC/Clang) ─────────────────────────────── */
/* JIT recorder hook: pre-execute observation point.
 *
 * Off-state cost: one load of vm->jit_enabled (in the same cache
 * line as the rest of vm) + one well-predicted branch.  The branch
 * is hinted CANDO_UNLIKELY so the predictor stays cold for non-JIT
 * runs.  Once jit_enabled is true the second load (vm->jit->recorder.
 * active) is also cheap because vm->jit was lazy-allocated by
 * cando_jit_enable so the pointer is non-NULL whenever jit_enabled
 * is true.  When recording is active, every opcode flows through
 * cando_recorder_observe; see source/jit/jit.c. */
#define JIT_OBSERVE() do { \
    if (CANDO_UNLIKELY(vm->jit_enabled && vm->jit->recorder.active)) \
        cando_recorder_observe(vm, ip); \
} while (0)

#ifdef __GNUC__
#   define DISPATCH()        do { JIT_OBSERVE(); \
                                  goto *dispatch_table[READ_BYTE()]; \
                             } while (0)
#   define OP_CASE(name)     lbl_##name
#   define INTERPRET_LOOP()  DISPATCH();
    static const void *dispatch_table[OP_COUNT] = {
        [OP_CONST]            = &&lbl_OP_CONST,
        [OP_NULL]             = &&lbl_OP_NULL,
        [OP_TRUE]             = &&lbl_OP_TRUE,
        [OP_FALSE]            = &&lbl_OP_FALSE,
        [OP_POP]              = &&lbl_OP_POP,
        [OP_POP_N]            = &&lbl_OP_POP_N,
        [OP_DUP]              = &&lbl_OP_DUP,
        [OP_LOAD_LOCAL]       = &&lbl_OP_LOAD_LOCAL,
        [OP_STORE_LOCAL]      = &&lbl_OP_STORE_LOCAL,
        [OP_DEF_LOCAL]        = &&lbl_OP_DEF_LOCAL,
        [OP_DEF_CONST_LOCAL]  = &&lbl_OP_DEF_CONST_LOCAL,
        [OP_LOAD_GLOBAL]      = &&lbl_OP_LOAD_GLOBAL,
        [OP_STORE_GLOBAL]     = &&lbl_OP_STORE_GLOBAL,
        [OP_DEF_GLOBAL]       = &&lbl_OP_DEF_GLOBAL,
        [OP_DEF_CONST_GLOBAL] = &&lbl_OP_DEF_CONST_GLOBAL,
        [OP_LOAD_UPVAL]       = &&lbl_OP_LOAD_UPVAL,
        [OP_STORE_UPVAL]      = &&lbl_OP_STORE_UPVAL,
        [OP_CLOSE_UPVAL]      = &&lbl_OP_CLOSE_UPVAL,
        [OP_ADD]              = &&lbl_OP_ADD,
        [OP_SUB]              = &&lbl_OP_SUB,
        [OP_MUL]              = &&lbl_OP_MUL,
        [OP_DIV]              = &&lbl_OP_DIV,
        [OP_MOD]              = &&lbl_OP_MOD,
        [OP_POW]              = &&lbl_OP_POW,
        [OP_NEG]              = &&lbl_OP_NEG,
        [OP_POS]              = &&lbl_OP_POS,
        [OP_INCR]             = &&lbl_OP_INCR,
        [OP_DECR]             = &&lbl_OP_DECR,
        [OP_EQ]               = &&lbl_OP_EQ,
        [OP_NEQ]              = &&lbl_OP_NEQ,
        [OP_LT]               = &&lbl_OP_LT,
        [OP_GT]               = &&lbl_OP_GT,
        [OP_LEQ]              = &&lbl_OP_LEQ,
        [OP_GEQ]              = &&lbl_OP_GEQ,
        [OP_EQ_STACK]         = &&lbl_OP_EQ_STACK,
        [OP_NEQ_STACK]        = &&lbl_OP_NEQ_STACK,
        [OP_LT_STACK]         = &&lbl_OP_LT_STACK,
        [OP_GT_STACK]         = &&lbl_OP_GT_STACK,
        [OP_LEQ_STACK]        = &&lbl_OP_LEQ_STACK,
        [OP_GEQ_STACK]        = &&lbl_OP_GEQ_STACK,
        [OP_RANGE_CHECK]      = &&lbl_OP_RANGE_CHECK,
        [OP_BIT_AND]          = &&lbl_OP_BIT_AND,
        [OP_BIT_OR]           = &&lbl_OP_BIT_OR,
        [OP_BIT_XOR]          = &&lbl_OP_BIT_XOR,
        [OP_BIT_NOT]          = &&lbl_OP_BIT_NOT,
        [OP_LSHIFT]           = &&lbl_OP_LSHIFT,
        [OP_RSHIFT]           = &&lbl_OP_RSHIFT,
        [OP_NOT]              = &&lbl_OP_NOT,
        [OP_AND_JUMP]         = &&lbl_OP_AND_JUMP,
        [OP_OR_JUMP]          = &&lbl_OP_OR_JUMP,
        [OP_NEW_OBJECT]       = &&lbl_OP_NEW_OBJECT,
        [OP_NEW_ARRAY]        = &&lbl_OP_NEW_ARRAY,
        [OP_GET_FIELD]        = &&lbl_OP_GET_FIELD,
        [OP_SET_FIELD]        = &&lbl_OP_SET_FIELD,
        [OP_GET_INDEX]        = &&lbl_OP_GET_INDEX,
        [OP_SET_INDEX]        = &&lbl_OP_SET_INDEX,
        [OP_LEN]              = &&lbl_OP_LEN,
        [OP_KEYS_OF]          = &&lbl_OP_KEYS_OF,
        [OP_VALS_OF]          = &&lbl_OP_VALS_OF,
        [OP_JUMP]             = &&lbl_OP_JUMP,
        [OP_JUMP_IF_FALSE]    = &&lbl_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_TRUE]     = &&lbl_OP_JUMP_IF_TRUE,
        [OP_JUMP_IF_NULL]     = &&lbl_OP_JUMP_IF_NULL,
        [OP_LOOP]             = &&lbl_OP_LOOP,
        [OP_BREAK]            = &&lbl_OP_BREAK,
        [OP_CONTINUE]         = &&lbl_OP_CONTINUE,
        [OP_LOOP_MARK]        = &&lbl_OP_LOOP_MARK,
        [OP_LOOP_END]         = &&lbl_OP_LOOP_END,
        [OP_IF_MARK]          = &&lbl_OP_IF_MARK,
        [OP_IF_END]           = &&lbl_OP_IF_END,
        [OP_SETTLE]           = &&lbl_OP_SETTLE,
        [OP_IF_TEST_MATCHED]  = &&lbl_OP_IF_TEST_MATCHED,
        [OP_IF_TEST_PREV]     = &&lbl_OP_IF_TEST_PREV,
        [OP_IF_SET_RAN]       = &&lbl_OP_IF_SET_RAN,
        [OP_IF_CLEAR_PREV]    = &&lbl_OP_IF_CLEAR_PREV,
        [OP_CLOSURE]          = &&lbl_OP_CLOSURE,
        [OP_CALL]             = &&lbl_OP_CALL,
        [OP_METHOD_CALL]      = &&lbl_OP_METHOD_CALL,
        [OP_FLUENT_CALL]      = &&lbl_OP_FLUENT_CALL,
        [OP_RETURN]           = &&lbl_OP_RETURN,
        [OP_TAIL_CALL]        = &&lbl_OP_TAIL_CALL,
        [OP_LOAD_VARARG]      = &&lbl_OP_LOAD_VARARG,
        [OP_VARARG_LEN]       = &&lbl_OP_VARARG_LEN,
        [OP_UNPACK]           = &&lbl_OP_UNPACK,
        [OP_RANGE_ASC]        = &&lbl_OP_RANGE_ASC,
        [OP_RANGE_DESC]       = &&lbl_OP_RANGE_DESC,
        [OP_FOR_INIT]         = &&lbl_OP_FOR_INIT,
        [OP_FOR_NEXT]         = &&lbl_OP_FOR_NEXT,
        [OP_FOR_RANGE_INIT]   = &&lbl_OP_FOR_RANGE_INIT,
        [OP_FOR_RANGE_NEXT]   = &&lbl_OP_FOR_RANGE_NEXT,
        [OP_FOR_OVER_INIT]    = &&lbl_OP_FOR_OVER_INIT,
        [OP_FOR_OVER_NEXT]    = &&lbl_OP_FOR_OVER_NEXT,
        [OP_PIPE_INIT]        = &&lbl_OP_PIPE_INIT,
        [OP_PIPE_NEXT]        = &&lbl_OP_PIPE_NEXT,
        [OP_FILTER_NEXT]      = &&lbl_OP_FILTER_NEXT,
        [OP_PIPE_END]         = &&lbl_OP_PIPE_END,
        [OP_PIPE_COLLECT]     = &&lbl_OP_PIPE_COLLECT,
        [OP_FILTER_COLLECT]   = &&lbl_OP_FILTER_COLLECT,
        [OP_COND_FILTER_COLLECT] = &&lbl_OP_COND_FILTER_COLLECT,
        [OP_TRY_BEGIN]        = &&lbl_OP_TRY_BEGIN,
        [OP_TRY_END]          = &&lbl_OP_TRY_END,
        [OP_CATCH_BEGIN]      = &&lbl_OP_CATCH_BEGIN,
        [OP_FINALLY_BEGIN]    = &&lbl_OP_FINALLY_BEGIN,
        [OP_THROW]            = &&lbl_OP_THROW,
        [OP_RERAISE]          = &&lbl_OP_RERAISE,
        [OP_ASYNC]            = &&lbl_OP_ASYNC,
        [OP_AWAIT]            = &&lbl_OP_AWAIT,
        [OP_YIELD]            = &&lbl_OP_YIELD,
        [OP_THREAD]           = &&lbl_OP_THREAD,
        [OP_NEW_CLASS]         = &&lbl_OP_NEW_CLASS,
        [OP_BIND_METHOD]       = &&lbl_OP_BIND_METHOD,
        [OP_INHERIT]           = &&lbl_OP_INHERIT,
        [OP_BIND_DEFAULT_CALL] = &&lbl_OP_BIND_DEFAULT_CALL,
        [OP_MASK_PASS]        = &&lbl_OP_MASK_PASS,
        [OP_MASK_SKIP]        = &&lbl_OP_MASK_SKIP,
        [OP_MASK_APPLY]       = &&lbl_OP_MASK_APPLY,
        [OP_SPREAD_RET]       = &&lbl_OP_SPREAD_RET,
        [OP_ARRAY_SPREAD]     = &&lbl_OP_ARRAY_SPREAD,
        [OP_TRUNCATE_RET]     = &&lbl_OP_TRUNCATE_RET,
        [OP_EQ_SPREAD]        = &&lbl_OP_EQ_SPREAD,
        [OP_NEQ_SPREAD]       = &&lbl_OP_NEQ_SPREAD,
        [OP_LT_SPREAD]        = &&lbl_OP_LT_SPREAD,
        [OP_GT_SPREAD]        = &&lbl_OP_GT_SPREAD,
        [OP_LEQ_SPREAD]       = &&lbl_OP_LEQ_SPREAD,
        [OP_GEQ_SPREAD]       = &&lbl_OP_GEQ_SPREAD,
        [OP_NOP]              = &&lbl_OP_NOP,
        [OP_HALT]             = &&lbl_OP_HALT,
    };
#else  /* portable switch fallback */
#   define DISPATCH()        do { JIT_OBSERVE(); } while (0); switch (READ_BYTE())
#   define OP_CASE(name)     case name
#   define INTERPRET_LOOP()  for (;;) DISPATCH()
#endif

    INTERPRET_LOOP() {

        /* ── Band 0: Constants ──────────────────────────────────────── */
        OP_CASE(OP_CONST): {
            u16 idx = READ_U16();
            PUSH(cando_value_copy(frame->closure->chunk->constants[idx]));
            DISPATCH();
        }
        OP_CASE(OP_NULL):  PUSH(cando_null());          DISPATCH();
        OP_CASE(OP_TRUE):  PUSH(cando_bool(true));      DISPATCH();
        OP_CASE(OP_FALSE): PUSH(cando_bool(false));     DISPATCH();

        /* ── Band 1: Stack ──────────────────────────────────────────── */
        OP_CASE(OP_POP): {
            cando_value_release(POP());
            DISPATCH();
        }
        OP_CASE(OP_POP_N): {
            u16 n = READ_U16();
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            DISPATCH();
        }
        OP_CASE(OP_DUP): {
            PUSH(cando_value_copy(PEEK(0)));
            DISPATCH();
        }

        /* ── Band 2: Locals ─────────────────────────────────────────── */
        OP_CASE(OP_LOAD_LOCAL): {
            u16 slot = READ_U16();
            PUSH(cando_value_copy(frame->slots[slot]));
            DISPATCH();
        }
        OP_CASE(OP_STORE_LOCAL): {
            u16 slot = READ_U16();
            cando_value_release(frame->slots[slot]);
            frame->slots[slot] = cando_value_copy(PEEK(0));
            DISPATCH();
        }
        OP_CASE(OP_DEF_LOCAL): {
            u16 slot = READ_U16();
            cando_value_release(frame->slots[slot]);
            frame->slots[slot] = POP();
            vm->spread_extra = 0;
            DISPATCH();
        }
        OP_CASE(OP_DEF_CONST_LOCAL): {
            /* Const locals are identical at the VM level; the compiler
             * simply never emits OP_STORE_LOCAL for them.              */
            u16 slot = READ_U16();
            cando_value_release(frame->slots[slot]);
            frame->slots[slot] = POP();
            vm->spread_extra = 0;
            DISPATCH();
        }

        /* ── Band 3: Globals ────────────────────────────────────────── */
        OP_CASE(OP_LOAD_GLOBAL): {
            /* Inline cache: look up the entry pointer once per (chunk,
             * const-index) pair and reuse it on subsequent hits.  The
             * environment's `version` counter bumps on rehash, which is
             * the only event that can invalidate a cached pointer.
             *
             * We still take the read lock around the actual value read
             * so concurrent writers don't tear or free under us.  All
             * the lock now protects is a couple of pointer/int reads --
             * the hash + memcmp is gone, and that was the dominant
             * per-call cost in recursive scripts (e.g. fib's 5.2M
             * lookups of the global `fib`). */
            u16 ci = READ_U16();
            CandoChunk *chunk = frame->closure->chunk;
            CandoValue out_copy;
            bool found;
            cando_lock_read_acquire(&vm->globals->lock);
            CandoGlobalEntry *e = NULL;
            u32 cur_ver = vm->globals->version;
            if (ci < chunk->inline_cache_cap &&
                chunk->globals_version_seen == cur_ver) {
                e = chunk->inline_cache[ci];
            }
            if (CANDO_UNLIKELY(!e)) {
                CandoValue name_val = chunk->constants[ci];
                CANDO_ASSERT(cando_is_string(name_val));
                e = cando_vm_get_global_entry(vm,
                        cando_as_string(name_val)->data);
                if (e) {
                    /* Grow + populate cache, refreshing the version
                     * stamp if we noticed a rehash since last update. */
                    if (ci >= chunk->inline_cache_cap) {
                        u32 nc = chunk->inline_cache_cap
                                 ? chunk->inline_cache_cap * 2 : 8;
                        while (nc <= ci) nc *= 2;
                        chunk->inline_cache = (CandoGlobalEntry **)
                            cando_realloc(chunk->inline_cache,
                                          nc * sizeof(*chunk->inline_cache));
                        memset(chunk->inline_cache + chunk->inline_cache_cap,
                               0,
                               (nc - chunk->inline_cache_cap)
                                  * sizeof(*chunk->inline_cache));
                        chunk->inline_cache_cap = nc;
                    }
                    if (chunk->globals_version_seen != cur_ver) {
                        memset(chunk->inline_cache, 0,
                               chunk->inline_cache_cap
                                  * sizeof(*chunk->inline_cache));
                        chunk->globals_version_seen = cur_ver;
                    }
                    chunk->inline_cache[ci] = e;
                }
            }
            if (e) {
                out_copy = cando_value_copy(e->value);
                found = true;
            } else {
                out_copy = cando_null();
                found = false;
            }
            cando_lock_read_release(&vm->globals->lock);
            if (!found) {
                CandoValue name_val = chunk->constants[ci];
                vm_runtime_error(vm, "undefined variable '%s'",
                                 cando_as_string(name_val)->data);
                goto handle_error;
            }
            PUSH(out_copy);
            DISPATCH();
        }
        OP_CASE(OP_STORE_GLOBAL): {
            /* Same cache as OP_LOAD_GLOBAL.  Only fall back to the
             * hash-table path on cache miss / version mismatch / when
             * cando_vm_set_global needs to allocate a new entry (which
             * also bumps version, invalidating the cache for next
             * load). */
            u16 ci = READ_U16();
            CandoChunk *chunk = frame->closure->chunk;
            CandoValue val = PEEK(0);
            bool ok;
            bool need_slow = true;
            cando_lock_write_acquire(&vm->globals->lock);
            u32 cur_ver = vm->globals->version;
            if (ci < chunk->inline_cache_cap &&
                chunk->globals_version_seen == cur_ver) {
                CandoGlobalEntry *e = chunk->inline_cache[ci];
                if (e && !e->is_const) {
                    cando_value_release(e->value);
                    e->value = cando_value_copy(val);
                    ok = true;
                    need_slow = false;
                } else if (e && e->is_const) {
                    ok = false;
                    need_slow = false;
                }
            }
            if (need_slow) {
                CandoValue name_val = chunk->constants[ci];
                CANDO_ASSERT(cando_is_string(name_val));
                ok = vm_set_global_str(vm, cando_as_string(name_val),
                                       cando_value_copy(val), false);
                /* Refresh cache for next load if the entry now exists. */
                if (ok) {
                    CandoGlobalEntry *e = cando_vm_get_global_entry(vm,
                            cando_as_string(name_val)->data);
                    if (e) {
                        u32 v = vm->globals->version;
                        if (ci >= chunk->inline_cache_cap) {
                            u32 nc = chunk->inline_cache_cap
                                     ? chunk->inline_cache_cap * 2 : 8;
                            while (nc <= ci) nc *= 2;
                            chunk->inline_cache = (CandoGlobalEntry **)
                                cando_realloc(chunk->inline_cache,
                                              nc * sizeof(*chunk->inline_cache));
                            memset(chunk->inline_cache + chunk->inline_cache_cap,
                                   0,
                                   (nc - chunk->inline_cache_cap)
                                      * sizeof(*chunk->inline_cache));
                            chunk->inline_cache_cap = nc;
                        }
                        if (chunk->globals_version_seen != v) {
                            memset(chunk->inline_cache, 0,
                                   chunk->inline_cache_cap
                                      * sizeof(*chunk->inline_cache));
                            chunk->globals_version_seen = v;
                        }
                        chunk->inline_cache[ci] = e;
                    }
                }
            }
            cando_lock_write_release(&vm->globals->lock);
            if (!ok) {
                CandoValue name_val = chunk->constants[ci];
                vm_runtime_error(vm, "cannot assign to constant '%s'",
                                 cando_as_string(name_val)->data);
                goto handle_error;
            }
            DISPATCH();
        }
        OP_CASE(OP_DEF_GLOBAL): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            cando_lock_write_acquire(&vm->globals->lock);
            vm_set_global_str(vm, cando_as_string(name_val), POP(), false);
            cando_lock_write_release(&vm->globals->lock);
            vm->spread_extra = 0;
            DISPATCH();
        }
        OP_CASE(OP_DEF_CONST_GLOBAL): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            cando_lock_write_acquire(&vm->globals->lock);
            vm_set_global_str(vm, cando_as_string(name_val), POP(), true);
            cando_lock_write_release(&vm->globals->lock);
            vm->spread_extra = 0;
            DISPATCH();
        }

        /* ── Band 4: Upvalues ───────────────────────────────────────── */
        OP_CASE(OP_LOAD_UPVAL): {
            u16 uv = READ_U16();
            CandoUpvalue *up = frame->closure->upvalues[uv];
            cando_lock_read_acquire(&up->lock);
            CandoValue val = cando_value_copy(*up->location);
            cando_lock_read_release(&up->lock);
            PUSH(val);
            DISPATCH();
        }
        OP_CASE(OP_STORE_UPVAL): {
            u16 uv = READ_U16();
            CandoUpvalue *up = frame->closure->upvalues[uv];
            cando_lock_write_acquire(&up->lock);
            cando_value_release(*up->location);
            *up->location = cando_value_copy(PEEK(0));
            cando_lock_write_release(&up->lock);
            DISPATCH();
        }
        OP_CASE(OP_CLOSE_UPVAL): {
            u16 slot = READ_U16();
            SYNC_IP();
            vm_close_upvalues(vm, &frame->slots[slot]);
            DISPATCH();
        }

        /* ── Band 5: Arithmetic ─────────────────────────────────────── */

        /* Try a binary metamethod; dispatches and continues if found. */
#define TRY_BINARY_META(meta_key, _a, _b)                                   \
    if ((meta_key) && (cando_is_object(_a) || cando_is_object(_b))) {       \
        HandleIndex _h = cando_is_object(_a) ? cando_as_handle(_a)          \
                                              : cando_as_handle(_b);        \
        CandoValue _buf[2] = {_a, _b};                                     \
        if (cando_vm_call_meta(vm, _h,                                      \
                               (struct CdoString *)(meta_key), _buf, 2)) {  \
            cando_value_release(_a); cando_value_release(_b);               \
            DISPATCH();                                                     \
        }                                                                   \
        if (vm->has_error) {                                                \
            cando_value_release(_a); cando_value_release(_b);               \
            goto handle_error;                                              \
        }                                                                   \
    }

        OP_CASE(OP_ADD): {
            /* String concatenation or numeric addition.  If either operand
             * is a string, the other is coerced via cando_value_tostring and
             * the two are concatenated. */
            CandoValue b = PEEK(0), a = PEEK(1);
            if (cando_is_string(a) || cando_is_string(b)) {
                char *sa_buf = NULL, *sb_buf = NULL;
                const char *sa_data; u32 la;
                const char *sb_data; u32 lb;
                if (cando_is_string(a)) {
                    sa_data = cando_as_string(a)->data;
                    la = cando_as_string(a)->length;
                } else {
                    sa_buf = cando_value_tostring(a);
                    sa_data = sa_buf;
                    la = (u32)strlen(sa_buf);
                }
                if (cando_is_string(b)) {
                    sb_data = cando_as_string(b)->data;
                    lb = cando_as_string(b)->length;
                } else {
                    sb_buf = cando_value_tostring(b);
                    sb_data = sb_buf;
                    lb = (u32)strlen(sb_buf);
                }
                if (la > UINT32_MAX - lb - 1) {
                    free(sa_buf); free(sb_buf);
                    vm_runtime_error(vm,
                        "string concatenation length overflow");
                    goto handle_error;
                }
                DROP(); DROP();
                u32 total = la + lb;
                char *buf = cando_alloc(total + 1);
                memcpy(buf,      sa_data, la);
                memcpy(buf + la, sb_data, lb);
                buf[total] = '\0';
                CandoString *s = cando_string_new(buf, total);
                cando_free(buf);
                free(sa_buf);
                free(sb_buf);
                cando_value_release(a);
                cando_value_release(b);
                PUSH(cando_string_value(s));
            } else {
                CandoValue _b2 = POP(), _a2 = POP();
                TRY_BINARY_META(g_meta_add, _a2, _b2);
                if (CANDO_UNLIKELY(!cando_is_number(_a2) || !cando_is_number(_b2))) {
                    cando_value_release(_a2); cando_value_release(_b2);
                    vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                        cando_value_type_name(cando_value_tag(_a2)),
                        cando_value_type_name(cando_value_tag(_b2)));
                    goto handle_error;
                }
                PUSH(cando_number(cando_as_number(_a2) + cando_as_number(_b2)));
            }
            DISPATCH();
        }
        OP_CASE(OP_SUB): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_sub, a, b);
            if (CANDO_UNLIKELY(!cando_is_number(a) || !cando_is_number(b))) {
                vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                    cando_value_type_name(cando_value_tag(a)),
                    cando_value_type_name(cando_value_tag(b)));
                goto handle_error;
            }
            PUSH(cando_number(cando_as_number(a) - cando_as_number(b)));
            DISPATCH();
        }
        OP_CASE(OP_MUL): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_mul, a, b);
            if (CANDO_UNLIKELY(!cando_is_number(a) || !cando_is_number(b))) {
                vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                    cando_value_type_name(cando_value_tag(a)),
                    cando_value_type_name(cando_value_tag(b)));
                goto handle_error;
            }
            PUSH(cando_number(cando_as_number(a) * cando_as_number(b)));
            DISPATCH();
        }
        OP_CASE(OP_DIV): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_div, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            if (cando_as_number(b) == 0.0) {
                vm_runtime_error(vm, "division by zero");
                goto handle_error;
            }
            PUSH(cando_number(cando_as_number(a) / cando_as_number(b)));
            DISPATCH();
        }
        OP_CASE(OP_MOD): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_mod, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(fmod(cando_as_number(a), cando_as_number(b))));
            DISPATCH();
        }
        OP_CASE(OP_POW): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_pow, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(pow(cando_as_number(a), cando_as_number(b))));
            DISPATCH();
        }
#undef TRY_BINARY_META
        OP_CASE(OP_NEG): {
            CandoValue a = POP();
            if (cando_is_object(a) && g_meta_unm) {
                if (cando_vm_call_meta(vm, cando_as_handle(a),
                                       (struct CdoString *)g_meta_unm,
                                       &a, 1)) {
                    cando_value_release(a);
                    DISPATCH();
                }
                if (vm->has_error) { cando_value_release(a); goto handle_error; }
            }
            if (!cando_is_number(a)) {
                cando_value_release(a);
                vm_runtime_error(vm, "unary '-' requires a number");
                goto handle_error;
            }
            PUSH(cando_number(-cando_as_number(a)));
            DISPATCH();
        }
        OP_CASE(OP_POS): {
            CandoValue a = POP();
            if (!cando_is_number(a)) {
                vm_runtime_error(vm, "unary '+' requires a number");
                goto handle_error;
            }
            PUSH(a); /* no-op for numbers */
            DISPATCH();
        }
        OP_CASE(OP_INCR): {
            if (!cando_is_number(PEEK(0))) {
                vm_runtime_error(vm, "'++'  requires a number");
                goto handle_error;
            }
            cando_set_number(&vm->stack_top[-1],
                             cando_as_number(vm->stack_top[-1]) + 1.0);
            DISPATCH();
        }
        OP_CASE(OP_DECR): {
            if (!cando_is_number(PEEK(0))) {
                vm_runtime_error(vm, "'--' requires a number");
                goto handle_error;
            }
            cando_set_number(&vm->stack_top[-1],
                             cando_as_number(vm->stack_top[-1]) - 1.0);
            DISPATCH();
        }

        /* ── Band 6: Comparison ─────────────────────────────────────── */

        /* Try a comparison metamethod and DISPATCH on success.  `swap` is
         * true for GT/GEQ which forward to __lt/__le on the swapped pair;
         * `negate` is true for NEQ which negates the __eq result.  The
         * comparison ops also share a number-only fallback for the cases
         * where no meta is involved -- CMP_NUM_FALLBACK below.            */
#define TRY_CMP_META(meta_key, swap, negate, _a, _b)                          \
    if ((meta_key) && (cando_is_object(_a) || cando_is_object(_b))) {         \
        HandleIndex _h = cando_is_object(_a) ? cando_as_handle(_a)            \
                                              : cando_as_handle(_b);          \
        CandoValue _buf[2] = { (swap) ? (_b) : (_a),                          \
                                (swap) ? (_a) : (_b) };                       \
        if (cando_vm_call_meta(vm, _h,                                        \
                               (struct CdoString *)(meta_key), _buf, 2)) {    \
            if (negate) {                                                     \
                CandoValue _r = cando_vm_pop(vm);                             \
                PUSH(cando_bool(!vm_is_truthy(_r)));                          \
                cando_value_release(_r);                                      \
            }                                                                 \
            cando_value_release(_a); cando_value_release(_b);                 \
            DISPATCH();                                                       \
        }                                                                     \
        if (vm->has_error) {                                                  \
            cando_value_release(_a); cando_value_release(_b);                 \
            goto handle_error;                                                \
        }                                                                     \
    }

#define CMP_NUM_FALLBACK(_a, _b, op_sym)  do {                                \
    if (!cando_is_number(_a) || !cando_is_number(_b)) {                       \
        cando_value_release(_a); cando_value_release(_b);                     \
        vm_runtime_error(vm, "comparison requires numbers");                  \
        goto handle_error;                                                    \
    }                                                                         \
    PUSH(cando_bool(cando_as_number(_a) op_sym cando_as_number(_b)));         \
    cando_value_release(_a); cando_value_release(_b);                         \
} while (0)

        OP_CASE(OP_EQ): {
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_eq, false, false, a, b);
            PUSH(cando_bool(cando_value_equal(a, b)));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_NEQ): {
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_eq, false, true, a, b);
            PUSH(cando_bool(!cando_value_equal(a, b)));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_LT): {
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_lt, false, false, a, b);
            CMP_NUM_FALLBACK(a, b, <);
            DISPATCH();
        }
        OP_CASE(OP_GT): {
            /* a > b is implemented as __lt(b, a) — swap the arguments. */
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_lt, true, false, a, b);
            CMP_NUM_FALLBACK(a, b, >);
            DISPATCH();
        }
        OP_CASE(OP_LEQ): {
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_le, false, false, a, b);
            CMP_NUM_FALLBACK(a, b, <=);
            DISPATCH();
        }
        OP_CASE(OP_GEQ): {
            /* a >= b is implemented as __le(b, a) — swap the arguments. */
            CandoValue b = POP(), a = POP();
            TRY_CMP_META(g_meta_le, true, false, a, b);
            CMP_NUM_FALLBACK(a, b, >=);
            DISPATCH();
        }
#undef TRY_CMP_META
#undef CMP_NUM_FALLBACK

        /* Multi-value comparisons: pop A right-hand values, then left. */
        OP_CASE(OP_EQ_STACK): {
            u16 n = READ_U16();
            /* Right-hand values are at [top-n .. top-1]; left is below. */
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n && result; i++) {
                if (!cando_value_equal(left, *(vm->stack_top - n + i)))
                    result = false;
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP()); /* left */
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_NEQ_STACK): {
            u16 n = READ_U16();
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n; i++) {
                if (cando_value_equal(left, *(vm->stack_top - n + i))) {
                    result = false; break;
                }
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_LT_STACK): {
            u16 n = READ_U16();
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) < cando_as_number(r))) result = false;
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_GT_STACK): {
            u16 n = READ_U16();
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) > cando_as_number(r))) result = false;
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_LEQ_STACK): {
            u16 n = READ_U16();
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) <= cando_as_number(r))) result = false;
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_GEQ_STACK): {
            u16 n = READ_U16();
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (u16 i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) >= cando_as_number(r))) result = false;
            }
            for (u16 i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        /* Range check: A encodes (left_inclusive | right_inclusive<<1)
         * Stack before: [min, val, max]                                 */
        OP_CASE(OP_RANGE_CHECK): {
            u16 flags      = READ_U16();
            bool left_inc  = (flags & 1) != 0;
            bool right_inc = (flags & 2) != 0;
            CandoValue vmax = POP(), vval = POP(), vmin = POP();
            if (!cando_is_number(vmin)||!cando_is_number(vval)||
                !cando_is_number(vmax)) {
                vm_runtime_error(vm, "range check requires numbers");
                goto handle_error;
            }
            f64 mn = cando_as_number(vmin), v = cando_as_number(vval), mx = cando_as_number(vmax);
            bool ok = (left_inc  ? (mn <= v) : (mn < v)) &&
                      (right_inc ? (v <= mx) : (v < mx));
            PUSH(cando_bool(ok));
            DISPATCH();
        }

        /* ── Band 7: Bitwise ────────────────────────────────────────── */
        OP_CASE(OP_BIT_AND): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)cando_as_number(a) & (i64)cando_as_number(b))));
            DISPATCH();
        }
        OP_CASE(OP_BIT_OR): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)cando_as_number(a) | (i64)cando_as_number(b))));
            DISPATCH();
        }
        OP_CASE(OP_BIT_XOR): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)cando_as_number(a) ^ (i64)cando_as_number(b))));
            DISPATCH();
        }
        OP_CASE(OP_BIT_NOT): {
            CandoValue a = POP();
            PUSH(cando_number((f64)(~(i64)cando_as_number(a))));
            DISPATCH();
        }
        OP_CASE(OP_LSHIFT): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)cando_as_number(a) << (i64)cando_as_number(b))));
            DISPATCH();
        }
        OP_CASE(OP_RSHIFT): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)cando_as_number(a) >> (i64)cando_as_number(b))));
            DISPATCH();
        }

        /* ── Band 8: Logical ────────────────────────────────────────── */
        /* ── Band 8: Logical / short-circuit ────────────────────────── */
        OP_CASE(OP_NOT): {
            CandoValue a = POP();
            {
                bool meta_ok;
                bool truthy = vm_is_truthy_meta(vm, a, &meta_ok);
                if (!meta_ok) { cando_value_release(a); goto handle_error; }
                PUSH(cando_bool(!truthy));
            }
            cando_value_release(a);
            DISPATCH();
        }
        OP_CASE(OP_AND_JUMP): {
            /* Peek TOS; if falsy jump to end (left stays as result).
             * If truthy, fall through — the explicit OP_POP in the bytecode
             * discards the truthy left before the right side is evaluated.  */
            i16 offset = READ_I16();
            {
                bool meta_ok;
                bool truthy = vm_is_truthy_meta(vm, PEEK(0), &meta_ok);
                if (!meta_ok) goto handle_error;
                if (!truthy) ip += offset;
            }
            DISPATCH();
        }
        OP_CASE(OP_OR_JUMP): {
            /* Peek TOS; if truthy jump to end (left stays as result).
             * If falsy, fall through — the explicit OP_POP in the bytecode
             * discards the falsy left before the right side is evaluated.   */
            i16 offset = READ_I16();
            {
                bool meta_ok;
                bool truthy = vm_is_truthy_meta(vm, PEEK(0), &meta_ok);
                if (!meta_ok) goto handle_error;
                if (truthy) ip += offset;
            }
            DISPATCH();
        }

        /* ── Band 9: Objects / arrays ───────────────────────────────── */
        /* ── Band 9: Object / array construction & access ───────────── */
        OP_CASE(OP_NEW_OBJECT): {
            PUSH(cando_bridge_new_object(vm));
            vm_gc_maybe_collect(vm);
            DISPATCH();
        }
        OP_CASE(OP_NEW_ARRAY): {
            u16 static_n = READ_U16();
            /* array_extra accumulates extra elements from ...spread and
             * masked multi-return calls inside the array literal.        */
            int _arr_total = (int)static_n + vm->array_extra;
            u32 n = _arr_total < 0 ? 0u : (u32)_arr_total;
            vm->array_extra = 0;
            CandoValue arr_val = cando_bridge_new_array(vm);
            if (n > 0) {
                CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(arr_val));
                /* Items are on the stack: stack_top-n .. stack_top-1 */
                CandoValue *base = vm->stack_top - n;
                for (u32 i = 0; i < n; i++) {
                    CdoValue item = cando_bridge_to_cdo(vm, base[i]);
                    cdo_array_push(arr, item);   /* retains internally */
                    cdo_value_release(item);
                    cando_value_release(base[i]);
                }
                vm->stack_top -= n;
            }
            PUSH(arr_val);
            vm_gc_maybe_collect(vm);
            DISPATCH();
        }
        OP_CASE(OP_GET_FIELD): {
            u16 ci = READ_U16();
            CandoValue obj_val = POP();

            /* String field access: look up in string prototype. */
            if (cando_is_string(obj_val)) {
                if (cando_is_object(vm->string_proto)) {
                    CdoObject   *proto = cando_bridge_resolve(
                                            vm, cando_as_handle(vm->string_proto));
                    CandoString *ks    = cando_as_string(frame->closure->chunk->constants[ci]);
                    CdoString   *key   = cando_bridge_intern_key(ks);
                    CdoValue     raw;
                    if (cdo_object_get(proto, key, &raw)) {
                        PUSH(cando_bridge_to_cando(vm, raw));
                    } else {
                        PUSH(cando_null());
                    }
                    cdo_string_release(key);
                } else {
                    PUSH(cando_null());
                }
                cando_value_release(obj_val);
                DISPATCH();
            }

            if (!cando_is_object(obj_val)) {
                cando_value_release(obj_val);
                vm_runtime_error(vm, "field access on non-object (got %s)",
                                 cando_value_type_name(cando_value_tag(obj_val)));
                goto handle_error;
            }
            CdoObject  *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            CandoString *ks = cando_as_string(frame->closure->chunk->constants[ci]);
            CdoString   *key = cando_bridge_intern_key(ks);
            CdoValue     result;
            bool         got = false;
            /* Use cdo_object_get for __index prototype-chain traversal. */
            if (cdo_object_get(obj, key, &result)) {
                got = true;
            } else if (obj->kind == OBJ_THREAD &&
                       cando_is_object(vm->thread_proto)) {
                /* Thread instances have no slot table; fall back to the
                 * cached `_meta.thread` prototype for method lookups. */
                CdoObject *tproto = cando_bridge_resolve(
                    vm, cando_as_handle(vm->thread_proto));
                if (cdo_object_get(tproto, key, &result)) got = true;
            }
            if (!got) {
                /* Try a callable __index: __index(obj, key) -> value. */
                CdoValue idx_callable;
                if (cdo_object_index_callable(obj, &idx_callable)) {
                    CandoValue idx_args[2];
                    idx_args[0] = obj_val;
                    idx_args[1] = cando_string_value(
                        cando_string_new(key->data, key->length));
                    bool ok = cando_vm_dispatch_callable(
                        vm, &idx_callable, idx_args, 2);
                    cando_value_release(idx_args[1]);
                    if (vm->has_error) {
                        cdo_string_release(key);
                        cando_value_release(obj_val);
                        goto handle_error;
                    }
                    if (ok) {
                        cdo_string_release(key);
                        cando_value_release(obj_val);
                        DISPATCH();
                    }
                }
            }
            if (got) PUSH(cando_bridge_to_cando(vm, result));
            else     PUSH(cando_null());
            cdo_string_release(key);
            cando_value_release(obj_val);
            DISPATCH();
        }
        OP_CASE(OP_SET_FIELD): {
            u16 ci = READ_U16();
            CandoValue val     = POP();
            CandoValue obj_val = PEEK(0);  /* object stays on stack */
            if (!cando_is_object(obj_val)) {
                cando_value_release(val);
                vm_runtime_error(vm, "field assignment on non-object");
                goto handle_error;
            }
            CdoObject  *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            CandoString *ks = cando_as_string(frame->closure->chunk->constants[ci]);
            CdoString   *key = cando_bridge_intern_key(ks);
            CdoValue     existing;
            if (!cdo_object_rawget(obj, key, &existing) && g_meta_newindex) {
                CandoValue key_cv = cando_string_value(
                    cando_string_new(key->data, key->length));
                CandoValue args[3] = { obj_val, key_cv, val };
                if (cando_vm_call_meta(vm, cando_as_handle(obj_val),
                                       (struct CdoString *)g_meta_newindex,
                                       args, 3)) {
                    cdo_string_release(key);
                    cando_value_release(key_cv);
                    cando_value_release(val);
                    DISPATCH();
                }
                cando_value_release(key_cv);
                if (vm->has_error) {
                    cdo_string_release(key);
                    cando_value_release(val);
                    goto handle_error;
                }
            }
            CdoValue cdo_val = cando_bridge_to_cdo(vm, val);
            cdo_object_rawset(obj, key, cdo_val, FIELD_NONE);  /* retains */
            cdo_value_release(cdo_val);
            cdo_string_release(key);
            cando_value_release(val);
            DISPATCH();
        }
        OP_CASE(OP_GET_INDEX): {
            CandoValue idx_val = POP();
            CandoValue obj_val = POP();
            if (!cando_is_object(obj_val)) {
                cando_value_release(idx_val);
                cando_value_release(obj_val);
                vm_runtime_error(vm, "index access on non-object");
                goto handle_error;
            }
            CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            if (cando_is_number(idx_val)) {
                u32      idx = (u32)cando_as_number(idx_val);
                CdoValue result;
                if (cdo_array_rawget_idx(obj, idx, &result)) {
                    PUSH(cando_bridge_to_cando(vm, result));
                } else {
                    PUSH(cando_null());
                }
            } else if (cando_is_string(idx_val)) {
                CdoString *key = cando_bridge_intern_key(cando_as_string(idx_val));
                CdoValue   result;
                if (cdo_object_rawget(obj, key, &result)) {
                    PUSH(cando_bridge_to_cando(vm, result));
                } else {
                    PUSH(cando_null());
                }
                cdo_string_release(key);
            } else {
                cando_value_release(idx_val);
                cando_value_release(obj_val);
                vm_runtime_error(vm, "index must be a number or string");
                goto handle_error;
            }
            cando_value_release(idx_val);
            cando_value_release(obj_val);
            DISPATCH();
        }
        OP_CASE(OP_SET_INDEX): {
            CandoValue val     = POP();
            CandoValue idx_val = POP();
            CandoValue obj_val = PEEK(0);  /* object stays on stack */
            if (!cando_is_object(obj_val)) {
                cando_value_release(val);
                cando_value_release(idx_val);
                vm_runtime_error(vm, "index assignment on non-object");
                goto handle_error;
            }
            CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            CdoValue   cdo_val = cando_bridge_to_cdo(vm, val);
            if (cando_is_number(idx_val)) {
                u32 idx = (u32)cando_as_number(idx_val);
                cdo_array_rawset_idx(obj, idx, cdo_val);   /* retains */
            } else if (cando_is_string(idx_val)) {
                CdoString *key = cando_bridge_intern_key(cando_as_string(idx_val));
                CdoValue   existing;
                if (!cdo_object_rawget(obj, key, &existing) && g_meta_newindex) {
                    CandoValue args[3] = { obj_val, idx_val, val };
                    if (cando_vm_call_meta(vm, cando_as_handle(obj_val),
                                           (struct CdoString *)g_meta_newindex,
                                           args, 3)) {
                        cdo_value_release(cdo_val);
                        cdo_string_release(key);
                        cando_value_release(val);
                        cando_value_release(idx_val);
                        DISPATCH();
                    }
                    if (vm->has_error) {
                        cdo_value_release(cdo_val);
                        cdo_string_release(key);
                        cando_value_release(val);
                        cando_value_release(idx_val);
                        goto handle_error;
                    }
                }
                cdo_object_rawset(obj, key, cdo_val, FIELD_NONE);   /* retains */
                cdo_string_release(key);
            } else {
                cdo_value_release(cdo_val);
                cando_value_release(val);
                cando_value_release(idx_val);
                vm_runtime_error(vm, "index must be a number or string");
                goto handle_error;
            }
            cdo_value_release(cdo_val);
            cando_value_release(val);
            cando_value_release(idx_val);
            DISPATCH();
        }
        /* ── Band 10: Collection introspection ──────────────────────── */
        OP_CASE(OP_LEN): {
            CandoValue a = POP();
            if (cando_is_string(a)) {
                u32 len = cando_as_string(a)->length;
                cando_value_release(a);
                PUSH(cando_number((f64)len));
            } else if (cando_is_object(a)) {
                /* Check __len meta-method first. */
                if (g_meta_len) {
                    CandoValue a_copy = cando_value_copy(a);
                    if (cando_vm_call_meta(vm, cando_as_handle(a),
                                           (struct CdoString *)g_meta_len,
                                           &a_copy, 1)) {
                        cando_value_release(a);
                        cando_value_release(a_copy);
                        DISPATCH();
                    }
                    if (vm->has_error) {
                        cando_value_release(a);
                        cando_value_release(a_copy);
                        goto handle_error;
                    }
                    cando_value_release(a_copy);
                }
                CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(a));
                u32 len = cdo_object_length(obj);
                cando_value_release(a);
                PUSH(cando_number((f64)len));
            } else {
                cando_value_release(a);
                vm_runtime_error(vm, "# operator requires a string or object");
                goto handle_error;
            }
            DISPATCH();
        }
        OP_CASE(OP_KEYS_OF): {
            CandoValue obj_val = POP();
            if (!cando_is_object(obj_val)) {
                cando_value_release(obj_val);
                vm_runtime_error(vm, "IN operator requires an object");
                goto handle_error;
            }
            CdoObject  *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            CandoValue  arr_val = cando_bridge_new_array(vm);
            CdoObject  *arr     = cando_bridge_resolve(vm, cando_as_handle(arr_val));
            u32 si = obj->fifo_head;
            while (si != UINT32_MAX) {
                ObjSlot *slot = &obj->slots[si];
                CdoValue key_val = cdo_string_value(
                    cdo_string_retain(slot->key));
                cdo_array_push(arr, key_val);
                si = slot->fifo_next;
            }
            cando_value_release(obj_val);
            PUSH(arr_val);
            DISPATCH();
        }
        OP_CASE(OP_VALS_OF): {
            CandoValue obj_val = POP();
            if (!cando_is_object(obj_val)) {
                cando_value_release(obj_val);
                vm_runtime_error(vm, "OF operator requires an object");
                goto handle_error;
            }
            CdoObject  *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
            CandoValue  arr_val = cando_bridge_new_array(vm);
            CdoObject  *arr     = cando_bridge_resolve(vm, cando_as_handle(arr_val));
            u32 si = obj->fifo_head;
            while (si != UINT32_MAX) {
                ObjSlot *slot = &obj->slots[si];
                cdo_array_push(arr, cdo_value_copy(slot->value));
                si = slot->fifo_next;
            }
            cando_value_release(obj_val);
            PUSH(arr_val);
            DISPATCH();
        }

        /* ── Band 10: Control flow ──────────────────────────────────── */
        /* ── Band 11: Control flow (jumps, loops, break/continue) ───── */
        OP_CASE(OP_JUMP): {
            i16 offset = READ_I16();
            ip += offset;
            DISPATCH();
        }
        OP_CASE(OP_JUMP_IF_FALSE): {
            i16 offset = READ_I16();
            bool meta_ok;
            bool truthy = vm_is_truthy_meta(vm, PEEK(0), &meta_ok);
            if (!meta_ok) goto handle_error;
            if (!truthy) ip += offset;
            DISPATCH();
        }
        OP_CASE(OP_JUMP_IF_TRUE): {
            i16 offset = READ_I16();
            bool meta_ok;
            bool truthy = vm_is_truthy_meta(vm, PEEK(0), &meta_ok);
            if (!meta_ok) goto handle_error;
            if (truthy) ip += offset;
            DISPATCH();
        }
        OP_CASE(OP_JUMP_IF_NULL): {
            /* Peek TOS; jump if exactly null (no pop). Used by the safety
             * indexer (?. / ?[) so that hitting null mid-chain leaves the
             * null on the stack as the chain's result.                     */
            i16 offset = READ_I16();
            if (cando_is_null(PEEK(0))) ip += offset;
            DISPATCH();
        }
        OP_CASE(OP_LOOP): {
            u16 back = READ_U16();
            ip -= back;
            if (CANDO_UNLIKELY(vm->jit_enabled)) {
                vm->jit_stats.backedge_hits++;
                /* Per-PC hot counter: when threshold trips for this
                 * specific backedge, the recorder activates.  ip now
                 * points at the loop body, which is the natural trace
                 * head; the next DISPATCH() will route through
                 * cando_recorder_observe and start recording. */
                cando_jit_hot_hit(vm, ip);
                /* Phase 8.3: while the recorder is ACTIVE, skip
                 * trace dispatch entirely.  Otherwise an inner-loop
                 * trace would run iters 2+ during outer-trace
                 * recording, leaving the outer's IR with only iter 1
                 * + a stale EXIT guard that fires on every replay.
                 *
                 * Phase 8.6: multi-version specialization -- iterate
                 * up to 8 sibling traces at this PC, ordered by
                 * recency.  Run each until one LOOP_DONE's at least
                 * once; otherwise side-exit + try the next.  After
                 * all siblings fail, fall through to bytecode. */
                if (vm->jit && !vm->jit->recorder.active) {
                    /* Cap siblings tried at 3 to bound dispatch
                     * overhead.  cando_jit_find_traces returns by
                     * last_used DESC, so older one-shot specialisations
                     * (e.g. OP_BREAK paths in mandelbrot) sort to the
                     * tail and aren't tried at every backedge. */
                    CandoTrace *traces[3];
                    u32 ntr = cando_jit_find_traces(vm, ip, traces,
                                                      sizeof(traces) /
                                                      sizeof(traces[0]));
                    for (u32 ti = 0; ti < ntr; ti++) {
                        CandoTrace *t = traces[ti];
                        /* Phase 8.9: mcode body's IR_LOOP loops
                         * internally and only returns on side-exit
                         * (TRACE_GUARD_FAILED) or trace-bail.  We
                         * just call once; t->run_iter_count is bumped
                         * inside mcode and harvested below. */
                        t->run_iter_count = 0;
                        CandoTraceStatus s = cando_trace_run(vm, t, false);
                        vm->jit_stats.trace_iters += t->run_iter_count;
                        if (t->run_iter_count > 0) {
                            t->consecutive_exits = 0;
                        }
                        if (s == TRACE_LOOP_DONE) {
                            /* IR-interpreter path (mcode_fn==NULL):
                             * old contract, returns LOOP_DONE per
                             * iter.  Loop here to drain. */
                            bool skip_inv = true;
                            for (;;) {
                                t->run_iter_count = 0;
                                s = cando_trace_run(vm, t, skip_inv);
                                vm->jit_stats.trace_iters += t->run_iter_count;
                                if (s == TRACE_LOOP_DONE) {
                                    vm->jit_stats.trace_iters++;
                                    continue;
                                }
                                break;
                            }
                            goto trace_done;
                        }
                        vm->jit_stats.trace_exits++;
                        t->consecutive_exits++;
                        if (t->run_iter_count > 0) goto trace_done;
                    }
                    /* All siblings side-exited prematurely.  Cap
                     * sibling count at 8 per PC; if we're under the
                     * cap, accumulate dispatch misses on the OLDEST
                     * sibling and un-blacklist start_pc when the
                     * miss count crosses 16 -- the next ~50 backedge
                     * hits will retrigger the recorder, which appends
                     * a new sibling specialised for whatever inner
                     * length is current at that future trigger. */
                    if (ntr > 0 && ntr < 4) {
                        CandoTrace *oldest = traces[ntr - 1];
                        oldest->total_dispatch_misses++;
                        /* Trigger at 64 misses (vs the inner-loop
                         * trace's 50 hot threshold), and only the
                         * first 4 sibling spawns -- nbody-style
                         * patterns where each outer iter has a
                         * different inner length show that more
                         * siblings cost more in dispatch overhead
                         * than they save in matched runs. */
                        if (oldest->total_dispatch_misses == 64 ||
                            oldest->total_dispatch_misses == 128 ||
                            oldest->total_dispatch_misses == 256 ||
                            oldest->total_dispatch_misses == 512) {
                            cando_hot_unblacklist(&vm->jit->hot, ip);
                        }
                    }
                  trace_done: ;
                }
            }
            DISPATCH();
        }
        OP_CASE(OP_BREAK): {
            u16 depth = READ_U16();
            if (vm->loop_depth == 0 || depth >= vm->loop_depth) {
                vm_runtime_error(vm, "BREAK outside loop (depth %u, loop_depth %u)",
                                 depth, vm->loop_depth);
                goto handle_error;
            }
            u32 idx  = vm->loop_depth - 1 - depth;
            u32 save = vm->loop_stack[idx].stack_save;
            u8  ltyp = vm->loop_stack[idx].loop_type;

            /* Release any temporaries above the saved stack mark. */
            while ((u32)(vm->stack_top - vm->stack) > save)
                cando_value_release(POP());

            /* Release the loop's own iterator state left on the stack.
             * FOR IN/OF:   [..., source_array, len_signed, index]  → pop 3
             * FOR OVER:    [..., iter, state, control, nvar]       → pop 4
             * WHILE:       no extra state                          → pop 0 */
            if (ltyp == CANDO_LOOP_FOR_OVER) {
                cando_value_release(POP()); /* nvar    */
                cando_value_release(POP()); /* control */
                cando_value_release(POP()); /* state   */
                cando_value_release(POP()); /* iter    */
            } else if (ltyp == CANDO_LOOP_FOR) {
                cando_value_release(POP()); /* index   */
                cando_value_release(POP()); /* len     */
                cando_value_release(POP()); /* source  */
            } else if (ltyp == CANDO_LOOP_FOR_RANGE) {
                /* All three slots are numbers (cur, step, end); the POP
                 * is still needed to advance stack_top. */
                DROP();
                DROP();
                DROP();
            }

            vm->spread_extra = 0;
            ip = vm->loop_stack[idx].break_ip;
            /* Restore if-chain depth: BREAK from inside an IF body skips
             * the chain's OP_IF_END, so without this each iteration leaks
             * an if_stack frame and eventually corrupts adjacent VM state. */
            vm->if_depth   = vm->loop_stack[idx].if_save;
            vm->loop_depth = idx;
            DISPATCH();
        }
        OP_CASE(OP_CONTINUE): {
            u16 depth = READ_U16();
            if (vm->loop_depth == 0 || depth >= vm->loop_depth) {
                vm_runtime_error(vm, "CONTINUE outside loop (depth %u, loop_depth %u)",
                                 depth, vm->loop_depth);
                goto handle_error;
            }
            u32 idx = vm->loop_depth - 1 - depth;
            ip      = vm->loop_stack[idx].cont_ip;
            /* Same rationale as OP_BREAK: CONTINUE from inside an IF body
             * bypasses OP_IF_END and would otherwise leak if_stack frames. */
            vm->if_depth   = vm->loop_stack[idx].if_save;
            vm->loop_depth = idx; /* pop frame; LOOP_MARK will re-push on next iteration */
            DISPATCH();
        }
        OP_CASE(OP_LOOP_MARK): {
            /* Parser emits OP_LOOP_MARK at the top of each loop body.
             * A = forward offset from (ip after instruction) to break target.
             * B = packed: bits[13:0] = backward offset to continue target,
             *             bits[15:14] = loop_type (CANDO_LOOP_*).          */
            u16 break_fwd = READ_U16();
            u16 b_packed  = READ_U16();
            u16 cont_back = b_packed & 0x3FFF;
            u8  loop_type = (u8)((b_packed >> 14) & 0x3);
            CANDO_ASSERT_MSG(vm->loop_depth < CANDO_LOOP_MAX,
                             "loop depth overflow");
            CandoLoopFrame *lf = &vm->loop_stack[vm->loop_depth];
            lf->break_ip  = ip + break_fwd;
            lf->cont_ip   = ip - cont_back;
            lf->stack_save = (u32)(vm->stack_top - vm->stack);
            lf->if_save    = vm->if_depth;
            lf->loop_type  = loop_type;
            vm->loop_depth++;
            DISPATCH();
        }
        OP_CASE(OP_LOOP_END): {
            if (vm->loop_depth > 0) vm->loop_depth--;
            DISPATCH();
        }
        OP_CASE(OP_IF_MARK): {
            /* Parser emits OP_IF_MARK at the start of each IF chain.
             * A = forward byte offset from (ip after instruction) to the
             * SETTLE target (the byte just past the chain's OP_IF_END).   */
            u16 settle_fwd = READ_U16();
            CANDO_ASSERT_MSG(vm->if_depth < CANDO_IF_MAX,
                             "if-chain depth overflow");
            CandoIfFrame *iff = &vm->if_stack[vm->if_depth];
            iff->settle_ip  = ip + settle_fwd;
            iff->stack_save = (u32)(vm->stack_top - vm->stack);
            iff->matched    = false;
            iff->prev_ran   = false;
            vm->if_depth++;
            DISPATCH();
        }
        OP_CASE(OP_IF_END): {
            if (vm->if_depth > 0) vm->if_depth--;
            DISPATCH();
        }
        OP_CASE(OP_SETTLE): {
            u16 depth = READ_U16();
            if (vm->if_depth == 0 || depth >= vm->if_depth) {
                vm_runtime_error(vm, "SETTLE outside IF (depth %u, if_depth %u)",
                                 depth, vm->if_depth);
                goto handle_error;
            }
            u32 idx  = vm->if_depth - 1 - depth;
            u32 save = vm->if_stack[idx].stack_save;
            /* Release any temporaries the IF chain pushed above its mark. */
            while ((u32)(vm->stack_top - vm->stack) > save)
                cando_value_release(POP());
            vm->spread_extra = 0;
            ip = vm->if_stack[idx].settle_ip;
            vm->if_depth = idx;
            DISPATCH();
        }
        OP_CASE(OP_IF_TEST_MATCHED): {
            CANDO_ASSERT_MSG(vm->if_depth > 0,
                             "OP_IF_TEST_MATCHED outside IF chain");
            CandoIfFrame *iff = &vm->if_stack[vm->if_depth - 1];
            cando_vm_push(vm, cando_bool(iff->matched));
            DISPATCH();
        }
        OP_CASE(OP_IF_TEST_PREV): {
            CANDO_ASSERT_MSG(vm->if_depth > 0,
                             "OP_IF_TEST_PREV outside IF chain");
            CandoIfFrame *iff = &vm->if_stack[vm->if_depth - 1];
            cando_vm_push(vm, cando_bool(iff->prev_ran));
            DISPATCH();
        }
        OP_CASE(OP_IF_SET_RAN): {
            CANDO_ASSERT_MSG(vm->if_depth > 0,
                             "OP_IF_SET_RAN outside IF chain");
            CandoIfFrame *iff = &vm->if_stack[vm->if_depth - 1];
            iff->matched  = true;
            iff->prev_ran = true;
            DISPATCH();
        }
        OP_CASE(OP_IF_CLEAR_PREV): {
            CANDO_ASSERT_MSG(vm->if_depth > 0,
                             "OP_IF_CLEAR_PREV outside IF chain");
            CandoIfFrame *iff = &vm->if_stack[vm->if_depth - 1];
            iff->prev_ran = false;
            DISPATCH();
        }

        /* ── Band 11: Functions ─────────────────────────────────────── */
        /* ── Band 12: Closures, calls, returns ──────────────────────── */
        OP_CASE(OP_CLOSURE): {
            /* The constant at index ci is a cando_number(fn_pc) — the byte
             * offset of the function body within the current chunk.
             * Following the operand the parser writes:
             *
             *     u16 capture_count
             *     u16 outer_slot[capture_count]
             *
             * For each captured slot we snapshot the current frame's value
             * into a fresh CandoUpvalue and attach the array to a fresh
             * CandoClosure -- so each invocation of OP_CLOSURE produces a
             * function whose OP_LOAD_UPVAL/OP_STORE_UPVAL access *its*
             * captures, not whatever the parent frame happened to share. */
            u16 ci    = READ_U16();
            u32 fn_pc = (u32)cando_as_number(frame->closure->chunk->constants[ci]);

            u16 cap_count = READ_U16();

            CandoClosure *new_closure = (CandoClosure *)cando_alloc(
                sizeof(CandoClosure));
            new_closure->chunk         = frame->closure->chunk;
            new_closure->upvalue_count = cap_count;
            new_closure->upvalues      = NULL;
            if (cap_count > 0) {
                new_closure->upvalues = (CandoUpvalue **)cando_alloc(
                    cap_count * sizeof(CandoUpvalue *));
                for (u16 ui = 0; ui < cap_count; ui++) {
                    u16 slot = READ_U16();
                    /* Capture a live alias of the outer frame's slot;
                     * sibling closures that close the same slot get the
                     * *same* CandoUpvalue, so they share the variable. */
                    CandoUpvalue *uv = vm_capture_upvalue(vm,
                                                          &frame->slots[slot]);
                    atomic_fetch_add_explicit(&uv->refcount, 1u,
                                              memory_order_relaxed);
                    new_closure->upvalues[ui] = uv;
                }
            }

            CdoObject  *fn_obj = cdo_function_new(fn_pc,
                                                  (void *)new_closure,
                                                  NULL, 0);
            /* Tell cdo_object_destroy how to dispose of the attached
             * CandoClosure when the OBJ_FUNCTION is reclaimed, and how
             * to trace the closure's upvalues during GC mark.           */
            fn_obj->fn.script.bytecode_free =
                (void (*)(void *))cando_closure_free;
            /* Wrapper so cando_closure_trace's signature
             *   (CandoClosure*, CandoVM*, mark, ud)
             * adapts to the object-layer's
             *   (bytecode, CdoMarkFn, ud)
             * without exposing CandoVM to object/.  ud is a TraceCtx*
             * the VM-side root walker passes through.                  */
            fn_obj->fn.script.bytecode_trace = vm_closure_trace_adapter;
            HandleIndex h = cando_bridge_track_obj(vm, fn_obj);
            PUSH(cando_object_value(h));
            vm_gc_maybe_collect(vm);
            DISPATCH();
        }
        OP_CASE(OP_CALL): {
            u16 static_argc = READ_U16();
            /* Apply accumulated spread_extra from multi-return arguments.
             * spread_extra can be negative when a masked call keeps fewer
             * values than its compile-time slot count; clamp to zero so
             * arg_count never underflows.                                */
            int _total = (int)static_argc + vm->spread_extra;
            u32 arg_count = _total < 0 ? 0u : (u32)_total;
            vm->spread_extra = 0;
            /* The function value sits just below the arguments. */
            CandoValue callee = *(vm->stack_top - arg_count - 1);

        op_call_dispatch:
            /* Native function: negative-number sentinel convention. */
            if (IS_NATIVE_FN(callee)) {
                u32 ni = NATIVE_INDEX(callee);
                if (ni >= vm->native_count) {
                    vm_runtime_error(vm, "invalid native function index %u",
                                     ni);
                    goto handle_error;
                }
                /* Stack layout: [..., callee, arg0, ..., argN-1]
                 * Native pushes return values above the args via cando_vm_push().
                 * After the call: [..., callee, arg0..argN-1, ret0..retM-1] */
                CandoValue *callee_slot = vm->stack_top - arg_count - 1;
                CandoValue *args = callee_slot + 1;
                SYNC_IP();
                int ret_count = vm->native_fns[ni](vm, (int)arg_count, args);
                if (vm->has_error) goto handle_error;
                if (ret_count < 0) ret_count = 0;
                /* Return values are now at vm->stack_top - ret_count. */
                CandoValue *ret_src = vm->stack_top - ret_count;
                /* Release args (callee is a number sentinel — no heap). */
                for (u32 i = 0; i < (u32)arg_count; i++)
                    cando_value_release(args[i]);
                /* Slide return values down to callee_slot. */
                for (int i = 0; i < ret_count; i++)
                    callee_slot[i] = ret_src[i];
                /* Normalize: ensure at least 1 value on the stack so that
                 * expression-statement OP_POP always finds something to pop.
                 * A void native call evaluates to null in expression context.
                 * Set last_ret_count = 1 in this case so OP_SPREAD_RET does
                 * not produce a negative spread_extra for void callee args.  */
                if (ret_count == 0) {
                    callee_slot[0] = cando_null();
                    vm->stack_top = callee_slot + 1;
                    vm->last_ret_count = 1;
                } else {
                    vm->stack_top = callee_slot + ret_count;
                    vm->last_ret_count = ret_count;
                }
                DISPATCH();
            }

            /* Positive number: user-defined function stored as PC offset
             * in the current chunk (parser emits cando_number((f64)fn_start)). */
            if (cando_is_number(callee)) {
                u32 pc = (u32)cando_as_number(callee);
                SYNC_IP();
                if (!vm_push_frame(vm, frame->closure,
                                   frame->closure->chunk->code + pc,
                                   arg_count, false))
                    goto handle_error;
                LOAD_FRAME();
                DISPATCH();
            }

            if (!cando_is_object(callee)) {
                vm_runtime_error(vm, "can only call functions (got %s)",
                                 cando_value_type_name(cando_value_tag(callee)));
                goto handle_error;
            }

            /* OBJ_FUNCTION: script closure created by OP_CLOSURE.
             * fn.script.bytecode  = (void *)CandoClosure *
             * fn.script.param_count = fn_pc (byte offset of function body) */
            {
                CdoObject *fn_obj = cando_bridge_resolve(vm, cando_as_handle(callee));
                if (fn_obj && fn_obj->kind == OBJ_FUNCTION &&
                    fn_obj->fn.script.bytecode) {
                    CandoClosure *fn_closure =
                        (CandoClosure *)fn_obj->fn.script.bytecode;
                    u32 fn_pc = fn_obj->fn.script.param_count;

                    SYNC_IP();
                    if (!vm_push_frame(vm, fn_closure,
                                       fn_closure->chunk->code + fn_pc,
                                       arg_count, false))
                        goto handle_error;
                    LOAD_FRAME();
                    DISPATCH();
                }

                /* __call meta-method: invoking a non-function object whose
                 * __call is a callable.  Splice the original receiver in as
                 * the first argument and re-enter dispatch with the meta
                 * function as the new callee.  Used by CLASS objects: every
                 * class binds vm->default_class_call to its __call.        */
                if (fn_obj && g_meta_call) {
                    CdoValue meta_val;
                    if (cdo_object_get(fn_obj, g_meta_call, &meta_val)) {
                        if (vm->stack_top >= vm->stack + CANDO_STACK_MAX) {
                            vm_runtime_error(vm,
                                "stack overflow in __call dispatch");
                            goto handle_error;
                        }
                        CandoValue meta_callee =
                            cando_bridge_to_cando(vm, meta_val);
                        /* Layout before splice (arg_count = N):
                         *     base[0]   = callee
                         *     base[1..N] = args
                         *     stack_top = base + N + 1
                         * Layout after splice (new arg_count = N+1):
                         *     base[0]   = meta_callee
                         *     base[1]   = original callee (now first arg)
                         *     base[2..N+1] = original args
                         *     stack_top = base + N + 2
                         */
                        CandoValue *base = vm->stack_top - arg_count - 1;
                        for (int i = (int)arg_count; i >= 0; i--)
                            base[i + 1] = base[i];
                        base[0] = meta_callee;
                        vm->stack_top++;
                        arg_count++;
                        callee = meta_callee;
                        goto op_call_dispatch;
                    }
                }
            }

            vm_runtime_error(vm, "can only call functions (got object)");
            goto handle_error;
        }
        OP_CASE(OP_METHOD_CALL):
        OP_CASE(OP_FLUENT_CALL): {
            CandoOpcode op = (CandoOpcode)ip[-1];
            u16 name_ci   = READ_U16();
            u16 arg_count = READ_U16();
            bool is_fluent = (op == OP_FLUENT_CALL);

            CandoValue receiver = *(vm->stack_top - arg_count - 1);
            CdoValue method_cdo = cdo_null();

            if (cando_is_string(receiver)) {
                if (cando_is_object(vm->string_proto)) {
                    CandoValue name_val = frame->closure->chunk->constants[name_ci];
                    CdoString *skey = cando_bridge_intern_key(cando_as_string(name_val));
                    CdoObject *sproto = cando_bridge_resolve(vm, cando_as_handle(vm->string_proto));
                    cdo_object_get(sproto, skey, &method_cdo);
                    cdo_string_release(skey);
                }
            } else if (cando_is_object(receiver)) {
                CdoObject *robj = cando_bridge_resolve(vm, cando_as_handle(receiver));
                CandoValue name_val = frame->closure->chunk->constants[name_ci];
                CdoString *key = cando_bridge_intern_key(cando_as_string(name_val));

                /* Array method special case: look in array_proto if not found in array object. */
                if (robj->kind == OBJ_ARRAY && cando_is_object(vm->array_proto)) {
                    if (!cdo_object_get(robj, key, &method_cdo)) {
                        CdoObject *aproto = cando_bridge_resolve(vm, cando_as_handle(vm->array_proto));
                        cdo_object_get(aproto, key, &method_cdo);
                    }
                } else if (robj->kind == OBJ_THREAD && cando_is_object(vm->thread_proto)) {
                    /* Thread instances have no slot table; methods live on
                     * the cached `_meta.thread` prototype. */
                    CdoObject *tproto = cando_bridge_resolve(vm, cando_as_handle(vm->thread_proto));
                    cdo_object_get(tproto, key, &method_cdo);
                } else {
                    cdo_object_get(robj, key, &method_cdo);
                }
                cdo_string_release(key);
            } else {
                vm_runtime_error(vm, "method call on non-object (got %s)",
                                 cando_value_type_name(cando_value_tag(receiver)));
                goto handle_error;
            }

            CandoValue method = cando_bridge_to_cando(vm, method_cdo);
            cdo_value_release(method_cdo);

            bool callable = false;
            if (IS_NATIVE_FN(method)) callable = true;
            else if (cando_is_number(method)) callable = true;
            else if (cando_is_object(method)) {
                CdoObject *mo = cando_bridge_resolve(vm, cando_as_handle(method));
                if (mo->kind == OBJ_FUNCTION || mo->kind == OBJ_NATIVE) callable = true;
            }

            if (!callable) {
                cando_value_release(method);
                vm_runtime_error(vm, "%s method is not callable", is_fluent ? "fluent" : "");
                goto handle_error;
            }

            /* Shift stack up by 1 to make room for method at base[0].
             * base layout: [method, receiver, arg1, ..., argN] */
            if (vm->stack_top >= vm->stack + CANDO_STACK_MAX) {
                cando_value_release(method);
                vm_runtime_error(vm, "stack overflow in method call");
                goto handle_error;
            }
            CandoValue *base = vm->stack_top - arg_count - 1;
            for (int i = arg_count; i >= 0; i--) {
                base[i + 1] = base[i];
            }
            base[0] = method;
            vm->stack_top++;
            u32 total_argc = arg_count + 1;

            /* Native function (sentinel) */
            if (IS_NATIVE_FN(method)) {
                u32 ni = NATIVE_INDEX(method);
                CandoValue *args = base + 1;
                SYNC_IP();
                int ret_count = vm->native_fns[ni](vm, (int)total_argc, args);
                if (vm->has_error) goto handle_error;
                if (ret_count < 0) ret_count = 0;

                if (is_fluent) {
                    /* Discard all return values. */
                    for (int i = 0; i < ret_count; i++) {
                        cando_value_release(cando_vm_pop(vm));
                    }
                    /* Keep receiver, release method and arguments. */
                    CandoValue rec = base[1];
                    cando_value_release(base[0]);
                    for (u32 i = 2; i < total_argc + 1; i++) cando_value_release(base[i]);
                    base[0] = rec;
                    vm->stack_top = base + 1;
                    vm->last_ret_count = 1;
                } else {
                    CandoValue *ret_src = vm->stack_top - ret_count;
                    for (u32 i = 0; i < total_argc + 1; i++) cando_value_release(base[i]);
                    for (int i = 0; i < ret_count; i++) base[i] = ret_src[i];
                    if (ret_count == 0) {
                        base[0] = cando_null();
                        vm->stack_top = base + 1;
                    } else {
                        vm->stack_top = base + ret_count;
                    }
                    vm->last_ret_count = ret_count;
                }
                DISPATCH();
            }

            /* OBJ_NATIVE object */
            if (cando_is_object(method)) {
                CdoObject *mo = cando_bridge_resolve(vm, cando_as_handle(method));
                if (mo->kind == OBJ_NATIVE) {
                    CdoValue *cdo_args = (CdoValue *)cando_alloc(total_argc * sizeof(CdoValue));
                    for (u32 i = 0; i < total_argc; i++) cdo_args[i] = cando_bridge_to_cdo(vm, base[i + 1]);
                    SYNC_IP();
                    CdoValue result = mo->fn.native.fn(NULL, cdo_args, total_argc);
                    for (u32 i = 0; i < total_argc; i++) cdo_value_release(cdo_args[i]);
                    cando_free(cdo_args);
                    if (vm->has_error) { cdo_value_release(result); goto handle_error; }

                    if (is_fluent) {
                        cdo_value_release(result);
                        CandoValue rec = base[1];
                        cando_value_release(base[0]);
                        for (u32 i = 2; i < total_argc + 1; i++) cando_value_release(base[i]);
                        base[0] = rec;
                        vm->stack_top = base + 1;
                        vm->last_ret_count = 1;
                    } else {
                        CandoValue cv = cando_bridge_to_cando(vm, result);
                        cdo_value_release(result);
                        for (u32 i = 0; i < total_argc + 1; i++) cando_value_release(base[i]);
                        base[0] = cv;
                        vm->stack_top = base + 1;
                        vm->last_ret_count = 1;
                    }
                    DISPATCH();
                }
            }

            /* OBJ_FUNCTION (script closure) */
            if (cando_is_object(method)) {
                CdoObject *fn_obj = cando_bridge_resolve(vm, cando_as_handle(method));
                if (fn_obj->kind == OBJ_FUNCTION && fn_obj->fn.script.bytecode) {
                    CandoClosure *fn_closure = (CandoClosure *)fn_obj->fn.script.bytecode;
                    u32 fn_pc = fn_obj->fn.script.param_count;
                    SYNC_IP();
                    if (!vm_push_frame(vm, fn_closure,
                                       fn_closure->chunk->code + fn_pc,
                                       total_argc, is_fluent))
                        goto handle_error;
                    LOAD_FRAME();
                    DISPATCH();
                }
            }

            /* Raw PC number: inline function in the same chunk.  Unlike
             * OP_CALL's number-callee branch this site historically did
             * not pre-allocate local nulls; vm_push_frame will fill them
             * if the chunk declares any (no-op when local_count == 0).  */
            if (cando_is_number(method)) {
                u32 pc = (u32)cando_as_number(method);
                SYNC_IP();
                if (!vm_push_frame(vm, frame->closure,
                                   frame->closure->chunk->code + pc,
                                   total_argc, is_fluent))
                    goto handle_error;
                LOAD_FRAME();
                DISPATCH();
            }

            CANDO_UNREACHABLE();
        }
        OP_CASE(OP_RETURN): {
            u16 ret_count = READ_U16();
            if (!frame->is_fluent) vm->last_ret_count = (int)ret_count;
            SYNC_IP();

            /* Close any upvalues that pointed into this frame's stack. */
            vm_close_upvalues(vm, frame->slots);

            CandoValue fluent_receiver = cando_null();
            if (frame->is_fluent) {
                /* Receiver is at frame->slots[1]. We must retain it. */
                fluent_receiver = cando_value_copy(frame->slots[1]);
            }

            /* Save return values (they're on top of the stack). */
            CandoValue *ret_start = vm->stack_top - ret_count;

            /* Release locals below the return values. */
            CandoValue *slot = frame->slots;
            while (slot < ret_start) {
                cando_value_release(*slot++);
            }

            /* Pop the frame.  Also unwind the loop stack back to whatever
             * depth was active when the frame was entered, so any FOR-IN /
             * WHILE / FOR-OVER loops the function left open (e.g. via an
             * early RETURN inside the body) cannot leak frames into the
             * caller's BREAK/CONTINUE depth math.  Likewise the if-chain
             * stack so SETTLE depth math stays per-frame.                */
            vm->loop_depth = frame->loop_save;
            vm->if_depth   = frame->if_save;
            vm->frame_count--;

            /* Thread result capture: if we've returned to the thread boundary,
             * stash ALL return values and signal the outer vm_run to stop.  */
            if (vm->thread_stop_frame != ~0u &&
                vm->frame_count == vm->thread_stop_frame) {
                if (frame->is_fluent) {
                    /* Discard return values, use receiver. */
                    for (u16 i = 0; i < ret_count; i++) cando_value_release(ret_start[i]);
                    vm->thread_results[0] = fluent_receiver;
                    vm->thread_result_count = 1;
                } else {
                    vm->thread_result_count = ret_count < CANDO_MAX_THROW_ARGS
                                              ? ret_count : CANDO_MAX_THROW_ARGS;
                    for (u32 i = 0; i < vm->thread_result_count; i++)
                        vm->thread_results[i] = cando_value_copy(ret_start[i]);
                    for (u16 i = 0; i < ret_count; i++)
                        cando_value_release(ret_start[i]);
                }
                vm->stack_top = frame->slots;
                return VM_EVAL_DONE;
            }

            /* Eval re-entrancy: if we've returned back to the eval boundary,
             * stash the results and signal the outer vm_run to stop. */
            if (vm->eval_stop_frame != ~0u &&
                vm->frame_count == vm->eval_stop_frame) {
                vm->last_ret_count = (int)ret_count;
                if (frame->is_fluent) {
                    for (u16 i = 0; i < ret_count; i++) cando_value_release(ret_start[i]);
                    vm->eval_results = (CandoValue *)cando_alloc(sizeof(CandoValue));
                    vm->eval_results[0] = fluent_receiver;
                    vm->eval_result_count = 1;
                    vm->eval_result_cap = 1;
                } else {
                    vm->eval_result_count = ret_count;
                    if (ret_count > 0) {
                        vm->eval_result_cap = ret_count;
                        vm->eval_results = (CandoValue *)cando_alloc(
                            ret_count * sizeof(CandoValue));
                        for (u16 i = 0; i < ret_count; i++)
                            vm->eval_results[i] = cando_value_copy(ret_start[i]);
                        for (u16 i = 0; i < ret_count; i++)
                            cando_value_release(ret_start[i]);
                    } else {
                        vm->eval_result_cap = 0;
                        vm->eval_results    = NULL;
                    }
                }
                vm->stack_top = frame->slots;
                return VM_EVAL_DONE;
            }

            if (vm->frame_count == 0) {
                /* Returning from the top-level script. */
                vm->stack_top = vm->stack;
                cando_value_release(fluent_receiver);
                return VM_HALT;
            }

            /* Restore the caller's frame. */
            CandoValue *new_top = frame->slots; /* old frame base */

            if (frame->is_fluent) {
                /* Discard all return values from the function. */
                for (u16 i = 0; i < ret_count; i++) {
                    cando_value_release(ret_start[i]);
                }
                new_top[0] = fluent_receiver;
                vm->stack_top = new_top + 1;
                vm->last_ret_count = 1;
            } else {
                for (u16 i = 0; i < ret_count; i++) {
                    new_top[i] = ret_start[i];
                }
                vm->stack_top = new_top + ret_count;
            }

            LOAD_FRAME();
            DISPATCH();
        }
        OP_CASE(OP_TAIL_CALL): {
            /* Skeleton: implemented as a regular call for now.          */
            u16 arg_count = READ_U16();
            CANDO_UNUSED(arg_count);
            vm_runtime_error(vm, "tail call not yet implemented");
            goto handle_error;
        }

        /* ── Band 12: Varargs ───────────────────────────────────────── */
        /* ── Band 13: Vararg / unpack ───────────────────────────────── */
        OP_CASE(OP_LOAD_VARARG): {
            /* TODO: vararg access requires vararg calling convention.     */
            u16 slot = READ_U16(); CANDO_UNUSED(slot);
            PUSH(cando_null());
            DISPATCH();
        }
        OP_CASE(OP_VARARG_LEN): {
            /* TODO: vararg length requires vararg calling convention.     */
            PUSH(cando_number(0.0));
            DISPATCH();
        }
        OP_CASE(OP_UNPACK): {
            /* Pop top value. If array, push all elements. Else push it.
             * Sets last_ret_count to the number of values pushed so that
             * a following OP_ARRAY_SPREAD can accumulate the extra count. */
            CandoValue v = POP();
            if (cando_is_object(v)) {
                CdoObject *obj = cando_bridge_resolve(vm, (HandleIndex)cando_as_handle(v));
                if (obj->kind == OBJ_ARRAY) {
                    u32 len = cdo_array_len(obj);
                    for (u32 ui = 0; ui < len; ui++) {
                        CdoValue cv = cdo_null();
                        cdo_array_rawget_idx(obj, ui, &cv);
                        PUSH(cando_bridge_to_cando(vm, cv));
                    }
                    cando_value_release(v);
                    vm->last_ret_count = (int)len;
                } else {
                    PUSH(v);
                    vm->last_ret_count = 1;
                }
            } else {
                PUSH(v);
                vm->last_ret_count = 1;
            }
            DISPATCH();
        }

        /* ── Band 13: Iteration ─────────────────────────────────────── */
        /* ── Band 14: Ranges & iteration ────────────────────────────── */
        OP_CASE(OP_RANGE_ASC): {
            CandoValue b = POP(), a = POP();
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "range requires numbers");
                goto handle_error;
            }
            i64 from  = (i64)cando_as_number(a);
            i64 to    = (i64)cando_as_number(b);
            /* Build an array so the range is a single value on the stack.
             * This lets it be used as a for-loop iterable, a function
             * argument, an assignment target, etc. without corrupting the
             * call frame. */
            SYNC_IP();
            CandoValue arr_val = cando_bridge_new_array(vm);
            CdoObject *arr     = cando_bridge_resolve(vm, cando_as_handle(arr_val));
            for (i64 v = from; v <= to; v++) {
                CdoValue cv = cdo_number((f64)v);
                cdo_array_push(arr, cv);
            }
            PUSH(arr_val);
            DISPATCH();
        }
        OP_CASE(OP_RANGE_DESC): {
            CandoValue b = POP(), a = POP();
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "range requires numbers");
                goto handle_error;
            }
            i64 from  = (i64)cando_as_number(a);
            i64 to    = (i64)cando_as_number(b);
            SYNC_IP();
            CandoValue arr_val = cando_bridge_new_array(vm);
            CdoObject *arr     = cando_bridge_resolve(vm, cando_as_handle(arr_val));
            for (i64 v = from; v >= to; v--) {
                CdoValue cv = cdo_number((f64)v);
                cdo_array_push(arr, cv);
            }
            PUSH(arr_val);
            DISPATCH();
        }
        OP_CASE(OP_FOR_INIT): {
            /* mode: 1 = keys (FOR IN), 0 = values (FOR OF/OVER)
             *
             * Stack before: [..., iterable]
             * Stack after:  [..., source_array, len_signed, 0]
             *
             * The state is always 3 slots regardless of the iterable's
             * size; this avoids the stack-overflow that the old design
             * hit for FOR i IN 1 -> N with N >= ~2046 (each element was
             * pushed onto the 2048-slot VM stack).
             *
             * `len_signed` encodes the iteration mode:
             *   >= 0 -- values mode: NEXT pushes source_array[index]
             *   <  0 -- keys mode (arrays only): NEXT pushes index;
             *           the real length is -len_signed.
             *
             * For object IN/OF and scalar iteration the source array is
             * a one-shot heap snapshot built here so that mutating the
             * underlying object during the loop does not derail
             * iteration -- same semantics as the old snapshot path.    */
            u16 keys_mode = READ_U16();
            CandoValue iterable = POP();

            CandoValue source_val;
            i64        signed_len;

            if (cando_is_object(iterable)) {
                CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(iterable));
                if (obj->kind == OBJ_ARRAY) {
                    /* Iterate the array in place -- no snapshot needed. */
                    source_val = iterable;
                    u32 len    = cdo_array_len(obj);
                    signed_len = keys_mode ? -(i64)len : (i64)len;
                } else {
                    /* Plain object: snapshot keys-or-values into a heap
                     * array so we get array-style indexed access in
                     * OP_FOR_NEXT and never touch the VM stack per
                     * element.                                          */
                    SYNC_IP();
                    source_val = cando_bridge_new_array(vm);
                    CdoObject *snap =
                        cando_bridge_resolve(vm, cando_as_handle(source_val));
                    u32 fi = obj->fifo_head;
                    u32 count = 0;
                    while (fi != UINT32_MAX) {
                        ObjSlot *slot = &obj->slots[fi];
                        if (keys_mode) {
                            CdoString *ks = cdo_string_intern(
                                slot->key->data, slot->key->length);
                            cdo_array_push(snap, cdo_string_value(ks));
                        } else {
                            cdo_array_push(snap, slot->value);
                        }
                        count++;
                        fi = slot->fifo_next;
                    }
                    signed_len = (i64)count;  /* values mode for snapshot */
                }
            } else {
                /* Scalar: wrap in a one-element heap array so the rest
                 * of the iteration machinery is uniform.                */
                SYNC_IP();
                source_val = cando_bridge_new_array(vm);
                CdoObject *snap =
                    cando_bridge_resolve(vm, cando_as_handle(source_val));
                cdo_array_push(snap, cando_bridge_to_cdo(vm, iterable));
                cando_value_release(iterable);
                signed_len = 1;
            }

            PUSH(source_val);
            PUSH(cando_number((f64)signed_len));
            PUSH(cando_number(0.0));
            DISPATCH();
        }
        OP_CASE(OP_FOR_NEXT): {
            /* Stack: [..., source_array, len_signed, index]
             * If index >= abs(len_signed): pop the 3-slot state, jump.
             * Else (values mode):  push source_array[index], index++.
             * Else (keys mode):    push index,                index++. */
            i16 off       = (i16)(READ_U16());
            f64 index_f   = cando_as_number(*(vm->stack_top - 1));
            f64 len_f     = cando_as_number(*(vm->stack_top - 2));
            i64 abs_len   = (i64)(len_f < 0.0 ? -len_f : len_f);
            bool keys     = len_f < 0.0;

            if ((i64)index_f >= abs_len) {
                cando_value_release(POP()); /* index   */
                cando_value_release(POP()); /* len     */
                cando_value_release(POP()); /* source  */
                ip += off;
            } else {
                if (keys) {
                    PUSH(cando_number(index_f));
                } else {
                    CandoValue source = *(vm->stack_top - 3);
                    CdoObject *src    =
                        cando_bridge_resolve(vm, cando_as_handle(source));
                    CdoValue cv = cdo_null();
                    cdo_array_rawget_idx(src, (u32)index_f, &cv);
                    PUSH(cando_bridge_to_cando(vm, cv));
                }
                /* PUSH above moved stack_top; the index slot is now at -2. */
                cando_set_number(&vm->stack_top[-2], index_f + 1.0);
                if (CANDO_UNLIKELY(vm->jit_enabled))
                    vm->jit_stats.iter_next_hits++;
            }
            DISPATCH();
        }
        OP_CASE(OP_FOR_RANGE_INIT): {
            /* Numeric range FOR -- avoids allocating a heap array.
             * A = 0 ascending (step = +1), 1 descending (step = -1).
             * Stack before: [..., lo, hi]   (lo pushed first by parser)
             * Stack after:  [..., end, step, cur]
             *
             *   ASC : cur = lo, end = hi, step = +1  -- iterate lo..hi
             *   DESC: cur = lo, end = hi, step = -1  -- iterate lo..hi descending
             *
             * Match OP_RANGE_ASC / OP_RANGE_DESC operand convention:
             * OP_RANGE_ASC iterates from a (first-pushed) up to b
             * (second-pushed); OP_RANGE_DESC iterates from a down to b.
             * So for ASC we step +1 from lo toward hi, for DESC step -1.
             *
             * If lo and hi are in the wrong direction (e.g. ASC with lo > hi),
             * OP_FOR_RANGE_NEXT exits immediately on its first check, matching
             * the empty-array behaviour of the OP_RANGE_ASC + OP_FOR_INIT
             * path (RANGE_ASC builds an empty array for from > to). */
            u16 a = READ_U16();
            CandoValue hi = POP();
            CandoValue lo = POP();
            if (!cando_is_number(lo) || !cando_is_number(hi)) {
                vm_runtime_error(vm, "range requires numbers");
                goto handle_error;
            }
            f64 cur_f  = cando_as_number(lo);
            f64 end_f  = cando_as_number(hi);
            f64 step_f = (a == 0) ? 1.0 : -1.0;
            PUSH(cando_number(end_f));
            PUSH(cando_number(step_f));
            PUSH(cando_number(cur_f));
            DISPATCH();
        }
        OP_CASE(OP_FOR_RANGE_NEXT): {
            /* Stack: [..., end, step, cur]
             * step is +1 (ascending) or -1 (descending).
             * Exit condition: cur has moved past end in the step direction.
             * On non-exit, push cur as the loop variable, advance cur by step. */
            i16 off    = (i16)(READ_U16());
            f64 cur_f  = cando_as_number(*(vm->stack_top - 1));
            f64 step_f = cando_as_number(*(vm->stack_top - 2));
            f64 end_f  = cando_as_number(*(vm->stack_top - 3));

            bool done = (step_f > 0.0) ? (cur_f > end_f) : (cur_f < end_f);
            if (done) {
                /* Numbers are inline values; no release needed. Just drop. */
                DROP();
                DROP();
                DROP();
                ip += off;
            } else {
                PUSH(cando_number(cur_f));
                /* After PUSH the cur slot is at -2. */
                cando_set_number(&vm->stack_top[-2], cur_f + step_f);
                if (CANDO_UNLIKELY(vm->jit_enabled))
                    vm->jit_stats.iter_next_hits++;
            }
            DISPATCH();
        }
        OP_CASE(OP_FOR_OVER_INIT): {
            /* Lua-style triplet iterator initialization.
             * MODE: Parser emits OP_SPREAD_RET before this if the last expr was a call.
             *
             * A = number of loop variables to bind.
             * B = bit-packed: count of values provided in the expression | 0x8000 if last was call.
             *
             * Protocol:
             * 1. iterator function
             * 2. state
             * 3. initial control variable
             *
             * Stack after: [..., iter_fn, state, control, num_vars (const)]
             */
            u16 nvar    = READ_U16();
            u16 packed  = READ_U16();
            u16 count   = packed & 0x7FFF;
            bool is_call = (packed & 0x8000) != 0;

            /* Total values on stack from the 'over' expression. */
            int total = (int)count;
            if (is_call) {
                /* last_ret_count already includes the 1 we assumed in count. */
                total = (int)count - 1 + vm->last_ret_count;
            }
            if (total < 1) {
                PUSH(cando_null()); // iter
                PUSH(cando_null()); // state
                PUSH(cando_null()); // control
            } else if (total == 1) {
                PUSH(cando_null()); // state
                PUSH(cando_null()); // control
            } else if (total == 2) {
                PUSH(cando_null()); // control
            } else if (total > 3) {
                /* Truncate to triplet. */
                for (int i = 0; i < total - 3; i++) {
                    cando_value_release(POP());
                }
            }

            /* Record the number of loop variables. */
            PUSH(cando_number((f64)nvar));
            vm->last_ret_count = 0;
            DISPATCH();
        }
        OP_CASE(OP_FOR_OVER_NEXT): {
            /* Lua-style triplet iterator step.
             * Stack: [..., iterator_fn, state, control, nvar]
             *
             * Action: Call iterator_fn(state, control).
             * 1. First return value -> new control.
             * 2. Subsequent values -> loop variables (up to 16).
             * 3. If new control is NULL -> exit loop.
             */
            i16 off = (i16)(READ_U16());
            u16 nvar = (u16)(cando_as_number(vm->stack_top[-1]));
            CandoValue control = *(vm->stack_top - 2);
            CandoValue state   = *(vm->stack_top - 3);
            CandoValue iter    = *(vm->stack_top - 4);

            /* Prepare arguments for call: [state, control] */
            CandoValue args[2];
            args[0] = cando_value_copy(state);
            args[1] = cando_value_copy(control);

            SYNC_IP();
            int ret_count = cando_vm_call_value(vm, iter, args, 2);
            cando_value_release(args[0]);
            cando_value_release(args[1]);
            vm->spread_extra = 0; /* prevent inner-call contamination */

            if (vm->has_error) goto handle_error;

            /* Lua-style termination: stop when the iterator returns nothing
             * or its first return value is null (the new control value).    */
            bool stop_loop = (ret_count == 0) ||
                             cando_is_null(*(vm->stack_top - ret_count));

            if (stop_loop) {
                /* Loop finished. Clean up protocol triplet and nvar. */
                for (int i = 0; i < ret_count; i++) cando_value_release(POP());
                cando_value_release(POP()); /* nvar */
                cando_value_release(POP()); /* control */
                cando_value_release(POP()); /* state */
                cando_value_release(POP()); /* iter */
                vm->spread_extra = 0;
                ip += off;
            } else {
                /* 1. Update control value in-place in the triplet. */
                CandoValue first_ret = (ret_count > 0) ? *(vm->stack_top - ret_count) : cando_null();
                cando_value_release(*(vm->stack_top - ret_count - 2));
                *(vm->stack_top - ret_count - 2) = cando_value_copy(first_ret);

                /* 2. Push nvar loop variables.
                 * Variables come from return values 2..N.
                 * Pad with null if fewer than nvar.
                 */
                CandoValue *ret_base = vm->stack_top - ret_count;
                CandoValue vars[16];

                for (u16 i = 0; i < nvar && i < 16; i++) {
                    if (i + 1 < (u16)ret_count) {
                        vars[i] = cando_value_copy(ret_base[i + 1]);
                    } else {
                        vars[i] = cando_null();
                    }
                }

                /* Release all return values. */
                for (int i = 0; i < ret_count; i++) cando_value_release(POP());

                /* Push the variables onto the stack for OP_DEF_LOCAL to pick up.
                 * We push them in FORWARD order (var1, var2, ..., varN) because
                 * the parser emits OP_DEF_LOCAL in REVERSE order (pop varN, ..., pop var1).
                 */
                for (int i = 0; i < (int)nvar && i < 16; i++) {
                    PUSH(vars[i]);
                }
                if (CANDO_UNLIKELY(vm->jit_enabled))
                    vm->jit_stats.iter_next_hits++;
            }
            DISPATCH();
        }
        /* ── Band 15: Pipe / filter ─────────────────────────────────── */
        OP_CASE(OP_PIPE_INIT): {
            /* Pop source value, create result array, expand source elements.
             * Pipe/filter (~> / ~!>) only operate on arrays; a non-array
             * source is a runtime error.
             * Stack after: [result_arr, v0..v(N-1), count=N, src_idx=0]   */
            u16 n = READ_U16(); CANDO_UNUSED(n);
            CandoValue src = POP();

            if (!cando_is_object(src)) {
                cando_value_release(src);
                vm_runtime_error(vm, "pipe/filter (~>/~!>) requires an array source");
                goto handle_error;
            }
            CdoObject *src_obj = cando_bridge_resolve(vm,
                                     (HandleIndex)cando_as_handle(src));
            if (src_obj->kind != OBJ_ARRAY) {
                cando_value_release(src);
                vm_runtime_error(vm, "pipe/filter (~>/~!>) requires an array source");
                goto handle_error;
            }

            /* Create empty result array; push it first (stays at bottom). */
            CandoValue arr_v = cando_bridge_new_array(vm);
            PUSH(arr_v);

            u32 elem_count = cdo_array_len(src_obj);
            for (u32 ai = 0; ai < elem_count; ai++) {
                CdoValue cv = cdo_null();
                cdo_array_rawget_idx(src_obj, ai, &cv);
                PUSH(cando_bridge_to_cando(vm, cv));
            }
            cando_value_release(src);

            PUSH(cando_number((f64)elem_count)); /* count */
            PUSH(cando_number(0.0));             /* src_idx */
            DISPATCH();
        }
        OP_CASE(OP_PIPE_NEXT): {
            /* Stack: [..., result_arr, v0..v(N-1), count, src_idx]
             * If src_idx >= count: jump forward A bytes (exit).
             * Else: push v[src_idx] for DEF_LOCAL pipe; increment src_idx. */
            i16 off = READ_I16();
            f64 src_index = cando_as_number(vm->stack_top[-1]);
            f64 count     = cando_as_number(vm->stack_top[-2]);
            if (src_index >= count) {
                ip += off;
            } else {
                cando_set_number(&vm->stack_top[-1], src_index + 1.0);
                CandoValue *val_ptr =
                    vm->stack_top - 2 - (i64)count + (i64)src_index;
                PUSH(cando_value_copy(*val_ptr));
                if (CANDO_UNLIKELY(vm->jit_enabled))
                    vm->jit_stats.iter_next_hits++;
            }
            DISPATCH();
        }
        OP_CASE(OP_FILTER_NEXT): {
            /* Identical to PIPE_NEXT; filter logic is in OP_FILTER_COLLECT. */
            i16 off = READ_I16();
            f64 src_index = cando_as_number(vm->stack_top[-1]);
            f64 count     = cando_as_number(vm->stack_top[-2]);
            if (src_index >= count) {
                ip += off;
            } else {
                cando_set_number(&vm->stack_top[-1], src_index + 1.0);
                CandoValue *val_ptr =
                    vm->stack_top - 2 - (i64)count + (i64)src_index;
                PUSH(cando_value_copy(*val_ptr));
                if (CANDO_UNLIKELY(vm->jit_enabled))
                    vm->jit_stats.iter_next_hits++;
            }
            DISPATCH();
        }
        OP_CASE(OP_PIPE_END): {
            /* Clean up pipe state; result_arr (already filled) stays on top.
             * Stack before: [..., result_arr, v0..v(N-1), count=N, src_idx=N]
             * Stack after:  [..., result_arr]                               */
            cando_value_release(POP());          /* pop src_idx (number) */
            CandoValue count_v = POP();          /* pop count */
            i64 n = (i64)cando_as_number(count_v);
            for (i64 vi = 0; vi < n; vi++)
                cando_value_release(POP());      /* pop source values */
            /* result_arr is now on top — leave it as the pipe result. */
            DISPATCH();
        }
        OP_CASE(OP_PIPE_COLLECT): {
            /* Body result is on top. Append it to result_arr.
             * After POP: stack_top[-1]=src_idx, stack_top[-2]=count=N
             * result_arr is at stack_top - 3 - N                          */
            CandoValue result = POP();
            f64 count_f = cando_as_number(vm->stack_top[-2]);
            i64 N = (i64)count_f;
            CandoValue *arr_ptr = vm->stack_top - 3 - N;
            CdoObject  *arr = cando_bridge_resolve(vm,
                                  (HandleIndex)cando_as_handle(*arr_ptr));
            CdoValue cdo_result = cando_bridge_to_cdo(vm, result);
            cdo_array_push(arr, cdo_result);   /* retains */
            cdo_value_release(cdo_result);
            cando_value_release(result);
            DISPATCH();
        }
        OP_CASE(OP_FILTER_COLLECT): {
            /* Like PIPE_COLLECT but null results are not appended. */
            CandoValue result = POP();
            if (!cando_is_null(result)) {
                f64 count_f = cando_as_number(vm->stack_top[-2]);
                i64 N = (i64)count_f;
                CandoValue *arr_ptr = vm->stack_top - 3 - N;
                CdoObject  *arr = cando_bridge_resolve(vm,
                                      (HandleIndex)cando_as_handle(*arr_ptr));
                CdoValue cdo_result = cando_bridge_to_cdo(vm, result);
                cdo_array_push(arr, cdo_result);   /* retains */
                cdo_value_release(cdo_result);
            }
            cando_value_release(result);
            DISPATCH();
        }
        OP_CASE(OP_COND_FILTER_COLLECT): {
            /* Conditional filter (~&>): body produced a predicate result.
             * If truthy, append the *original source element* (not the body
             * result) to result_arr.  src_idx was already incremented by
             * OP_FILTER_NEXT, so the iterated element sits at src_idx-1.   */
            CandoValue pred = POP();
            bool keep = vm_is_truthy(pred);
            cando_value_release(pred);
            if (keep) {
                f64 src_idx_f = cando_as_number(vm->stack_top[-1]);
                f64 count_f   = cando_as_number(vm->stack_top[-2]);
                i64 N   = (i64)count_f;
                i64 idx = (i64)src_idx_f - 1;
                CandoValue *val_ptr = vm->stack_top - 2 - N + idx;
                CandoValue *arr_ptr = vm->stack_top - 3 - N;
                CdoObject  *arr = cando_bridge_resolve(vm,
                                      (HandleIndex)cando_as_handle(*arr_ptr));
                CandoValue elem_copy = cando_value_copy(*val_ptr);
                CdoValue cdo_elem = cando_bridge_to_cdo(vm, elem_copy);
                cdo_array_push(arr, cdo_elem);   /* retains */
                cdo_value_release(cdo_elem);
                cando_value_release(elem_copy);
            }
            DISPATCH();
        }

        /* ── Band 14: Error handling ────────────────────────────────── */
        /* ── Band 16: Exceptions (try / catch / finally / throw) ────── */
        OP_CASE(OP_TRY_BEGIN): {
            i16 catch_off = READ_I16();
            CANDO_ASSERT_MSG(vm->try_depth < CANDO_TRY_MAX,
                             "try stack overflow");
            CandoTryFrame *tf = &vm->try_stack[vm->try_depth++];
            tf->catch_ip   = ip + catch_off;
            tf->finally_ip = NULL;
            tf->stack_save = (u32)(vm->stack_top - vm->stack);
            tf->frame_save = vm->frame_count;
            tf->loop_save  = vm->loop_depth;
            tf->if_save    = vm->if_depth;
            DISPATCH();
        }
        OP_CASE(OP_TRY_END): {
            if (vm->try_depth > 0) vm->try_depth--;
            DISPATCH();
        }
        OP_CASE(OP_CATCH_BEGIN): {
            /* A = number of catch parameters declared.
             * Push A values from error_vals[0..A-1], padding with null
             * for any missing args.  Push in reverse so that the first
             * arg ends up on top; successive OP_DEF_LOCAL pops bind
             * them in declaration order.                                */
            u16 n = READ_U16();
            for (u32 i = n; i-- > 0; ) {
                if (i < vm->error_val_count)
                    PUSH(cando_value_copy(vm->error_vals[i]));
                else
                    PUSH(cando_null());
            }
            /* Release error state — it has been copied to the stack.   */
            for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
                cando_value_release(vm->error_vals[i]);
                vm->error_vals[i] = cando_null();
            }
            vm->error_val_count = 0;
            DISPATCH();
        }
        OP_CASE(OP_FINALLY_BEGIN): {
            i16 off = READ_I16();
            if (vm->try_depth > 0) {
                vm->try_stack[vm->try_depth - 1].finally_ip = ip + off;
            }
            DISPATCH();
        }
        OP_CASE(OP_THROW): {
            u16 count = READ_U16();
            if (count == 0) count = 1;
            /* Release any previous error values. */
            for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
                cando_value_release(vm->error_vals[i]);
                vm->error_vals[i] = cando_null();
            }
            /* Pop values; stack order is [..., arg0, arg1, ..., argN-1]
             * so POP() gives argN-1 first — store in reverse.         */
            vm->error_val_count = count;
            for (u32 i = count; i-- > 0; )
                vm->error_vals[i] = POP();
            vm->has_error = true;
            /* Format error_msg from first thrown value, then append a
             * stack trace so an uncaught throw is debuggable.            */
            char *s = cando_value_tostring(vm->error_vals[0]);
            snprintf(vm->error_msg, sizeof(vm->error_msg), "%s", s);
            cando_free(s);
            SYNC_IP();
            vm_append_stack_trace(vm);
            goto handle_error;
        }
        OP_CASE(OP_RERAISE): {
            if (!vm->has_error) {
                vm_runtime_error(vm, "RERAISE outside of catch block");
            }
            goto handle_error;
        }

        /* ── Band 15: Threads ───────────────────────────────────────── */
        /* ── Band 17: Concurrency (async / await / yield / thread) ──── */
        OP_CASE(OP_ASYNC): {
            vm_runtime_error(vm, "ASYNC not implemented (use 'thread' instead)");
            goto handle_error;
        }
        OP_CASE(OP_AWAIT): {
            /* Pop the thread handle and block until it finishes. */
            CandoValue thread_val = POP();
            if (!cando_is_object(thread_val)) {
                cando_value_release(thread_val);
                vm_runtime_error(vm, "await: expected a thread handle");
                goto handle_error;
            }
            CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(thread_val));
            if (!obj || obj->kind != OBJ_THREAD) {
                cando_value_release(thread_val);
                vm_runtime_error(vm, "await: value is not a thread");
                goto handle_error;
            }
            CdoThread *t = (CdoThread *)obj;

            /* Block until done. */
            SYNC_IP();
            cdo_thread_wait(t);

            if (atomic_load(&t->state) == CDO_THREAD_ERROR) {
                /* The error is being delivered to user code via the
                 * propagating throw -- mark it as observed so the
                 * destructor doesn't also log it on stderr.            */
                cando_os_mutex_lock(&t->done_mutex);
                t->error_observed = true;
                cando_os_mutex_unlock(&t->done_mutex);

                /* Propagate error: push to error_vals and go to error handler. */
                cando_value_release(vm->error_vals[0]);
                vm->error_vals[0]   = cando_value_copy(t->error);
                vm->error_val_count = 1;
                vm->has_error       = true;
                /* Use the thread's actual error message when it is a string,
                 * so the surfaced error reads like the original throw. */
                if (cando_is_string(t->error) && cando_as_string(t->error)) {
                    snprintf(vm->error_msg, sizeof(vm->error_msg), "%.*s",
                             (int)cando_as_string(t->error)->length,
                             cando_as_string(t->error)->data);
                } else {
                    snprintf(vm->error_msg, sizeof(vm->error_msg),
                             "thread raised an error");
                }
                cando_value_release(thread_val);
                goto handle_error;
            }

            /* Push all return values onto the stack. */
            for (u32 i = 0; i < t->result_count; i++)
                cando_vm_push(vm, cando_value_copy(t->results[i]));

            /* Ensure at least one value so expression context is always ok. */
            if (t->result_count == 0)
                cando_vm_push(vm, cando_null());

            vm->last_ret_count = (int)(t->result_count > 0 ? t->result_count : 1);
            cando_value_release(thread_val);
            DISPATCH();
        }
        OP_CASE(OP_YIELD): {
            vm_runtime_error(vm, "YIELD not implemented (use 'thread' instead)");
            goto handle_error;
        }
        OP_CASE(OP_THREAD): {
            /* Pop the OBJ_FUNCTION closure value from stack. */
            CandoValue fn_val = POP();
            if (!cando_is_object(fn_val)) {
                cando_value_release(fn_val);
                vm_runtime_error(vm, "thread: expected a function");
                goto handle_error;
            }
            CdoObject *fn_obj = cando_bridge_resolve(vm, cando_as_handle(fn_val));
            if (!fn_obj || fn_obj->kind != OBJ_FUNCTION) {
                cando_value_release(fn_val);
                vm_runtime_error(vm, "thread: value is not a function");
                goto handle_error;
            }

            /* Create the CdoThread object. */
            CdoThread *t = cdo_thread_new(fn_val);

            /* Allocate a handle for it in the shared handle table. */
            HandleIndex h = cando_handle_alloc(vm->handles, (void *)t);
            CandoValue  thread_val = cando_object_value(h);

            /* Store the handle index on the thread for thread.current(). */
            t->handle_idx = h;

            /* Set up the thread argument for the trampoline. */
            ThreadArg *ta = (ThreadArg *)cando_alloc(sizeof(ThreadArg));
            ta->parent_vm         = vm;
            ta->fn_val            = cando_value_copy(fn_val);
            ta->thread            = t;
            ta->thread_handle_val = thread_val;

            /* Register thread in the registry before spawning. */
            if (vm->thread_registry) {
                cando_os_mutex_lock(&vm->thread_registry->mutex);
                vm->thread_registry->count++;
                cando_os_mutex_unlock(&vm->thread_registry->mutex);
            }

            /* Spawn the OS thread. */
            SYNC_IP();
            if (!cando_os_thread_create(&t->os_thread,
                                        (cando_thread_fn_t)vm_thread_trampoline,
                                        ta)) {
                /* Undo the registry increment on failure. */
                if (vm->thread_registry) {
                    cando_os_mutex_lock(&vm->thread_registry->mutex);
                    vm->thread_registry->count--;
                    cando_os_cond_broadcast(&vm->thread_registry->cond);
                    cando_os_mutex_unlock(&vm->thread_registry->mutex);
                }
                cando_value_release(fn_val);
                cando_value_release(ta->fn_val);
                cando_free(ta);
                /* Untrack so the upcoming destroy isn't followed by a
                 * second sweep at VM teardown.  cdo_thread_destroy now
                 * frees the struct itself, so no separate cando_free.   */
                cando_gc_untrack(t);
                cdo_thread_destroy(t);
                cando_handle_free(vm->handles, h);
                vm_runtime_error(vm, "thread: failed to create OS thread");
                goto handle_error;
            }
            /* Detach immediately — resources reclaimed when thread exits. */
            cando_os_thread_detach(t->os_thread);

            cando_value_release(fn_val);
            PUSH(thread_val);
            vm_gc_maybe_collect(vm);
            DISPATCH();
        }

        /* ── Band 18: OOP (classes, inheritance, method binding) ────── */
        OP_CASE(OP_NEW_CLASS): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));

            CandoValue cls_val = cando_bridge_new_object(vm);
            if (g_meta_type) {
                CdoObject  *cls     = cando_bridge_resolve(vm, cando_as_handle(cls_val));
                CdoString  *cdo_key = cando_bridge_intern_key(cando_as_string(name_val));
                CdoValue    tv      = cdo_string_value(cdo_string_retain(cdo_key));
                cdo_object_rawset(cls, g_meta_type, tv, FIELD_STATIC);
                cdo_value_release(tv);
                cdo_string_release(cdo_key);
            }
            PUSH(cls_val);
            vm_gc_maybe_collect(vm);
            DISPATCH();
        }
        OP_CASE(OP_BIND_METHOD): {
            /* Stack: [..., class_obj, method_val]
             * Pop method_val, rawset it on the class object (keep class on TOS). */
            u16 ci = READ_U16();
            CandoValue method_val = POP();
            CandoValue cls_val    = PEEK(0);
            if (!cando_is_object(cls_val)) {
                cando_value_release(method_val);
                vm_runtime_error(vm, "BIND_METHOD: expected class object");
                goto handle_error;
            }
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            CdoObject *cls     = cando_bridge_resolve(vm, cando_as_handle(cls_val));
            CdoString *cdo_key = cando_bridge_intern_key(cando_as_string(name_val));
            CdoValue   mv      = cando_bridge_to_cdo(vm, method_val);
            cdo_object_rawset(cls, cdo_key, mv, FIELD_NONE);
            cdo_value_release(mv);
            cdo_string_release(cdo_key);
            cando_value_release(method_val);
            DISPATCH();
        }
        OP_CASE(OP_INHERIT): {
            /* Stack: [..., parent_class, child_class]
             * Set child.__index = parent and leave only child on TOS. */
            CandoValue child_val  = POP();
            CandoValue parent_val = POP();
            if (!cando_is_object(child_val) || !cando_is_object(parent_val)) {
                cando_value_release(child_val);
                cando_value_release(parent_val);
                vm_runtime_error(vm, "INHERIT: expected class objects");
                goto handle_error;
            }
            if (g_meta_index) {
                CdoObject *child  = cando_bridge_resolve(vm, cando_as_handle(child_val));
                CdoValue   pv     = cando_bridge_to_cdo(vm, parent_val);
                cdo_object_rawset(child, g_meta_index, pv, FIELD_NONE);
                cdo_value_release(pv);
            }
            cando_value_release(parent_val);
            PUSH(child_val);
            DISPATCH();
        }
        OP_CASE(OP_BIND_DEFAULT_CALL): {
            /* Stack: [..., class_obj]
             * Set class.__call to the VM-wide default constructor wrapper.
             * Class stays on TOS so further bindings can proceed.          */
            CandoValue cls_val = PEEK(0);
            if (!cando_is_object(cls_val)) {
                vm_runtime_error(vm, "BIND_DEFAULT_CALL: expected class object");
                goto handle_error;
            }
            if (g_meta_call) {
                CdoObject *cls = cando_bridge_resolve(vm, cando_as_handle(cls_val));
                CdoValue   cv  = cando_bridge_to_cdo(vm, vm->default_class_call);
                cdo_object_rawset(cls, g_meta_call, cv, FIELD_NONE);
                cdo_value_release(cv);
            }
            DISPATCH();
        }

        /* ── Band 17: Mask / selector ───────────────────────────────── */
        /* ── Band 19: Multi-return masks & spread ───────────────────── */
        OP_CASE(OP_MASK_PASS): {
            /* No-op at the VM level; the compiler handles stack picking. */
            DISPATCH();
        }
        OP_CASE(OP_MASK_SKIP): {
            cando_value_release(POP());
            DISPATCH();
        }
        OP_CASE(OP_MASK_APPLY): {
            /* Apply a bitmask to the return values from the last function
             * call.  n_bits is the number of explicit mask positions; the
             * actual value count comes from last_ret_count so mismatched
             * arities never read garbage off the stack.
             *   bit i=1 → keep,  bit i=0 → skip.
             *   Values at positions >= n_bits are always skipped.
             * last_ret_count is updated to the kept count so a following
             * OP_SPREAD_RET propagates the real number to OP_CALL.       */
            u16 n_bits  = READ_U16();
            u16 bitmask = READ_U16();
            int count   = vm->last_ret_count;
            CandoValue *base = vm->stack_top - count;
            int out = 0;
            for (int i = 0; i < count; i++) {
                bool keep = (i < (int)n_bits) && ((bitmask >> i) & 1);
                if (keep) {
                    base[out++] = base[i]; /* keep: compact in place */
                } else {
                    cando_value_release(base[i]); /* skip: release */
                }
            }
            vm->stack_top = base + out;
            vm->last_ret_count = out;
            DISPATCH();
        }

        /* ── Band 18: Multi-return spreading ────────────────────────── */
        OP_CASE(OP_SPREAD_RET): {
            vm->spread_extra += vm->last_ret_count - 1;
            DISPATCH();
        }
        OP_CASE(OP_ARRAY_SPREAD): {
            /* Like OP_SPREAD_RET but accumulates into array_extra, which
             * is consumed by OP_NEW_ARRAY.  This keeps array spread counts
             * separate from function-call spread counts so OP_CALL inside
             * an array literal does not consume the array's extra count.  */
            vm->array_extra += vm->last_ret_count - 1;
            DISPATCH();
        }

        /* ── Band 19: Call-result comparison ────────────────────────── */
        OP_CASE(OP_TRUNCATE_RET): {
            /* Pop all but the first return value from the last call.
             * Stack before: [..., ret0, ret1, ..., retN-1]  (N = last_ret_count)
             * Stack after:  [..., ret0]                                    */
            int extra = vm->last_ret_count - 1;
            for (int i = 0; i < extra; i++) cando_value_release(POP());
            vm->last_ret_count = 1;
            DISPATCH();
        }
        OP_CASE(OP_EQ_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = false;
            for (int i = 0; i < n; i++) {
                if (cando_value_equal(left, *(vm->stack_top - n + i))) {
                    result = true; break;
                }
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_NEQ_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (int i = 0; i < n; i++) {
                if (cando_value_equal(left, *(vm->stack_top - n + i))) {
                    result = false; break;
                }
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_LT_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (int i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) < cando_as_number(r))) result = false;
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_GT_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (int i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) > cando_as_number(r))) result = false;
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_LEQ_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (int i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) <= cando_as_number(r))) result = false;
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }
        OP_CASE(OP_GEQ_SPREAD): {
            int n = vm->last_ret_count;
            CandoValue left = *(vm->stack_top - n - 1);
            bool result = true;
            for (int i = 0; i < n && result; i++) {
                CandoValue r = *(vm->stack_top - n + i);
                if (!cando_is_number(left) || !cando_is_number(r))
                    result = false;
                else if (!(cando_as_number(left) >= cando_as_number(r))) result = false;
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }

        /* ── Sentinels ──────────────────────────────────────────────── */
        /* ── Band 20: Misc (NOP, HALT) ──────────────────────────────── */
        OP_CASE(OP_NOP): {
            DISPATCH();
        }
        OP_CASE(OP_HALT): {
            return VM_HALT;
        }

    } /* end INTERPRET_LOOP */

    /* ── Error unwinding ────────────────────────────────────────────── */
handle_error:
    /* Walk the try stack looking for a handler in the current execution
     * context.                                                           */
    while (vm->try_depth > 0) {
        CandoTryFrame *tf = &vm->try_stack[vm->try_depth - 1];

        /* Unwind value stack and frame stack to handler's save point.   */
        while (vm->frame_count > tf->frame_save) {
            CandoCallFrame *f = &vm->frames[vm->frame_count - 1];
            vm_close_upvalues(vm, f->slots);
            vm->frame_count--;
        }
        while ((u32)(vm->stack_top - vm->stack) > tf->stack_save) {
            cando_value_release(POP());
        }
        vm->loop_depth = tf->loop_save;
        vm->if_depth   = tf->if_save;
        vm->try_depth--;

        if (tf->catch_ip) {
            /* Jump to the catch block.  OP_CATCH_BEGIN will push the
             * error values from vm->error_vals[] onto the stack.       */
            vm->has_error = false;
            LOAD_FRAME();
            ip = tf->catch_ip;
            DISPATCH();
        }
        /* No catch: check for finally. */
        if (tf->finally_ip) {
            LOAD_FRAME();
            ip = tf->finally_ip;
            DISPATCH();
        }
    }

    /* No handler found — propagate as a VM error. */
    return VM_RUNTIME_ERR;
}
