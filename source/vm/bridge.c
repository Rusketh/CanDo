/*
 * bridge.c -- CandoValue <-> CdoValue conversion layer.
 *
 * Must compile with gcc -std=c11.
 */

#include "bridge.h"
#include "../object/string.h"
#include "../object/thread.h"

CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h) {
    return (CdoObject *)cando_handle_get(vm->handles, h);
}

/* Read whichever handle_idx field a tracked heap object stores, by
 * dispatching on the shared `kind` byte at offset 16.  OBJ_THREAD has
 * a different layout from CdoObject past that prefix.                */
static HandleIndex bridge_existing_handle(void *p) {
    u8 kind = *((u8 *)p + 16);
    if (kind == OBJ_THREAD) return ((CdoThread *)p)->handle_idx;
    return ((CdoObject *)p)->handle_idx;
}

HandleIndex cando_bridge_track_obj(CandoVM *vm, CdoObject *obj) {
    HandleIndex h   = cando_handle_alloc(vm->handles, obj);
    obj->handle_idx = h;
    return h;
}

CandoValue cando_bridge_new_object(CandoVM *vm) {
    CdoObject  *obj = cdo_object_new();
    HandleIndex h   = cando_bridge_track_obj(vm, obj);
    return cando_object_value(h);
}

CandoValue cando_bridge_new_array(CandoVM *vm) {
    CdoObject  *arr = cdo_array_new();
    HandleIndex h   = cando_bridge_track_obj(vm, arr);
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
            /* If the underlying object already has a handle (it usually
             * does, set at allocation time), reuse it -- otherwise the
             * GC sweep would only reclaim ONE handle per object even
             * when several CandoValues reference it.  Allocate a fresh
             * handle only for objects that haven't been tracked yet
             * (legacy paths from before Stage 1).                      */
            HandleIndex existing = bridge_existing_handle(v.as.object);
            if (existing != CANDO_INVALID_HANDLE)
                return cando_object_value(existing);
            HandleIndex h = cando_handle_alloc(vm->handles, v.as.object);
            ((CdoObject *)v.as.object)->handle_idx = h;
            return cando_object_value(h);
        }
        default: return cando_null();
    }
}

CdoValue cando_bridge_to_cdo(CandoVM *vm, CandoValue v) {
    switch (cando_value_tag(v)) {
        case TYPE_NULL:   return cdo_null();
        case TYPE_BOOL:   return cdo_bool(cando_as_bool(v));
        case TYPE_NUMBER: return cdo_number(cando_as_number(v));
        case TYPE_STRING: {
            CandoString *cs = cando_as_string(v);
            CdoString   *s  = cdo_string_new(cs->data, cs->length);
            return cdo_string_value(s);
        }
        case TYPE_OBJECT: {
            CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
            switch (obj->kind) {
                case OBJ_ARRAY:    return cdo_array_value(obj);
                case OBJ_FUNCTION: return cdo_function_value(obj);
                case OBJ_NATIVE:   return cdo_native_value(obj);
                default:           return cdo_object_value(obj);
            }
        }
        default: return cdo_null();
    }
}
