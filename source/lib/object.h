/*
 * lib/object.h -- Object standard library for Cando.
 *
 * Registers a global `object` object providing utility functions for
 * plain key-value objects.
 *
 * Methods:
 *   object.lock(o)                  -- acquire exclusive write lock
 *   object.locked(o)      → bool    -- true if o is write-locked by any thread
 *   object.unlock(o)                -- release exclusive write lock
 *   object.copy(o)        → object  -- shallow copy
 *   object.assign(o, ...) → object  -- merge sources into o (mutates o)
 *   object.apply(o, ...)  → object  -- merge o + sources into new object
 *   object.get(o, key)    → value   -- raw field get (bypasses __index)
 *   object.set(o, key, v) → bool    -- raw field set (bypasses __newindex)
 *   object.setPrototype(o, proto)   -- set __index on o
 *   object.getPrototype(o)→ object  -- get __index from o
 *   object.keys(o)        → array   -- array of field names (insertion order)
 *   object.values(o)      → array   -- array of field values (insertion order)
 */

#ifndef CANDO_LIB_OBJECT_H
#define CANDO_LIB_OBJECT_H

#include "../vm/vm.h"

void cando_lib_object_register(CandoVM *vm);

#endif /* CANDO_LIB_OBJECT_H */
