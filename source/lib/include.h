/*
 * lib/include.h -- Native include(path) function for Cando.
 *
 * include(path) -- load a Cando script or binary extension module.
 *
 *   path  (string) -- file to load.  May be:
 *     - An absolute path ("/home/user/lib/mod.cdo")
 *     - A path relative to the calling script's directory ("./utils.cdo",
 *       "../shared/helpers.cdo")
 *
 *   Script modules (.cdo files):
 *     The file is compiled in eval mode and executed.  Whatever the module
 *     returns via a top-level RETURN statement (or the value of its last
 *     expression) is the value returned by include().
 *
 *   Binary modules (.so / .dylib / .dll files):
 *     The shared library is loaded with dlopen() and the symbol
 *     cando_module_init(CandoVM *vm) → CandoValue is called.
 *     That function is responsible for registering any native functions
 *     and returning the module's exported value.
 *
 *   Caching:
 *     Every path is resolved to its canonical absolute form (realpath).
 *     The first load executes the module; subsequent include() calls with
 *     the same resolved path return the cached CandoValue without
 *     re-executing — identical to Node.js require() semantics.
 */

#ifndef CANDO_LIB_INCLUDE_H
#define CANDO_LIB_INCLUDE_H

#include "../vm/vm.h"

/*
 * cando_lib_include_register -- register include() as a global function
 * in the given VM.
 */
CANDO_API void cando_lib_include_register(CandoVM *vm);

#endif /* CANDO_LIB_INCLUDE_H */
