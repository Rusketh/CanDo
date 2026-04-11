/*
 * lib/datetime.h -- Date and time standard library for Cando.
 *
 * Registers a global `datetime` object with:
 *
 *   datetime.now()                → number (epoch seconds)
 *   datetime.format(ts, fmt)      → string
 *   datetime.parse(s, fmt)        → number | null
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_DATETIME_H
#define CANDO_LIB_DATETIME_H

#include "../vm/vm.h"

CANDO_API void cando_lib_datetime_register(CandoVM *vm);

#endif /* CANDO_LIB_DATETIME_H */
