/*
 * natives.h -- Native function interface for the Cando interpreter.
 *
 * Native functions are registered as global variables with negative number
 * sentinel values: print=-1.0, type=-2.0, toString=-3.0.
 * IS_NATIVE_FN and NATIVE_INDEX macros identify and dispatch them.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_NATIVES_H
#define CANDO_NATIVES_H

#include "vm/vm.h"

/* =========================================================================
 * Native function declarations
 * ===================================================================== */

/* print(...) -- variadic; space-separated stdout, newline-terminated.
 * Pushes 0 values; returns 0. */
int cando_native_print(CandoVM *vm, int argc, CandoValue *args);

/* type(val) -- push the type name of val as a string; returns 1. */
int cando_native_type(CandoVM *vm, int argc, CandoValue *args);

/* toString(val) -- push string representation of val; returns 1. */
int cando_native_tostring(CandoVM *vm, int argc, CandoValue *args);

/* Dispatch table indexed by NATIVE_INDEX.
 * Sized to CANDO_NATIVE_MAX; unused trailing slots are NULL.
 * Iterate until a NULL entry to discover the count. */
extern CandoNativeFn cando_native_table[CANDO_NATIVE_MAX];

/* Parallel name table (NULL-terminated) for registration in main(). */
extern const char *cando_native_names[CANDO_NATIVE_MAX];

#endif /* CANDO_NATIVES_H */
