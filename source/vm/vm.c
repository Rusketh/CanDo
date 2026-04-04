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
    vm->eval_stop_frame = 0;
    vm->eval_result     = cando_null();
    vm->thread_stop_frame  = ~0u;  /* ~0u = "not set"; 0 is a valid stop boundary */
    vm->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) vm->thread_results[i] = cando_null();
    vm->string_proto    = cando_null();
    vm->array_proto     = cando_null();

    /* Module cache — initially empty. */
    vm->module_cache       = NULL;
    vm->module_cache_count = 0;
    vm->module_cache_cap   = 0;

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
    child->eval_stop_frame = 0;
    child->eval_result     = cando_null();
    child->thread_stop_frame   = ~0u;  /* ~0u = "not set" */
    child->thread_result_count = 0;
    for (u32 i = 0; i < CANDO_MAX_THROW_ARGS; i++) child->thread_results[i] = cando_null();
    child->string_proto    = cando_value_copy(parent->string_proto);
    child->array_proto     = cando_value_copy(parent->array_proto);

    /* Module cache: child VMs do not cache modules independently. */
    child->module_cache       = NULL;
    child->module_cache_count = 0;
    child->module_cache_cap   = 0;

    /* Share parent's handle table and globals — no owned copies. */
    child->handles_owned = NULL;
    child->handles       = parent->handles;   /* shared, thread-safe */

    child->globals_owned = NULL;
    child->globals       = parent->globals;   /* shared, locked on access */

    /* Copy native function registry (read-only after init; safe to share). */
    child->native_count = parent->native_count;
    for (u32 i = 0; i < parent->native_count; i++)
        child->native_fns[i] = parent->native_fns[i];

    cdo_object_init(); /* idempotent — ensures meta-keys are ready */
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

    /* Release module cache. */
    for (u32 i = 0; i < vm->module_cache_count; i++) {
        CandoModuleEntry *e = &vm->module_cache[i];
        cando_free(e->path);
        cando_value_release(e->value);
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
    if (!cdo_object_rawget(obj, (CdoString *)meta_key, &raw))
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
            /* Ensure at least one result is on the stack. */
            if ((u32)(vm->stack_top - vm->stack) <= stack_before)
                cando_vm_push(vm, cando_null());
        }
        /* If multiple returns, keep only the top one. */
        while ((u32)(vm->stack_top - vm->stack) > stack_before + 1) {
            CandoValue extra = cando_vm_pop(vm);
            cando_value_release(extra);
        }
        return true;
    }

    /* OBJ_NATIVE CdoObject: call via fn.native.fn with CdoValue args. */
    if (cando_is_object(callee)) {
        CdoObject *fn_obj = cando_bridge_resolve(vm, callee.as.handle);
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

/*
 * vm_is_truthy_meta -- truthiness with __is meta-method dispatch for objects.
 * Sets *ok = false if a meta-method call failed (vm->has_error will be set).
 */
static bool vm_is_truthy_meta(CandoVM *vm, CandoValue v, bool *ok) {
    *ok = true;
    if (cando_is_null(v))  return false;
    if (cando_is_bool(v))  return v.as.boolean;
    if (cando_is_object(v) && g_meta_is) {
        if (cando_vm_call_meta(vm, v.as.handle,
                               (struct CdoString *)g_meta_is, &v, 1)) {
            CandoValue result = cando_vm_pop(vm);
            bool truthy = vm_is_truthy(result);
            cando_value_release(result);
            return truthy;
        }
        if (vm->has_error) { *ok = false; return false; }
    }
    return true; /* non-null, non-bool, no __is → truthy */
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
    cando_closure_free(top_closure);
    return result;
}

CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue *result_out) {
    /* Save outer eval state so nested evals don't interfere. */
    u32        saved_stop   = vm->eval_stop_frame;
    CandoValue saved_result = vm->eval_result;

    /* The stop marker is the current frame depth.  When OP_RETURN brings
     * frame_count back to this value, VM_EVAL_DONE is returned.          */
    vm->eval_stop_frame = vm->frame_count;
    vm->eval_result     = cando_null();

    CandoClosure *closure = cando_closure_new(chunk);
    cando_vm_push(vm, cando_null()); /* slot 0 placeholder */
    if (!vm_call(vm, closure, 0)) {
        cando_vm_pop(vm);            /* remove the placeholder */
        cando_closure_free(closure);
        vm->eval_stop_frame = saved_stop;
        vm->eval_result     = saved_result;
        return VM_RUNTIME_ERR;
    }

    CandoVMResult res = vm_run(vm);
    cando_closure_free(closure);

    if (result_out) {
        *result_out     = vm->eval_result;
    } else {
        cando_value_release(vm->eval_result);
    }

    /* Restore outer eval state. */
    vm->eval_result     = saved_result;
    vm->eval_stop_frame = saved_stop;
    return res;
}

CandoVMResult cando_vm_exec_eval_module(CandoVM *vm, CandoChunk *chunk,
                                         CandoValue *result_out,
                                         CandoClosure **closure_out) {
    /* Identical to cando_vm_exec_eval, but transfers the closure to the
     * caller instead of freeing it.  The caller is responsible for calling
     * cando_closure_free() when the closure is no longer needed.           */
    u32        saved_stop   = vm->eval_stop_frame;
    CandoValue saved_result = vm->eval_result;

    vm->eval_stop_frame = vm->frame_count;
    vm->eval_result     = cando_null();

    CandoClosure *closure = cando_closure_new(chunk);
    cando_vm_push(vm, cando_null()); /* slot 0 placeholder */
    if (!vm_call(vm, closure, 0)) {
        cando_vm_pop(vm);
        cando_closure_free(closure);
        vm->eval_stop_frame = saved_stop;
        vm->eval_result     = saved_result;
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

    if (result_out) {
        *result_out = vm->eval_result;
    } else {
        cando_value_release(vm->eval_result);
    }

    vm->eval_result     = saved_result;
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
    cando_free(ta);
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
                BINARY_NUM_OP(+);
            }
            DISPATCH();
        }
        OP_CASE(OP_SUB): BINARY_NUM_OP(-); DISPATCH();
        OP_CASE(OP_MUL): BINARY_NUM_OP(*); DISPATCH();
        OP_CASE(OP_DIV): {
            CandoValue b = POP(), a = POP();
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
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(fmod(a.as.number, b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_POW): {
            CandoValue b = POP(), a = POP();
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "operands must be numbers");
                goto handle_error;
            }
            PUSH(cando_number(pow(a.as.number, b.as.number)));
            DISPATCH();
        }
        OP_CASE(OP_NEG): {
            CandoValue a = POP();
            if (cando_is_object(a) && g_meta_negate) {
                if (cando_vm_call_meta(vm, a.as.handle,
                                       (struct CdoString *)g_meta_negate,
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
            if (g_meta_equal && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue eq_args[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_equal,
                                       eq_args, 2)) {
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
            if (g_meta_equal && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue eq_args[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_equal,
                                       eq_args, 2)) {
                    /* NEQ = !__equal result */
                    CandoValue eq_result = cando_vm_pop(vm);
                    PUSH(cando_bool(!vm_is_truthy(eq_result)));
                    cando_value_release(eq_result);
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
            if (g_meta_greater && g_meta_equal &&
                (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue cmp_args[2] = {a, b};
                bool gt = false, eq = false;
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_greater,
                                       cmp_args, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    gt = vm_is_truthy(r); cando_value_release(r);
                } else if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_equal,
                                       cmp_args, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    eq = vm_is_truthy(r); cando_value_release(r);
                } else if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
                PUSH(cando_bool(!gt && !eq));
                cando_value_release(a); cando_value_release(b);
                DISPATCH();
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
            CandoValue b = POP(), a = POP();
            if (g_meta_greater && (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue cmp_args[2] = {a, b};
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_greater,
                                       cmp_args, 2)) {
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
            if (g_meta_greater && g_meta_equal &&
                (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue cmp_args[2] = {a, b};
                bool gt = false;
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_greater,
                                       cmp_args, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    gt = vm_is_truthy(r); cando_value_release(r);
                } else if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
                PUSH(cando_bool(!gt));
                cando_value_release(a); cando_value_release(b);
                DISPATCH();
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
            CandoValue b = POP(), a = POP();
            if (g_meta_greater && g_meta_equal &&
                (cando_is_object(a) || cando_is_object(b))) {
                HandleIndex h = cando_is_object(a) ? a.as.handle : b.as.handle;
                CandoValue cmp_args[2] = {a, b};
                bool gt = false, eq = false;
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_greater,
                                       cmp_args, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    gt = vm_is_truthy(r); cando_value_release(r);
                } else if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
                if (cando_vm_call_meta(vm, h,
                                       (struct CdoString *)g_meta_equal,
                                       cmp_args, 2)) {
                    CandoValue r = cando_vm_pop(vm);
                    eq = vm_is_truthy(r); cando_value_release(r);
                } else if (vm->has_error) {
                    cando_value_release(a); cando_value_release(b);
                    goto handle_error;
                }
                PUSH(cando_bool(gt || eq));
                cando_value_release(a); cando_value_release(b);
                DISPATCH();
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
            /* Check __not meta-method first. */
            if (cando_is_object(a) && g_meta_not) {
                if (cando_vm_call_meta(vm, a.as.handle,
                                       (struct CdoString *)g_meta_not,
                                       &a, 1)) {
                    cando_value_release(a);
                    DISPATCH();
                }
                if (vm->has_error) { cando_value_release(a); goto handle_error; }
            }
            /* Fall back to __is-aware truthiness. */
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
            u16 n = READ_U16();
            CandoValue arr_val = cando_bridge_new_array(vm);
            if (n > 0) {
                CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);
                /* Items are on the stack: stack_top-n .. stack_top-1 */
                CandoValue *base = vm->stack_top - n;
                for (u16 i = 0; i < n; i++) {
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
            CdoValue     cdo_val = cando_bridge_to_cdo(vm, val);
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
                    if (cando_vm_call_meta(vm, a.as.handle,
                                           (struct CdoString *)g_meta_len,
                                           &a, 1)) {
                        cando_value_release(a);
                        DISPATCH();
                    }
                    if (vm->has_error) {
                        cando_value_release(a); goto handle_error;
                    }
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
                vm_runtime_error(vm, "break depth %u exceeds loop depth %u",
                                 depth, vm->loop_depth);
                goto handle_error;
            }
            u32 idx   = vm->loop_depth - 1 - depth;
            ip        = vm->loop_stack[idx].break_ip;
            vm->loop_depth = idx;
            DISPATCH();
        }
        OP_CASE(OP_CONTINUE): {
            u16 depth = READ_U16();
            if (vm->loop_depth == 0 || depth >= vm->loop_depth) {
                vm_runtime_error(vm, "continue depth %u exceeds loop depth %u",
                                 depth, vm->loop_depth);
                goto handle_error;
            }
            u32 idx = vm->loop_depth - 1 - depth;
            ip      = vm->loop_stack[idx].cont_ip;
            DISPATCH();
        }
        OP_CASE(OP_LOOP_MARK): {
            /* Compiler inserts LOOP_MARK with A = forward offset to the
             * break target.  We record cont_ip = current ip, break_ip =
             * ip + A after reading the operand.                         */
            u16 break_fwd = READ_U16();
            CANDO_ASSERT_MSG(vm->loop_depth < CANDO_LOOP_MAX,
                             "loop depth overflow");
            vm->loop_stack[vm->loop_depth].cont_ip  = ip; /* top of body */
            vm->loop_stack[vm->loop_depth].break_ip = ip + break_fwd;
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
            /* Apply accumulated spread_extra from multi-return arguments. */
            u32 arg_count = (u32)static_argc + (u32)(vm->spread_extra > 0 ? vm->spread_extra : 0);
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
                 * A void native call evaluates to null in expression context. */
                if (ret_count == 0) {
                    callee_slot[0] = cando_null();
                    vm->stack_top = callee_slot + 1;
                } else {
                    vm->stack_top = callee_slot + ret_count;
                }
                vm->last_ret_count = ret_count;
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
        OP_CASE(OP_METHOD_CALL): {
            u16 name_ci   = READ_U16();
            u16 arg_count = READ_U16();
            CandoValue receiver = *(vm->stack_top - arg_count - 1);

            /* String method call: receiver is args[0], explicit args follow. */
            if (cando_is_string(receiver)) {
                if (!cando_is_object(vm->string_proto)) {
                    vm_runtime_error(vm,
                        "method call on string: no string library loaded");
                    goto handle_error;
                }
                CandoValue name_val = frame->closure->chunk->constants[name_ci];
                CdoString *skey = cando_bridge_intern_key(name_val.as.string);
                CdoObject *sproto = cando_bridge_resolve(
                                        vm, vm->string_proto.as.handle);
                CdoValue smethod_cdo = cdo_null();
                cdo_object_get(sproto, skey, &smethod_cdo);
                cdo_string_release(skey);
                CandoValue smethod = cando_bridge_to_cando(vm, smethod_cdo);

                if (IS_NATIVE_FN(smethod)) {
                    u32 ni = NATIVE_INDEX(smethod);
                    if (ni >= vm->native_count) {
                        vm_runtime_error(vm,
                            "invalid native string method index %u", ni);
                        goto handle_error;
                    }
                    /* Pass receiver as args[0], explicit args as args[1..N]. */
                    CandoValue *callee_slot = vm->stack_top - arg_count - 1;
                    SYNC_IP();
                    int ret = vm->native_fns[ni](vm,
                                (int)arg_count + 1, callee_slot);
                    if (vm->has_error) goto handle_error;
                    if (ret < 0) ret = 0;
                    CandoValue *ret_src = vm->stack_top - ret;
                    /* Release receiver + explicit args. */
                    for (u32 i = 0; i < (u32)arg_count + 1; i++)
                        cando_value_release(callee_slot[i]);
                    for (int i = 0; i < ret; i++)
                        callee_slot[i] = ret_src[i];
                    if (ret == 0) {
                        callee_slot[0] = cando_null();
                        vm->stack_top = callee_slot + 1;
                    } else {
                        vm->stack_top = callee_slot + ret;
                    }
                    vm->last_ret_count = ret;
                    DISPATCH();
                }
                vm_runtime_error(vm, "string method is not callable");
                goto handle_error;
            }

            /* Array method call. */
            if (cando_is_object(receiver)) {
                CdoObject *robj = cando_bridge_resolve(vm, receiver.as.handle);
                if (robj && robj->kind == OBJ_ARRAY && cando_is_object(vm->array_proto)) {
                    CandoValue name_val = frame->closure->chunk->constants[name_ci];
                    CdoString *akey = cando_bridge_intern_key(name_val.as.string);
                    CdoObject *aproto = cando_bridge_resolve(vm, vm->array_proto.as.handle);
                    CdoValue amethod_cdo = cdo_null();
                    cdo_object_get(aproto, akey, &amethod_cdo);
                    cdo_string_release(akey);
                    CandoValue amethod = cando_bridge_to_cando(vm, amethod_cdo);

                    if (IS_NATIVE_FN(amethod)) {
                        u32 ni = NATIVE_INDEX(amethod);
                        CandoValue *callee_slot = vm->stack_top - arg_count - 1;
                        SYNC_IP();
                        int ret = vm->native_fns[ni](vm, (int)arg_count + 1, callee_slot);
                        if (vm->has_error) goto handle_error;
                        if (ret < 0) ret = 0;
                        CandoValue *ret_src = vm->stack_top - ret;
                        for (u32 i = 0; i < (u32)arg_count + 1; i++) cando_value_release(callee_slot[i]);
                        for (int i = 0; i < ret; i++) callee_slot[i] = ret_src[i];
                        if (ret == 0) {
                            callee_slot[0] = cando_null();
                            vm->stack_top = callee_slot + 1;
                        } else {
                            vm->stack_top = callee_slot + ret;
                        }
                        vm->last_ret_count = ret;
                        DISPATCH();
                    }
                }
            }

            if (!cando_is_object(receiver)) {
                vm_runtime_error(vm, "method call on non-object (got %s)",
                                 cando_value_type_name((TypeTag)receiver.tag));
                goto handle_error;
            }

            CandoValue name_val = frame->closure->chunk->constants[name_ci];
            CandoString *name_cs = name_val.as.string;
            CdoString *key = cando_bridge_intern_key(name_cs);
            CdoObject *obj = cando_bridge_resolve(vm, (HandleIndex)receiver.as.handle);
            CdoValue method_cdo = cdo_null();
            cdo_object_get(obj, key, &method_cdo);
            cdo_string_release(key);

            /* OBJ_FUNCTION (script closure created by OP_CLOSURE):
             * access the raw CdoObject directly to avoid handle allocation. */
            if (cdo_is_function(method_cdo)) {
                CdoObject    *fn_obj    = method_cdo.as.object;
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
                u32 np = (u32)arg_count + 1;
                if (fn_closure->chunk->local_count > np) {
                    u32 ne = fn_closure->chunk->local_count - np;
                    for (u32 i = 0; i < ne; i++) cando_vm_push(vm, cando_null());
                }
                LOAD_FRAME();
                DISPATCH();
            }

            CandoValue method = cando_bridge_to_cando(vm, method_cdo);

            if (IS_NATIVE_FN(method)) {
                u32 ni = NATIVE_INDEX(method);
                if (ni >= vm->native_count) {
                    vm_runtime_error(vm, "invalid native method index %u", ni);
                    goto handle_error;
                }
                CandoValue *callee_slot = vm->stack_top - arg_count - 1;
                CandoValue *args = callee_slot + 1;
                SYNC_IP();
                int ret_count = vm->native_fns[ni](vm, (int)arg_count, args);
                if (vm->has_error) goto handle_error;
                if (ret_count < 0) ret_count = 0;
                CandoValue *ret_src = vm->stack_top - ret_count;
                for (u32 i = 0; i < (u32)arg_count; i++)
                    cando_value_release(args[i]);
                for (int i = 0; i < ret_count; i++)
                    callee_slot[i] = ret_src[i];
                if (ret_count == 0) {
                    callee_slot[0] = cando_null();
                    vm->stack_top = callee_slot + 1;
                } else {
                    vm->stack_top = callee_slot + ret_count;
                }
                vm->last_ret_count = ret_count;
                DISPATCH();
            }

            if (cando_is_number(method)) {
                /* Legacy path: method stored as a raw PC number (no closure). */
                u32 pc = (u32)method.as.number;
                CandoChunk *chunk = frame->closure->chunk;
                if (vm->frame_count >= CANDO_FRAMES_MAX) {
                    vm_runtime_error(vm, "call stack overflow");
                    goto handle_error;
                }
                SYNC_IP();
                CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure   = frame->closure;
                new_frame->ip        = chunk->code + pc;
                new_frame->slots     = vm->stack_top - arg_count - 1;
                new_frame->ret_count = 0;
                LOAD_FRAME();
                DISPATCH();
            }

            vm_runtime_error(vm, "method is not callable");
            goto handle_error;
        }
        OP_CASE(OP_FLUENT_CALL): {
            u16 name_ci   = READ_U16();
            u16 arg_count = READ_U16();
            CandoValue receiver = *(vm->stack_top - arg_count - 1);

            if (!cando_is_object(receiver)) {
                vm_runtime_error(vm, "fluent call on non-object (got %s)",
                                 cando_value_type_name((TypeTag)receiver.tag));
                goto handle_error;
            }

            CandoValue fname_val = frame->closure->chunk->constants[name_ci];
            CandoString *fname_cs = fname_val.as.string;
            CdoString *fkey = cando_bridge_intern_key(fname_cs);
            CdoObject *fobj = cando_bridge_resolve(vm, (HandleIndex)receiver.as.handle);
            CdoValue fmethod_cdo = cdo_null();
            cdo_object_get(fobj, fkey, &fmethod_cdo);
            cdo_string_release(fkey);

            /* OBJ_FUNCTION (script closure via OP_CLOSURE). */
            if (cdo_is_function(fmethod_cdo)) {
                CdoObject    *fn_obj     = fmethod_cdo.as.object;
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
                u32 np = (u32)arg_count + 1;
                if (fn_closure->chunk->local_count > np) {
                    u32 ne = fn_closure->chunk->local_count - np;
                    for (u32 i = 0; i < ne; i++) cando_vm_push(vm, cando_null());
                }
                LOAD_FRAME();
                DISPATCH();
            }

            CandoValue fmethod = cando_bridge_to_cando(vm, fmethod_cdo);

            if (cando_is_number(fmethod)) {
                /* Legacy path: method stored as a raw PC number (no closure). */
                u32 pc = (u32)fmethod.as.number;
                CandoChunk *chunk = frame->closure->chunk;
                if (vm->frame_count >= CANDO_FRAMES_MAX) {
                    vm_runtime_error(vm, "call stack overflow");
                    goto handle_error;
                }
                SYNC_IP();
                CandoCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure   = frame->closure;
                new_frame->ip        = chunk->code + pc;
                new_frame->slots     = vm->stack_top - arg_count - 1;
                new_frame->ret_count = 0;
                LOAD_FRAME();
                DISPATCH();
            }

            vm_runtime_error(vm, "fluent method is not callable");
            goto handle_error;
        }
        OP_CASE(OP_RETURN): {
            u16 ret_count = READ_U16();
            vm->last_ret_count = (int)ret_count;
            SYNC_IP();

            /* Close any upvalues that pointed into this frame's stack. */
            vm_close_upvalues(vm, frame->slots);

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
                vm->thread_result_count = ret_count < CANDO_MAX_THROW_ARGS
                                          ? ret_count : CANDO_MAX_THROW_ARGS;
                for (u32 i = 0; i < vm->thread_result_count; i++)
                    vm->thread_results[i] = cando_value_copy(ret_start[i]);
                for (u16 i = 0; i < ret_count; i++)
                    cando_value_release(ret_start[i]);
                vm->stack_top = frame->slots;
                return VM_EVAL_DONE;
            }

            /* Eval re-entrancy: if we've returned back to the eval boundary,
             * stash the result and signal the outer vm_run to stop. */
            if (vm->eval_stop_frame != 0 &&
                vm->frame_count == vm->eval_stop_frame) {
                vm->eval_result = (ret_count > 0) ? ret_start[0] : cando_null();
                for (u16 i = 1; i < ret_count; i++)
                    cando_value_release(ret_start[i]);
                vm->stack_top = frame->slots;
                return VM_EVAL_DONE;
            }

            if (vm->frame_count == 0) {
                /* Returning from the top-level script. */
                vm->stack_top = vm->stack;
                return VM_HALT;
            }

            /* Restore the caller's frame. */
            CandoValue *new_top = frame->slots; /* old frame base */
            for (u16 i = 0; i < ret_count; i++) {
                new_top[i] = ret_start[i];
            }
            vm->stack_top = new_top + ret_count;

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
            /* Pop top value. If array, push all elements. Else push it.  */
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
                } else {
                    PUSH(v);
                }
            } else {
                PUSH(v);
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
            /* Push values ascending from ceil(a) to floor(b) inclusive,
             * then push the count so FOR_INIT knows how many values.    */
            i64 from = (i64)a.as.number;
            i64 to   = (i64)b.as.number;
            i64 count = (to >= from) ? (to - from + 1) : 0;
            for (i64 v = from; v <= to; v++) {
                if (vm->stack_top - vm->stack >= CANDO_STACK_MAX - 2) {
                    vm_runtime_error(vm, "range too large for stack");
                    goto handle_error;
                }
                PUSH(cando_number((f64)v));
            }
            PUSH(cando_number((f64)count));
            DISPATCH();
        }
        OP_CASE(OP_RANGE_DESC): {
            CandoValue b = POP(), a = POP();
            if (!cando_is_number(a) || !cando_is_number(b)) {
                vm_runtime_error(vm, "range requires numbers");
                goto handle_error;
            }
            i64 from = (i64)a.as.number;
            i64 to   = (i64)b.as.number;
            i64 count = (from >= to) ? (from - to + 1) : 0;
            for (i64 v = from; v >= to; v--) {
                if (vm->stack_top - vm->stack >= CANDO_STACK_MAX - 2) {
                    vm_runtime_error(vm, "range too large for stack");
                    goto handle_error;
                }
                PUSH(cando_number((f64)v));
            }
            PUSH(cando_number((f64)count));
            DISPATCH();
        }
        OP_CASE(OP_FOR_INIT): {
            /* mode: 1 = keys (FOR IN), 0 = values (FOR OF/OVER)
             * Stack before: [..., iterable]
             * Range (iterable is number): values already on stack; push [count, 0].
             * Array IN:  push indices 0..len-1, then [count, 0].
             * Array OF:  push element values,   then [count, 0].
             * Object IN: push field name strings (FIFO order), then [count, 0].
             * Object OF: push field values (FIFO order),       then [count, 0].
             * Scalar:    push the scalar itself, then [1, 0].
             * Stack after: [..., val0..valN-1, count, index=0]             */
            u16 keys_mode = READ_U16();
            CandoValue iterable = POP();

            if (cando_is_number(iterable)) {
                /* Range: count value; range values already on stack.      */
                i64 count = (i64)iterable.as.number;
                PUSH(cando_number((f64)count));
                PUSH(cando_number(0.0));
            } else if (cando_is_object(iterable)) {
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
            /* Function-based iterator.
             * Stack before: [..., iterator_fn]
             * Stack after:  [..., iterator_fn, null_state, 0]             */
            u16 nvar = READ_U16(); CANDO_UNUSED(nvar);
            /* iterator_fn stays; push null state and call count */
            PUSH(cando_null());           /* state */
            PUSH(cando_number(0.0));      /* call_count (unused for now) */
            DISPATCH();
        }
        OP_CASE(OP_FOR_OVER_NEXT): {
            /* Stack: [..., iterator_fn, state, call_count]
             * Call iterator_fn(). If result is null, done (pop 3, jump).
             * Else push result as loop variable.
             * Only native iterator functions supported.                   */
            i16 off = (i16)(READ_U16());
            CandoValue call_count_v = *(vm->stack_top - 1);
            CandoValue state_v      = *(vm->stack_top - 2);
            CandoValue iter_fn      = *(vm->stack_top - 3);
            CANDO_UNUSED(state_v);

            if (!IS_NATIVE_FN(iter_fn)) {
                vm_runtime_error(vm, "FOR OVER: only native iterator functions supported");
                goto handle_error;
            }
            u32 ni = NATIVE_INDEX(iter_fn);
            if (ni >= vm->native_count) {
                vm_runtime_error(vm, "invalid native iterator index %u", ni);
                goto handle_error;
            }
            SYNC_IP();
            int ret = vm->native_fns[ni](vm, 0, NULL);
            if (vm->has_error) goto handle_error;
            if (ret < 0) ret = 0;

            if (ret == 0) {
                /* Iterator exhausted */
                cando_value_release(POP()); /* call_count */
                cando_value_release(POP()); /* state */
                cando_value_release(POP()); /* iter_fn */
                ip += off;
            } else {
                /* Result is on stack_top; just increment call_count */
                (vm->stack_top - 2 - ret)->as.number = call_count_v.as.number + 1.0;
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

            /* Spawn the OS thread. */
            SYNC_IP();
            if (!cando_os_thread_create(&t->os_thread,
                                        (cando_thread_fn_t)vm_thread_trampoline,
                                        ta)) {
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
            u16 ci = READ_U16(); CANDO_UNUSED(ci);
            vm_runtime_error(vm, "CLASS not yet implemented");
            goto handle_error;
        }
        OP_CASE(OP_BIND_METHOD): {
            u16 ci = READ_U16(); CANDO_UNUSED(ci);
            vm_runtime_error(vm, "BIND_METHOD not yet implemented");
            goto handle_error;
        }
        OP_CASE(OP_INHERIT): {
            vm_runtime_error(vm, "INHERIT not yet implemented");
            goto handle_error;
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
            /* Apply a bitmask to the top `count` stack values from a
             * multi-return function call.  bit i=1 → keep, bit i=0 → skip.
             * Values are indexed from the bottom of the window (bit 0 = the
             * value that was pushed first / sits deepest).               */
            u16 count   = READ_U16();
            u16 bitmask = READ_U16();
            CandoValue *base = vm->stack_top - count;
            int out = 0;
            for (int i = 0; i < (int)count; i++) {
                if ((bitmask >> i) & 1) {
                    base[out++] = base[i]; /* keep: compact in place */
                } else {
                    cando_value_release(base[i]); /* skip: release */
                }
            }
            vm->stack_top = base + out;
            DISPATCH();
        }

        /* ── Band 18: Multi-return spreading ────────────────────────── */
        OP_CASE(OP_SPREAD_RET): {
            vm->spread_extra += vm->last_ret_count - 1;
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
