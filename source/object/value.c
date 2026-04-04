/*
 * value.c -- CdoValue operations: copy, release, tostring, equality.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "value.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

CdoValue cdo_value_copy(CdoValue v) {
    if (v.tag == CDO_STRING && v.as.string)
        cdo_string_retain(v.as.string);
    return v;
}

void cdo_value_release(CdoValue v) {
    if (v.tag == CDO_STRING)
        cdo_string_release(v.as.string);
    /* Object lifetime managed by cdo_object_destroy(); no action here. */
}

char *cdo_value_tostring(CdoValue v) {
    char buf[64];
    switch (v.tag) {
        case CDO_NULL:
            return strdup("null");
        case CDO_BOOL:
            return strdup(v.as.boolean ? "true" : "false");
        case CDO_NUMBER: {
            f64 n = v.as.number;
            if (n == (i64)n)
                snprintf(buf, sizeof(buf), "%" PRId64, (i64)n);
            else
                snprintf(buf, sizeof(buf), "%.17g", n);
            return strdup(buf);
        }
        case CDO_STRING:
            return strdup(v.as.string ? v.as.string->data : "");
        case CDO_OBJECT:
            snprintf(buf, sizeof(buf), "object(%p)", (void *)v.as.object);
            return strdup(buf);
        case CDO_ARRAY:
            snprintf(buf, sizeof(buf), "array(%p)", (void *)v.as.object);
            return strdup(buf);
        case CDO_FUNCTION:
            snprintf(buf, sizeof(buf), "function(%p)", (void *)v.as.object);
            return strdup(buf);
        case CDO_NATIVE:
            snprintf(buf, sizeof(buf), "native(%p)", (void *)v.as.object);
            return strdup(buf);
        default:
            return strdup("?");
    }
}

bool cdo_value_equal(CdoValue a, CdoValue b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case CDO_NULL:   return true;
        case CDO_BOOL:   return a.as.boolean == b.as.boolean;
        case CDO_NUMBER: return a.as.number  == b.as.number;
        case CDO_STRING: {
            CdoString *sa = a.as.string, *sb = b.as.string;
            if (sa == sb) return true;
            if (!sa || !sb) return false;
            if (sa->length != sb->length) return false;
            if (cdo_string_hash(sa) != cdo_string_hash(sb)) return false;
            return memcmp(sa->data, sb->data, sa->length) == 0;
        }
        case CDO_OBJECT:
        case CDO_ARRAY:
        case CDO_FUNCTION:
        case CDO_NATIVE:
            return a.as.object == b.as.object;
        default:
            return false;
    }
}
