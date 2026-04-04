/*
 * class.c -- CLASS scaffold construction.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "class.h"

CdoObject *cdo_class_new(CdoString *type_name, CdoObject *proto) {
    CdoObject *cls = cdo_object_new();

    /* __type = type_name (static: immutable class identity). */
    if (type_name && g_meta_type) {
        CdoValue tv = cdo_string_value(cdo_string_retain(type_name));
        cdo_object_rawset(cls, g_meta_type, tv, FIELD_STATIC);
        cdo_value_release(tv);
    }

    /* __index = proto so instances inherit from the class object. */
    if (proto && g_meta_index) {
        CdoValue pv;
        pv.tag       = CDO_OBJECT;
        pv.as.object = proto;
        cdo_object_rawset(cls, g_meta_index, pv, FIELD_NONE);
    }

    return cls;
}
