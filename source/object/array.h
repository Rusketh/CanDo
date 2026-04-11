/*
 * array.h -- Array specialisation for CdoObject.
 *
 * Arrays are CdoObjects with kind == OBJ_ARRAY and a dense CdoValue[]
 * for numeric-indexed storage.  They also carry a hash table for
 * non-numeric / meta fields.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_ARRAY_H
#define CDO_ARRAY_H

#include "object.h"

/* -----------------------------------------------------------------------
 * Array creation
 * --------------------------------------------------------------------- */

/* Allocate a new empty array object. */
CANDO_API CdoObject *cdo_array_new(void);

/* -----------------------------------------------------------------------
 * Array operations (valid only on OBJ_ARRAY objects)
 * --------------------------------------------------------------------- */

/* Append val to the end of the dense storage. Returns false if readonly. */
CANDO_API bool cdo_array_push(CdoObject *arr, CdoValue val);

/* Read item at idx from dense storage. Returns false if out of bounds. */
CANDO_API bool cdo_array_rawget_idx(const CdoObject *arr, u32 idx, CdoValue *out);

/* Write item at idx (grows and zero-fills if needed). Returns false if readonly. */
CANDO_API bool cdo_array_rawset_idx(CdoObject *arr, u32 idx, CdoValue val);

/* Return the number of items in the dense storage. */
CANDO_API u32 cdo_array_len(const CdoObject *arr);

/* Insert val at idx, shifting elements at [idx..end) right by one.
   If idx >= items_len the array is grown and gap-filled with null.
   Returns false if readonly. */
CANDO_API bool cdo_array_insert(CdoObject *arr, u32 idx, CdoValue val);

/* Remove the element at idx, shifting elements at (idx..end) left by one.
   Writes the removed value to *out if out is non-NULL.
   Returns false if idx is out-of-bounds or array is readonly. */
CANDO_API bool cdo_array_remove(CdoObject *arr, u32 idx, CdoValue *out);

#endif /* CDO_ARRAY_H */
