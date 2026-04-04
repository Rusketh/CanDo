/*
 * function.h -- Script function and native C function objects.
 *
 * Both script closures (OBJ_FUNCTION) and native C wrappers (OBJ_NATIVE)
 * are CdoObjects so they share the same meta-method interface.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_FUNCTION_H
#define CDO_FUNCTION_H

#include "object.h"

/* -----------------------------------------------------------------------
 * Function creation
 * --------------------------------------------------------------------- */

/*
 * cdo_function_new -- create a script closure.
 *
 * upvalues[] is deep-copied (cdo_value_copy per element); the caller
 * retains ownership of the original array.  bytecode is an opaque JIT
 * pointer stored as-is (no ownership transfer).
 */
CdoObject *cdo_function_new(u32 param_count, void *bytecode,
                             CdoValue *upvalues, u32 upvalue_count);

/*
 * cdo_native_new -- wrap a C function pointer as a CdoObject.
 *
 * fn must not be NULL.
 */
CdoObject *cdo_native_new(CdoNativeFn fn, u32 param_count);

#endif /* CDO_FUNCTION_H */
