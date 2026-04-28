/*
 * lib/meta.h -- Global `_meta` registry for built-in object types.
 *
 * `_meta` is a writable global object holding named subtables ("meta tables").
 * Each subtable acts as the prototype (__index target) for instances of a
 * built-in type, letting users extend the methods available on them:
 *
 *     _meta.http_response.write = FUNCTION(self, data) {
 *         self.body = self.body + data;
 *     }
 *
 * Native libraries register their default methods on the appropriate subtable
 * via cando_lib_meta_table().  Instances created by the library are stamped
 * with `__index = _meta.<name>` so method dispatch flows through the chain.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_META_H
#define CANDO_LIB_META_H

#include "../vm/vm.h"
#include "../object/object.h"

/*
 * cando_lib_meta_register -- create the `_meta` global as an empty writable
 * object.  Idempotent: calling it twice is a no-op.  Must be called before
 * any cando_lib_meta_table() lookups.
 */
CANDO_API void cando_lib_meta_register(CandoVM *vm);

/*
 * cando_lib_meta_table -- return the CdoObject for `_meta.<name>`, creating
 * an empty subtable if none exists.  The returned pointer is owned by the
 * `_meta` global and remains valid for the lifetime of the VM.
 *
 * The returned object's `__type` is set to <name> so user code can inspect
 * instances with type().
 *
 * Returns NULL if `_meta` has not been registered yet.
 */
CANDO_API CdoObject *cando_lib_meta_table(CandoVM *vm, const char *name);

/*
 * cando_lib_meta_attach -- set instance.__index = _meta.<name>.
 *
 * Convenience helper for native libraries that build instance objects.  No-op
 * if either `instance` or the named meta table is missing.
 */
CANDO_API void cando_lib_meta_attach(CandoVM *vm, CdoObject *instance,
                                     const char *name);

#endif /* CANDO_LIB_META_H */
