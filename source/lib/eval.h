/*
 * eval.h -- Native eval() function for the Cando runtime.
 */

#ifndef CANDO_LIB_EVAL_H
#define CANDO_LIB_EVAL_H

#include "../vm/vm.h"

/* Register the global eval() native function. */
CANDO_API void cando_lib_eval_register(CandoVM *vm);

#endif /* CANDO_LIB_EVAL_H */
