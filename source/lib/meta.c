/*
 * lib/meta.c -- Global `_meta` registry for built-in object types.
 *
 * Implements the helpers declared in meta.h.  The `_meta` global is a plain
 * writable CdoObject whose fields are subtables for each native type.  Native
 * libraries call cando_lib_meta_table() during registration to obtain (or
 * lazily create) their subtable and populate it with default methods, then
 * call cando_lib_meta_attach() on every new instance to wire up `__index`.
 *
 * Must compile with gcc -std=c11.
 */

#include "meta.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"

#include <string.h>

#define META_GLOBAL_NAME "_meta"

static CdoObject *meta_root_obj(CandoVM *vm)
{
    CandoValue meta_val = cando_null();
    if (!cando_vm_get_global(vm, META_GLOBAL_NAME, &meta_val))
        return NULL;
    if (!cando_is_object(meta_val))
        return NULL;
    return cando_bridge_resolve(vm, meta_val.as.handle);
}

void cando_lib_meta_register(CandoVM *vm)
{
    /* Idempotent: do nothing if _meta is already a global object. */
    if (meta_root_obj(vm) != NULL) return;

    CandoValue meta_val = cando_bridge_new_object(vm);
    /* Writable: users must be able to add methods at runtime.  The subtable
     * fields hold native-method sentinels and are likewise mutable. */
    cando_vm_set_global(vm, META_GLOBAL_NAME, meta_val, false);
}

CdoObject *cando_lib_meta_table(CandoVM *vm, const char *name)
{
    CdoObject *root = meta_root_obj(vm);
    if (!root) return NULL;

    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool       ok  = cdo_object_rawget(root, key, &v);

    if (ok && v.tag == CDO_OBJECT && v.as.object) {
        cdo_string_release(key);
        return v.as.object;
    }
    /* Stale or wrong-type slot: replace it. */

    CandoValue tbl_val = cando_bridge_new_object(vm);
    CdoObject *tbl     = cando_bridge_resolve(vm, tbl_val.as.handle);

    /* Stamp __type so type() on instances yields the meta name. */
    CdoString *type_key = cdo_string_intern(META_TYPE, (u32)(sizeof(META_TYPE) - 1));
    CdoString *type_val = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(tbl, type_key,
                      cdo_string_value(type_val), FIELD_STATIC);
    cdo_string_release(type_key);
    cdo_string_release(type_val);

    cdo_object_rawset(root, key, cdo_object_value(tbl), FIELD_NONE);
    cdo_string_release(key);
    return tbl;
}

void cando_lib_meta_attach(CandoVM *vm, CdoObject *instance, const char *name)
{
    if (!instance) return;
    CdoObject *tbl = cando_lib_meta_table(vm, name);
    if (!tbl) return;

    cdo_object_rawset(instance, g_meta_index,
                      cdo_object_value(tbl), FIELD_NONE);
}
