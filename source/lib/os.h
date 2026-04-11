/*
 * lib/os.h -- Operating System interface standard library for Cando.
 *
 * Registers a global `os` object with the following methods:
 *
 *   os.getenv(name)       → string | null
 *   os.setenv(name, val)  → bool
 *   os.execute(command)   → number (exit code)
 *   os.exit(code)         → (terminates process)
 *   os.time()             → number (epoch seconds)
 *   os.clock()            → number (CPU seconds)
 *   os.name               → string ("unix", "windows", etc.)
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_OS_H
#define CANDO_LIB_OS_H

#include "../vm/vm.h"

/*
 * cando_lib_os_register -- create the `os` global object and register
 * all methods on it.
 */
CANDO_API void cando_lib_os_register(CandoVM *vm);

#endif /* CANDO_LIB_OS_H */
