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
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#if !defined(_WIN32) && !defined(_WIN64)
#  include <dlfcn.h>   /* dlclose() for binary module cleanup */
#endif

/* =========================================================================
 * Thread-local: current CdoThread for the running OS thread (NULL = main)
 * ===================================================================== */
_Thread_local static CdoThread *tl_current_thread = NULL;

CdoThread *cando_current_thread(void) {
    return tl_current_thread;
}

void cando_vm_wait_all_threads(CandoVM *vm) {
    CandoThreadRegistry *reg = vm->thread_registry;
    if (!reg) return;
    cando_os_mutex_lock(&reg->mutex);
    while (reg->count > 0)
        cando_os_cond_wait(&reg->cond, &reg->mutex);
    cando_os_mutex_unlock(&reg->mutex);
}

/* =========================================================================
 * Internal forward declarations
 * ===================================================================== */
static CandoVMResult  vm_run(CandoVM *vm);
static void           vm_runtime_error(CandoVM *vm, const char *fmt, ...);
static bool           vm_call(CandoVM *vm, CandoClosure *closure, u32 arg_count);
static CandoUpvalue  *vm_capture_upvalue(CandoVM *vm, CandoValue *local);
static void           vm_close_upvalues(CandoVM *vm, CandoValue *last);
static bool           vm_is_truthy(CandoValue v);
static bool           vm_is_truthy_meta(CandoVM *vm, CandoValue v, bool *ok);
static u32            vm_global_hash(const char *str, u32 len);
static void           vm_call_closure_with_args(CandoVM *vm, CdoObject *fn_obj,
                                                 CandoValue *args, u32 argc);

/* =========================================================================
 * VM lifecycle
 * ===================================================================== */

void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem) {
    vm->stack_top      = vm->stack;
    vm->frame_count    = 0;
    vm->try_depth      = 0;
    vm->loop_depth     = 0;
    vm->open_upvalues  = NULL;
    vm->native_count   = 0;
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

    /* Module cache — initially empty. */
    vm->module_cache       = NULL;
    vm->module_cache_count = 0;
    vm->module_cache_cap   = 0;

    /* Thread registry — root VM owns it. */
    vm->thread_registry_owned = (CandoThreadRegistry *)cando_alloc(sizeof(CandoThreadRegistry));
    cando_os_mutex_init(&vm->thread_registry_owned->mutex);
    cando_os_cond_init(&vm->thread_registry_owned->cond);
    vm->thread_registry_owned->count = 0;
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
    vm->globals_owned->entries  = (CandoGlobalEntry *)cando_alloc(
                                       64 * sizeof(CandoGlobalEntry));
    memset(vm->globals_owned->entries, 0, 64 * sizeof(CandoGlobalEntry));
    vm->globals = vm->globals_owned;
}

void cando_vm_init_child(CandoVM *child, const CandoVM *parent) {
    /* Start with a clean state. */
    child->stack_top      = child->stack;
    child->frame_count    = 0;
    child->try_depth      = 0;
    child->loop_depth     = 0;
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

    /* Copy native function registry (read-only after init; safe to share). */
    child->native_count = parent->native_count;
    for (u32 i = 0; i < parent->native_count; i++)
        child->native_fns[i] = parent->native_fns[i];

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

    /* Release open upvalues. */
    CandoUpvalue *uv = vm->open_upvalues;
    while (uv) {
        CandoUpvalue *next = uv->next;
        cando_free(uv);
        uv = next;
    }

    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) cando_value_release(vm->error_vals[i]);

    /* Release eval results. */
    for (u32 i = 0; i < vm->eval_result_count; i++) {
        cando_value_release(vm->eval_results[i]);
    }
    cando_free(vm->eval_results);

    /* Release module cache. */
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
            FreeLibrary((HMODULE)e->dl_handle);
#else
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
    /* Upvalue objects are managed by the VM; we only free the pointer array. */
    cando_free(closure->upvalues);
    cando_free(closure);
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

bool cando_vm_register_native(CandoVM *vm, const char *name,
                               CandoNativeFn fn) {
    if (vm->native_count >= CANDO_NATIVE_MAX) return false;
    u32 idx = vm->native_count++;
    vm->native_fns[idx] = fn;
    /* Sentinel value: native #idx → -(idx+1) */
    CandoValue sentinel = cando_number(-(f64)(idx + 1));
    cando_vm_set_global(vm, name, sentinel, true);
    return true;
}

CandoValue cando_vm_add_native(CandoVM *vm, CandoNativeFn fn) {
    if (vm->native_count >= CANDO_NATIVE_MAX) return cando_null();
    u32 idx = vm->native_count++;
    vm->native_fns[idx] = fn;
    return cando_number(-(f64)(idx + 1));
}

/* =========================================================================
 * Upvalue management
 * ===================================================================== */

/* vm_capture_upvalue -- used by OP_CLOSURE (full closure support pending). */
static CandoUpvalue *vm_capture_upvalue(CandoVM *vm, CandoValue *local)
    __attribute__((unused));
static CandoUpvalue *vm_capture_upvalue(CandoVM *vm, CandoValue *local) {
    /* Walk the open list looking for an upvalue already pointing here. */
    CandoUpvalue *prev = NULL;
    CandoUpvalue *cur  = vm->open_upvalues;
    while (cur && cur->location > local) {
        prev = cur;
        cur  = cur->next;
    }
    if (cur && cur->location == local) return cur;

    CandoUpvalue *uv = (CandoUpvalue *)cando_alloc(sizeof(CandoUpvalue));
    cando_lock_init(&uv->lock);
    uv->location     = local;
    uv->closed       = cando_null();
    uv->next         = cur;
    if (prev) prev->next       = uv;
    else      vm->open_upvalues = uv;
    return uv;
}

static void vm_close_upvalues(CandoVM *vm, CandoValue *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        CandoUpvalue *uv = vm->open_upvalues;
        uv->closed       = *uv->location;
        uv->location     = &uv->closed;
        vm->open_upvalues = uv->next;
    }
}

/* =========================================================================
 * Meta-method dispatch helper
 * ===================================================================== */

bool cando_vm_call_meta(CandoVM *vm, HandleIndex h,
                         struct CdoString *meta_key,
                         CandoValue *args, u32 argc) {
    CdoObject *obj = cando_bridge_resolve(vm, h);
    if (!obj || !meta_key) return false;

    CdoValue raw;
    if (!cdo_object_get(obj, (CdoString *)meta_key, &raw))
        return false;

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
        u32 pc = (u32)callee.as.number;
        CandoCallFrame *cur_frame = &vm->frames[vm->frame_count - 1];

        u32 saved_stop = vm->thread_stop_frame;
        vm->thread_stop_frame   = vm->frame_count;
        vm->thread_result_count = 0;
        for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) {
            cando_value_release(vm->thread_results[i]);
            vm->thread_results[i] = cando_null();
        }

        cando_vm_push(vm, cando_null()); /* slot-0 sentinel */

        if (vm->frame_count >= CANDO_FRAMES_MAX) {
            cando_vm_pop(vm);
            vm->thread_stop_frame = saved_stop;
            vm_runtime_error(vm, "call stack overflow in meta-method");
            return false;
        }

        CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->closure   = cur_frame->closure;
        new_frame->ip        = cur_frame->closure->chunk->code + pc;
        new_frame->slots     = vm->stack_top - 1;
        new_frame->ret_count = 0;
        new_frame->is_fluent = false;

        for (u32 i = 0; i < argc; i++)
            cando_vm_push(vm, cando_value_copy(args[i]));
        u32 np = argc + 1;
        if (cur_frame->closure->chunk->local_count > np) {
            for (u32 i = np; i < cur_frame->closure->chunk->local_count; i++)
                cando_vm_push(vm, cando_null());
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
        CdoObject *fn_obj = cando_bridge_resolve(vm, callee.as.handle);

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

/* =========================================================================
 * Truthiness (NULL and FALSE are falsy; everything else is truthy)
 * ===================================================================== */

static bool vm_is_truthy(CandoValue v) {
    if (cando_is_null(v))  return false;
    if (cando_is_bool(v))  return v.as.boolean;
    return true;
}

static bool vm_is_truthy_meta(CandoVM *vm, CandoValue v, bool *ok) {
    CANDO_UNUSED(vm);
    *ok = true;
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

/* vm_runtime_error -- internal; like cando_vm_error but appends the
 * current source file + line number from the active call frame.          */
static void vm_runtime_error(CandoVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);

    /* Attach source location from current frame. */
    if (vm->frame_count > 0) {
        CandoCallFrame *frame = &vm->frames[vm->frame_count - 1];
        CandoChunk     *chunk = frame->closure->chunk;
        u32 offset = (u32)(frame->ip - chunk->code - 1); /* -1: already advanced */
        if (offset < chunk->code_len) {
            u32 line = chunk->lines[offset];
            char loc[128];
            snprintf(loc, sizeof(loc), " [%s line %u]", chunk->name, line);
            strncat(vm->error_msg, loc,
                    sizeof(vm->error_msg) - strlen(vm->error_msg) - 1);
        }
    }

    vm_error_commit(vm);
}

/* =========================================================================
 * Function call
 * ===================================================================== */

static bool vm_call(CandoVM *vm, CandoClosure *closure, u32 arg_count) {
    CandoChunk *chunk = closure->chunk;

    if (!chunk->has_vararg && arg_count != chunk->arity) {
        vm_runtime_error(vm,
            "function '%s' expects %u argument(s) but got %u",
            chunk->name, chunk->arity, arg_count);
        return false;
    }

    if (vm->frame_count >= CANDO_FRAMES_MAX) {
        vm_runtime_error(vm, "call stack overflow");
        return false;
    }

    CandoCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure   = closure;
    frame->ip        = chunk->code;
    /* The function itself is just below the arguments on the stack. */
    frame->slots     = vm->stack_top - arg_count - 1;
    frame->ret_count = 0;
    frame->is_fluent = false;

    /* Pre-allocate null slots for all local variables so the expression
     * evaluation stack always sits above the local variable area.
     * Without this, pushes during expression evaluation overwrite locals.
     * n_present = slot 0 (fn sentinel) + all arguments already on stack.
     * We push (local_count - n_present) nulls to fill the gap.           */
    u32 n_present = arg_count + 1; /* sentinel + args */
    if (chunk->local_count > n_present) {
        u32 n_extra = chunk->local_count - n_present;
        for (u32 i = 0; i < n_extra; i++)
            cando_vm_push(vm, cando_null());
    }
    return true;
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

#define PUSH(v)        cando_vm_push(vm, (v))
#define POP()          cando_vm_pop(vm)
#define PEEK(d)        cando_vm_peek(vm, (d))

/* Numerical binary operation macro. */
#define BINARY_NUM_OP(op_sym)  do {                                      \
    CandoValue _b = POP(), _a = POP();                                   \
    if (CANDO_UNLIKELY(!cando_is_number(_a) || !cando_is_number(_b))) {  \
        vm_runtime_error(vm, "operands must be numbers (got %s and %s)", \
            cando_value_type_name((TypeTag)_a.tag),                      \
            cando_value_type_name((TypeTag)_b.tag));                     \
        goto handle_error;                                               \
    }                                                                    \
    PUSH(cando_number(_a.as.number op_sym _b.as.number));                \
} while (0)

/* Comparison operator (returns bool). */
#define CMP_OP(op_sym)  do {                                             \
    CandoValue _b = POP(), _a = POP();                                   \
    if (!cando_is_number(_a) || !cando_is_number(_b)) {                  \
        vm_runtime_error(vm, "comparison requires numbers");             \
        goto handle_error;                                               \
    }                                                                    \
    PUSH(cando_bool(_a.as.number op_sym _b.as.number));                  \
} while (0)

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

    /* Push slot-0 sentinel and create a call frame at fn_pc. */
    cando_vm_push(vm, cando_null());
    if (vm->frame_count >= CANDO_FRAMES_MAX) {
        vm_runtime_error(vm, "call stack overflow in thread");
        vm->thread_stop_frame   = saved_stop;
        vm->thread_result_count = saved_count;
        cando_vm_pop(vm);
        return VM_RUNTIME_ERR;
    }
    CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->closure   = closure;
    new_frame->ip        = closure->chunk->code + fn_pc;
    new_frame->slots     = vm->stack_top - 1; /* just the sentinel */
    new_frame->ret_count = 0;
    new_frame->is_fluent = false;

    /* Pre-allocate null locals. */
    if (closure->chunk->local_count > 1) {
        for (u32 i = 1; i < closure->chunk->local_count; i++)
            cando_vm_push(vm, cando_null());
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

    /* Push slot-0 sentinel. */
    cando_vm_push(vm, cando_null());
    if (vm->frame_count >= CANDO_FRAMES_MAX) {
        cando_vm_pop(vm);
        vm->thread_stop_frame = saved_stop;
        return;
    }

    CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->closure   = closure;
    new_frame->ip        = closure->chunk->code + fn_pc;
    new_frame->slots     = vm->stack_top - 1;   /* points at slot-0 */
    new_frame->ret_count = 0;
    new_frame->is_fluent = false;

    /* Push the passed arguments as parameter slots, then nulls for any
     * remaining local variable slots declared by the main chunk.
     * We use argc (not closure->chunk->arity which is the top-level script's
     * arity=0) because all nested functions share the flat chunk. */
    for (u32 i = 0; i < argc; i++)
        cando_vm_push(vm, cando_value_copy(args[i]));
    if (closure->chunk->local_count > 1 + argc) {
        for (u32 i = 1 + argc; i < closure->chunk->local_count; i++)
            cando_vm_push(vm, cando_null());
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
    CdoObject *fn_obj = cando_bridge_resolve(vm, fn_val.as.handle);
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
        CdoObject *fn_obj = cando_bridge_resolve(&child, ta->fn_val.as.handle);
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

        cando_os_mutex_lock(&ta->thread->done_mutex);
        if (final_state == CDO_THREAD_DONE && cando_is_object(ta->thread->then_fn)) {
            cb = cando_value_copy(ta->thread->then_fn);
        } else if (final_state == CDO_THREAD_ERROR &&
                   cando_is_object(ta->thread->catch_fn)) {
            cb = cando_value_copy(ta->thread->catch_fn);
        }
        cando_os_mutex_unlock(&ta->thread->done_mutex);

        if (cando_is_object(cb)) {
            CdoObject *cb_obj = cando_bridge_resolve(&child, cb.as.handle);
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
            }
            cando_value_release(cb);
        }
    }

cleanup:
    tl_current_thread = NULL;
    cando_vm_destroy(&child);
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
#ifdef __GNUC__
#   define DISPATCH()        goto *dispatch_table[READ_BYTE()]
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
        [OP_LOOP]             = &&lbl_OP_LOOP,
        [OP_BREAK]            = &&lbl_OP_BREAK,
        [OP_CONTINUE]         = &&lbl_OP_CONTINUE,
        [OP_LOOP_MARK]        = &&lbl_OP_LOOP_MARK,
        [OP_LOOP_END]         = &&lbl_OP_LOOP_END,
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
        [OP_FOR_OVER_INIT]    = &&lbl_OP_FOR_OVER_INIT,
        [OP_FOR_OVER_NEXT]    = &&lbl_OP_FOR_OVER_NEXT,
        [OP_PIPE_INIT]        = &&lbl_OP_PIPE_INIT,
        [OP_PIPE_NEXT]        = &&lbl_OP_PIPE_NEXT,
        [OP_FILTER_NEXT]      = &&lbl_OP_FILTER_NEXT,
        [OP_PIPE_END]         = &&lbl_OP_PIPE_END,
        [OP_PIPE_COLLECT]     = &&lbl_OP_PIPE_COLLECT,
        [OP_FILTER_COLLECT]   = &&lbl_OP_FILTER_COLLECT,
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
        [OP_NEW_CLASS]        = &&lbl_OP_NEW_CLASS,
        [OP_BIND_METHOD]      = &&lbl_OP_BIND_METHOD,
        [OP_INHERIT]          = &&lbl_OP_INHERIT,
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
#   define DISPATCH()        switch (READ_BYTE())
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
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            CandoValue out;
            cando_lock_read_acquire(&vm->globals->lock);
            bool found = vm_get_global_str(vm, name_val.as.string, &out);
            CandoValue out_copy = found ? cando_value_copy(out) : cando_null();
            cando_lock_read_release(&vm->globals->lock);
            if (!found) {
                vm_runtime_error(vm, "undefined variable '%s'",
                                 name_val.as.string->data);
                goto handle_error;
            }
            PUSH(out_copy);
            DISPATCH();
        }
        OP_CASE(OP_STORE_GLOBAL): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            CandoValue val = PEEK(0);
            cando_lock_write_acquire(&vm->globals->lock);
            bool ok = vm_set_global_str(vm, name_val.as.string,
                                        cando_value_copy(val), false);
            cando_lock_write_release(&vm->globals->lock);
            if (!ok) {
                vm_runtime_error(vm, "cannot assign to constant '%s'",
                                 name_val.as.string->data);
                goto handle_error;
            }
            DISPATCH();
        }
        OP_CASE(OP_DEF_GLOBAL): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            cando_lock_write_acquire(&vm->globals->lock);
            vm_set_global_str(vm, name_val.as.string, POP(), false);
            cando_lock_write_release(&vm->globals->lock);
            vm->spread_extra = 0;
            DISPATCH();
        }
        OP_CASE(OP_DEF_CONST_GLOBAL): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));
            cando_lock_write_acquire(&vm->globals->lock);
            vm_set_global_str(vm, name_val.as.string, POP(), true);
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
        HandleIndex _h = cando_is_object(_a) ? (_a).as.handle               \
                                              : (_b).as.handle;             \
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
            /* String concatenation or numeric addition. */
            CandoValue b = PEEK(0), a = PEEK(1);
            if (cando_is_string(a) && cando_is_string(b)) {
                POP(); POP();
                u32 la = a.as.string->length, lb = b.as.string->length;
                u32 total = la + lb;
                char *buf = cando_alloc(total + 1);
                memcpy(buf,      a.as.string->data, la);
                memcpy(buf + la, b.as.string->data, lb);
                buf[total] = '\0';
                CandoString *s = cando_string_new(buf, total);
                cando_free(buf);
                cando_value_release(a);
                cando_value_release(b);
                PUSH(cando_string_value(s));
            } else {
                CandoValue _b2 = POP(), _a2 = POP();
                TRY_BINARY_META(g_meta_add, _a2, _b2);
                if (CANDO_UNLIKELY(!cando_is_number(_a2) || !cando_is_number(_b2))) {
                    cando_value_release(_a2); cando_value_release(_b2);
                    vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                        cando_value_type_name((TypeTag)_a2.tag),
                        cando_value_type_name((TypeTag)_b2.tag));
                    goto handle_error;
                }
                PUSH(cando_number(_a2.as.number + _b2.as.number));
            }
            DISPATCH();
        }
        OP_CASE(OP_SUB): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_sub, a, b);
            if (CANDO_UNLIKELY(!cando_is_number(a) || !cando_is_number(b))) {
                vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                    cando_value_type_name((TypeTag)a.tag),
                    cando_value_type_name((TypeTag)b.tag));
                goto handle_error;
            }
            PUSH(cando_number(a.as.number - b.as.number));
            DISPATCH();
        }
        OP_CASE(OP_MUL): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_mul, a, b);
            if (CANDO_UNLIKELY(!cando_is_number(a) || !cando_is_number(b))) {
                vm_runtime_error(vm, "operands must be numbers (got %s and %s)",
                    cando_value_type_name((TypeTag)a.tag),
                    cando_value_type_name((TypeTag)b.tag));
                goto handle_error;
            }
            PUSH(cando_number(a.as.number * b.as.number));
            DISPATCH();
        }
        OP_CASE(OP_DIV): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_div, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            if (b.as.number == 0.0) {
                vm_runtime_error(vm, "division by zero");
                goto handle_error;
            }
            PUSH(cando_number(a.as.number / b.as.number));
            DISPATCH();
        }
        OP_CASE(OP_MOD): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_mod, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(fmod(a.as.number, b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_POW): {
            CandoValue b = POP(), a = POP();
            TRY_BINARY_META(g_meta_pow, a, b);
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(pow(a.as.number, b.as.number)));
            DISPATCH();
        }
#undef TRY_BINARY_META
        OP_CASE(OP_NEG): {
            CandoValue a = POP();
            if (cando_is_object(a) && g_meta_unm) {
                if (cando_vm_call_meta(vm, a.as.handle,
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
            PUSH(cando_number(-a.as.number));
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
            vm->stack_top[-1].as.number += 1.0;
            DISPATCH();
        }
        OP_CASE(OP_DECR): {
            if (!cando_is_number(PEEK(0))) {
                vm_runtime_error(vm, "'--' requires a number");
                goto handle_error;
            }
            vm->stack_top[-1].as.number -= 1.0;
            DISPATCH();
        }

        /* ── Band 6: Comparison ─────────────────────────────────────── */
        OP_CASE(OP_EQ): {
            CandoValue b = POP(), a = POP();
            if (g_meta_eq && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_eq,
                                       buf, 2)) {
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            PUSH(cando_bool(cando_value_equal(a, b)));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_NEQ): {
            CandoValue b = POP(), a = POP();
            if (g_meta_eq && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_eq,
                                       buf, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    PUSH(cando_bool(!vm_is_truthy(r)));
                    cando_value_release(r);
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            PUSH(cando_bool(!cando_value_equal(a, b)));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_LT): {
            CandoValue b = POP(), a = POP();
            if (g_meta_lt && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_lt,
                                       buf, 2)) {
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            if (!cando_is_number(a) || !cando_is_number(b)) {
                cando_value_release(a); cando_value_release(b);
                vm_runtime_error(vm, "comparison requires numbers");
                goto handle_error;
            }
            PUSH(cando_bool(a.as.number < b.as.number));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_GT): {
            /* a > b is implemented as __lt(b, a) — swap the arguments. */
            CandoValue b = POP(), a = POP();
            if (g_meta_lt && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {b, a}; /* swapped for __lt */
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_lt,
                                       buf, 2)) {
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            if (!cando_is_number(a) || !cando_is_number(b)) {
                cando_value_release(a); cando_value_release(b);
                vm_runtime_error(vm, "comparison requires numbers");
                goto handle_error;
            }
            PUSH(cando_bool(a.as.number > b.as.number));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_LEQ): {
            CandoValue b = POP(), a = POP();
            if (g_meta_le && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_le,
                                       buf, 2)) {
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            if (!cando_is_number(a) || !cando_is_number(b)) {
                cando_value_release(a); cando_value_release(b);
                vm_runtime_error(vm, "comparison requires numbers");
                goto handle_error;
            }
            PUSH(cando_bool(a.as.number <= b.as.number));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }
        OP_CASE(OP_GEQ): {
            /* a >= b is implemented as __le(b, a) — swap the arguments. */
            CandoValue b = POP(), a = POP();
            if (g_meta_le && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue buf[2] = {b, a}; /* swapped for __le */
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_le,
                                       buf, 2)) {
                    cando_value_release(a); cando_value_release(b);
                    DISPATCH();
                }
                if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
            }
            if (!cando_is_number(a) || !cando_is_number(b)) {
                cando_value_release(a); cando_value_release(b);
                vm_runtime_error(vm, "comparison requires numbers");
                goto handle_error;
            }
            PUSH(cando_bool(a.as.number >= b.as.number));
            cando_value_release(a); cando_value_release(b);
            DISPATCH();
        }

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
                else if (!(left.as.number < r.as.number)) result = false;
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
                else if (!(left.as.number > r.as.number)) result = false;
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
                else if (!(left.as.number <= r.as.number)) result = false;
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
                else if (!(left.as.number >= r.as.number)) result = false;
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
            f64 mn = vmin.as.number, v = vval.as.number, mx = vmax.as.number;
            bool ok = (left_inc  ? (mn <= v) : (mn < v)) &&
                      (right_inc ? (v <= mx) : (v < mx));
            PUSH(cando_bool(ok));
            DISPATCH();
        }

        /* ── Band 7: Bitwise ────────────────────────────────────────── */
        OP_CASE(OP_BIT_AND): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)a.as.number & (i64)b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_BIT_OR): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)a.as.number | (i64)b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_BIT_XOR): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)a.as.number ^ (i64)b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_BIT_NOT): {
            CandoValue a = POP();
            PUSH(cando_number((f64)(~(i64)a.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_LSHIFT): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)a.as.number << (i64)b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_RSHIFT): {
            CandoValue b = POP(), a = POP();
            PUSH(cando_number((f64)((i64)a.as.number >> (i64)b.as.number)));
            DISPATCH();
        }

        /* ── Band 8: Logical ────────────────────────────────────────── */
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
        OP_CASE(OP_NEW_OBJECT): {
            PUSH(cando_bridge_new_object(vm));
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
                CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);
                /* Items are on the stack: stack_top-n .. stack_top-1 */
                CandoValue *base = vm->stack_top - n;
                for (u32 i = 0; i < n; i++) {
                    CdoValue item = cando_bridge_to_cdo(vm, base[i]);
                    cdo_array_push(arr, item);
                    cando_value_release(base[i]);
                }
                vm->stack_top -= n;
            }
            PUSH(arr_val);
            DISPATCH();
        }
        OP_CASE(OP_GET_FIELD): {
            u16 ci = READ_U16();
            CandoValue obj_val = POP();

            /* String field access: look up in string prototype. */
            if (cando_is_string(obj_val)) {
                if (cando_is_object(vm->string_proto)) {
                    CdoObject   *proto = cando_bridge_resolve(
                                            vm, vm->string_proto.as.handle);
                    CandoString *ks    = frame->closure->chunk->constants[ci].as.string;
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
                                 cando_value_type_name((TypeTag)obj_val.tag));
                goto handle_error;
            }
            CdoObject  *obj = cando_bridge_resolve(vm, obj_val.as.handle);
            CandoString *ks = frame->closure->chunk->constants[ci].as.string;
            CdoString   *key = cando_bridge_intern_key(ks);
            CdoValue     result;
            /* Use cdo_object_get for __index prototype-chain traversal. */
            if (cdo_object_get(obj, key, &result)) {
                PUSH(cando_bridge_to_cando(vm, result));
            } else {
                PUSH(cando_null());
            }
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
            CdoObject  *obj = cando_bridge_resolve(vm, obj_val.as.handle);
            CandoString *ks = frame->closure->chunk->constants[ci].as.string;
            CdoString   *key = cando_bridge_intern_key(ks);
            CdoValue     existing;
            if (!cdo_object_rawget(obj, key, &existing) && g_meta_newindex) {
                CandoValue key_cv = cando_string_value(
                    cando_string_new(key->data, key->length));
                CandoValue args[3] = { obj_val, key_cv, val };
                if (cando_vm_call_meta(vm, obj_val.as.handle,
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
            cdo_object_rawset(obj, key, cdo_val, FIELD_NONE);
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
            CdoObject *obj = cando_bridge_resolve(vm, obj_val.as.handle);
            if (cando_is_number(idx_val)) {
                u32      idx = (u32)idx_val.as.number;
                CdoValue result;
                if (cdo_array_rawget_idx(obj, idx, &result)) {
                    PUSH(cando_bridge_to_cando(vm, result));
                } else {
                    PUSH(cando_null());
                }
            } else if (cando_is_string(idx_val)) {
                CdoString *key = cando_bridge_intern_key(idx_val.as.string);
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
            CdoObject *obj     = cando_bridge_resolve(vm, obj_val.as.handle);
            CdoValue   cdo_val = cando_bridge_to_cdo(vm, val);
            if (cando_is_number(idx_val)) {
                u32 idx = (u32)idx_val.as.number;
                cdo_array_rawset_idx(obj, idx, cdo_val);
            } else if (cando_is_string(idx_val)) {
                CdoString *key = cando_bridge_intern_key(idx_val.as.string);
                CdoValue   existing;
                if (!cdo_object_rawget(obj, key, &existing) && g_meta_newindex) {
                    CandoValue args[3] = { obj_val, idx_val, val };
                    if (cando_vm_call_meta(vm, obj_val.as.handle,
                                           (struct CdoString *)g_meta_newindex,
                                           args, 3)) {
                        cdo_string_release(key);
                        cando_value_release(val);
                        cando_value_release(idx_val);
                        DISPATCH();
                    }
                    if (vm->has_error) {
                        cdo_string_release(key);
                        cando_value_release(val);
                        cando_value_release(idx_val);
                        goto handle_error;
                    }
                }
                cdo_object_rawset(obj, key, cdo_val, FIELD_NONE);
                cdo_string_release(key);
            } else {
                cando_value_release(val);
                cando_value_release(idx_val);
                vm_runtime_error(vm, "index must be a number or string");
                goto handle_error;
            }
            cando_value_release(val);
            cando_value_release(idx_val);
            DISPATCH();
        }
        OP_CASE(OP_LEN): {
            CandoValue a = POP();
            if (cando_is_string(a)) {
                u32 len = a.as.string->length;
                cando_value_release(a);
                PUSH(cando_number((f64)len));
            } else if (cando_is_object(a)) {
                /* Check __len meta-method first. */
                if (g_meta_len) {
                    CandoValue a_copy = cando_value_copy(a);
                    if (cando_vm_call_meta(vm, a.as.handle,
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
                CdoObject *obj = cando_bridge_resolve(vm, a.as.handle);
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
            CdoObject  *obj     = cando_bridge_resolve(vm, obj_val.as.handle);
            CandoValue  arr_val = cando_bridge_new_array(vm);
            CdoObject  *arr     = cando_bridge_resolve(vm, arr_val.as.handle);
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
            CdoObject  *obj     = cando_bridge_resolve(vm, obj_val.as.handle);
            CandoValue  arr_val = cando_bridge_new_array(vm);
            CdoObject  *arr     = cando_bridge_resolve(vm, arr_val.as.handle);
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
        OP_CASE(OP_LOOP): {
            u16 back = READ_U16();
            ip -= back;
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
             * FOR IN/OF:   [..., val0..valN, count, index]  → pop 2 + count
             * FOR OVER:    [..., iter, state, control, nvar] → pop 4
             * WHILE:       no extra state                    → pop 0        */
            if (ltyp == CANDO_LOOP_FOR_OVER) {
                cando_value_release(POP()); /* nvar    */
                cando_value_release(POP()); /* control */
                cando_value_release(POP()); /* state   */
                cando_value_release(POP()); /* iter    */
            } else if (ltyp == CANDO_LOOP_FOR) {
                f64 count = (vm->stack_top - 1)->as.number; /* peek */
                cando_value_release(POP()); /* index */
                cando_value_release(POP()); /* count */
                for (i64 vi = 0; vi < (i64)count; vi++)
                    cando_value_release(POP());
            }

            vm->spread_extra = 0;
            ip = vm->loop_stack[idx].break_ip;
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
            lf->loop_type  = loop_type;
            vm->loop_depth++;
            DISPATCH();
        }
        OP_CASE(OP_LOOP_END): {
            if (vm->loop_depth > 0) vm->loop_depth--;
            DISPATCH();
        }

        /* ── Band 11: Functions ─────────────────────────────────────── */
        OP_CASE(OP_CLOSURE): {
            /* The constant at index ci is a cando_number(fn_pc) — the byte
             * offset of the function body within the current chunk.
             * We wrap (current closure, fn_pc) in a CdoObject of kind
             * OBJ_FUNCTION so the function value carries its home chunk
             * and can be called correctly even after crossing module
             * boundaries via include().                                  */
            u16 ci    = READ_U16();
            u32 fn_pc = (u32)frame->closure->chunk->constants[ci].as.number;

            CdoObject  *fn_obj = cdo_function_new(fn_pc,
                                                  (void *)frame->closure,
                                                  NULL, 0);
            HandleIndex h = cando_handle_alloc(vm->handles, fn_obj);
            PUSH(cando_object_value(h));
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
                u32 pc = (u32)callee.as.number;
                CandoChunk *chunk = frame->closure->chunk;

                if (vm->frame_count >= CANDO_FRAMES_MAX) {
                    vm_runtime_error(vm, "call stack overflow");
                    goto handle_error;
                }

                SYNC_IP();
                CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure   = frame->closure; /* same chunk */
                new_frame->ip        = chunk->code + pc;
                new_frame->slots     = vm->stack_top - arg_count - 1;
                new_frame->ret_count = 0;
                new_frame->is_fluent = false;

                /* Pre-allocate null slots for local variables so expression
                 * evaluation never overwrites them (same logic as vm_call). */
                {
                    u32 np = arg_count + 1;
                    if (chunk->local_count > np) {
                        u32 ne = chunk->local_count - np;
                        for (u32 i = 0; i < ne; i++)
                            cando_vm_push(vm, cando_null());
                    }
                }

                LOAD_FRAME();
                DISPATCH();
            }

            if (!cando_is_object(callee)) {
                vm_runtime_error(vm, "can only call functions (got %s)",
                                 cando_value_type_name((TypeTag)callee.tag));
                goto handle_error;
            }

            /* OBJ_FUNCTION: script closure created by OP_CLOSURE.
             * fn.script.bytecode  = (void *)CandoClosure *
             * fn.script.param_count = fn_pc (byte offset of function body) */
            {
                CdoObject *fn_obj = cando_bridge_resolve(vm, callee.as.handle);
                if (fn_obj && fn_obj->kind == OBJ_FUNCTION &&
                    fn_obj->fn.script.bytecode) {
                    CandoClosure *fn_closure =
                        (CandoClosure *)fn_obj->fn.script.bytecode;
                    u32 fn_pc = fn_obj->fn.script.param_count;

                    if (vm->frame_count >= CANDO_FRAMES_MAX) {
                        vm_runtime_error(vm, "call stack overflow");
                        goto handle_error;
                    }
                    SYNC_IP();
                    CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->closure   = fn_closure;
                    new_frame->ip        = fn_closure->chunk->code + fn_pc;
                    new_frame->slots     = vm->stack_top - arg_count - 1;
                    new_frame->ret_count = 0;
                    new_frame->is_fluent = false;

                    u32 np = arg_count + 1;
                    if (fn_closure->chunk->local_count > np) {
                        u32 ne = fn_closure->chunk->local_count - np;
                        for (u32 i = 0; i < ne; i++)
                            cando_vm_push(vm, cando_null());
                    }
                    LOAD_FRAME();
                    DISPATCH();
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
                    CdoString *skey = cando_bridge_intern_key(name_val.as.string);
                    CdoObject *sproto = cando_bridge_resolve(vm, vm->string_proto.as.handle);
                    cdo_object_get(sproto, skey, &method_cdo);
                    cdo_string_release(skey);
                }
            } else if (cando_is_object(receiver)) {
                CdoObject *robj = cando_bridge_resolve(vm, receiver.as.handle);
                CandoValue name_val = frame->closure->chunk->constants[name_ci];
                CdoString *key = cando_bridge_intern_key(name_val.as.string);

                /* Array method special case: look in array_proto if not found in array object. */
                if (robj->kind == OBJ_ARRAY && cando_is_object(vm->array_proto)) {
                    if (!cdo_object_get(robj, key, &method_cdo)) {
                        CdoObject *aproto = cando_bridge_resolve(vm, vm->array_proto.as.handle);
                        cdo_object_get(aproto, key, &method_cdo);
                    }
                } else {
                    cdo_object_get(robj, key, &method_cdo);
                }
                cdo_string_release(key);
            } else {
                vm_runtime_error(vm, "method call on non-object (got %s)",
                                 cando_value_type_name((TypeTag)receiver.tag));
                goto handle_error;
            }

            CandoValue method = cando_bridge_to_cando(vm, method_cdo);
            cdo_value_release(method_cdo);

            bool callable = false;
            if (IS_NATIVE_FN(method)) callable = true;
            else if (cando_is_number(method)) callable = true;
            else if (cando_is_object(method)) {
                CdoObject *mo = cando_bridge_resolve(vm, method.as.handle);
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
                CdoObject *mo = cando_bridge_resolve(vm, method.as.handle);
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
                CdoObject *fn_obj = cando_bridge_resolve(vm, method.as.handle);
                if (fn_obj->kind == OBJ_FUNCTION && fn_obj->fn.script.bytecode) {
                    CandoClosure *fn_closure = (CandoClosure *)fn_obj->fn.script.bytecode;
                    u32 fn_pc = fn_obj->fn.script.param_count;
                    if (vm->frame_count >= CANDO_FRAMES_MAX) {
                        vm_runtime_error(vm, "call stack overflow");
                        goto handle_error;
                    }
                    SYNC_IP();
                    CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->closure   = fn_closure;
                    new_frame->ip        = fn_closure->chunk->code + fn_pc;
                    new_frame->slots     = base;
                    new_frame->ret_count = 0;
                    new_frame->is_fluent = is_fluent;
                    u32 np = total_argc + 1;
                    if (fn_closure->chunk->local_count > np) {
                        u32 ne = fn_closure->chunk->local_count - np;
                        for (u32 i = 0; i < ne; i++) cando_vm_push(vm, cando_null());
                    }
                    LOAD_FRAME();
                    DISPATCH();
                }
            }

            /* Raw PC number */
            if (cando_is_number(method)) {
                u32 pc = (u32)method.as.number;
                if (vm->frame_count >= CANDO_FRAMES_MAX) {
                    vm_runtime_error(vm, "call stack overflow");
                    goto handle_error;
                }
                SYNC_IP();
                CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure   = frame->closure;
                new_frame->ip        = frame->closure->chunk->code + pc;
                new_frame->slots     = base;
                new_frame->ret_count = 0;
                new_frame->is_fluent = is_fluent;
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

            /* Pop the frame. */
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
                CdoObject *obj = cando_bridge_resolve(vm, (HandleIndex)v.as.handle);
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
        OP_CASE(OP_RANGE_ASC): {
            CandoValue b = POP(), a = POP();
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "range requires numbers");
                goto handle_error;
            }
            i64 from  = (i64)a.as.number;
            i64 to    = (i64)b.as.number;
            /* Build an array so the range is a single value on the stack.
             * This lets it be used as a for-loop iterable, a function
             * argument, an assignment target, etc. without corrupting the
             * call frame. */
            SYNC_IP();
            CandoValue arr_val = cando_bridge_new_array(vm);
            CdoObject *arr     = cando_bridge_resolve(vm, arr_val.as.handle);
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
            i64 from  = (i64)a.as.number;
            i64 to    = (i64)b.as.number;
            SYNC_IP();
            CandoValue arr_val = cando_bridge_new_array(vm);
            CdoObject *arr     = cando_bridge_resolve(vm, arr_val.as.handle);
            for (i64 v = from; v >= to; v--) {
                CdoValue cv = cdo_number((f64)v);
                cdo_array_push(arr, cv);
            }
            PUSH(arr_val);
            DISPATCH();
        }
        OP_CASE(OP_FOR_INIT): {
            /* mode: 1 = keys (FOR IN), 0 = values (FOR OF/OVER)
             * Stack before: [..., iterable]
             * Array IN:  push indices 0..len-1, then [count, 0].
             * Array OF:  push element values,   then [count, 0].
             * Object IN: push field name strings (FIFO order), then [count, 0].
             * Object OF: push field values (FIFO order),       then [count, 0].
             * Scalar:    push the scalar itself, then [1, 0].
             * Stack after: [..., val0..valN-1, count, index=0]             */
            u16 keys_mode = READ_U16();
            CandoValue iterable = POP();

            if (cando_is_object(iterable)) {
                CdoObject *obj = cando_bridge_resolve(vm, (HandleIndex)iterable.as.handle);
                if (obj->kind == OBJ_ARRAY) {
                    u32 len = cdo_array_len(obj);
                    if (keys_mode) {
                        for (u32 ai = 0; ai < len; ai++)
                            PUSH(cando_number((f64)ai));
                    } else {
                        for (u32 ai = 0; ai < len; ai++) {
                            CdoValue cv = cdo_null();
                            cdo_array_rawget_idx(obj, ai, &cv);
                            PUSH(cando_bridge_to_cando(vm, cv));
                        }
                    }
                    PUSH(cando_number((f64)len));
                    PUSH(cando_number(0.0));
                } else {
                    /* Plain object: walk FIFO list for keys or values.   */
                    u32 count = 0;
                    u32 fi = obj->fifo_head;
                    while (fi != UINT32_MAX) {
                        ObjSlot *slot = &obj->slots[fi];
                        if (keys_mode) {
                            CandoString *s = cando_string_new(
                                slot->key->data, slot->key->length);
                            PUSH(cando_string_value(s));
                        } else {
                            PUSH(cando_bridge_to_cando(vm, slot->value));
                        }
                        count++;
                        fi = slot->fifo_next;
                    }
                    PUSH(cando_number((f64)count));
                    PUSH(cando_number(0.0));
                }
            } else {
                /* Scalar: iterate as single value.                       */
                PUSH(iterable);
                PUSH(cando_number(1.0));
                PUSH(cando_number(0.0));
            }
            DISPATCH();
        }
        OP_CASE(OP_FOR_NEXT): {
            /* Stack: [..., val0..valN-1, count, index]
             * If index >= count: pop [count, index] and all N values,
             *   then jump by offset (exit loop).
             * Else: push vals[index] as loop variable, increment index.   */
            i16 off = (i16)(READ_U16());
            f64 index = (vm->stack_top - 1)->as.number;
            f64 count = (vm->stack_top - 2)->as.number;
            if (index >= count) {
                /* Loop exhausted: pop state [count, index] */
                cando_value_release(POP()); /* index */
                cando_value_release(POP()); /* count */
                /* Pop all iterable values */
                i64 n = (i64)count;
                for (i64 vi = 0; vi < n; vi++)
                    cando_value_release(POP());
                ip += off;
            } else {
                /* Copy value at position: stack_top[-2 - count + index] */
                CandoValue *val_ptr = vm->stack_top - 2 - (i64)count + (i64)index;
                /* Increment index in place before PUSH so it stays correct */
                (vm->stack_top - 1)->as.number = index + 1.0;
                PUSH(cando_value_copy(*val_ptr));
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
            u16 nvar = (u16)((vm->stack_top - 1)->as.number);
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
            }
            DISPATCH();
        }
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
                                     (HandleIndex)src.as.handle);
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
            f64 src_index = (vm->stack_top - 1)->as.number;
            f64 count     = (vm->stack_top - 2)->as.number;
            if (src_index >= count) {
                ip += off;
            } else {
                (vm->stack_top - 1)->as.number = src_index + 1.0;
                CandoValue *val_ptr =
                    vm->stack_top - 2 - (i64)count + (i64)src_index;
                PUSH(cando_value_copy(*val_ptr));
            }
            DISPATCH();
        }
        OP_CASE(OP_FILTER_NEXT): {
            /* Identical to PIPE_NEXT; filter logic is in OP_FILTER_COLLECT. */
            i16 off = READ_I16();
            f64 src_index = (vm->stack_top - 1)->as.number;
            f64 count     = (vm->stack_top - 2)->as.number;
            if (src_index >= count) {
                ip += off;
            } else {
                (vm->stack_top - 1)->as.number = src_index + 1.0;
                CandoValue *val_ptr =
                    vm->stack_top - 2 - (i64)count + (i64)src_index;
                PUSH(cando_value_copy(*val_ptr));
            }
            DISPATCH();
        }
        OP_CASE(OP_PIPE_END): {
            /* Clean up pipe state; result_arr (already filled) stays on top.
             * Stack before: [..., result_arr, v0..v(N-1), count=N, src_idx=N]
             * Stack after:  [..., result_arr]                               */
            cando_value_release(POP());          /* pop src_idx (number) */
            CandoValue count_v = POP();          /* pop count */
            i64 n = (i64)count_v.as.number;
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
            f64 count_f = (vm->stack_top - 2)->as.number;
            i64 N = (i64)count_f;
            CandoValue *arr_ptr = vm->stack_top - 3 - N;
            CdoObject  *arr = cando_bridge_resolve(vm,
                                  (HandleIndex)arr_ptr->as.handle);
            CdoValue cdo_result = cando_bridge_to_cdo(vm, result);
            cdo_array_push(arr, cdo_result);
            cando_value_release(result);
            DISPATCH();
        }
        OP_CASE(OP_FILTER_COLLECT): {
            /* Like PIPE_COLLECT but null results are not appended. */
            CandoValue result = POP();
            if (!cando_is_null(result)) {
                f64 count_f = (vm->stack_top - 2)->as.number;
                i64 N = (i64)count_f;
                CandoValue *arr_ptr = vm->stack_top - 3 - N;
                CdoObject  *arr = cando_bridge_resolve(vm,
                                      (HandleIndex)arr_ptr->as.handle);
                CdoValue cdo_result = cando_bridge_to_cdo(vm, result);
                cdo_array_push(arr, cdo_result);
            }
            cando_value_release(result);
            DISPATCH();
        }

        /* ── Band 14: Error handling ────────────────────────────────── */
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
            /* Format error_msg from first thrown value. */
            char *s = cando_value_tostring(vm->error_vals[0]);
            snprintf(vm->error_msg, sizeof(vm->error_msg), "%s", s);
            cando_free(s);
            goto handle_error;
        }
        OP_CASE(OP_RERAISE): {
            if (!vm->has_error) {
                vm_runtime_error(vm, "RERAISE outside of catch block");
            }
            goto handle_error;
        }

        /* ── Band 15: Threads ───────────────────────────────────────── */
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
            CdoObject *obj = cando_bridge_resolve(vm, thread_val.as.handle);
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
                /* Propagate error: push to error_vals and go to error handler. */
                cando_value_release(vm->error_vals[0]);
                vm->error_vals[0]   = cando_value_copy(t->error);
                vm->error_val_count = 1;
                vm->has_error       = true;
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                         "thread raised an error");
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
            CdoObject *fn_obj = cando_bridge_resolve(vm, fn_val.as.handle);
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
                cdo_thread_destroy(t);
                cando_handle_free(vm->handles, h);
                cando_free(t);
                vm_runtime_error(vm, "thread: failed to create OS thread");
                goto handle_error;
            }
            /* Detach immediately — resources reclaimed when thread exits. */
            cando_os_thread_detach(t->os_thread);

            cando_value_release(fn_val);
            PUSH(thread_val);
            DISPATCH();
        }

        /* ── Band 16: Classes ───────────────────────────────────────── */
        OP_CASE(OP_NEW_CLASS): {
            u16 ci = READ_U16();
            CandoValue name_val = frame->closure->chunk->constants[ci];
            CANDO_ASSERT(cando_is_string(name_val));

            CandoValue cls_val = cando_bridge_new_object(vm);
            if (g_meta_type) {
                CdoObject  *cls     = cando_bridge_resolve(vm, cls_val.as.handle);
                CdoString  *cdo_key = cando_bridge_intern_key(name_val.as.string);
                CdoValue    tv      = cdo_string_value(cdo_string_retain(cdo_key));
                cdo_object_rawset(cls, g_meta_type, tv, FIELD_STATIC);
                cdo_value_release(tv);
                cdo_string_release(cdo_key);
            }
            PUSH(cls_val);
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
            CdoObject *cls     = cando_bridge_resolve(vm, cls_val.as.handle);
            CdoString *cdo_key = cando_bridge_intern_key(name_val.as.string);
            CdoValue   mv      = cando_bridge_to_cdo(vm, method_val);
            cdo_object_rawset(cls, cdo_key, mv, FIELD_NONE);
            cdo_value_release(mv);
            cdo_string_release(cdo_key);
            cando_value_release(method_val);
            DISPATCH();
        }
        OP_CASE(OP_INHERIT): {
            /* Stack: [..., parent_class, child_class]
             * Set child.__index = parent so instances find methods on both. */
            CandoValue child_val  = POP();
            CandoValue parent_val = PEEK(0);
            if (!cando_is_object(child_val) || !cando_is_object(parent_val)) {
                cando_value_release(child_val);
                vm_runtime_error(vm, "INHERIT: expected class objects");
                goto handle_error;
            }
            if (g_meta_index) {
                CdoObject *child  = cando_bridge_resolve(vm, child_val.as.handle);
                CdoValue   pv     = cando_bridge_to_cdo(vm, parent_val);
                cdo_object_rawset(child, g_meta_index, pv, FIELD_NONE);
                cdo_value_release(pv);
            }
            PUSH(child_val);
            DISPATCH();
        }

        /* ── Band 17: Mask / selector ───────────────────────────────── */
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
                else if (!(left.as.number < r.as.number)) result = false;
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
                else if (!(left.as.number > r.as.number)) result = false;
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
                else if (!(left.as.number <= r.as.number)) result = false;
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
                else if (!(left.as.number >= r.as.number)) result = false;
            }
            for (int i = 0; i < n; i++) cando_value_release(POP());
            cando_value_release(POP());
            PUSH(cando_bool(result));
            DISPATCH();
        }

        /* ── Sentinels ──────────────────────────────────────────────── */
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
