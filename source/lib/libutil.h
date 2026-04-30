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
 * Argument validators -- raise vm_error and return NULL/false on miss
 *
 * Soft fallbacks (libutil_arg_*_at) return NULL or a default value when
 * the argument is missing/wrong-type, leaving the caller to decide what
 * to do.  These hard variants raise a uniform vm_error with the form
 *   "<fn_name>: argument <idx+1> must be a <type>"
 * and return NULL/false so the caller can immediately `return -1`.
 *
 * Typical use:
 *
 *     const char *path = libutil_require_cstr_at(vm, args, argc, 0,
 *                                                "file.read");
 *     if (!path) return -1;
 *
 *     CdoObject *obj;
 *     if (!libutil_require_object_at(vm, args, argc, 0,
 *                                    "object.copy", &obj))
 *         return -1;
 *
 * The error message is a best-effort default; libraries that need a
 * more specific message can still write their own validator.
 * ======================================================================= */

CANDO_API const char *libutil_require_cstr_at(CandoVM *vm, CandoValue *args,
                                              int argc, int idx,
                                              const char *fn_name);

CANDO_API CandoString *libutil_require_str_at(CandoVM *vm, CandoValue *args,
                                              int argc, int idx,
                                              const char *fn_name);

CANDO_API bool libutil_require_num_at(CandoVM *vm, CandoValue *args,
                                      int argc, int idx,
                                      const char *fn_name, f64 *out);

CANDO_API bool libutil_require_object_at(CandoVM *vm, CandoValue *args,
                                         int argc, int idx,
                                         const char *fn_name,
                                         CdoObject **out_obj);

/* =========================================================================
 * String push helpers
 * ======================================================================= */

/*
 * libutil_push_str -- create a new CandoString from data[0..len) and push
 * it onto the VM stack.
 */
CANDO_API void libutil_push_str(CandoVM *vm, const char *data, u32 len);

/*
 * libutil_push_cstr -- create a new CandoString from a NUL-terminated C
 * string and push it onto the VM stack.
 */
CANDO_API void libutil_push_cstr(CandoVM *vm, const char *str);

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
CANDO_API void libutil_set_method(CandoVM *vm, CdoObject *obj,
                        const char *name, CandoNativeFn fn);

/*
 * LibutilMethodEntry -- one row in a static method table.
 * A NULL `name` terminates the table when used with the variadic-loop
 * form below; the count form uses an explicit length and ignores name.
 */
typedef struct {
    const char    *name;
    CandoNativeFn  fn;
} LibutilMethodEntry;

/*
 * libutil_register_methods -- bulk variant of libutil_set_method.
 *
 * Walks `entries[0..count)` and registers each row.  The recommended
 * pattern is to define a static const table next to a library's
 * register function:
 *
 *     static const LibutilMethodEntry mylib_methods[] = {
 *         { "foo", mylib_foo },
 *         { "bar", mylib_bar },
 *     };
 *     libutil_register_methods(vm, obj, mylib_methods,
 *                              CANDO_ARRAY_LEN(mylib_methods));
 *
 * Adding a new method becomes one new row instead of one row + one
 * matching libutil_set_method call.
 */
CANDO_API void libutil_register_methods(CandoVM *vm, CdoObject *obj,
                                        const LibutilMethodEntry *entries,
                                        usize count);

#endif /* CANDO_LIB_LIBUTIL_H */
