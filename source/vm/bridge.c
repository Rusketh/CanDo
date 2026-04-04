/*
 * bridge.c -- CandoValue <-> CdoValue conversion layer.
 *
 * Must compile with gcc -std=c11.
 */

#include "bridge.h"
#include "../object/string.h"

CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h) {
    return (CdoObject *)cando_handle_get(vm->handles, h);
}

CandoValue cando_bridge_new_object(CandoVM *vm) {
    CdoObject  *obj = cdo_object_new();
    HandleIndex h   = cando_handle_alloc(vm->handles, obj);
    return cando_object_value(h);
}

CandoValue cando_bridge_new_array(CandoVM *vm) {
    CdoObject  *arr = cdo_array_new();
    HandleIndex h   = cando_handle_alloc(vm->handles, arr);
    return cando_object_value(h);
}

CdoString *cando_bridge_intern_key(CandoString *cs) {
    return cdo_string_intern(cs->data, cs->length);
}

CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v) {
    switch (v.tag) {
        case CDO_NULL:   return cando_null();
        case CDO_BOOL:   return cando_bool(v.as.boolean);
        case CDO_NUMBER: return cando_number(v.as.number);
        case CDO_STRING: {
            CandoString *s = cando_string_new(v.as.string->data,
                                               v.as.string->length);
            return cando_string_value(s);
        }
        case CDO_OBJECT:
        case CDO_ARRAY:
        case CDO_FUNCTION:
        case CDO_NATIVE: {
            HandleIndex h = cando_handle_alloc(vm->handles, v.as.object);
            return cando_object_value(h);
        }
        default: return cando_null();
    }
}

CdoValue cando_bridge_to_cdo(CandoVM *vm, CandoValue v) {
    switch ((TypeTag)v.tag) {
        case TYPE_NULL:   return cdo_null();
        case TYPE_BOOL:   return cdo_bool(v.as.boolean);
        case TYPE_NUMBER: return cdo_number(v.as.number);
        case TYPE_STRING: {
            CdoString *s = cdo_string_new(v.as.string->data,
                                           v.as.string->length);
            return cdo_string_value(s);
        }
        case TYPE_OBJECT: {
            CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
            return cdo_object_value(obj);
        }
        default: return cdo_null();
    }
}
