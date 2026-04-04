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
 * --------------------------------------------------------------------- */
typedef struct CandoString {
    u32   ref_count;   /* Managed by cando_string_retain / _release       */
    u32   length;      /* Byte length, excluding NUL                      */
    u32   hash;        /* FNV-1a hash (0 = not yet computed)              */
    char  data[];      /* Flexible array member; NUL-terminated           */
} CandoString;

CandoString *cando_string_new(const char *src, u32 length);
CandoString *cando_string_retain(CandoString *s);
void         cando_string_release(CandoString *s);

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
 * Value operations
 * --------------------------------------------------------------------- */

/* Returns a human-readable type name string (static lifetime). */
const char *cando_value_type_name(TypeTag tag);

/* Equality check (structural for primitives, identity for objects). */
bool cando_value_equal(CandoValue a, CandoValue b);

/* Returns a heap-allocated string representation; caller must free. */
char *cando_value_tostring(CandoValue v);

/*
 * Deep copy: for TYPE_STRING increments ref_count; for TYPE_OBJECT the
 * same HandleIndex is copied (the object itself is not cloned).
 */
CandoValue cando_value_copy(CandoValue v);

/*
 * Release resources held by a value (decrements string ref_count, etc.).
 * Safe to call on any tag.
 */
void cando_value_release(CandoValue v);

#endif /* CANDO_VALUE_H */
