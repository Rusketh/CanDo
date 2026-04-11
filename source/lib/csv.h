/*
 * lib/csv.h -- CSV encode/decode standard library for Cando.
 *
 * Registers a global `csv` object with the following methods:
 *
 *   csv.parse(str, delim?, header?)   → array of arrays | array of objects
 *   csv.stringify(data, delim?, headers?) → string
 *
 * csv.parse():
 *   str    -- the CSV text to parse
 *   delim  -- optional single-character delimiter string (default: ",")
 *   header -- optional bool; if true the first row becomes field names and
 *             subsequent rows are returned as objects (default: false)
 *
 * csv.stringify():
 *   data    -- array of arrays  OR  array of objects
 *   delim   -- optional single-character delimiter string (default: ",")
 *   headers -- optional array of strings used as:
 *                · the header row written first
 *                · the key order when serialising objects
 *              If omitted and data contains objects, keys are taken from
 *              the first object in FIFO insertion order.
 *
 * All cell values returned by parse() are strings.  stringify() converts
 * cell values to strings with the following rules:
 *   null    → ""   |  bool   → "true"/"false"
 *   number  → formatted decimal  |  string  → verbatim
 *   object/array → "" (not representable in CSV)
 *
 * Fields are automatically quoted when they contain the delimiter, a double-
 * quote, a CR, or a LF.  Embedded double-quotes are escaped as "".
 * Lines are terminated with "\r\n" per RFC 4180.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_CSV_H
#define CANDO_LIB_CSV_H

#include "../vm/vm.h"

/*
 * cando_lib_csv_register -- create the `csv` global object and register
 * all methods on it.  Must be called after cdo_object_init() and after the
 * VM has been initialised with cando_vm_init().
 */
CANDO_API void cando_lib_csv_register(CandoVM *vm);

#endif /* CANDO_LIB_CSV_H */
