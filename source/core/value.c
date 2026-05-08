/*
 * value.c -- CandoValue and CandoString implementation.
 *
 * Storage is NaN-boxed (see value.h for layout).  The accessor inlines
 * in value.h handle the bit-pattern manipulation; this file holds the
 * routines that operate on whole CandoValues -- equality, tostring,
 * copy, release, and the type-name table.
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
    atomic_store_explicit(&s->ref_count, 1u, memory_order_relaxed);
    s->length    = length;
    atomic_store_explicit(&s->hash,      0u, memory_order_relaxed);
    if (length > 0)
        memcpy(s->data, src, length);
    s->data[length] = '\0';
    return s;
}

CandoString *cando_string_retain(CandoString *s) {
    CANDO_ASSERT(s != NULL);
    atomic_fetch_add_explicit(&s->ref_count, 1u, memory_order_relaxed);
    return s;
}

void cando_string_release(CandoString *s) {
    if (!s) return;
    u32 prev = atomic_fetch_sub_explicit(&s->ref_count, 1u,
                                         memory_order_acq_rel);
    CANDO_ASSERT_MSG(prev > 0, "cando_string_release: ref_count underflow");
    if (prev == 1)
        cando_free(s);
}

static u32 cando_string_hash(CandoString *s) {
    u32 h = atomic_load_explicit(&s->hash, memory_order_relaxed);
    if (h == 0) {
        h = fnv1a(s->data, s->length);
        atomic_store_explicit(&s->hash, h, memory_order_relaxed);
    }
    return h;
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
 *
 * For boxed values (everything except numbers) the bit pattern uniquely
 * identifies the value, so a u64 compare suffices for the common case.
 * The exceptions:
 *   - Number == Number is a double compare (NaN != NaN, -0.0 == 0.0).
 *   - String == String falls back to hash + memcmp for interned content
 *     equality (two distinct CandoString allocations with the same
 *     bytes must compare equal).
 * --------------------------------------------------------------------- */
bool cando_value_equal(CandoValue a, CandoValue b) {
    if (cando_is_number(a) || cando_is_number(b)) {
        if (!cando_is_number(a) || !cando_is_number(b)) return false;
        return cando_as_number(a) == cando_as_number(b);
    }
    if (a.u == b.u) return true;  /* same boxed bit pattern */
    if (cando_is_string(a) && cando_is_string(b)) {
        CandoString *sa = cando_as_string(a);
        CandoString *sb = cando_as_string(b);
        if (sa->length != sb->length) return false;
        if (cando_string_hash(sa) != cando_string_hash(sb)) return false;
        return memcmp(sa->data, sb->data, sa->length) == 0;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * tostring
 * --------------------------------------------------------------------- */
char *cando_value_tostring(CandoValue v) {
    char buf[64];
    if (cando_is_null(v)) return strdup("null");
    if (cando_is_bool(v)) return strdup(cando_as_bool(v) ? "true" : "false");
    if (cando_is_number(v)) {
        f64 n = cando_as_number(v);
        if (n == (i64)n)
            snprintf(buf, sizeof(buf), "%" PRId64, (i64)n);
        else
            snprintf(buf, sizeof(buf), "%.17g", n);
        return strdup(buf);
    }
    if (cando_is_string(v)) return strdup(cando_as_string(v)->data);
    if (cando_is_object(v)) {
        snprintf(buf, sizeof(buf), "object(%" PRIu32 ")",
                 cando_as_handle(v));
        return strdup(buf);
    }
    return strdup("?");
}

/* -----------------------------------------------------------------------
 * Copy / release
 * --------------------------------------------------------------------- */
CandoValue cando_value_copy(CandoValue v) {
    if (cando_is_string(v)) {
        CandoString *s = cando_as_string(v);
        if (s) cando_string_retain(s);
    }
    return v;
}

void cando_value_release(CandoValue v) {
    if (cando_is_string(v))
        cando_string_release(cando_as_string(v));
    /* Objects are managed through the Handle Table / GC; no action here. */
}
