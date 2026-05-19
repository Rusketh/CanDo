/*
 * lib/array.c -- Array standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "array.h"
#include "libutil.h"
#include "meta.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/array.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * array.length(a) → number
 * ======================================================================= */
static int arr_length(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }
    cando_vm_push(vm, cando_number((f64)cdo_array_len(obj)));
    return 1;
}

/* =========================================================================
 * array.push(a, val) → bool           -- append to end
 * array.push(a, index, val) → bool    -- insert at index
 * ======================================================================= */
static int arr_push(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }

    if (argc >= 3 && cando_is_number(args[1])) {
        /* Insert at index. */
        u32 idx = (u32)(i64)cando_as_number(args[1]);
        CdoValue cv = cando_bridge_to_cdo(vm, args[2]);
        bool res = cdo_array_insert(obj, idx, cv);
        cando_vm_push(vm, cando_bool(res));
    } else {
        /* Append. */
        CdoValue cv = cando_bridge_to_cdo(vm, args[1]);
        bool res = cdo_array_push(obj, cv);
        cando_vm_push(vm, cando_bool(res));
    }
    return 1;
}

/* =========================================================================
 * array.pop(a) → value | null
 * ======================================================================= */
static int arr_pop(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (obj->kind != OBJ_ARRAY || obj->items_len == 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    cando_lock_write_acquire(&obj->lock);
    CdoValue val = obj->items[--obj->items_len];
    cando_lock_write_release(&obj->lock);

    cando_vm_push(vm, cando_bridge_to_cando(vm, val));
    cdo_value_release(val);
    return 1;
}

/* =========================================================================
 * array.map(a, f) → array
 * ======================================================================= */
static int arr_map(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_object(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *src = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue fn = args[1];

    CandoValue res_val = cando_bridge_new_array(vm);
    CdoObject *res_obj = cando_bridge_resolve(vm, cando_as_handle(res_val));

    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(src, i, &cv);
        CandoValue arg = cando_bridge_to_cando(vm, cv);

        int nret = cando_vm_call_value(vm, fn, &arg, 1);
        if (vm->has_error) {
            cando_value_release(arg);
            return -1;
        }

        if (nret > 0) {
            CandoValue ret = cando_vm_pop(vm);
            cdo_array_push(res_obj, cando_bridge_to_cdo(vm, ret));
            cando_value_release(ret);
        } else {
            cdo_array_push(res_obj, cdo_null());
        }
        cando_value_release(arg);
    }

    cando_vm_push(vm, res_val);
    return 1;
}

/* =========================================================================
 * array.filter(a, f) → array
 * ======================================================================= */
static int arr_filter(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_object(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *src = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue fn = args[1];

    CandoValue res_val = cando_bridge_new_array(vm);
    CdoObject *res_obj = cando_bridge_resolve(vm, cando_as_handle(res_val));

    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(src, i, &cv);
        CandoValue arg = cando_bridge_to_cando(vm, cv);

        int nret = cando_vm_call_value(vm, fn, &arg, 1);
        if (vm->has_error) {
            cando_value_release(arg);
            return -1;
        }

        if (nret > 0) {
            CandoValue ret = cando_vm_pop(vm);
            /* Simple truthiness check: not null and not false. */
            bool keep = true;
            if (cando_is_null(ret) || (cando_is_bool(ret) && !cando_as_bool(ret)))
                keep = false;

            if (keep) {
                cdo_array_push(res_obj, cando_bridge_to_cdo(vm, arg));
            }
            cando_value_release(ret);
        }
        cando_value_release(arg);
    }

    cando_vm_push(vm, res_val);
    return 1;
}

/* =========================================================================
 * array.reduce(a, f, init) → value
 * ======================================================================= */
static int arr_reduce(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_object(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *src = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue fn = args[1];
    CandoValue acc = (argc >= 3) ? cando_value_copy(args[2]) : cando_null();

    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(src, i, &cv);
        CandoValue val = cando_bridge_to_cando(vm, cv);

        CandoValue call_args[2] = { acc, val };
        int nret = cando_vm_call_value(vm, fn, call_args, 2);

        cando_value_release(val);
        cando_value_release(acc);

        if (vm->has_error) return -1;

        if (nret > 0) {
            acc = cando_vm_pop(vm);
        } else {
            acc = cando_null();
        }
    }

    cando_vm_push(vm, acc);
    return 1;
}

/* =========================================================================
 * array.splice(a, start, len*) → array  (removed elements)
 *
 * Removes up to `len` elements starting at `start` from `a` and returns
 * them as a new array.  If `len` is omitted all elements from `start` to
 * the end of the array are removed.
 * ======================================================================= */
static int arr_splice(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    u32 arr_len = cdo_array_len(obj);
    u32 start   = (argc >= 2 && cando_is_number(args[1]))
                  ? (u32)(i64)cando_as_number(args[1]) : 0;
    if (start > arr_len) start = arr_len;

    u32 count;
    if (argc >= 3 && cando_is_number(args[2])) {
        count = (u32)(i64)cando_as_number(args[2]);
        if (count > arr_len - start) count = arr_len - start;
    } else {
        count = arr_len - start;
    }

    /* Collect removed elements into a new array. */
    CandoValue result_val = cando_bridge_new_array(vm);
    CdoObject *result_obj = cando_bridge_resolve(vm, cando_as_handle(result_val));

    for (u32 i = 0; i < count; i++) {
        CdoValue removed;
        /* Always remove at `start`; successive removes shift the array. */
        if (cdo_array_remove(obj, start, &removed)) {
            cdo_array_push(result_obj, removed);
            cdo_value_release(removed);
        }
    }

    cando_vm_push(vm, result_val);
    return 1;
}

/* =========================================================================
 * array.remove(a, index) → value | null
 * ======================================================================= */
static int arr_remove(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_number(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    u32 idx = (u32)(i64)cando_as_number(args[1]);
    CdoValue removed;
    if (!cdo_array_remove(obj, idx, &removed)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cando_vm_push(vm, cando_bridge_to_cando(vm, removed));
    cdo_value_release(removed);
    return 1;
}

/* =========================================================================
 * array.copy(a) → array
 * ======================================================================= */
static int arr_copy(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *src = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue copy_val = cando_bridge_new_array(vm);
    CdoObject *copy_obj = cando_bridge_resolve(vm, cando_as_handle(copy_val));

    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(src, i, &cv))
            cdo_array_push(copy_obj, cv);
    }

    cando_vm_push(vm, copy_val);
    return 1;
}

/* =========================================================================
 * Helpers for the methods below.
 *
 * Each helper takes an `argv` window (the script-visible arguments,
 * with args[0] being the array) and returns the underlying CdoObject*
 * after kind-checking.  Returns NULL and pushes `null`/`bool false`
 * onto the VM stack if the value isn't an array; callers should just
 * `return 1` after that.
 *
 * For mutating methods we always return the array itself (chainable)
 * unless documented otherwise.
 * ======================================================================= */

static CdoObject *arr_resolve(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return NULL;
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    if (!o || o->kind != OBJ_ARRAY) return NULL;
    return o;
}

/* Convert a possibly-negative index to an absolute index, JS-Array
 * style.  Out-of-range still possible; caller validates. */
static i64 arr_norm_index(i64 idx, u32 len)
{
    if (idx < 0) idx += (i64)len;
    return idx;
}

/* =========================================================================
 * Querying: indexOf, lastIndexOf, includes, find, findIndex,
 * findLast, findLastIndex, some, every
 * ======================================================================= */

static int arr_indexOf(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2) { cando_vm_push(vm, cando_number(-1)); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_number(-1)); return 1; }
    u32 len = cdo_array_len(arr);
    i64 from = (argc >= 3 && cando_is_number(args[2]))
        ? arr_norm_index((i64)cando_as_number(args[2]), len) : 0;
    if (from < 0) from = 0;
    CdoValue needle = cando_bridge_to_cdo(vm, args[1]);
    i64 found = -1;
    for (u32 i = (u32)from; i < len; i++) {
        CdoValue cv;
        if (!cdo_array_rawget_idx(arr, i, &cv)) continue;
        if (cdo_value_equal(cv, needle)) { found = (i64)i; break; }
    }
    cdo_value_release(needle);
    cando_vm_push(vm, cando_number((f64)found));
    return 1;
}

static int arr_lastIndexOf(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2) { cando_vm_push(vm, cando_number(-1)); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_number(-1)); return 1; }
    u32 len = cdo_array_len(arr);
    i64 from = (argc >= 3 && cando_is_number(args[2]))
        ? arr_norm_index((i64)cando_as_number(args[2]), len) : (i64)len - 1;
    if (from >= (i64)len) from = (i64)len - 1;
    CdoValue needle = cando_bridge_to_cdo(vm, args[1]);
    i64 found = -1;
    for (i64 i = from; i >= 0; i--) {
        CdoValue cv;
        if (!cdo_array_rawget_idx(arr, (u32)i, &cv)) continue;
        if (cdo_value_equal(cv, needle)) { found = i; break; }
    }
    cdo_value_release(needle);
    cando_vm_push(vm, cando_number((f64)found));
    return 1;
}

static int arr_includes(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2) { cando_vm_push(vm, cando_bool(false)); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_bool(false)); return 1; }
    u32 len = cdo_array_len(arr);
    CdoValue needle = cando_bridge_to_cdo(vm, args[1]);
    bool found = false;
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        if (!cdo_array_rawget_idx(arr, i, &cv)) continue;
        if (cdo_value_equal(cv, needle)) { found = true; break; }
    }
    cdo_value_release(needle);
    cando_vm_push(vm, cando_bool(found));
    return 1;
}

/* Internal helper: walk an array, calling fn(value) per element, and
 * report either the matching index or value depending on `want_index`.
 * `direction` is +1 (forward) or -1 (reverse). */
static int arr_find_impl(CandoVM *vm, int argc, CandoValue *args,
                         bool want_index, int direction)
{
    if (argc < 2) {
        cando_vm_push(vm, want_index ? cando_number(-1) : cando_null());
        return 1;
    }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) {
        cando_vm_push(vm, want_index ? cando_number(-1) : cando_null());
        return 1;
    }
    CandoValue fn = args[1];
    u32 len = cdo_array_len(arr);
    if (len == 0) {
        cando_vm_push(vm, want_index ? cando_number(-1) : cando_null());
        return 1;
    }

    i64 start = (direction > 0) ? 0 : (i64)len - 1;
    i64 end   = (direction > 0) ? (i64)len : -1;
    for (i64 i = start; i != end; i += direction) {
        CdoValue cv;
        cdo_array_rawget_idx(arr, (u32)i, &cv);
        CandoValue v = cando_bridge_to_cando(vm, cv);
        int nret = cando_vm_call_value(vm, fn, &v, 1);
        if (vm->has_error) { cando_value_release(v); return -1; }
        bool keep = false;
        if (nret > 0) {
            CandoValue r = cando_vm_pop(vm);
            keep = !(cando_is_null(r) || (cando_is_bool(r) && !cando_as_bool(r)));
            cando_value_release(r);
        }
        if (keep) {
            if (want_index) {
                cando_value_release(v);
                cando_vm_push(vm, cando_number((f64)i));
            } else {
                cando_vm_push(vm, v);
            }
            return 1;
        }
        cando_value_release(v);
    }
    cando_vm_push(vm, want_index ? cando_number(-1) : cando_null());
    return 1;
}

static int arr_find(CandoVM *vm, int argc, CandoValue *args)
{ return arr_find_impl(vm, argc, args, false, +1); }
static int arr_findIndex(CandoVM *vm, int argc, CandoValue *args)
{ return arr_find_impl(vm, argc, args, true, +1); }
static int arr_findLast(CandoVM *vm, int argc, CandoValue *args)
{ return arr_find_impl(vm, argc, args, false, -1); }
static int arr_findLastIndex(CandoVM *vm, int argc, CandoValue *args)
{ return arr_find_impl(vm, argc, args, true, -1); }

/* some / every: short-circuit truthiness reduction. */
static int arr_some_or_every(CandoVM *vm, int argc, CandoValue *args, bool want_every)
{
    if (argc < 2) {
        cando_vm_push(vm, cando_bool(want_every));
        return 1;
    }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_bool(want_every)); return 1; }
    CandoValue fn = args[1];
    u32 len = cdo_array_len(arr);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(arr, i, &cv);
        CandoValue v = cando_bridge_to_cando(vm, cv);
        int nret = cando_vm_call_value(vm, fn, &v, 1);
        if (vm->has_error) { cando_value_release(v); return -1; }
        bool truthy = false;
        if (nret > 0) {
            CandoValue r = cando_vm_pop(vm);
            truthy = !(cando_is_null(r) || (cando_is_bool(r) && !cando_as_bool(r)));
            cando_value_release(r);
        }
        cando_value_release(v);
        if (want_every && !truthy) {
            cando_vm_push(vm, cando_bool(false));
            return 1;
        }
        if (!want_every && truthy) {
            cando_vm_push(vm, cando_bool(true));
            return 1;
        }
    }
    cando_vm_push(vm, cando_bool(want_every));
    return 1;
}

static int arr_some(CandoVM *vm, int argc, CandoValue *args)
{ return arr_some_or_every(vm, argc, args, false); }
static int arr_every(CandoVM *vm, int argc, CandoValue *args)
{ return arr_some_or_every(vm, argc, args, true); }

/* =========================================================================
 * Iteration: forEach
 * ======================================================================= */

static int arr_forEach(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue fn = args[1];
    u32 len = cdo_array_len(arr);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(arr, i, &cv);
        CandoValue call_args[2] = {
            cando_bridge_to_cando(vm, cv),
            cando_number((f64)i)
        };
        int nret = cando_vm_call_value(vm, fn, call_args, 2);
        if (vm->has_error) {
            cando_value_release(call_args[0]);
            return -1;
        }
        if (nret > 0) {
            CandoValue r = cando_vm_pop(vm);
            cando_value_release(r);
        }
        cando_value_release(call_args[0]);
    }
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Reducing: reduceRight
 * ======================================================================= */

static int arr_reduceRight(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue fn = args[1];
    u32 len = cdo_array_len(arr);
    CandoValue acc = (argc >= 3) ? cando_value_copy(args[2]) : cando_null();

    for (i64 i = (i64)len - 1; i >= 0; i--) {
        CdoValue cv;
        cdo_array_rawget_idx(arr, (u32)i, &cv);
        CandoValue val = cando_bridge_to_cando(vm, cv);
        CandoValue call_args[2] = { acc, val };
        int nret = cando_vm_call_value(vm, fn, call_args, 2);
        cando_value_release(val);
        cando_value_release(acc);
        if (vm->has_error) return -1;
        acc = (nret > 0) ? cando_vm_pop(vm) : cando_null();
    }
    cando_vm_push(vm, acc);
    return 1;
}

/* =========================================================================
 * Transforming: flat, flatMap
 * ======================================================================= */

static void flat_helper(CandoVM *vm, CdoObject *src, CdoObject *dst, int depth)
{
    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        if (!cdo_array_rawget_idx(src, i, &cv)) continue;
        /* Recurse into nested arrays.  Arrays may be tagged CDO_ARRAY
         * (the optimised tag) or CDO_OBJECT with kind == OBJ_ARRAY. */
        bool is_arr = (cv.tag == CDO_ARRAY) ||
                      (cv.tag == CDO_OBJECT && cv.as.object
                       && cv.as.object->kind == OBJ_ARRAY);
        if (depth > 0 && is_arr) {
            flat_helper(vm, cv.as.object, dst, depth - 1);
        } else {
            cdo_array_push(dst, cv);
        }
    }
}

static int arr_flat(CandoVM *vm, int argc, CandoValue *args)
{
    CdoObject *src = arr_resolve(vm, argc ? args[0] : cando_null());
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    int depth = (argc >= 2 && cando_is_number(args[1]))
        ? (int)cando_as_number(args[1]) : 1;
    if (depth < 0) depth = 0;
    if (depth > 64) depth = 64;
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    flat_helper(vm, src, out, depth);
    cando_vm_push(vm, out_val);
    return 1;
}

static int arr_flatMap(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *src = arr_resolve(vm, args[0]);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue fn = args[1];
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(src, i, &cv);
        CandoValue v = cando_bridge_to_cando(vm, cv);
        int nret = cando_vm_call_value(vm, fn, &v, 1);
        if (vm->has_error) { cando_value_release(v); return -1; }
        if (nret > 0) {
            CandoValue r = cando_vm_pop(vm);
            if (cando_is_object(r)) {
                CdoObject *ro = cando_bridge_resolve(vm, cando_as_handle(r));
                if (ro && ro->kind == OBJ_ARRAY) {
                    /* Splice the inner array's elements into out. */
                    u32 rlen = cdo_array_len(ro);
                    for (u32 j = 0; j < rlen; j++) {
                        CdoValue jv;
                        if (cdo_array_rawget_idx(ro, j, &jv))
                            cdo_array_push(out, jv);
                    }
                    cando_value_release(r);
                    cando_value_release(v);
                    continue;
                }
            }
            /* Not an array -- push the value itself (flat depth 1). */
            cdo_array_push(out, cando_bridge_to_cdo(vm, r));
            cando_value_release(r);
        }
        cando_value_release(v);
    }
    cando_vm_push(vm, out_val);
    return 1;
}

/* =========================================================================
 * Combining: concat, slice, join
 * ======================================================================= */

static int arr_concat(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *base = arr_resolve(vm, args[0]);
    if (!base) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    /* Copy base. */
    u32 blen = cdo_array_len(base);
    for (u32 i = 0; i < blen; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(base, i, &cv))
            cdo_array_push(out, cv);
    }
    /* Append each extra: if array, splice its elements; else push as-is. */
    for (int a = 1; a < argc; a++) {
        if (cando_is_object(args[a])) {
            CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(args[a]));
            if (o && o->kind == OBJ_ARRAY) {
                u32 l = cdo_array_len(o);
                for (u32 i = 0; i < l; i++) {
                    CdoValue cv;
                    if (cdo_array_rawget_idx(o, i, &cv))
                        cdo_array_push(out, cv);
                }
                continue;
            }
        }
        cdo_array_push(out, cando_bridge_to_cdo(vm, args[a]));
    }
    cando_vm_push(vm, out_val);
    return 1;
}

static int arr_slice(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *src = arr_resolve(vm, args[0]);
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    u32 len = cdo_array_len(src);
    i64 start = 0, end = (i64)len;
    if (argc >= 2 && cando_is_number(args[1]))
        start = arr_norm_index((i64)cando_as_number(args[1]), len);
    if (argc >= 3 && cando_is_number(args[2]))
        end = arr_norm_index((i64)cando_as_number(args[2]), len);
    if (start < 0) start = 0;
    if (end   > (i64)len) end = (i64)len;
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    for (i64 i = start; i < end; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(src, (u32)i, &cv))
            cdo_array_push(out, cv);
    }
    cando_vm_push(vm, out_val);
    return 1;
}

/* Convert one value to its scriptish toString form for join().
 * Numbers use %g; bool uses true/false; null becomes ""; strings
 * pass through; objects/arrays use a placeholder "[object]". */
static char *value_to_string_dup(CdoValue v, size_t *out_len)
{
    char numbuf[64];
    const char *src = "";
    size_t n = 0;
    switch (v.tag) {
        case CDO_NULL:    src = ""; n = 0; break;
        case CDO_BOOL:    src = v.as.boolean ? "true" : "false";
                          n   = v.as.boolean ? 4 : 5; break;
        case CDO_NUMBER: {
            double d = v.as.number;
            if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15) {
                n = (size_t)snprintf(numbuf, sizeof(numbuf), "%lld", (long long)d);
            } else {
                n = (size_t)snprintf(numbuf, sizeof(numbuf), "%g", d);
            }
            src = numbuf;
            break;
        }
        case CDO_STRING:
            src = v.as.string->data;
            n   = v.as.string->length;
            break;
        case CDO_OBJECT:
            if (v.as.object && v.as.object->kind == OBJ_ARRAY) {
                src = "[array]"; n = 7;
            } else {
                src = "[object]"; n = 8;
            }
            break;
        default:
            src = ""; n = 0; break;
    }
    char *out = (char *)cando_alloc(n + 1);
    memcpy(out, src, n);
    out[n] = '\0';
    if (out_len) *out_len = n;
    return out;
}

static int arr_join(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { libutil_push_str(vm, "", 0); return 1; }
    CdoObject *src = arr_resolve(vm, args[0]);
    if (!src) { libutil_push_str(vm, "", 0); return 1; }
    const char *sep = ",";
    u32 sep_len = 1;
    if (argc >= 2 && cando_is_string(args[1])) {
        CandoString *s = cando_as_string(args[1]);
        sep = s->data; sep_len = s->length;
    }
    u32 n = cdo_array_len(src);
    if (n == 0) { libutil_push_str(vm, "", 0); return 1; }

    /* Two-pass: gather string forms + total length, then concatenate. */
    char  **parts = (char **)cando_alloc(sizeof(char *) * n);
    size_t *plens = (size_t *)cando_alloc(sizeof(size_t) * n);
    size_t total = 0;
    for (u32 i = 0; i < n; i++) {
        CdoValue cv;
        cdo_array_rawget_idx(src, i, &cv);
        parts[i] = value_to_string_dup(cv, &plens[i]);
        total   += plens[i];
    }
    total += (size_t)sep_len * (n - 1);

    char *out = (char *)cando_alloc(total + 1);
    size_t off = 0;
    for (u32 i = 0; i < n; i++) {
        if (i > 0) {
            memcpy(out + off, sep, sep_len);
            off += sep_len;
        }
        memcpy(out + off, parts[i], plens[i]);
        off += plens[i];
        cando_free(parts[i]);
    }
    out[off] = '\0';
    cando_free(parts);
    cando_free(plens);
    libutil_push_str(vm, out, (u32)total);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * Mutating (chainable): reverse, sort, fill
 * Mutating (non-chainable): shift, unshift
 * ======================================================================= */

static int arr_reverse(CandoVM *vm, int argc, CandoValue *args)
{
    CdoObject *arr = arr_resolve(vm, argc ? args[0] : cando_null());
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    cando_lock_write_acquire(&arr->lock);
    u32 len = arr->items_len;
    for (u32 i = 0; i < len / 2; i++) {
        CdoValue tmp = arr->items[i];
        arr->items[i] = arr->items[len - 1 - i];
        arr->items[len - 1 - i] = tmp;
    }
    cando_lock_write_release(&arr->lock);
    cando_vm_push(vm, args[0]);     /* chainable */
    return 1;
}

/* Sort context -- thread-local because comparators can re-enter the VM. */
typedef struct SortCtx {
    CandoVM    *vm;
    CandoValue  comparator;       /* may be cando_null() for default */
    bool        had_error;
} SortCtx;

static _Thread_local SortCtx *t_sort_ctx = NULL;

static int sort_cmp_default(CdoValue a, CdoValue b)
{
    /* JS-style: stringify both and lexicographically compare. */
    size_t al, bl;
    char *as = value_to_string_dup(a, &al);
    char *bs = value_to_string_dup(b, &bl);
    size_t n = al < bl ? al : bl;
    int rc = memcmp(as, bs, n);
    if (rc == 0) rc = (al < bl) ? -1 : (al > bl) ? 1 : 0;
    cando_free(as); cando_free(bs);
    return rc;
}

static int sort_qsort_cmp(const void *pa, const void *pb)
{
    SortCtx *ctx = t_sort_ctx;
    if (ctx->had_error) return 0;
    const CdoValue *a = (const CdoValue *)pa;
    const CdoValue *b = (const CdoValue *)pb;
    if (cando_is_null(ctx->comparator)) {
        return sort_cmp_default(*a, *b);
    }
    CandoValue av = cando_bridge_to_cando(ctx->vm, *a);
    CandoValue bv = cando_bridge_to_cando(ctx->vm, *b);
    CandoValue call_args[2] = { av, bv };
    int nret = cando_vm_call_value(ctx->vm, ctx->comparator, call_args, 2);
    int rc = 0;
    if (ctx->vm->has_error) ctx->had_error = true;
    else if (nret > 0) {
        CandoValue r = cando_vm_pop(ctx->vm);
        if (cando_is_number(r)) {
            double d = cando_as_number(r);
            rc = (d < 0) ? -1 : (d > 0) ? 1 : 0;
        }
        cando_value_release(r);
    }
    cando_value_release(av);
    cando_value_release(bv);
    return rc;
}

static int arr_sort(CandoVM *vm, int argc, CandoValue *args)
{
    CdoObject *arr = arr_resolve(vm, argc ? args[0] : cando_null());
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    SortCtx ctx;
    ctx.vm = vm;
    ctx.comparator = (argc >= 2) ? args[1] : cando_null();
    ctx.had_error = false;
    SortCtx *prev = t_sort_ctx;
    t_sort_ctx = &ctx;
    cando_lock_write_acquire(&arr->lock);
    qsort(arr->items, arr->items_len, sizeof(CdoValue), sort_qsort_cmp);
    cando_lock_write_release(&arr->lock);
    t_sort_ctx = prev;
    if (ctx.had_error) return -1;
    cando_vm_push(vm, args[0]);
    return 1;
}

static int arr_shift(CandoVM *vm, int argc, CandoValue *args)
{
    CdoObject *arr = arr_resolve(vm, argc ? args[0] : cando_null());
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    CdoValue removed;
    if (!cdo_array_remove(arr, 0, &removed)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cando_vm_push(vm, cando_bridge_to_cando(vm, removed));
    cdo_value_release(removed);
    return 1;
}

static int arr_unshift(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_number(0)); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_number(0)); return 1; }
    /* Insert each new value at the front, preserving argument order. */
    for (int i = argc - 1; i >= 1; i--) {
        CdoValue v = cando_bridge_to_cdo(vm, args[i]);
        cdo_array_insert(arr, 0, v);
        cdo_value_release(v);
    }
    cando_vm_push(vm, cando_number((f64)cdo_array_len(arr)));
    return 1;
}

static int arr_fill(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    u32 len = cdo_array_len(arr);
    i64 start = (argc >= 3 && cando_is_number(args[2]))
        ? arr_norm_index((i64)cando_as_number(args[2]), len) : 0;
    i64 end   = (argc >= 4 && cando_is_number(args[3]))
        ? arr_norm_index((i64)cando_as_number(args[3]), len) : (i64)len;
    if (start < 0) start = 0;
    if (end > (i64)len) end = (i64)len;
    CdoValue v = cando_bridge_to_cdo(vm, args[1]);
    for (i64 i = start; i < end; i++) {
        cdo_array_rawset_idx(arr, (u32)i, v);
    }
    cdo_value_release(v);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Other: at, unique, intersection, union, difference
 * ======================================================================= */

static int arr_at(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_number(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CdoObject *arr = arr_resolve(vm, args[0]);
    if (!arr) { cando_vm_push(vm, cando_null()); return 1; }
    u32 len = cdo_array_len(arr);
    i64 idx = arr_norm_index((i64)cando_as_number(args[1]), len);
    if (idx < 0 || idx >= (i64)len) { cando_vm_push(vm, cando_null()); return 1; }
    CdoValue cv;
    cdo_array_rawget_idx(arr, (u32)idx, &cv);
    cando_vm_push(vm, cando_bridge_to_cando(vm, cv));
    return 1;
}

/* Push value onto `dst` only if not already present (via cdo_value_equal). */
static void push_if_absent(CdoObject *dst, CdoValue v)
{
    u32 n = cdo_array_len(dst);
    for (u32 i = 0; i < n; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(dst, i, &cv) && cdo_value_equal(cv, v)) return;
    }
    cdo_array_push(dst, v);
}

static bool array_contains(CdoObject *arr, CdoValue v)
{
    u32 n = cdo_array_len(arr);
    for (u32 i = 0; i < n; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(arr, i, &cv) && cdo_value_equal(cv, v)) return true;
    }
    return false;
}

static int arr_unique(CandoVM *vm, int argc, CandoValue *args)
{
    CdoObject *src = arr_resolve(vm, argc ? args[0] : cando_null());
    if (!src) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    u32 len = cdo_array_len(src);
    for (u32 i = 0; i < len; i++) {
        CdoValue cv;
        if (cdo_array_rawget_idx(src, i, &cv)) push_if_absent(out, cv);
    }
    cando_vm_push(vm, out_val);
    return 1;
}

static int arr_setop(CandoVM *vm, int argc, CandoValue *args, int op)
{
    /* op: 0 = intersection, 1 = union, 2 = difference */
    if (argc < 2) { cando_vm_push(vm, cando_null()); return 1; }
    CdoObject *a = arr_resolve(vm, args[0]);
    CdoObject *b = arr_resolve(vm, args[1]);
    if (!a || !b) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue out_val = cando_bridge_new_array(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));
    if (op == 0) {
        u32 alen = cdo_array_len(a);
        for (u32 i = 0; i < alen; i++) {
            CdoValue cv;
            if (cdo_array_rawget_idx(a, i, &cv)
                && array_contains(b, cv))
                push_if_absent(out, cv);
        }
    } else if (op == 1) {
        u32 alen = cdo_array_len(a);
        for (u32 i = 0; i < alen; i++) {
            CdoValue cv;
            if (cdo_array_rawget_idx(a, i, &cv)) push_if_absent(out, cv);
        }
        u32 blen = cdo_array_len(b);
        for (u32 i = 0; i < blen; i++) {
            CdoValue cv;
            if (cdo_array_rawget_idx(b, i, &cv)) push_if_absent(out, cv);
        }
    } else {
        u32 alen = cdo_array_len(a);
        for (u32 i = 0; i < alen; i++) {
            CdoValue cv;
            if (cdo_array_rawget_idx(a, i, &cv)
                && !array_contains(b, cv))
                push_if_absent(out, cv);
        }
    }
    cando_vm_push(vm, out_val);
    return 1;
}

static int arr_intersection(CandoVM *vm, int argc, CandoValue *args)
{ return arr_setop(vm, argc, args, 0); }
static int arr_union(CandoVM *vm, int argc, CandoValue *args)
{ return arr_setop(vm, argc, args, 1); }
static int arr_difference(CandoVM *vm, int argc, CandoValue *args)
{ return arr_setop(vm, argc, args, 2); }

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry array_methods[] = {
    /* Existing */
    { "length",        arr_length        },
    { "push",          arr_push          },
    { "pop",           arr_pop           },
    { "splice",        arr_splice        },
    { "remove",        arr_remove        },
    { "copy",          arr_copy          },
    { "map",           arr_map           },
    { "filter",        arr_filter        },
    { "reduce",        arr_reduce        },

    /* Querying */
    { "indexOf",       arr_indexOf       },
    { "lastIndexOf",   arr_lastIndexOf   },
    { "includes",      arr_includes      },
    { "contains",      arr_includes      },  /* alias */
    { "find",          arr_find          },
    { "findIndex",     arr_findIndex     },
    { "findLast",      arr_findLast      },
    { "findLastIndex", arr_findLastIndex },
    { "some",          arr_some          },
    { "every",         arr_every         },

    /* Iteration & reduction */
    { "forEach",       arr_forEach       },
    { "reduceRight",   arr_reduceRight   },

    /* Transforming */
    { "flat",          arr_flat          },
    { "flatMap",       arr_flatMap       },

    /* Combining */
    { "concat",        arr_concat        },
    { "slice",         arr_slice         },
    { "join",          arr_join          },

    /* Mutating */
    { "reverse",       arr_reverse       },
    { "sort",          arr_sort          },
    { "shift",         arr_shift         },
    { "unshift",       arr_unshift       },
    { "fill",          arr_fill          },

    /* Other */
    { "at",            arr_at            },
    { "unique",        arr_unique        },
    { "intersection",  arr_intersection  },
    { "union",         arr_union         },
    { "difference",    arr_difference    },
};

void cando_lib_array_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, cando_as_handle(proto_val));

    libutil_register_methods(vm, proto, array_methods,
                             CANDO_ARRAY_LEN(array_methods));

    cando_vm_set_global(vm, "array", proto_val, true);
    vm->array_proto = proto_val;

    /* Mirror onto `_meta.array` so user scripts can extend the prototype
     * via either name (`array.foo = ...` or `_meta.array.foo = ...`). */
    cando_lib_meta_register(vm);
    cando_lib_meta_set(vm, "array", proto);
}
