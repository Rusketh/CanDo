/*
 * object.c -- Core CdoObject: open-addressing hash map with FIFO ordering.
 *
 * Implements: global init/teardown, raw field access, prototype-chain
 * lookup, readonly flag, FIFO iteration, and object length.
 *
 * Array, function, and class operations live in their respective modules.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "object.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Tombstone sentinel
 *
 * A unique address used to mark deleted hash table slots.
 * Defined in string.c; referenced here via extern.
 * --------------------------------------------------------------------- */
extern CdoString *const cdo_string_tombstone;
#define OBJSLOT_TOMBSTONE (cdo_string_tombstone)

/* -----------------------------------------------------------------------
 * Non-retaining intern lookup (fallback when g_meta_index not yet set)
 * Declared in string.c.
 * --------------------------------------------------------------------- */
CdoString *cdo_intern_weak(const char *src, u32 length);

/* -----------------------------------------------------------------------
 * Cached meta-key globals (valid after cdo_object_init)
 * --------------------------------------------------------------------- */
CdoString *g_meta_index    = NULL;
CdoString *g_meta_call     = NULL;
CdoString *g_meta_type     = NULL;
CdoString *g_meta_tostring = NULL;
CdoString *g_meta_equal    = NULL;
CdoString *g_meta_greater  = NULL;
CdoString *g_meta_is       = NULL;
CdoString *g_meta_negate   = NULL;
CdoString *g_meta_not      = NULL;
CdoString *g_meta_add      = NULL;
CdoString *g_meta_len      = NULL;
CdoString *g_meta_newindex = NULL;

/* -----------------------------------------------------------------------
 * Global initialisation
 * --------------------------------------------------------------------- */
void cdo_object_init(void) {
    cdo_intern_init();

#define INTERN_META(var, name) \
    var = cdo_string_intern(name, (u32)(sizeof(name) - 1))

    INTERN_META(g_meta_index,    META_INDEX);
    INTERN_META(g_meta_call,     META_CALL);
    INTERN_META(g_meta_type,     META_TYPE);
    INTERN_META(g_meta_tostring, META_TOSTRING);
    INTERN_META(g_meta_equal,    META_EQUAL);
    INTERN_META(g_meta_greater,  META_GREATER);
    INTERN_META(g_meta_is,       META_IS);
    INTERN_META(g_meta_negate,   META_NEGATE);
    INTERN_META(g_meta_not,      META_NOT);
    INTERN_META(g_meta_add,      META_ADD);
    INTERN_META(g_meta_len,      META_LEN);
    INTERN_META(g_meta_newindex, META_NEWINDEX);

#undef INTERN_META
}

void cdo_object_destroy_globals(void) {
    cdo_string_release(g_meta_index);    g_meta_index    = NULL;
    cdo_string_release(g_meta_call);     g_meta_call     = NULL;
    cdo_string_release(g_meta_type);     g_meta_type     = NULL;
    cdo_string_release(g_meta_tostring); g_meta_tostring = NULL;
    cdo_string_release(g_meta_equal);    g_meta_equal    = NULL;
    cdo_string_release(g_meta_greater);  g_meta_greater  = NULL;
    cdo_string_release(g_meta_is);       g_meta_is       = NULL;
    cdo_string_release(g_meta_negate);   g_meta_negate   = NULL;
    cdo_string_release(g_meta_not);      g_meta_not      = NULL;
    cdo_string_release(g_meta_add);      g_meta_add      = NULL;
    cdo_string_release(g_meta_len);      g_meta_len      = NULL;
    cdo_string_release(g_meta_newindex); g_meta_newindex = NULL;

    cdo_intern_destroy();
}

/* -----------------------------------------------------------------------
 * Internal allocation helper (shared by array.c, function.c, class.c)
 * --------------------------------------------------------------------- */
CdoObject *cdo_obj_alloc(ObjectKind kind) {
    CdoObject *obj       = cando_alloc(sizeof(CdoObject));
    cando_lock_init(&obj->lock);
    atomic_store(&obj->user_lock_id, (u64)0);
    atomic_store(&obj->user_lock_depth, (u32)0);
    obj->kind            = (u8)kind;
    obj->readonly        = false;
    obj->slots           = NULL;
    obj->slot_cap        = 0;
    obj->field_count     = 0;
    obj->tombstone_count = 0;
    obj->fifo_head       = UINT32_MAX;
    obj->fifo_tail       = UINT32_MAX;
    obj->items           = NULL;
    obj->items_len       = 0;
    obj->items_cap       = 0;
    memset(&obj->fn, 0, sizeof(obj->fn));
    return obj;
}

CdoObject *cdo_object_new(void) {
    return cdo_obj_alloc(OBJ_OBJECT);
}

void cdo_object_destroy(CdoObject *obj) {
    if (!obj) return;

    /* Release all field values. */
    u32 idx = obj->fifo_head;
    while (idx != UINT32_MAX) {
        ObjSlot *s = &obj->slots[idx];
        u32 next   = s->fifo_next;
        cdo_value_release(s->value);
        /* Keys are interned; the intern table owns their ref -- no release. */
        idx = next;
    }
    cando_free(obj->slots);

    /* Release dense array items. */
    if (obj->items) {
        for (u32 i = 0; i < obj->items_len; i++)
            cdo_value_release(obj->items[i]);
        cando_free(obj->items);
    }

    /* Release upvalues for script functions. */
    if (obj->kind == OBJ_FUNCTION && obj->fn.script.upvalues) {
        for (u32 i = 0; i < obj->fn.script.upvalue_count; i++)
            cdo_value_release(obj->fn.script.upvalues[i]);
        cando_free(obj->fn.script.upvalues);
    }

    cando_free(obj);
}

/* -----------------------------------------------------------------------
 * Hash table internals
 * --------------------------------------------------------------------- */

/* Mix pointer bits to reduce clustering on aligned allocations. */
static u32 ptr_hash(const CdoString *key) {
    uptr p = (uptr)key;
    p ^= p >> 16;
    p *= 0x45d9f3bu;
    p ^= p >> 16;
    return (u32)p;
}

/*
 * obj_lookup -- find the slot for key.
 *
 * Returns the slot index if found, otherwise UINT32_MAX.
 * If insert_slot is non-NULL, writes the best slot for a new insertion.
 */
static u32 obj_lookup(const CdoObject *obj, CdoString *key,
                      u32 *insert_slot) {
    if (obj->slot_cap == 0) {
        if (insert_slot) *insert_slot = UINT32_MAX;
        return UINT32_MAX;
    }
    u32 h     = ptr_hash(key) & (obj->slot_cap - 1);
    u32 start = h;
    u32 tomb  = UINT32_MAX;

    for (;;) {
        ObjSlot *s = &obj->slots[h];
        if (s->key == key) {
            if (insert_slot) *insert_slot = h;
            return h;
        }
        if (s->key == OBJSLOT_TOMBSTONE) {
            if (tomb == UINT32_MAX) tomb = h;
        } else if (s->key == NULL) {
            if (insert_slot)
                *insert_slot = (tomb != UINT32_MAX) ? tomb : h;
            return UINT32_MAX;
        }
        h = (h + 1) & (obj->slot_cap - 1);
        if (h == start) {
            if (insert_slot)
                *insert_slot = (tomb != UINT32_MAX) ? tomb : UINT32_MAX;
            return UINT32_MAX;
        }
    }
}

/*
 * obj_rehash -- rebuild hash table with new_cap slots (must be power of 2).
 * Re-inserts live fields in FIFO order; tombstones are dropped.
 */
static void obj_rehash(CdoObject *obj, u32 new_cap) {
    CANDO_ASSERT(new_cap > 0 && (new_cap & (new_cap - 1)) == 0);

    ObjSlot *new_slots = cando_alloc(new_cap * sizeof(ObjSlot));
    memset(new_slots, 0, new_cap * sizeof(ObjSlot));
    for (u32 i = 0; i < new_cap; i++) {
        new_slots[i].fifo_prev = UINT32_MAX;
        new_slots[i].fifo_next = UINT32_MAX;
    }

    u32 new_head = UINT32_MAX;
    u32 new_tail = UINT32_MAX;

    u32 idx = obj->fifo_head;
    while (idx != UINT32_MAX) {
        ObjSlot *old      = &obj->slots[idx];
        u32      next_idx = old->fifo_next;

        u32 h = ptr_hash(old->key) & (new_cap - 1);
        while (new_slots[h].key != NULL)
            h = (h + 1) & (new_cap - 1);

        new_slots[h].key       = old->key;
        new_slots[h].value     = old->value;
        new_slots[h].flags     = old->flags;
        new_slots[h].fifo_prev = new_tail;
        new_slots[h].fifo_next = UINT32_MAX;

        if (new_head == UINT32_MAX) {
            new_head = h;
        } else {
            new_slots[new_tail].fifo_next = h;
        }
        new_tail = h;
        idx = next_idx;
    }

    cando_free(obj->slots);
    obj->slots           = new_slots;
    obj->slot_cap        = new_cap;
    obj->tombstone_count = 0;
    obj->fifo_head       = new_head;
    obj->fifo_tail       = new_tail;
}

/* Ensure hash table can hold one more entry; rehash at 75% load. */
static void obj_ensure_capacity(CdoObject *obj) {
    u32 used = obj->field_count + obj->tombstone_count + 1;
    if (obj->slot_cap == 0 || used * 4 > obj->slot_cap * 3) {
        u32 new_cap = (obj->slot_cap == 0) ? 8 : obj->slot_cap * 2;
        while ((obj->field_count + 1) * 4 > new_cap * 3)
            new_cap *= 2;
        obj_rehash(obj, new_cap);
    }
}

/* -----------------------------------------------------------------------
 * Raw field access
 * --------------------------------------------------------------------- */
bool cdo_object_rawget(const CdoObject *obj, CdoString *key, CdoValue *out) {
    CANDO_ASSERT(obj != NULL);
    CANDO_ASSERT(key != NULL && key != OBJSLOT_TOMBSTONE);

    /* Non-OBJ_OBJECT kinds (thread, array, function, class) share the
     * CdoObject header layout but do NOT have a slots[] array.  Guard here
     * so callers (e.g. cando_vm_call_meta) never access invalid memory. */
    if (obj->kind != OBJ_OBJECT) return false;

    cando_lock_read_acquire((CandoLockHeader *)&obj->lock);
    u32  idx   = obj_lookup(obj, key, NULL);
    bool found = (idx != UINT32_MAX);
    if (found && out)
        *out = obj->slots[idx].value;
    cando_lock_read_release((CandoLockHeader *)&obj->lock);
    return found;
}

bool cdo_object_rawset(CdoObject *obj, CdoString *key, CdoValue val, u8 flags) {
    CANDO_ASSERT(obj != NULL);
    CANDO_ASSERT(key != NULL && key != OBJSLOT_TOMBSTONE);

    cando_lock_write_acquire(&obj->lock);

    if (obj->readonly) {
        cando_lock_write_release(&obj->lock);
        return false;
    }

    obj_ensure_capacity(obj);

    u32 insert_at;
    u32 found = obj_lookup(obj, key, &insert_at);

    if (found != UINT32_MAX) {
        ObjSlot *s = &obj->slots[found];
        if (s->flags & FIELD_STATIC) {
            cando_lock_write_release(&obj->lock);
            return false;
        }
        cdo_value_release(s->value);
        s->value = cdo_value_copy(val);
        s->flags = flags;
        cando_lock_write_release(&obj->lock);
        return true;
    }

    CANDO_ASSERT(insert_at != UINT32_MAX);
    ObjSlot *s = &obj->slots[insert_at];

    if (s->key == OBJSLOT_TOMBSTONE)
        obj->tombstone_count--;

    s->key       = key;
    s->value     = cdo_value_copy(val);
    s->flags     = flags;
    s->fifo_prev = obj->fifo_tail;
    s->fifo_next = UINT32_MAX;

    if (obj->fifo_head == UINT32_MAX) {
        obj->fifo_head = insert_at;
    } else {
        obj->slots[obj->fifo_tail].fifo_next = insert_at;
    }
    obj->fifo_tail = insert_at;
    obj->field_count++;

    cando_lock_write_release(&obj->lock);
    return true;
}

bool cdo_object_rawdelete(CdoObject *obj, CdoString *key) {
    CANDO_ASSERT(obj != NULL);
    CANDO_ASSERT(key != NULL && key != OBJSLOT_TOMBSTONE);

    cando_lock_write_acquire(&obj->lock);

    if (obj->readonly) {
        cando_lock_write_release(&obj->lock);
        return false;
    }

    u32 idx = obj_lookup(obj, key, NULL);
    if (idx == UINT32_MAX) {
        cando_lock_write_release(&obj->lock);
        return false;
    }

    ObjSlot *s = &obj->slots[idx];
    if (s->flags & FIELD_STATIC) {
        cando_lock_write_release(&obj->lock);
        return false;
    }

    if (s->fifo_prev != UINT32_MAX)
        obj->slots[s->fifo_prev].fifo_next = s->fifo_next;
    else
        obj->fifo_head = s->fifo_next;

    if (s->fifo_next != UINT32_MAX)
        obj->slots[s->fifo_next].fifo_prev = s->fifo_prev;
    else
        obj->fifo_tail = s->fifo_prev;

    cdo_value_release(s->value);
    s->key       = OBJSLOT_TOMBSTONE;
    s->value     = cdo_null();
    s->flags     = FIELD_NONE;
    s->fifo_prev = UINT32_MAX;
    s->fifo_next = UINT32_MAX;

    obj->field_count--;
    obj->tombstone_count++;

    cando_lock_write_release(&obj->lock);
    return true;
}

/* -----------------------------------------------------------------------
 * Prototype-chain lookup
 * --------------------------------------------------------------------- */
bool cdo_object_get(CdoObject *obj, CdoString *key, CdoValue *out) {
    CdoObject *cur = obj;

    for (int depth = 0; depth < CANDO_PROTO_DEPTH_MAX; depth++) {
        if (cdo_object_rawget(cur, key, out))
            return true;

        CdoString *meta_index = g_meta_index
            ? g_meta_index
            : cdo_intern_weak(META_INDEX, (u32)(sizeof(META_INDEX) - 1));
        if (!meta_index)
            return false;

        CdoValue idx_val;
        if (!cdo_object_rawget(cur, meta_index, &idx_val))
            return false;

        if (!cdo_is_any_object(idx_val) || !idx_val.as.object)
            return false;

        CdoObject *next = idx_val.as.object;
        if (next == cur)
            return false; /* self-loop guard */
        cur = next;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Readonly
 * --------------------------------------------------------------------- */
void cdo_object_set_readonly(CdoObject *obj, bool ro) {
    CANDO_ASSERT(obj != NULL);
    cando_lock_write_acquire(&obj->lock);
    obj->readonly = ro;
    cando_lock_write_release(&obj->lock);
}

bool cdo_object_is_readonly(const CdoObject *obj) {
    CANDO_ASSERT(obj != NULL);
    return obj->readonly;
}

/* -----------------------------------------------------------------------
 * FIFO iteration
 * --------------------------------------------------------------------- */
void cdo_object_foreach(const CdoObject *obj, CdoIterFn fn, void *ud) {
    CANDO_ASSERT(obj != NULL && fn != NULL);

    cando_lock_read_acquire((CandoLockHeader *)&obj->lock);

    u32 idx = obj->fifo_head;
    while (idx != UINT32_MAX) {
        ObjSlot *s    = &obj->slots[idx];
        u32      next = s->fifo_next;
        cando_lock_read_release((CandoLockHeader *)&obj->lock);

        bool cont = fn(s->key, &s->value, s->flags, ud);

        cando_lock_read_acquire((CandoLockHeader *)&obj->lock);
        if (!cont) break;
        idx = next;
    }

    cando_lock_read_release((CandoLockHeader *)&obj->lock);
}

/* -----------------------------------------------------------------------
 * Object length
 * --------------------------------------------------------------------- */
u32 cdo_object_length(const CdoObject *obj) {
    CANDO_ASSERT(obj != NULL);
    if (obj->kind == OBJ_ARRAY)
        return obj->items_len;
    return obj->field_count;
}
