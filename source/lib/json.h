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
CANDO_API void cando_lib_json_register(CandoVM *vm);

/*
 * cando_lib_json_parse_buffer -- parse a JSON document from a raw byte
 * buffer and store the resulting Cando value in *out.  Returns true on
 * success.  On failure sets a VM error (with the supplied `where` string
 * used as the prefix, e.g. "json.parse" or "include") and returns false;
 * *out is left as cando_null() in that case.
 *
 * Provided so other library code (notably include()) can reuse the JSON
 * parser without going through the VM-level json.parse native.
 */
CANDO_API bool cando_lib_json_parse_buffer(CandoVM *vm,
                                           const char *src, usize len,
                                           const char *where,
                                           CandoValue *out);

#endif /* CANDO_LIB_JSON_H */
