/*
 * lib/libutil.c -- Shared utility functions for Cando native libraries.
 *
 * Implements the non-trivial helpers declared in libutil.h.  The simple
 * argument-extraction helpers are static inline in the header.
 *
 * Must compile with gcc -std=c11.
 */

#include "libutil.h"
#include "../object/string.h"
#include "../vm/bridge.h"
#include <string.h>

/* =========================================================================
 * Argument validators (hard error)
 * ======================================================================= */

static void libutil_arg_error(CandoVM *vm, const char *fn_name,
                              int idx, const char *type_name)
{
    cando_vm_error(vm, "%s: argument %d must be a %s",
                   fn_name, idx + 1, type_name);
}

const char *libutil_require_cstr_at(CandoVM *vm, CandoValue *args,
                                    int argc, int idx,
                                    const char *fn_name)
{
    if (idx >= argc || !cando_is_string(args[idx])) {
        libutil_arg_error(vm, fn_name, idx, "string");
        return NULL;
    }
    return args[idx].as.string->data;
}

CandoString *libutil_require_str_at(CandoVM *vm, CandoValue *args,
                                    int argc, int idx,
                                    const char *fn_name)
{
    if (idx >= argc || !cando_is_string(args[idx])) {
        libutil_arg_error(vm, fn_name, idx, "string");
        return NULL;
    }
    return args[idx].as.string;
}

bool libutil_require_num_at(CandoVM *vm, CandoValue *args,
                            int argc, int idx,
                            const char *fn_name, f64 *out)
{
    if (idx >= argc || !cando_is_number(args[idx])) {
        libutil_arg_error(vm, fn_name, idx, "number");
        return false;
    }
    *out = args[idx].as.number;
    return true;
}

bool libutil_require_object_at(CandoVM *vm, CandoValue *args,
                               int argc, int idx,
                               const char *fn_name,
                               CdoObject **out_obj)
{
    if (idx >= argc || !cando_is_object(args[idx])) {
        libutil_arg_error(vm, fn_name, idx, "object");
        return false;
    }
    *out_obj = cando_bridge_resolve(vm, args[idx].as.handle);
    return true;
}

/* =========================================================================
 * String push helpers
 * ======================================================================= */

void libutil_push_str(CandoVM *vm, const char *data, u32 len)
{
    CandoString *s = cando_string_new(data, len);
    cando_vm_push(vm, cando_string_value(s));
}

void libutil_push_cstr(CandoVM *vm, const char *str)
{
    libutil_push_str(vm, str, (u32)strlen(str));
}

/* =========================================================================
 * Object method registration
 * ======================================================================= */

void libutil_set_method(CandoVM *vm, CdoObject *obj,
                        const char *name, CandoNativeFn fn)
{
    CandoValue sentinel = cando_vm_add_native(vm, fn);
    /* If cando_vm_add_native returns null the native table is full: the
     * sentinel number would be 0 which OP_CALL misinterprets as a script-
     * function PC, causing silent infinite loops.  Catch this loudly. */
    CANDO_ASSERT(cando_is_number(sentinel) && "native table full — increase CANDO_NATIVE_MAX");
    CdoString *key      = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(sentinel.as.number), FIELD_NONE);
    cdo_string_release(key);
}
