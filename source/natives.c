/*
 * natives.c -- Native function implementations for the Cando interpreter.
 *
 * Must compile with gcc -std=c11.
 */

#include "natives.h"
#include "vm/bridge.h"
#include "object/array.h"

#include <stdio.h>
#include <string.h>

/* Forward declaration: dispatch a resolved callable CdoValue meta-method.
 * Defined in vm/vm.c; declared here without pulling object/value.h into the
 * public vm.h surface. */
bool cando_vm_dispatch_callable(CandoVM *vm, const struct CdoValue *raw,
                                 CandoValue *args, u32 argc);

static bool meta_is_callable(CdoValue v)
{
    return v.tag == CDO_FUNCTION || v.tag == CDO_NATIVE ||
           v.tag == CDO_NUMBER;
}

/* =========================================================================
 * Dispatch and name tables
 * Sized to CANDO_NATIVE_MAX; unused trailing slots are zero-initialised
 * (NULL), so callers can iterate until they hit a NULL entry.
 * ===================================================================== */
CandoNativeFn cando_native_table[CANDO_NATIVE_MAX] = {
    cando_native_print,     /* index 0, sentinel -1.0 */
    cando_native_type,      /* index 1, sentinel -2.0 */
    cando_native_tostring,  /* index 2, sentinel -3.0 */
};

const char *cando_native_names[CANDO_NATIVE_MAX] = {
    "print",
    "type",
    "toString",
};

/* =========================================================================
 * print(...) -- variadic, space-separated, newline-terminated.
 * Pushes 0 return values onto the stack; returns 0.
 * ===================================================================== */
int cando_native_print(CandoVM *vm, int argc, CandoValue *args)
{
    bool first = true;
    for (int i = 0; i < argc; i++) {
        /* Arrays (including range results) are expanded element-by-element. */
        if (cando_is_object(args[i])) {
            CdoObject *obj = cando_bridge_resolve(vm, args[i].as.handle);
            if (obj && obj->kind == OBJ_ARRAY) {
                u32 len = cdo_array_len(obj);
                for (u32 j = 0; j < len; j++) {
                    if (!first) putchar(' ');
                    first = false;
                    CdoValue cv = cdo_null();
                    cdo_array_rawget_idx(obj, j, &cv);
                    CandoValue v = cando_bridge_to_cando(vm, cv);
                    char *s = cando_value_tostring(v);
                    fputs(s, stdout);
                    free(s);
                }
                continue;
            }
        }
        if (!first) putchar(' ');
        first = false;
        char *s = cando_value_tostring(args[i]);
        fputs(s, stdout);
        free(s);
    }
    putchar('\n');
    fflush(stdout);
    return 0;
}

/* =========================================================================
 * type(val) -- push the type name of val as a CandoString; returns 1.
 * If the value is an object with a __type field, return that field's value.
 * __type may be a string (returned directly) or a callable (invoked as
 * __type(val) -> string).
 * ===================================================================== */
int cando_native_type(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (cando_is_object(args[0]) && g_meta_type) {
        /* Follow the prototype (__index) chain so a class instance whose
         * class declares __type reports that class as its type. */
        CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
        CdoValue type_val;
        if (cdo_object_get(obj, g_meta_type, &type_val)) {
            if (cdo_is_string(type_val)) {
                cando_vm_push(vm, cando_bridge_to_cando(vm, type_val));
                return 1;
            }
            if (meta_is_callable(type_val)) {
                if (cando_vm_dispatch_callable(vm, &type_val, args, 1))
                    return 1;
                if (vm->has_error) return -1;
            }
        }
    }
    /* Default: built-in type tag name. */
    const char *name = cando_value_type_name((TypeTag)args[0].tag);
    u32 len = (u32)strlen(name);
    CandoString *s = cando_string_new(name, len);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* =========================================================================
 * toString(val) -- push string representation as a CandoString; returns 1.
 * If the value is an object with a __tostring meta-method, return its value
 * directly when it is a string, or invoke it when it is a callable.
 * ===================================================================== */
int cando_native_tostring(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (cando_is_object(args[0]) && g_meta_tostring) {
        CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
        CdoValue ts_val;
        if (cdo_object_get(obj, g_meta_tostring, &ts_val)) {
            if (cdo_is_string(ts_val)) {
                cando_vm_push(vm, cando_bridge_to_cando(vm, ts_val));
                return 1;
            }
            if (meta_is_callable(ts_val)) {
                if (cando_vm_dispatch_callable(vm, &ts_val, args, 1))
                    return 1;
                if (vm->has_error) return -1;
            }
        }
    }
    /* Default: built-in tostring. */
    char *s = cando_value_tostring(args[0]);
    u32 len = (u32)strlen(s);
    CandoString *cs = cando_string_new(s, len);
    free(s);
    cando_vm_push(vm, cando_string_value(cs));
    return 1;
}
