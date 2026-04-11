/*
 * lib/array.h -- Array standard library for Cando.
 *
 * Registers a global `array` object and provides the default array prototype.
 *
 * Methods:
 *   array.length(a)             → number
 *   array.push(a, v)            → bool  (append)
 *   array.push(a, index, v)     → bool  (insert at index)
 *   array.pop(a)                → value | null
 *   array.splice(a, start, len*)→ array (removed elements)
 *   array.remove(a, index)      → value | null
 *   array.copy(a)               → array
 *   array.map(a, f)             → array
 *   array.filter(a, f)          → array
 *   array.reduce(a, f, init)    → value
 */

#ifndef CANDO_LIB_ARRAY_H
#define CANDO_LIB_ARRAY_H

#include "../vm/vm.h"

CANDO_API void cando_lib_array_register(CandoVM *vm);

#endif /* CANDO_LIB_ARRAY_H */
