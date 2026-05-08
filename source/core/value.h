/*
 * value.h -- NaN-boxed value type for the Cando runtime.
 *
 * Every script-visible datum is a CandoValue.  Storage is an 8-byte
 * union of an IEEE 754 double and a u64 bit pattern.
 *
 *   - Numbers are stored as ordinary doubles (the dominant case).
 *   - Other types are encoded as quiet NaNs whose mantissa carries a
 *     3-bit type tag and a 48-bit payload (pointer / handle / bool).
 *
 * Boxed-value bit layout (high to low):
 *
 *     63     | sign      = 1
 *     62..52 | exponent  = 0x7FF (NaN)
 *     51     | qNaN bit  = 1
 *     50..48 | tag       = TYPE_BOOL / TYPE_STRING / TYPE_OBJECT (1..3)
 *     47..0  | payload   = bool / CandoString* / HandleIndex
 *
 * TYPE_NULL uses tag=0; the canonical null value is 0xFFF8000000000000.
 * TYPE_NUMBER has no tag -- any value that does not match the boxed
 * pattern is interpreted as a number.
 *
 * Native-function sentinels (legacy) are encoded as negative finite
 * doubles, which are still TYPE_NUMBER under the new scheme.  See
 * cando_is_native_fn / cando_native_value below.
 *
 * NaN canonicalisation: arithmetic that produces NaN must canonicalise
 * to a positive-sign qNaN (0x7FF8000000000000) so the negative-sign
 * NaN encoding (0xFFF8000000000000) is reserved exclusively for boxed
 * null.  cando_number() does this canonicalisation when called with
 * isnan(n) input.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_VALUE_H
#define CANDO_VALUE_H

#include <math.h>     /* isnan */
#include <stdint.h>   /* uintptr_t */

#include "common.h"

/* -----------------------------------------------------------------------
 * TypeTag -- script-visible type identifier.
 *
 * The numeric values match the bit-encoded tag for boxed values
 * (TYPE_NULL..TYPE_OBJECT) plus an extra TYPE_NUMBER that has no
 * physical bit pattern -- any non-boxed CandoValue is a number.
 *
 * The values are stable; do not renumber.
 * --------------------------------------------------------------------- */
typedef enum {
    TYPE_NULL   = 0,   /* boxed: tag=0, payload=0                        */
    TYPE_BOOL   = 1,   /* boxed: tag=1, payload=0|1                      */
    TYPE_NUMBER = 2,   /* not boxed -- stored as IEEE 754 double         */
    TYPE_STRING = 3,   /* boxed: tag=3, payload=CandoString*              */
    TYPE_OBJECT = 4,   /* boxed: tag=4, payload=HandleIndex u32           */
} TypeTag;

/* -----------------------------------------------------------------------
 * HandleIndex -- opaque index into the global Handle Table.
 * --------------------------------------------------------------------- */
typedef u32 HandleIndex;
#define CANDO_INVALID_HANDLE ((HandleIndex)UINT32_MAX)

/* -----------------------------------------------------------------------
 * CandoString -- ref-counted, immutable heap string (unchanged).
 * --------------------------------------------------------------------- */
typedef struct CandoString {
    _Atomic(u32) ref_count;
    u32          length;
    _Atomic(u32) hash;
    char         data[];
} CandoString;

CANDO_API CandoString *cando_string_new(const char *src, u32 length);
CANDO_API CandoString *cando_string_retain(CandoString *s);
CANDO_API void         cando_string_release(CandoString *s);

/* -----------------------------------------------------------------------
 * CandoValue -- 8-byte NaN-boxed union.
 * --------------------------------------------------------------------- */
typedef union CandoValue {
    f64 d;
    u64 u;
} CandoValue;

/* -----------------------------------------------------------------------
 * NaN-box constants.
 *
 * CANDO_NB_MASK  -- the high 13 bits (sign + NaN exp + qNaN bit) that
 *                   together identify a boxed value.  Anything that
 *                   doesn't AND-equal this mask is a number.
 *
 * CANDO_NB_TAG_* -- the full high 16 bits (mask + 3-bit tag) for each
 *                   boxed type, prepositioned at bits 63..48.
 * --------------------------------------------------------------------- */
#define CANDO_NB_MASK         ((u64)0xFFF8000000000000ULL)
#define CANDO_NB_PAYLOAD_MASK ((u64)0x0000FFFFFFFFFFFFULL)

#define CANDO_NB_TAG_NULL     ((u64)0xFFF8000000000000ULL)  /* tag = 0 */
#define CANDO_NB_TAG_BOOL     ((u64)0xFFF9000000000000ULL)  /* tag = 1 */
                                                            /* tag = 2 reserved (was TYPE_NUMBER) */
#define CANDO_NB_TAG_STRING   ((u64)0xFFFB000000000000ULL)  /* tag = 3 */
#define CANDO_NB_TAG_OBJECT   ((u64)0xFFFC000000000000ULL)  /* tag = 4 */

#define CANDO_NB_TAG_BITS_MASK ((u64)0xFFFF000000000000ULL)

/* -----------------------------------------------------------------------
 * Predicates
 * --------------------------------------------------------------------- */
CANDO_INLINE bool cando_is_number(CandoValue v) {
    /* Anything that isn't a boxed pattern is a number, including all
     * finite doubles, +/-inf, and the canonical positive qNaN. */
    return (v.u & CANDO_NB_MASK) != CANDO_NB_MASK;
}
CANDO_INLINE bool cando_is_null(CandoValue v) {
    return v.u == CANDO_NB_TAG_NULL;
}
CANDO_INLINE bool cando_is_bool(CandoValue v) {
    return (v.u & CANDO_NB_TAG_BITS_MASK) == CANDO_NB_TAG_BOOL;
}
CANDO_INLINE bool cando_is_string(CandoValue v) {
    return (v.u & CANDO_NB_TAG_BITS_MASK) == CANDO_NB_TAG_STRING;
}
CANDO_INLINE bool cando_is_object(CandoValue v) {
    return (v.u & CANDO_NB_TAG_BITS_MASK) == CANDO_NB_TAG_OBJECT;
}

/* -----------------------------------------------------------------------
 * Constructors
 * --------------------------------------------------------------------- */
CANDO_INLINE CandoValue cando_null(void) {
    CandoValue v; v.u = CANDO_NB_TAG_NULL; return v;
}
CANDO_INLINE CandoValue cando_bool(bool b) {
    CandoValue v; v.u = CANDO_NB_TAG_BOOL | (u64)(b ? 1u : 0u); return v;
}
CANDO_INLINE CandoValue cando_number(f64 n) {
    CandoValue v;
    v.d = n;
    /* Force any NaN to canonical positive qNaN so the negative-sign
     * encoding stays exclusive to boxed null. */
    if (CANDO_UNLIKELY(isnan(n))) {
        v.u = (u64)0x7FF8000000000000ULL;
    }
    return v;
}
/* Takes ownership of the string pointer. */
CANDO_INLINE CandoValue cando_string_value(CandoString *s) {
    CandoValue v;
    v.u = CANDO_NB_TAG_STRING | ((u64)(uintptr_t)s & CANDO_NB_PAYLOAD_MASK);
    return v;
}
CANDO_INLINE CandoValue cando_object_value(HandleIndex h) {
    CandoValue v;
    v.u = CANDO_NB_TAG_OBJECT | (u64)h;
    return v;
}

/* -----------------------------------------------------------------------
 * Accessors -- assume the predicate has already been checked.
 * --------------------------------------------------------------------- */
CANDO_INLINE TypeTag cando_value_tag(CandoValue v) {
    if ((v.u & CANDO_NB_MASK) != CANDO_NB_MASK) return TYPE_NUMBER;
    /* Read the 3-bit tag at bits 50..48; map back to TypeTag. */
    u64 t = (v.u >> 48) & 0x7u;
    switch (t) {
        case 0: return TYPE_NULL;
        case 1: return TYPE_BOOL;
        case 3: return TYPE_STRING;
        case 4: return TYPE_OBJECT;
        default: return TYPE_NULL;  /* unreachable for well-formed values */
    }
}
CANDO_INLINE bool cando_as_bool(CandoValue v) {
    return (v.u & 0x1u) != 0;
}
CANDO_INLINE f64 cando_as_number(CandoValue v) {
    return v.d;
}
CANDO_INLINE CandoString *cando_as_string(CandoValue v) {
    return (CandoString *)(uintptr_t)(v.u & CANDO_NB_PAYLOAD_MASK);
}
CANDO_INLINE HandleIndex cando_as_handle(CandoValue v) {
    return (HandleIndex)(v.u & 0xFFFFFFFFu);
}

/* -----------------------------------------------------------------------
 * Native function sentinel.
 *
 * Encoded as a negative finite double: native #N is stored as
 * -(f64)(N + 1).  Survives the NaN-box flip unchanged because negative
 * finite doubles are still TYPE_NUMBER.
 *
 * Limit: index < UINT32_MAX (the +1 wraps at the boundary).  The native
 * table caps at CANDO_NATIVE_MAX (128) so the limit is unreachable.
 * --------------------------------------------------------------------- */
CANDO_INLINE bool cando_is_native_fn(CandoValue v) {
    return cando_is_number(v) && cando_as_number(v) < 0.0;
}
CANDO_INLINE u32 cando_native_index(CandoValue v) {
    return (u32)(-cando_as_number(v) - 1.0);
}
CANDO_INLINE CandoValue cando_native_value(u32 index) {
    CANDO_ASSERT_MSG(index < UINT32_MAX,
                     "cando_native_value: index would wrap encoding");
    return cando_number(-(f64)(index + 1u));
}

/* -----------------------------------------------------------------------
 * In-place setter -- replaces the value at *p with cando_number(n).
 * Used by OP_INCR / OP_DECR / iterator-index updates that previously did
 * `p->as.number += 1.0`.  No tag check; caller must guarantee *p is
 * already a number.
 * --------------------------------------------------------------------- */
CANDO_INLINE void cando_set_number(CandoValue *p, f64 n) {
    *p = cando_number(n);
}

/* -----------------------------------------------------------------------
 * Value operations
 * --------------------------------------------------------------------- */
CANDO_API const char *cando_value_type_name(TypeTag tag);
CANDO_API bool        cando_value_equal(CandoValue a, CandoValue b);
CANDO_API char       *cando_value_tostring(CandoValue v);
CANDO_API CandoValue  cando_value_copy(CandoValue v);
CANDO_API void        cando_value_release(CandoValue v);

#endif /* CANDO_VALUE_H */
