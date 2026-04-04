/*
 * value.h -- CdoValue: the object layer's tagged value type.
 *
 * CdoValue is finer-grained than the core CandoValue: object subtypes
 * (array, function, native) have distinct tags so the VM can skip the
 * handle-table round-trip for type checks.
 *
 * Object subtypes are stored as raw CdoObject* for efficient internal
 * traversal; the VM layer maps these to HandleIndex for script-visible
 * references.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_VALUE_H
#define CDO_VALUE_H

#include "common.h"
#include "string.h"

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */
typedef struct CdoObject CdoObject;
typedef struct CdoState  CdoState;   /* VM state -- defined elsewhere */

/* -----------------------------------------------------------------------
 * CdoTypeTag -- discriminant for CdoValue
 * --------------------------------------------------------------------- */
typedef enum {
    CDO_NULL     = 0,  /* absent / nil                                    */
    CDO_BOOL     = 1,  /* true / false                                    */
    CDO_NUMBER   = 2,  /* IEEE 754 double                                 */
    CDO_STRING   = 3,  /* CdoString* (ref-counted, interned)              */
    CDO_OBJECT   = 4,  /* CdoObject* (plain key-value object)             */
    CDO_ARRAY    = 5,  /* CdoObject* (array specialisation)               */
    CDO_FUNCTION = 6,  /* CdoObject* (script function / closure)          */
    CDO_NATIVE   = 7,  /* CdoObject* (native C function wrapper)          */
} CdoTypeTag;

/* -----------------------------------------------------------------------
 * CdoValue -- the object layer's tagged value
 * --------------------------------------------------------------------- */
typedef struct {
    u8 tag;  /* CdoTypeTag stored as u8 */
    union {
        bool        boolean;   /* CDO_BOOL                                */
        f64         number;    /* CDO_NUMBER                              */
        CdoString  *string;    /* CDO_STRING                              */
        CdoObject  *object;    /* CDO_OBJECT / CDO_ARRAY / CDO_FUNCTION / CDO_NATIVE */
    } as;
} CdoValue;

/* -----------------------------------------------------------------------
 * Convenience constructors
 * --------------------------------------------------------------------- */
CANDO_INLINE CdoValue cdo_null(void) {
    return (CdoValue){ .tag = CDO_NULL };
}
CANDO_INLINE CdoValue cdo_bool(bool v) {
    return (CdoValue){ .tag = CDO_BOOL, .as = { .boolean = v } };
}
CANDO_INLINE CdoValue cdo_number(f64 v) {
    return (CdoValue){ .tag = CDO_NUMBER, .as = { .number = v } };
}
/* Takes ownership (caller must have retained if they want their own ref). */
CANDO_INLINE CdoValue cdo_string_value(CdoString *s) {
    return (CdoValue){ .tag = CDO_STRING, .as = { .string = s } };
}
CANDO_INLINE CdoValue cdo_object_value(CdoObject *o) {
    return (CdoValue){ .tag = CDO_OBJECT, .as = { .object = o } };
}
CANDO_INLINE CdoValue cdo_array_value(CdoObject *o) {
    return (CdoValue){ .tag = CDO_ARRAY, .as = { .object = o } };
}
CANDO_INLINE CdoValue cdo_function_value(CdoObject *o) {
    return (CdoValue){ .tag = CDO_FUNCTION, .as = { .object = o } };
}
CANDO_INLINE CdoValue cdo_native_value(CdoObject *o) {
    return (CdoValue){ .tag = CDO_NATIVE, .as = { .object = o } };
}

/* -----------------------------------------------------------------------
 * Type predicates
 * --------------------------------------------------------------------- */
CANDO_INLINE bool cdo_is_null(CdoValue v)     { return v.tag == CDO_NULL;     }
CANDO_INLINE bool cdo_is_bool(CdoValue v)     { return v.tag == CDO_BOOL;     }
CANDO_INLINE bool cdo_is_number(CdoValue v)   { return v.tag == CDO_NUMBER;   }
CANDO_INLINE bool cdo_is_string(CdoValue v)   { return v.tag == CDO_STRING;   }
CANDO_INLINE bool cdo_is_object(CdoValue v)   { return v.tag == CDO_OBJECT;   }
CANDO_INLINE bool cdo_is_array(CdoValue v)    { return v.tag == CDO_ARRAY;    }
CANDO_INLINE bool cdo_is_function(CdoValue v) { return v.tag == CDO_FUNCTION; }
CANDO_INLINE bool cdo_is_native(CdoValue v)   { return v.tag == CDO_NATIVE;   }
/* True for any object subtype. */
CANDO_INLINE bool cdo_is_any_object(CdoValue v) {
    return v.tag >= CDO_OBJECT && v.tag <= CDO_NATIVE;
}

/* -----------------------------------------------------------------------
 * Value operations
 * --------------------------------------------------------------------- */

/* Retain string; objects are NOT cloned (same pointer copied). */
CdoValue cdo_value_copy(CdoValue v);

/* Release string ref_count.  Object lifetimes managed separately. */
void cdo_value_release(CdoValue v);

/* Returns heap-allocated string representation; caller must free. */
char *cdo_value_tostring(CdoValue v);

/* Structural equality (identity for objects). */
bool cdo_value_equal(CdoValue a, CdoValue b);

#endif /* CDO_VALUE_H */
