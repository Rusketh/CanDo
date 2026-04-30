/*
 * natives.h -- Native function interface for the Cando interpreter.
 *
 * Native functions are registered as global variables with negative number
 * sentinel values: print=-1.0, type=-2.0, toString=-3.0, inspect=-4.0.
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

/* inspect(val, depth*) -- push a debug string showing array/object contents;
 * cycle-safe.  depth = 0 (default) means unlimited; depth > 0 truncates
 * nested arrays/objects beyond that level.  Returns 1. */
int cando_native_inspect(CandoVM *vm, int argc, CandoValue *args);


/* Static dispatch table for the small set of *core* natives that ship
 * inside libcando itself (print, type, toString, inspect, ...).  Sized to
 * CANDO_NATIVE_MAX with unused trailing slots left NULL; iterate until a
 * NULL entry to discover the count.  Standard-library natives registered
 * via cando_vm_register_native() / cando_vm_add_native() do NOT live here
 * — they live in the per-VM dynamic registry, which has no fixed cap. */
extern CandoNativeFn cando_native_table[CANDO_NATIVE_MAX];

/* Parallel name table (NULL-terminated) for the core dispatch table. */
extern const char *cando_native_names[CANDO_NATIVE_MAX];

#endif /* CANDO_NATIVES_H */
