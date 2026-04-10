/*
 * string.h -- CdoString: ref-counted heap strings with interning.
 *
 * Provides:
 *   - CdoString struct (ref-counted, NUL-terminated, FNV-1a hashed)
 *   - Lifecycle: cdo_string_new / cdo_string_retain / cdo_string_release
 *   - Interning:  cdo_string_intern -- pointer-equality key lookup O(1)
 *   - Init/teardown of the global intern table (called by object.h layer)
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_STRING_H
#define CDO_STRING_H

#include "common.h"

/* -----------------------------------------------------------------------
 * CdoString -- ref-counted, internable heap string
 *
 * Equal interned strings share the same pointer, enabling O(1) equality
 * checks when used as object field keys.
 *
 * Thread-safety: ref_count and hash are _Atomic(u32) so that concurrent
 * retain/release and hash computation from multiple threads is safe without
 * external locks.  Intern-table traversal and insertion is protected by a
 * separate exclusive spinlock inside string.c.
 * --------------------------------------------------------------------- */
typedef struct CdoString {
    _Atomic(u32) ref_count;   /* managed atomically by retain / release    */
    u32          length;      /* byte length, excluding NUL                */
    _Atomic(u32) hash;        /* FNV-1a hash; 0 = not yet computed         */
    bool         interned;    /* true when owned by the intern table       */
    char         data[];      /* flexible array, NUL-terminated            */
} CdoString;

/* -----------------------------------------------------------------------
 * Basic lifecycle
 * --------------------------------------------------------------------- */

/* Allocate a new CdoString (ref_count = 1, not interned). */
CdoString *cdo_string_new(const char *src, u32 length);

/* Increment ref_count; returns s for chaining. */
CdoString *cdo_string_retain(CdoString *s);

/* Decrement ref_count; frees when it reaches 0. Safe on NULL. */
void cdo_string_release(CdoString *s);

/* Return (and lazily compute) the FNV-1a hash of s. */
u32 cdo_string_hash(CdoString *s);

/* -----------------------------------------------------------------------
 * Intern table
 *
 * cdo_string_intern() returns (or creates) a canonical CdoString* for the
 * given byte content.  Two calls with identical bytes return the same
 * pointer, so key comparison in hash tables reduces to a pointer compare.
 *
 * The returned pointer is retained on behalf of the caller; caller must
 * call cdo_string_release() when done.
 *
 * cdo_intern_init() / cdo_intern_destroy() manage the global table
 * lifetime.  They are called by the object layer's init/destroy.
 * --------------------------------------------------------------------- */
void       cdo_intern_init(void);
void       cdo_intern_destroy(void);
CdoString *cdo_string_intern(const char *src, u32 length);

#endif /* CDO_STRING_H */
