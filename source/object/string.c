/*
 * string.c -- CdoString lifecycle and string intern table.
 *
 * _GNU_SOURCE needed for syscall() used transitively via lock.h.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "string.h"
#include "lock.h"

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
/* Static initializer of atomic fields: C11 guarantees integer literals are
 * valid initializers for _Atomic(u32) in aggregate initializers. */
static CdoString g_tombstone_storage = { .ref_count = 0, .length = 0,
                                         .hash = 0, .interned = false };
/* Suppress -Wc11-extensions / -Wmissing-field-initializers for the atomic
 * flexible member: data[] is intentionally absent (tombstone is pointer-only). */
CdoString *const cdo_string_tombstone = &g_tombstone_storage;

/* -----------------------------------------------------------------------
 * CdoString lifecycle
 * --------------------------------------------------------------------- */
CdoString *cdo_string_new(const char *src, u32 length) {
    CANDO_ASSERT(src != NULL || length == 0);
    CdoString *s = cando_alloc(sizeof(CdoString) + length + 1);
    atomic_store_explicit(&s->ref_count, 1, memory_order_relaxed);
    s->length = length;
    atomic_store_explicit(&s->hash, 0, memory_order_relaxed);
    s->interned = false;
    if (length > 0)
        memcpy(s->data, src, length);
    s->data[length] = '\0';
    return s;
}

CdoString *cdo_string_retain(CdoString *s) {
    CANDO_ASSERT(s != NULL);
    atomic_fetch_add_explicit(&s->ref_count, 1, memory_order_relaxed);
    return s;
}

void cdo_string_release(CdoString *s) {
    if (!s || s == cdo_string_tombstone) return;
    /* fetch_sub returns the value *before* subtraction; if it was 1 the
     * count just hit 0 and we own the final deallocation. */
    u32 prev = atomic_fetch_sub_explicit(&s->ref_count, 1, memory_order_acq_rel);
    CANDO_ASSERT_MSG(prev > 0, "cdo_string_release: ref_count underflow");
    if (prev == 1)
        cando_free(s);
}

u32 cdo_string_hash(CdoString *s) {
    CANDO_ASSERT(s != NULL);
    /* Relaxed load: if hash is already computed any thread's copy is fine. */
    u32 h = atomic_load_explicit(&s->hash, memory_order_relaxed);
    if (h == 0) {
        h = fnv1a(s->data, s->length);
        /* Benign race: two threads may store the same value simultaneously. */
        atomic_store_explicit(&s->hash, h, memory_order_relaxed);
    }
    return h;
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

static InternEntry    *g_intern[INTERN_BUCKETS];
static bool            g_intern_ready = false;

/* Exclusive spinlock protecting g_intern[] and g_intern_ready.
 * All intern-table traversals, insertions, and destroys hold this lock so
 * that concurrent callers never corrupt the linked lists.
 * cdo_string_retain / cdo_string_release are safe to call under this lock
 * because they only perform atomic operations (no lock acquisition). */
static CandoLockHeader g_intern_lock;

void cdo_intern_init(void) {
    if (g_intern_ready) return; /* idempotent: skip if already initialised */
    memset(g_intern, 0, sizeof(g_intern));
    cando_lock_init(&g_intern_lock);
    g_intern_ready = true;
}

void cdo_intern_destroy(void) {
    cando_lock_write_acquire(&g_intern_lock);
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
    cando_lock_write_release(&g_intern_lock);
}

CdoString *cdo_string_intern(const char *src, u32 length) {
    CANDO_ASSERT(g_intern_ready);
    u32 h = fnv1a(src, length) & (INTERN_BUCKETS - 1);

    cando_lock_write_acquire(&g_intern_lock);

    for (InternEntry *e = g_intern[h]; e; e = e->next) {
        if (e->str->length == length &&
            memcmp(e->str->data, src, length) == 0) {
            /* Retain inside the lock so the string can't be destroyed
             * by a concurrent release between our lookup and retain. */
            CdoString *result = cdo_string_retain(e->str);
            cando_lock_write_release(&g_intern_lock);
            return result;
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

    cando_lock_write_release(&g_intern_lock);
    return s;
}

/* Non-retaining lookup -- returns NULL if not interned yet.
 * Safe to use only when the intern table is guaranteed to outlive the
 * returned pointer (i.e. before cdo_intern_destroy is called).
 * Used internally by object.c for the prototype-chain fallback. */
CdoString *cdo_intern_weak(const char *src, u32 length) {
    if (!g_intern_ready) return NULL;
    u32 h = fnv1a(src, length) & (INTERN_BUCKETS - 1);

    cando_lock_write_acquire(&g_intern_lock);
    CdoString *result = NULL;
    for (InternEntry *e = g_intern[h]; e; e = e->next) {
        if (e->str->length == length &&
            memcmp(e->str->data, src, length) == 0) {
            result = e->str;
            break;
        }
    }
    cando_lock_write_release(&g_intern_lock);
    return result;
}
