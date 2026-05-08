/*
 * value.h -- Tagged-union value type for the Cando runtime.
 *
 * Every script-visible datum is a CandoValue.  The TypeTag identifies
 * which union member is live.  The lock header (LockID) is embedded so
 * that the auto-locking layer can protect individual values.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_VALUE_H
#define CANDO_VALUE_H

#include "common.h"

/* -----------------------------------------------------------------------
 * TypeTag -- 8-bit discriminant for a CandoValue
 *
 * Declared as int enum for C11 compatibility; stored in value structs as u8.
 * --------------------------------------------------------------------- */
typedef enum {
    TYPE_NULL   = 0,   /* The nil / absent value                          */
    TYPE_BOOL   = 1,   /* Boolean: true / false                           */
    TYPE_NUMBER = 2,   /* IEEE 754 double                                 */
    TYPE_STRING = 3,   /* Immutable heap string                           */
    TYPE_OBJECT = 4,   /* Heap object referenced via a HandleIndex        */
} TypeTag;

/* -----------------------------------------------------------------------
 * HandleIndex -- opaque index into the global Handle Table.
 * An object is never stored by raw pointer inside a CandoValue; only by
 * its handle index so that the GC can move memory freely.
 * --------------------------------------------------------------------- */
typedef u32 HandleIndex;
#define CANDO_INVALID_HANDLE ((HandleIndex)UINT32_MAX)

/* -----------------------------------------------------------------------
 * CandoString -- ref-counted, immutable heap string
 *
 * ref_count and hash are atomic so retain/release and the lazy-hash
 * fast path are safe to call concurrently from multiple threads (a
 * single CandoString may be shared across thread boundaries via a
 * VM stack value or a captured closure upvalue).  Same pattern as
 * CdoString in source/object/string.h.
 * --------------------------------------------------------------------- */
typedef struct CandoString {
    _Atomic(u32) ref_count; /* Managed by cando_string_retain / _release  */
    u32          length;    /* Byte length, excluding NUL                 */
    _Atomic(u32) hash;      /* FNV-1a hash (0 = not yet computed)         */
    char         data[];    /* Flexible array member; NUL-terminated      */
} CandoString;

CANDO_API CandoString *cando_string_new(const char *src, u32 length);
CANDO_API CandoString *cando_string_retain(CandoString *s);
CANDO_API void         cando_string_release(CandoString *s);

/* -----------------------------------------------------------------------
 * CandoValue -- the core tagged union
 *
 * Layout (16 bytes on 64-bit):
 *   [type : u8][padding : u8 x 3][lock_id : atomic_u64 — in header]
 *
 * LockID lives in the *object* header (CandoObjectHeader), not in every
 * CandoValue copy, so that primitive copies remain cheap.  For objects the
 * CandoValue holds a HandleIndex; the actual lock is on the pointed-to
 * block.
 * --------------------------------------------------------------------- */
typedef struct CandoValue {
    u8 tag;        /* discriminant — TypeTag values, stored as u8 */
    union {
        bool         boolean;  /* TYPE_BOOL   */
        f64          number;   /* TYPE_NUMBER */
        CandoString *string;   /* TYPE_STRING */
        HandleIndex  handle;   /* TYPE_OBJECT */
    } as;
} CandoValue;

/* -----------------------------------------------------------------------
 * Convenience constructors
 * --------------------------------------------------------------------- */
CANDO_INLINE CandoValue cando_null(void) {
    return (CandoValue){ .tag = TYPE_NULL };
}

CANDO_INLINE CandoValue cando_bool(bool v) {
    return (CandoValue){ .tag = TYPE_BOOL, .as = { .boolean = v } };
}

CANDO_INLINE CandoValue cando_number(f64 v) {
    return (CandoValue){ .tag = TYPE_NUMBER, .as = { .number = v } };
}

/* Takes ownership of the string pointer (caller should cando_string_retain
 * first if they want to keep their own reference). */
CANDO_INLINE CandoValue cando_string_value(CandoString *s) {
    return (CandoValue){ .tag = TYPE_STRING, .as = { .string = s } };
}

CANDO_INLINE CandoValue cando_object_value(HandleIndex h) {
    return (CandoValue){ .tag = TYPE_OBJECT, .as = { .handle = h } };
}

/* -----------------------------------------------------------------------
 * Type predicates
 * --------------------------------------------------------------------- */
CANDO_INLINE bool cando_is_null(CandoValue v)   { return v.tag == TYPE_NULL;   }
CANDO_INLINE bool cando_is_bool(CandoValue v)   { return v.tag == TYPE_BOOL;   }
CANDO_INLINE bool cando_is_number(CandoValue v) { return v.tag == TYPE_NUMBER; }
CANDO_INLINE bool cando_is_string(CandoValue v) { return v.tag == TYPE_STRING; }
CANDO_INLINE bool cando_is_object(CandoValue v) { return v.tag == TYPE_OBJECT; }

/* -----------------------------------------------------------------------
 * Accessors -- prefer these over direct .tag / .as.* reads.
 *
 * Today they are a thin shim over the tagged union.  The Phase 1 NaN-box
 * migration (docs/jit-plan.md §4.2) will reshape the underlying storage
 * but keep these signatures stable, so any code written against the
 * accessors will not need editing when the flip lands.
 *
 * No type checking: passing the wrong tag is undefined behaviour.  Use
 * the cando_is_* predicates first when the tag is not statically known.
 * --------------------------------------------------------------------- */
CANDO_INLINE TypeTag cando_value_tag(CandoValue v) {
    return (TypeTag)v.tag;
}
CANDO_INLINE bool cando_as_bool(CandoValue v) {
    return v.as.boolean;
}
CANDO_INLINE f64 cando_as_number(CandoValue v) {
    return v.as.number;
}
CANDO_INLINE CandoString *cando_as_string(CandoValue v) {
    return v.as.string;
}
CANDO_INLINE HandleIndex cando_as_handle(CandoValue v) {
    return v.as.handle;
}

/* -----------------------------------------------------------------------
 * Native function sentinel.
 *
 * Native functions are encoded as TYPE_NUMBER values with a negative
 * payload: native #N is stored as -(f64)(N + 1).  The macros below
 * isolate this convention so the NaN-box migration can replace it with
 * a dedicated tag without breaking call sites.  See
 * docs/value-types.md and docs/jit-plan.md §9.7.
 *
 * Limit: index must satisfy index < UINT32_MAX.  The encoding wraps
 * silently at index == UINT32_MAX (the "+1u" overflows to 0 and the
 * sentinel becomes 0.0 -- which cando_is_native_fn reads as non-native).
 * The native-function table in CandoVM caps at CANDO_NATIVE_MAX (128),
 * so this limit is comfortably out of reach in practice.
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
 * Value operations
 * --------------------------------------------------------------------- */

/* Returns a human-readable type name string (static lifetime). */
CANDO_API const char *cando_value_type_name(TypeTag tag);

/* Equality check (structural for primitives, identity for objects). */
CANDO_API bool cando_value_equal(CandoValue a, CandoValue b);

/* Returns a heap-allocated string representation; caller must free. */
CANDO_API char *cando_value_tostring(CandoValue v);

/*
 * Deep copy: for TYPE_STRING increments ref_count; for TYPE_OBJECT the
 * same HandleIndex is copied (the object itself is not cloned).
 */
CANDO_API CandoValue cando_value_copy(CandoValue v);

/*
 * Release resources held by a value (decrements string ref_count, etc.).
 * Safe to call on any tag.
 */
CANDO_API void cando_value_release(CandoValue v);

#endif /* CANDO_VALUE_H */
