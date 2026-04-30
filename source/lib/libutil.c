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
    /* cando_vm_add_native returns null only if the underlying realloc
     * failed (the registry grows on demand and is otherwise unbounded).
     * A null sentinel would be a 0 number which OP_CALL would misinterpret
     * as a script-function PC, so abort loudly rather than corrupt state. */
    CANDO_ASSERT(cando_is_number(sentinel) &&
                 "native registry allocation failed");
    CdoString *key      = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(sentinel.as.number), FIELD_NONE);
    cdo_string_release(key);
}
