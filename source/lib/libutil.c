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
#include <string.h>

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
