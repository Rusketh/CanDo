/*
 * lib/process.h -- Subprocess management standard library for Cando.
 *
 * Registers a global `process` object with:
 *
 *   process.pid()            → number
 *   process.ppid()           → number
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_PROCESS_H
#define CANDO_LIB_PROCESS_H

#include "../vm/vm.h"

CANDO_API void cando_lib_process_register(CandoVM *vm);

#endif /* CANDO_LIB_PROCESS_H */
