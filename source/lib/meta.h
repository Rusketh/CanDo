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
 * cando_lib_meta_set -- register an existing CdoObject as the meta table
 * for `name`.  Used when the table is also exposed elsewhere (e.g. the
 * `string` global is also published as `_meta.string`) so both names
 * resolve to the same underlying object.
 *
 * Does NOT touch `__type` on `table`; the caller is responsible for that
 * if desired.  No-op if `_meta` is unregistered.
 */
CANDO_API void cando_lib_meta_set(CandoVM *vm, const char *name,
                                  CdoObject *table);

/*
 * cando_lib_meta_attach -- set instance.__index = _meta.<name>.
 *
 * Convenience helper for native libraries that build instance objects.  No-op
 * if either `instance` or the named meta table is missing.
 */
CANDO_API void cando_lib_meta_attach(CandoVM *vm, CdoObject *instance,
                                     const char *name);

/*
 * cando_lib_meta_define -- like libutil_set_method, but a no-op if `name`
 * already has a non-null value on `tbl`.  Lets registration paths that may
 * be called more than once (e.g. once from `http` and once from `https`)
 * skip redundant entries instead of inflating the per-VM native table.
 */
CANDO_API void cando_lib_meta_define(CandoVM *vm, CdoObject *tbl,
                                     const char *name, CandoNativeFn fn);

/*
 * cando_lib_meta_alias -- copy an existing field from `src` onto `dst`
 * under the new key `dst_name`, reading `src_name` from `src`.  Useful for
 * exposing the same native-method sentinel under two names without burning
 * a second slot in the VM's native function table.
 *
 * No-op if either object is missing or the source key does not exist.
 */
CANDO_API void cando_lib_meta_alias(CdoObject *dst, const char *dst_name,
                                    const CdoObject *src, const char *src_name);

#endif /* CANDO_LIB_META_H */
