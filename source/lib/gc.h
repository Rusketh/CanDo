/*
 * lib/gc.h -- script-visible garbage collector controls.
 *
 * Registers a global `gc` object:
 *
 *   gc.collect()              → number  swept = number of objects freed
 *   gc.count()                → number  currently tracked live objects
 *   gc.threshold([n])         → number  get or set the auto-collect
 *                                       threshold; 0 disables auto-collect
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_GC_H
#define CANDO_LIB_GC_H

#include "../vm/vm.h"

CANDO_API void cando_lib_gc_register(CandoVM *vm);

#endif /* CANDO_LIB_GC_H */
