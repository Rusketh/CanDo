/*
 * lib/json.h -- JSON encode/decode standard library for Cando.
 *
 * Registers a global `json` object with the following methods:
 *
 *   json.parse(str)             → value | null
 *   json.stringify(val, indent?) → string
 *
 * json.parse() maps JSON types to native Cando values:
 *   JSON object  → object
 *   JSON array   → array
 *   JSON string  → string
 *   JSON number  → number
 *   JSON true    → true
 *   JSON false   → false
 *   JSON null    → null
 *
 * json.stringify() serialises a Cando value to a JSON string.
 *   indent: optional number of spaces for pretty-printing (default: compact).
 *   Functions/natives are serialised as null.
 *   Returns an empty string if the buffer allocation fails.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_JSON_H
#define CANDO_LIB_JSON_H

#include "../vm/vm.h"

/*
 * cando_lib_json_register -- create the `json` global object and register
 * all methods on it.  Must be called after cdo_object_init() and after the
 * VM has been initialised with cando_vm_init().
 */
void cando_lib_json_register(CandoVM *vm);

#endif /* CANDO_LIB_JSON_H */
