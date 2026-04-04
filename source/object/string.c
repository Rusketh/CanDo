/*
 * string.c -- CdoString lifecycle and string intern table.
 *
 * _GNU_SOURCE needed for syscall() used transitively via lock.h.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "string.h"

/* -----------------------------------------------------------------------
 * FNV-1a hash
 * --------------------------------------------------------------------- */
static u32 fnv1a(const char *data, u32 len) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < len; i++) {
        h ^= (u8)data[i];
        h *= 16777619u;
    }
    return h;
}

/* -----------------------------------------------------------------------
 * Tombstone sentinel (used by the object hash table, defined here so
 * cdo_string_release can skip it safely).
 *
 * We deliberately do NOT initialize the flexible-array member (data[])
 * to stay pedantic-clean.  The tombstone is only compared by pointer
 * address; its data is never accessed.
 * --------------------------------------------------------------------- */
/* Declared extern so object.c can reference it without a public header. */
static CdoString g_tombstone_storage = { .ref_count = 0, .length = 0,
                                         .hash = 0, .interned = false };
CdoString *const cdo_string_tombstone = &g_tombstone_storage;

/* -----------------------------------------------------------------------
 * CdoString lifecycle
 * --------------------------------------------------------------------- */
CdoString *cdo_string_new(const char *src, u32 length) {
    CANDO_ASSERT(src != NULL || length == 0);
    CdoString *s = cando_alloc(sizeof(CdoString) + length + 1);
    s->ref_count = 1;
    s->length    = length;
    s->hash      = 0;
    s->interned  = false;
    if (length > 0)
        memcpy(s->data, src, length);
    s->data[length] = '\0';
    return s;
}

CdoString *cdo_string_retain(CdoString *s) {
    CANDO_ASSERT(s != NULL);
    s->ref_count++;
    return s;
}

void cdo_string_release(CdoString *s) {
    if (!s || s == cdo_string_tombstone) return;
    CANDO_ASSERT(s->ref_count > 0);
    if (--s->ref_count == 0)
        cando_free(s);
}

u32 cdo_string_hash(CdoString *s) {
    CANDO_ASSERT(s != NULL);
    if (s->hash == 0)
        s->hash = fnv1a(s->data, s->length);
    return s->hash;
}

/* -----------------------------------------------------------------------
 * Intern table -- chained hash table, INTERN_BUCKETS buckets.
 *
 * The intern table holds one ref per string; cdo_string_intern() gives
 * the caller an additional ref.
 * --------------------------------------------------------------------- */
#define INTERN_BUCKETS 256u

typedef struct InternEntry {
    CdoString        *str;
    struct InternEntry *next;
} InternEntry;

static InternEntry *g_intern[INTERN_BUCKETS];
static bool         g_intern_ready = false;

void cdo_intern_init(void) {
    if (g_intern_ready) return; /* idempotent: skip if already initialised */
    memset(g_intern, 0, sizeof(g_intern));
    g_intern_ready = true;
}

void cdo_intern_destroy(void) {
    for (u32 i = 0; i < INTERN_BUCKETS; i++) {
        InternEntry *e = g_intern[i];
        while (e) {
            InternEntry *next = e->next;
            cdo_string_release(e->str);
            cando_free(e);
            e = next;
        }
        g_intern[i] = NULL;
    }
    g_intern_ready = false;
}

CdoString *cdo_string_intern(const char *src, u32 length) {
    CANDO_ASSERT(g_intern_ready);
    u32 h = fnv1a(src, length) & (INTERN_BUCKETS - 1);
    for (InternEntry *e = g_intern[h]; e; e = e->next) {
        if (e->str->length == length &&
            memcmp(e->str->data, src, length) == 0) {
            return cdo_string_retain(e->str);
        }
    }
    /* Not found: create (ref #1 for intern table), retain for caller (ref #2). */
    CdoString   *s   = cdo_string_new(src, length);
    s->interned      = true;
    InternEntry *ent = cando_alloc(sizeof(InternEntry));
    ent->str         = s;
    ent->next        = g_intern[h];
    g_intern[h]      = ent;
    cdo_string_retain(s); /* caller ref */
    return s;
}

/* Non-retaining lookup -- returns NULL if not interned yet.
 * Used internally by object.c for the prototype-chain fallback. */
CdoString *cdo_intern_weak(const char *src, u32 length) {
    if (!g_intern_ready) return NULL;
    u32 h = fnv1a(src, length) & (INTERN_BUCKETS - 1);
    for (InternEntry *e = g_intern[h]; e; e = e->next) {
        if (e->str->length == length &&
            memcmp(e->str->data, src, length) == 0)
            return e->str;
    }
    return NULL;
}
