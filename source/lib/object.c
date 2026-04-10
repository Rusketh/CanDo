/*
 * lib/object.c -- Object standard library for Cando.
 *
 * Provides utility functions for working with plain key-value objects,
 * including thread-safe locking, shallow copy, merging, raw field access,
 * prototype manipulation, and key/value enumeration.
 *
 * Must compile with gcc -std=c11.
 */

#include "object.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/value.h"
#include <string.h>

/* Platform yield for the user-level spinlock */
#if defined(CANDO_PLATFORM_WINDOWS)
#  include <windows.h>
#  define user_lock_yield() SwitchToThread()
#elif defined(CANDO_PLATFORM_LINUX) || defined(CANDO_PLATFORM_MACOS)
#  include <sched.h>
#  define user_lock_yield() sched_yield()
#else
#  define user_lock_yield() ((void)0)
#endif

#define USER_LOCK_SPIN 64

/* -------------------------------------------------------------------------
 * Internal helper: resolve args[idx] to a plain CdoObject* (OBJ_OBJECT).
 * Returns NULL and pushes null onto the VM stack if the arg is invalid.
 * ---------------------------------------------------------------------- */
static CdoObject *resolve_object(CandoVM *vm, CandoValue *args, int argc,
                                 int idx)
{
    if (idx >= argc || !cando_is_object(args[idx]))
        return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, args[idx].as.handle);
    if (!obj || obj->kind != OBJ_OBJECT)
        return NULL;
    return obj;
}

/* =========================================================================
 * object.lock(o)
 *
 * Acquire the user-level exclusive spinlock on o.  This is completely
 * separate from the internal RW lock used by the VM for individual field
 * access, so it never causes deadlocks with internal operations.
 * Supports re-entrance by the same thread (depth counter).
 * ======================================================================= */
static int obj_lock(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    u64 me = cando_thread_id();

    /* Re-entrance: same thread already holds the user lock. */
    if (atomic_load_explicit(&obj->user_lock_id, memory_order_acquire) == me) {
        atomic_fetch_add_explicit(&obj->user_lock_depth, 1u, memory_order_relaxed);
        cando_vm_push(vm, cando_null());
        return 1;
    }

    /* Spin until we can CAS user_lock_id from 0 to me. */
    int spins = 0;
    for (;;) {
        u64 expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                &obj->user_lock_id, &expected, me,
                memory_order_acq_rel, memory_order_relaxed))
            break;
        if (++spins >= USER_LOCK_SPIN) { spins = 0; user_lock_yield(); }
    }
    atomic_store_explicit(&obj->user_lock_depth, 1u, memory_order_relaxed);

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * object.locked(o) → bool
 *
 * Returns true if o is currently held by the user-level lock (by any
 * thread, including the caller's own thread).
 * ======================================================================= */
static int obj_locked(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    if (!obj) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    bool locked = atomic_load_explicit(&obj->user_lock_id,
                                       memory_order_acquire) != 0;
    cando_vm_push(vm, cando_bool(locked));
    return 1;
}

/* =========================================================================
 * object.unlock(o)
 *
 * Release one level of the user-level exclusive lock on o.  Only the owning
 * thread may call this; silently ignores calls from non-owners.
 * ======================================================================= */
static int obj_unlock(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    /* Only the owning thread may release. */
    u64 me = cando_thread_id();
    if (atomic_load_explicit(&obj->user_lock_id, memory_order_acquire) != me) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    u32 prev = atomic_fetch_sub_explicit(&obj->user_lock_depth, 1u,
                                         memory_order_acq_rel);
    if (prev == 1) {
        /* Depth reached zero: fully release the lock. */
        atomic_store_explicit(&obj->user_lock_id, (u64)0, memory_order_release);
    }

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * object.copy(o) → object
 *
 * Returns a shallow copy of o (own fields only; prototype not copied).
 * ======================================================================= */

typedef struct {
    CdoObject *dest;
} CopyCtx;

static bool copy_field(CdoString *key, CdoValue *val, u8 flags, void *ud) {
    CopyCtx *ctx = ud;
    cdo_object_rawset(ctx->dest, key, *val, flags);
    return true;
}

static int obj_copy(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *src = resolve_object(vm, args, argc, 0);
    if (!src) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue dest_val = cando_bridge_new_object(vm);
    CdoObject *dest_obj = cando_bridge_resolve(vm, dest_val.as.handle);

    CopyCtx ctx = { dest_obj };
    cdo_object_foreach(src, copy_field, &ctx);

    cando_vm_push(vm, dest_val);
    return 1;
}

/* =========================================================================
 * object.assign(o, ...sources) → o
 *
 * Copies all own fields from each source into o (in argument order).
 * Mutates and returns o.
 * ======================================================================= */

typedef struct {
    CdoObject *dest;
} AssignCtx;

static bool assign_field(CdoString *key, CdoValue *val, u8 flags, void *ud) {
    CANDO_UNUSED(flags);
    AssignCtx *ctx = ud;
    cdo_object_rawset(ctx->dest, key, *val, FIELD_NONE);
    return true;
}

static int obj_assign(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *target = resolve_object(vm, args, argc, 0);
    if (!target) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        CdoObject *src = resolve_object(vm, args, argc, i);
        if (!src) continue;
        AssignCtx ctx = { target };
        cdo_object_foreach(src, assign_field, &ctx);
    }

    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * object.apply(o, ...sources) → new object
 *
 * Creates a new object with all fields from o then merges each source into
 * it.  Does not modify o or the sources.
 * ======================================================================= */
static int obj_apply(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *base = resolve_object(vm, args, argc, 0);
    if (!base) {
        /* No base: return empty object. */
        cando_vm_push(vm, cando_bridge_new_object(vm));
        return 1;
    }

    CandoValue dest_val = cando_bridge_new_object(vm);
    CdoObject *dest_obj = cando_bridge_resolve(vm, dest_val.as.handle);

    /* Copy base fields. */
    CopyCtx base_ctx = { dest_obj };
    cdo_object_foreach(base, copy_field, &base_ctx);

    /* Merge sources. */
    for (int i = 1; i < argc; i++) {
        CdoObject *src = resolve_object(vm, args, argc, i);
        if (!src) continue;
        AssignCtx ctx = { dest_obj };
        cdo_object_foreach(src, assign_field, &ctx);
    }

    cando_vm_push(vm, dest_val);
    return 1;
}

/* =========================================================================
 * object.get(o, key) → value
 *
 * Reads a field directly from o's own hash table, bypassing __index.
 * ======================================================================= */
static int obj_get(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj || argc < 2 || !cando_is_string(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CdoString *key = cando_bridge_intern_key(args[1].as.string);
    CdoValue   out;
    bool found = cdo_object_rawget(obj, key, &out);
    cdo_string_release(key);

    if (!found) {
        cando_vm_push(vm, cando_null());
    } else {
        cando_vm_push(vm, cando_bridge_to_cando(vm, out));
    }
    return 1;
}

/* =========================================================================
 * object.set(o, key, value) → bool
 *
 * Writes a field directly into o's own hash table, bypassing __newindex.
 * ======================================================================= */
static int obj_set(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj || argc < 3 || !cando_is_string(args[1])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }

    CdoString *key = cando_bridge_intern_key(args[1].as.string);
    CdoValue   val = cando_bridge_to_cdo(vm, args[2]);
    bool ok = cdo_object_rawset(obj, key, val, FIELD_NONE);
    cdo_string_release(key);

    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * object.setPrototype(o, proto)
 *
 * Sets o's __index field to proto, establishing proto as o's prototype.
 * Pass null to remove the prototype.
 * ======================================================================= */
static int obj_setPrototype(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    if (argc < 2 || cando_is_null(args[1])) {
        /* Remove prototype. */
        cdo_object_rawdelete(obj, g_meta_index);
    } else if (cando_is_object(args[1])) {
        CdoValue proto_cdo = cando_bridge_to_cdo(vm, args[1]);
        cdo_object_rawset(obj, g_meta_index, proto_cdo, FIELD_NONE);
    }

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * object.getPrototype(o) → object | null
 *
 * Returns o's __index field (its prototype), or null if none is set.
 * ======================================================================= */
static int obj_getPrototype(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CdoValue proto_val;
    if (!cdo_object_rawget(obj, g_meta_index, &proto_val)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    cando_vm_push(vm, cando_bridge_to_cando(vm, proto_val));
    return 1;
}

/* =========================================================================
 * object.keys(o) → array
 *
 * Returns an array of all own field names in insertion order (same as
 * the keys yielded by `for k in o`).
 * ======================================================================= */

typedef struct {
    CdoObject *arr;
} KeysCtx;

static bool collect_key(CdoString *key, CdoValue *val, u8 flags, void *ud) {
    CANDO_UNUSED(val); CANDO_UNUSED(flags);
    KeysCtx *ctx = ud;
    /* Store the interned CdoString directly as a CDO_STRING value. */
    cdo_array_push(ctx->arr, cdo_string_value(key));
    return true;
}

static int obj_keys(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, arr_val.as.handle);

    KeysCtx ctx = { arr_obj };
    cdo_object_foreach(obj, collect_key, &ctx);

    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * object.values(o) → array
 *
 * Returns an array of all own field values in insertion order (same as
 * the values yielded by `for v of o`).
 * ======================================================================= */

typedef struct {
    CdoObject *arr;
} ValsCtx;

static bool collect_val(CdoString *key, CdoValue *val, u8 flags, void *ud) {
    CANDO_UNUSED(key); CANDO_UNUSED(flags);
    ValsCtx *ctx = ud;
    /* val is a CdoValue* — push a copy directly into the result array. */
    cdo_array_push(ctx->arr, *val);
    return true;
}

static int obj_values(CandoVM *vm, int argc, CandoValue *args) {
    CdoObject *obj = resolve_object(vm, args, argc, 0);
    if (!obj) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, arr_val.as.handle);

    ValsCtx ctx = { arr_obj };
    cdo_object_foreach(obj, collect_val, &ctx);

    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_object_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, proto_val.as.handle);

    libutil_set_method(vm, proto, "lock",         obj_lock);
    libutil_set_method(vm, proto, "locked",       obj_locked);
    libutil_set_method(vm, proto, "unlock",       obj_unlock);
    libutil_set_method(vm, proto, "copy",         obj_copy);
    libutil_set_method(vm, proto, "assign",       obj_assign);
    libutil_set_method(vm, proto, "apply",        obj_apply);
    libutil_set_method(vm, proto, "get",          obj_get);
    libutil_set_method(vm, proto, "set",          obj_set);
    libutil_set_method(vm, proto, "setPrototype", obj_setPrototype);
    libutil_set_method(vm, proto, "getPrototype", obj_getPrototype);
    libutil_set_method(vm, proto, "keys",         obj_keys);
    libutil_set_method(vm, proto, "values",       obj_values);

    cando_vm_set_global(vm, "object", proto_val, true);
}
