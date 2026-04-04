/*
 * lib/array.h -- Array standard library for Cando.
 *
 * Registers a global `array` object and provides the default array prototype.
 *
 * Methods:
 *   array.length(a)   → number
 *   array.push(a, v)  → bool
 *   array.pop(a)      → value | null
 *   array.map(a, f)   → array
 *   array.filter(a, f)→ array
 *   array.reduce(a, f, init) → value
 *   array.sort(a, f?) → array (in-place)
 */

#ifndef CANDO_LIB_ARRAY_H
#define CANDO_LIB_ARRAY_H

#include "../vm/vm.h"

void cando_lib_array_register(CandoVM *vm);

#endif /* CANDO_LIB_ARRAY_H */
