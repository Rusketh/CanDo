/*
 * lib/libutil.h -- Shared utility functions for Cando native libraries.
 *
 * Provides common argument-extraction helpers, string-push helpers, and
 * the set_method pattern used by every built-in library module.  Including
 * this header eliminates the need for each module to define its own copies.
 *
 * Simple accessors are declared static inline so the compiler can inline
 * them at every call site with zero overhead.  The helpers that perform
 * heap allocation or VM operations are real functions declared here and
 * implemented in libutil.c.
 */

#ifndef CANDO_LIB_LIBUTIL_H
#define CANDO_LIB_LIBUTIL_H

#include "../core/common.h"
#include "../core/value.h"
#include "../object/object.h"
#include "../vm/vm.h"

/* =========================================================================
 * Argument extractors — single value
 * ======================================================================= */

/*
 * libutil_arg_cstr -- return the NUL-terminated C string inside a
 * CandoValue, or NULL if the value is not TYPE_STRING.
 */
static inline const char *libutil_arg_cstr(CandoValue v)
{
    return cando_is_string(v) ? v.as.string->data : NULL;
}

/*
 * libutil_arg_str -- return the CandoString* inside a CandoValue,
 * or NULL if the value is not TYPE_STRING.
 */
static inline CandoString *libutil_arg_str(CandoValue v)
{
    return cando_is_string(v) ? v.as.string : NULL;
}

/*
 * libutil_arg_num -- return the f64 inside a CandoValue,
 * or `def` if the value is not TYPE_NUMBER.
 */
static inline f64 libutil_arg_num(CandoValue v, f64 def)
{
    return cando_is_number(v) ? v.as.number : def;
}

/* =========================================================================
 * Argument extractors — indexed (args array)
 * ======================================================================= */

/*
 * libutil_arg_cstr_at -- return the C string at args[idx], or NULL if
 * out-of-range or not a string.
 */
static inline const char *libutil_arg_cstr_at(CandoValue *args, int argc,
                                               int idx)
{
    if (idx >= argc || !cando_is_string(args[idx])) return NULL;
    return args[idx].as.string->data;
}

/*
 * libutil_arg_str_at -- return the CandoString* at args[idx], or NULL if
 * out-of-range or not a string.
 */
static inline CandoString *libutil_arg_str_at(CandoValue *args, int argc,
                                               int idx)
{
    if (idx >= argc || !cando_is_string(args[idx])) return NULL;
    return args[idx].as.string;
}

/*
 * libutil_arg_num_at -- return the number at args[idx], or `def` if
 * out-of-range or not a number.
 */
static inline f64 libutil_arg_num_at(CandoValue *args, int argc,
                                     int idx, f64 def)
{
    if (idx >= argc || !cando_is_number(args[idx])) return def;
    return args[idx].as.number;
}

/* =========================================================================
 * String push helpers
 * ======================================================================= */

/*
 * libutil_push_str -- create a new CandoString from data[0..len) and push
 * it onto the VM stack.
 */
void libutil_push_str(CandoVM *vm, const char *data, u32 len);

/*
 * libutil_push_cstr -- create a new CandoString from a NUL-terminated C
 * string and push it onto the VM stack.
 */
void libutil_push_cstr(CandoVM *vm, const char *str);

/* =========================================================================
 * Object method registration
 * ======================================================================= */

/*
 * libutil_set_method -- register fn as a native callable and store its
 * sentinel value as field `name` on obj.
 *
 * This is the standard pattern used by file.c, string.c, and every other
 * module that builds a method-table object.
 */
void libutil_set_method(CandoVM *vm, CdoObject *obj,
                        const char *name, CandoNativeFn fn);

#endif /* CANDO_LIB_LIBUTIL_H */
