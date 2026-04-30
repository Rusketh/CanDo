/*
 * lib/yaml.h -- YAML encode/decode standard library for Cando.
 *
 * Registers a global `yaml` object with the following methods:
 *
 *   yaml.parse(str)              → value | null
 *   yaml.stringify(val, indent?) → string
 *
 * yaml.parse() maps YAML constructs to native Cando values:
 *   block mapping / flow mapping        → object
 *   block sequence / flow sequence      → array
 *   plain / single-quoted / double-     → string  (heuristic typing rules
 *     quoted scalars                       turn unquoted scalars matching
 *                                          the integer/float grammar into
 *                                          numbers; "true"/"false"/"yes"/
 *                                          "no" become bool; "null"/"~"/""
 *                                          become null)
 *   literal (|) / folded (>) blocks     → string
 *
 * yaml.stringify() serialises a Cando value to a YAML 1.2 compatible
 * document.  Objects render as block mappings, arrays as block sequences,
 * scalars are quoted only when ambiguity rules require it.  The optional
 * `indent` argument controls the per-level indentation (default: 2,
 * clamped to 1..16).
 *
 * Both interfaces support nesting to arbitrary depth and are independent
 * of any external YAML library; the implementation is self-contained.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_YAML_H
#define CANDO_LIB_YAML_H

#include "../vm/vm.h"

/*
 * cando_lib_yaml_register -- create the `yaml` global object and register
 * all methods on it.  Must be called after cdo_object_init() and after the
 * VM has been initialised with cando_vm_init().
 */
CANDO_API void cando_lib_yaml_register(CandoVM *vm);

/*
 * cando_lib_yaml_parse_buffer -- parse a YAML document from a raw byte
 * buffer and store the resulting Cando value in *out.  Returns true on
 * success.  On failure sets a VM error (with the supplied `where` string
 * used as the prefix, e.g. "yaml.parse" or "include") and returns false;
 * *out is left as cando_null() in that case.
 *
 * Provided so other library code (notably include()) can reuse the YAML
 * parser without going through the VM-level yaml.parse native.
 */
CANDO_API bool cando_lib_yaml_parse_buffer(CandoVM *vm,
                                           const char *src, usize len,
                                           const char *where,
                                           CandoValue *out);

#endif /* CANDO_LIB_YAML_H */
