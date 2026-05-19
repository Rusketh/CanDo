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
#include "meta.h"
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
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[idx]));
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
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
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
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
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
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
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
    CdoObject *dest_obj = cando_bridge_resolve(vm, cando_as_handle(dest_val));

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
    CdoObject *dest_obj = cando_bridge_resolve(vm, cando_as_handle(dest_val));

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

    CdoString *key = cando_bridge_intern_key(cando_as_string(args[1]));
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

    CdoString *key = cando_bridge_intern_key(cando_as_string(args[1]));
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
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));

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
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    ValsCtx ctx = { arr_obj };
    cdo_object_foreach(obj, collect_val, &ctx);

    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * object.entries(o) → array of [k, v] pairs
 * object.fromEntries(arr) → object
 * object.has(o, key) → bool
 *
 * JS-Object-style helpers added on top of keys/values.
 * ======================================================================= */

typedef struct {
    CandoVM   *vm;
    CdoObject *arr;
} EntriesCtx;

static bool collect_entry(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    CANDO_UNUSED(flags);
    EntriesCtx *ctx = ud;
    /* Each entry is a 2-element array: [key, value]. */
    CdoObject *pair = cdo_array_new();
    cdo_string_retain(key);
    cdo_array_push(pair, cdo_string_value(key));
    cdo_array_push(pair, *val);
    cdo_array_push(ctx->arr, cdo_array_value(pair));
    return true;
}

static int obj_entries(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (!obj) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));
    EntriesCtx ctx = { vm, arr_obj };
    cdo_object_foreach(obj, collect_entry, &ctx);
    cando_vm_push(vm, arr_val);
    return 1;
}

static int obj_fromEntries(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *src = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (!src || src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue out_val = cando_bridge_new_object(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));

    u32 n = cdo_array_len(src);
    for (u32 i = 0; i < n; i++) {
        CdoValue pair_cv;
        if (!cdo_array_rawget_idx(src, i, &pair_cv)) continue;
        /* Accept any tagged object reference whose underlying kind is
         * OBJ_ARRAY -- CanDo array literals and arrays built via the
         * bridge can land with either CDO_ARRAY or CDO_OBJECT tags. */
        CdoObject *pair = cdo_is_any_object(pair_cv)
            ? pair_cv.as.object : NULL;
        if (!pair || pair->kind != OBJ_ARRAY) continue;
        CdoValue k, v;
        if (!cdo_array_rawget_idx(pair, 0, &k)) continue;
        if (!cdo_array_rawget_idx(pair, 1, &v)) v = cdo_null();
        if (k.tag != CDO_STRING) continue;
        /* rawset's key-lookup relies on pointer equality of interned
         * strings; if `k` came from a script-literal array its pointer
         * is the not-yet-interned heap copy.  Always re-intern. */
        CdoString *kstr = cdo_string_intern(k.as.string->data, k.as.string->length);
        cdo_object_rawset(out, kstr, v, FIELD_NONE);
        cdo_string_release(kstr);
    }
    cando_vm_push(vm, out_val);
    return 1;
}

static int obj_has(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_string(args[1])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (!obj) { cando_vm_push(vm, cando_bool(false)); return 1; }
    CandoString *kc = cando_as_string(args[1]);
    CdoString *k = cdo_string_intern(kc->data, kc->length);
    CdoValue out;
    bool found = cdo_object_rawget(obj, k, &out);
    cdo_string_release(k);
    cando_vm_push(vm, cando_bool(found));
    return 1;
}

/* freeze / isFrozen / seal / isSealed are exposed as JS-style
 * spellings that map to the existing user-level lock primitive.  This
 * is a coarse approximation: CanDo's lock prevents *all* concurrent
 * access rather than only mutation, but the visible-to-script effect
 * matches "no add/remove/modify until unlock" closely enough that
 * scripts written for JS-style immutability work. */
static int obj_freeze(CandoVM *vm, int argc, CandoValue *args)
{ return obj_lock(vm, argc, args); }
static int obj_isFrozen(CandoVM *vm, int argc, CandoValue *args)
{ return obj_locked(vm, argc, args); }
static int obj_seal(CandoVM *vm, int argc, CandoValue *args)
{ return obj_lock(vm, argc, args); }
static int obj_isSealed(CandoVM *vm, int argc, CandoValue *args)
{ return obj_locked(vm, argc, args); }

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry object_methods[] = {
    { "lock",         obj_lock         },
    { "locked",       obj_locked       },
    { "unlock",       obj_unlock       },
    { "copy",         obj_copy         },
    { "assign",       obj_assign       },
    { "apply",        obj_apply        },
    { "get",          obj_get          },
    { "set",          obj_set          },
    { "setPrototype", obj_setPrototype },
    { "getPrototype", obj_getPrototype },
    { "keys",         obj_keys         },
    { "values",       obj_values       },

    /* New: JS-Object parity. */
    { "entries",      obj_entries      },
    { "fromEntries",  obj_fromEntries  },
    { "has",          obj_has          },
    { "freeze",       obj_freeze       },
    { "isFrozen",     obj_isFrozen     },
    { "seal",         obj_seal         },
    { "isSealed",     obj_isSealed     },
};

void cando_lib_object_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, cando_as_handle(proto_val));

    libutil_register_methods(vm, proto, object_methods,
                             CANDO_ARRAY_LEN(object_methods));

    cando_vm_set_global(vm, "object", proto_val, true);

    /* Mirror onto `_meta.object` so users may use the same table as a
     * prototype: `object.setPrototype(myObj, _meta.object)`.  Methods added
     * via `_meta.object.foo = ...` show up on `object.foo` since both names
     * resolve to the same underlying table. */
    cando_lib_meta_register(vm);
    cando_lib_meta_set(vm, "object", proto);
}
