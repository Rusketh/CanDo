/*
 * function.c -- Script and native function object construction.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "function.h"

CdoObject *cdo_function_new(u32 param_count, void *bytecode,
                             CdoValue *upvalues, u32 upvalue_count) {
    CdoObject *obj = cdo_obj_alloc(OBJ_FUNCTION);
    obj->fn.script.param_count   = param_count;
    obj->fn.script.bytecode      = bytecode;
    obj->fn.script.upvalue_count = upvalue_count;
    if (upvalue_count > 0 && upvalues) {
        obj->fn.script.upvalues = cando_alloc(upvalue_count * sizeof(CdoValue));
        for (u32 i = 0; i < upvalue_count; i++)
            obj->fn.script.upvalues[i] = cdo_value_copy(upvalues[i]);
    }
    return obj;
}

CdoObject *cdo_native_new(CdoNativeFn fn, u32 param_count) {
    CANDO_ASSERT(fn != NULL);
    CdoObject *obj = cdo_obj_alloc(OBJ_NATIVE);
    obj->fn.native.fn          = fn;
    obj->fn.native.param_count = param_count;
    return obj;
}
