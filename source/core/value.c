/*
 * value.c -- CandoValue and CandoString implementation.
 */

#include "value.h"
#include <stdio.h>
#include <inttypes.h>

/* -----------------------------------------------------------------------
 * FNV-1a hash for string interning / equality fast-path
 * --------------------------------------------------------------------- */
static u32 fnv1a(const char *data, u32 len) {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < len; i++) {
        hash ^= (u8)data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* -----------------------------------------------------------------------
 * CandoString
 * --------------------------------------------------------------------- */
CandoString *cando_string_new(const char *src, u32 length) {
    CANDO_ASSERT(src != NULL || length == 0);
    CandoString *s = cando_alloc(sizeof(CandoString) + length + 1);
    s->ref_count = 1;
    s->length    = length;
    s->hash      = 0; /* lazy */
    if (length > 0)
        memcpy(s->data, src, length);
    s->data[length] = '\0';
    return s;
}

CandoString *cando_string_retain(CandoString *s) {
    CANDO_ASSERT(s != NULL);
    s->ref_count++;
    return s;
}

void cando_string_release(CandoString *s) {
    if (!s) return;
    CANDO_ASSERT(s->ref_count > 0);
    if (--s->ref_count == 0)
        cando_free(s);
}

static u32 cando_string_hash(CandoString *s) {
    if (s->hash == 0)
        s->hash = fnv1a(s->data, s->length);
    return s->hash;
}

/* -----------------------------------------------------------------------
 * Type name
 * --------------------------------------------------------------------- */
const char *cando_value_type_name(TypeTag tag) {
    switch (tag) {
        case TYPE_NULL:   return "null";
        case TYPE_BOOL:   return "bool";
        case TYPE_NUMBER: return "number";
        case TYPE_STRING: return "string";
        case TYPE_OBJECT: return "object";
        default:          return "unknown";
    }
}

/* -----------------------------------------------------------------------
 * Equality
 * --------------------------------------------------------------------- */
bool cando_value_equal(CandoValue a, CandoValue b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case TYPE_NULL:   return true;
        case TYPE_BOOL:   return a.as.boolean == b.as.boolean;
        case TYPE_NUMBER: return a.as.number  == b.as.number;
        case TYPE_STRING: {
            CandoString *sa = a.as.string, *sb = b.as.string;
            if (sa == sb) return true;
            if (sa->length != sb->length) return false;
            if (cando_string_hash(sa) != cando_string_hash(sb)) return false;
            return memcmp(sa->data, sb->data, sa->length) == 0;
        }
        case TYPE_OBJECT: return a.as.handle == b.as.handle;
        default:          return false;
    }
}

/* -----------------------------------------------------------------------
 * tostring
 * --------------------------------------------------------------------- */
char *cando_value_tostring(CandoValue v) {
    char buf[64];
    switch (v.tag) {
        case TYPE_NULL:
            return strdup("null");
        case TYPE_BOOL:
            return strdup(v.as.boolean ? "true" : "false");
        case TYPE_NUMBER: {
            /* Omit trailing .0 for whole numbers */
            f64 n = v.as.number;
            if (n == (i64)n)
                snprintf(buf, sizeof(buf), "%" PRId64, (i64)n);
            else
                snprintf(buf, sizeof(buf), "%.15g", n);
            return strdup(buf);
        }
        case TYPE_STRING:
            return strdup(v.as.string->data);
        case TYPE_OBJECT:
            snprintf(buf, sizeof(buf), "object(%" PRIu32 ")", v.as.handle);
            return strdup(buf);
        default:
            return strdup("?");
    }
}

/* -----------------------------------------------------------------------
 * Copy / release
 * --------------------------------------------------------------------- */
CandoValue cando_value_copy(CandoValue v) {
    if (v.tag == TYPE_STRING && v.as.string)
        cando_string_retain(v.as.string);
    return v;
}

void cando_value_release(CandoValue v) {
    if (v.tag == TYPE_STRING)
        cando_string_release(v.as.string);
    /* Objects are managed through the Handle Table / GC; no action here. */
}
