/*
 * src/core/cando_compat.h -- conditional libcando include / test stub.
 *
 * In production (FORMS_MODULE_TEST_BUILD undefined) this pulls the
 * real libcando headers so extension code can use CandoVM, CdoObject,
 * CdoString, CandoValue, the bridge helpers, etc.
 *
 * In the test build the cando headers are unavailable; we provide
 * just enough type aliases + stub helpers so any forms-module TU that
 * mentions these types compiles standalone.  The stubs are
 * deliberately useless at runtime -- the test harness exercises the
 * pure-C helpers, never the libcando-bound paths.
 *
 * Lifted out of forms_module.c so every src/core TU (slots, manager,
 * ...) that mentions CandoValue or libcando types shares one source
 * of truth.
 */

#ifndef CANDO_FORMS_CORE_CANDO_COMPAT_H
#define CANDO_FORMS_CORE_CANDO_COMPAT_H

#ifndef FORMS_MODULE_TEST_BUILD
#  include <cando.h>
#  include "vm/bridge.h"
#  include "object/object.h"
#  include "object/string.h"
#  include "object/value.h"
#  include "lib/libutil.h"
#  include "lib/meta.h"
#else
#  include <stdint.h>
#  include <stdbool.h>
#  ifndef FORMS_CANDO_COMPAT_TEST_STUBS_DEFINED
#    define FORMS_CANDO_COMPAT_TEST_STUBS_DEFINED 1
   typedef double   f64;
   typedef uint32_t u32;
   typedef struct CandoVM     CandoVM;
   typedef struct CdoObject   CdoObject;
   typedef struct CdoString   CdoString;
   /* Stub CandoString carries just enough surface (data + length) to
    * let parsers that look at `v.as.string->data` / `->length`
    * compile in both modes.  Production CandoString has a richer
    * layout in source/core/value.h; the parsers only touch these
    * two fields. */
   typedef struct CandoString {
       const char *data;
       u32         length;
   } CandoString;
   /* Field names mirror the production CandoValue layout in
    * source/core/value.h so any TU that compiles in both modes can
    * use `v.as.number` / `v.as.string` / `v.as.boolean` / `cando_as_handle(v)`
    * uniformly. */
   typedef struct CandoValue {
       int tag;
       union {
           bool          boolean;
           double        number;
           CandoString  *string;
           unsigned int  handle;
       } as;
   } CandoValue;
   /* CdoTypeTag values mirror source/object/value.h so the parsers that
    * compare `v.tag == CDO_NUMBER` keep working in the test build. */
   #ifndef CDO_NULL
   enum {
       CDO_NULL     = 0,
       CDO_BOOL     = 1,
       CDO_NUMBER   = 2,
       CDO_STRING   = 3,
       CDO_OBJECT   = 4,
       CDO_ARRAY    = 5,
       CDO_FUNCTION = 6,
       CDO_NATIVE   = 7,
   };
   #endif
   /* Match the production NaN-box accessors (source/core/value.h) so
    * forms TUs can use the same API in both build modes. */
   static inline bool         cando_is_null  (CandoValue v) { return v.tag == CDO_NULL; }
   static inline bool         cando_is_bool  (CandoValue v) { return v.tag == CDO_BOOL; }
   static inline bool         cando_is_number(CandoValue v) { return v.tag == CDO_NUMBER; }
   static inline bool         cando_is_string(CandoValue v) { return v.tag == CDO_STRING; }
   static inline bool         cando_is_object(CandoValue v) { return v.tag == CDO_OBJECT; }
   static inline bool         cando_as_bool  (CandoValue v) { return v.as.boolean; }
   static inline double       cando_as_number(CandoValue v) { return v.as.number; }
   static inline CandoString *cando_as_string(CandoValue v) { return v.as.string; }
   static inline unsigned int cando_as_handle(CandoValue v) { return v.as.handle; }
   static inline CdoString *cdo_string_intern(const char *s, u32 n) {
       (void)s; (void)n; return (CdoString *)0;
   }
   static inline void cdo_string_release(CdoString *s) { (void)s; }
   static inline CandoValue cdo_string_value(CdoString *s) {
       (void)s; CandoValue v = {0,{0}}; return v;
   }
   static inline CandoValue cdo_number(double d) {
       CandoValue v = {0,{0}}; v.as.number = d; return v;
   }
   static inline CandoValue cdo_bool(bool b) {
       CandoValue v = {0,{0}}; v.as.boolean = b; return v;
   }
   static inline bool cdo_object_rawset(CdoObject *o, CdoString *k,
                                        CandoValue v, int f) {
       (void)o; (void)k; (void)v; (void)f; return true;
   }
#    ifndef FIELD_NONE
#      define FIELD_NONE    0
#    endif
#    ifndef FIELD_STATIC
#      define FIELD_STATIC  1
#    endif
#    ifndef FIELD_PRIVATE
#      define FIELD_PRIVATE 2
#    endif
#  endif /* FORMS_CANDO_COMPAT_TEST_STUBS_DEFINED */
#endif

#endif /* CANDO_FORMS_CORE_CANDO_COMPAT_H */
