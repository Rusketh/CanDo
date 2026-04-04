/*
 * lib/math.h -- Math standard library for Cando.
 *
 * Registers a global `math` object with the following methods:
 *
 *   TODO: This List
 *
 *
 * Must compile with gcc -std=c11.
 */

 #ifndef CANDO_LIB_MATH_H
 #define CANDO_LIB_MATH_H
 
 #include "../vm/vm.h"
 
 /*
  * cando_lib_file_register -- create the `file` global object and register
  * all methods on it.  Must be called after cdo_object_init() and after the
  * VM has been initialised with cando_vm_init().
  */
 void cando_lib_math_register(CandoVM *vm);
 
 #endif /* CANDO_LIB_MATH_H */
 