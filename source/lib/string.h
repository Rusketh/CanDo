/*
 * string.h -- Native string library for the Cando runtime.
 *
 * Registers a global `string` object AND sets it as the default prototype
 * for all string values so methods can be called with colon syntax:
 *
 *   string.toLower("Hello")        -- module style
 *   "Hello":toLower()              -- method style (self = args[0])
 *
 * All methods receive the string as args[0].
 */

#ifndef CANDO_LIB_STRING_H
#define CANDO_LIB_STRING_H

#include "../vm/vm.h"

CANDO_API void cando_lib_string_register(CandoVM *vm);

#endif /* CANDO_LIB_STRING_H */
