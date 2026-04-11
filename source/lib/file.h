/*
 * lib/file.h -- File I/O standard library for Cando.
 *
 * Registers a global `file` object with the following methods:
 *
 *   file.read(path, encoding?)      → string | null
 *   file.write(path, data, encoding?) → bool
 *   file.append(path, data, encoding?) → bool
 *   file.exists(path)               → bool
 *   file.delete(path)               → bool
 *   file.copy(src, dst)             → bool
 *   file.move(src, dst)             → bool
 *   file.size(path)                 → number (bytes) | null
 *   file.lines(path, encoding?)     → array of strings | null
 *   file.mkdir(path)                → bool
 *   file.list(path)                 → array of strings | null
 *
 * encoding defaults to "utf8"; "binary" reads/writes raw bytes.
 * On error, read/size/lines/list return null; write/append/etc. return false.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_FILE_H
#define CANDO_LIB_FILE_H

#include "../vm/vm.h"

/*
 * cando_lib_file_register -- create the `file` global object and register
 * all methods on it.  Must be called after cdo_object_init() and after the
 * VM has been initialised with cando_vm_init().
 */
CANDO_API void cando_lib_file_register(CandoVM *vm);

#endif /* CANDO_LIB_FILE_H */
