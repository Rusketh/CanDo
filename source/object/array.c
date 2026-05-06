/*
 * array.c -- Array operations for CdoObject (OBJ_ARRAY kind).
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "array.h"

/* Ensure items[] can hold at least `min_cap` entries.  Grows by doubling
 * (initial capacity 8 when the array is empty).  Caller must hold the
 * write lock.  Returns false if the requested capacity cannot be
 * satisfied without overflow.                                          */
static bool array_ensure_cap(CdoObject *arr, u32 min_cap) {
    if (arr->items_cap >= min_cap) return true;
    u32 new_cap = arr->items_cap ? arr->items_cap : 8;
    while (new_cap < min_cap) {
        if (new_cap > UINT32_MAX / 2) { new_cap = min_cap; break; }
        new_cap *= 2;
    }
    arr->items     = cando_realloc(arr->items,
                                   (size_t)new_cap * sizeof(CdoValue));
    arr->items_cap = new_cap;
    return true;
}

CdoObject *cdo_array_new(void) {
    return cdo_obj_alloc(OBJ_ARRAY);
}

bool cdo_array_push(CdoObject *arr, CdoValue val) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);

    cando_lock_write_acquire(&arr->lock);

    if (arr->readonly || arr->items_len == UINT32_MAX) {
        cando_lock_write_release(&arr->lock);
        return false;
    }

    array_ensure_cap(arr, arr->items_len + 1);
    arr->items[arr->items_len++] = cdo_value_copy(val);

    cando_lock_write_release(&arr->lock);
    return true;
}

bool cdo_array_rawget_idx(const CdoObject *arr, u32 idx, CdoValue *out) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);

    cando_lock_read_acquire((CandoLockHeader *)&arr->lock);
    bool found = (idx < arr->items_len);
    if (found && out)
        *out = arr->items[idx];
    cando_lock_read_release((CandoLockHeader *)&arr->lock);
    return found;
}

bool cdo_array_rawset_idx(CdoObject *arr, u32 idx, CdoValue val) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);

    cando_lock_write_acquire(&arr->lock);

    if (arr->readonly || idx == UINT32_MAX) {
        cando_lock_write_release(&arr->lock);
        return false;
    }

    /* Grow capacity if idx is past the end; new slots beyond the
     * previous length are filled with null below.                      */
    u32 prev_cap = arr->items_cap;
    array_ensure_cap(arr, idx + 1);
    for (u32 i = prev_cap; i < arr->items_cap; i++)
        arr->items[i] = cdo_null();

    if (idx >= arr->items_len) {
        for (u32 i = arr->items_len; i < idx; i++)
            arr->items[i] = cdo_null();
        arr->items_len = idx + 1;
    }

    cdo_value_release(arr->items[idx]);
    arr->items[idx] = cdo_value_copy(val);

    cando_lock_write_release(&arr->lock);
    return true;
}

u32 cdo_array_len(const CdoObject *arr) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);
    cando_lock_read_acquire((CandoLockHeader *)&arr->lock);
    u32 len = arr->items_len;
    cando_lock_read_release((CandoLockHeader *)&arr->lock);
    return len;
}

bool cdo_array_insert(CdoObject *arr, u32 idx, CdoValue val) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);

    cando_lock_write_acquire(&arr->lock);

    if (arr->readonly || arr->items_len == UINT32_MAX) {
        cando_lock_write_release(&arr->lock);
        return false;
    }

    /* Clamp idx to items_len so append-at-end always works. */
    if (idx > arr->items_len)
        idx = arr->items_len;

    array_ensure_cap(arr, arr->items_len + 1);

    /* Shift elements [idx .. items_len) right by one. */
    for (u32 i = arr->items_len; i > idx; i--)
        arr->items[i] = arr->items[i - 1];

    arr->items[idx] = cdo_value_copy(val);
    arr->items_len++;

    cando_lock_write_release(&arr->lock);
    return true;
}

bool cdo_array_remove(CdoObject *arr, u32 idx, CdoValue *out) {
    CANDO_ASSERT(arr != NULL && arr->kind == OBJ_ARRAY);

    cando_lock_write_acquire(&arr->lock);

    if (arr->readonly || idx >= arr->items_len) {
        cando_lock_write_release(&arr->lock);
        return false;
    }

    if (out)
        *out = arr->items[idx]; /* hand ownership to caller */
    else
        cdo_value_release(arr->items[idx]);

    /* Shift elements (idx .. items_len) left by one. */
    for (u32 i = idx; i + 1 < arr->items_len; i++)
        arr->items[i] = arr->items[i + 1];

    arr->items_len--;

    cando_lock_write_release(&arr->lock);
    return true;
}
