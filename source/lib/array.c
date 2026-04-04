/*
 * lib/array.c -- Array standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "array.h"
#include "libutil.h"
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
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }
    cando_vm_push(vm, cando_number((f64)cdo_array_len(obj)));
    return 1;
}

/* =========================================================================
 * array.push(a, val) → bool
 * ======================================================================= */
static int arr_push(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    if (obj->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoValue cv = cando_bridge_to_cdo(vm, args[1]);
    bool res = cdo_array_push(obj, cv);
    cando_vm_push(vm, cando_bool(res));
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
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
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
    CdoObject *src = cando_bridge_resolve(vm, args[0].as.handle);
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue fn = args[1];

    CandoValue res_val = cando_bridge_new_array(vm);
    CdoObject *res_obj = cando_bridge_resolve(vm, res_val.as.handle);

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
    CdoObject *src = cando_bridge_resolve(vm, args[0].as.handle);
    if (src->kind != OBJ_ARRAY) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue fn = args[1];

    CandoValue res_val = cando_bridge_new_array(vm);
    CdoObject *res_obj = cando_bridge_resolve(vm, res_val.as.handle);

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
            if (cando_is_null(ret) || (cando_is_bool(ret) && !ret.as.boolean))
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
    CdoObject *src = cando_bridge_resolve(vm, args[0].as.handle);
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
 * Registration
 * ======================================================================= */

void cando_lib_array_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, proto_val.as.handle);

    libutil_set_method(vm, proto, "length", arr_length);
    libutil_set_method(vm, proto, "push",   arr_push);
    libutil_set_method(vm, proto, "pop",    arr_pop);
    libutil_set_method(vm, proto, "map",    arr_map);
    libutil_set_method(vm, proto, "filter", arr_filter);
    libutil_set_method(vm, proto, "reduce", arr_reduce);

    cando_vm_set_global(vm, "array", proto_val, true);
    vm->array_proto = proto_val;
}
