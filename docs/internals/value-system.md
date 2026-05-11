# Value System

CanDo has **two parallel value representations**.  Confusing them is
the single biggest source of bugs in this codebase, so this page
explains the model in detail.

## Two layers

### Layer 1 — `CandoValue` (the VM/script layer)

A small tagged union used:

- on the VM stack,
- as the parameter and return type of native callbacks,
- as the argument type of every public C API entry point.

Defined in `source/core/value.h`.  A `CandoValue` carries:

- a discriminator tag (`null`, `bool`, `number`, `string`, `object`),
- and a payload — inline for primitives, a pointer for strings (which
  are interned and immutable), and a `HandleIndex` for objects.

Crucially: **for heap objects, `CandoValue` does not carry a
pointer.**  It carries a `u32` index into a per-VM handle table.

### Layer 2 — `CdoObject` (the heap layer)

The heap-resident type.  Defined in `source/object/object.h`.  An
object owns:

- a hash table mapping keys to values (used for object fields, class
  methods, `__index`, metamethods),
- a dense `values[]` array for integer-indexed storage (used by
  arrays),
- an `ObjectKind` tag — one of `OBJ_OBJECT`, `OBJ_ARRAY`,
  `OBJ_FUNCTION`, `OBJ_NATIVE`, `OBJ_THREAD`.  Classes use
  `OBJ_OBJECT`; there is no dedicated `OBJ_CLASS`,
- type-specific metadata (function bytecode, native pointer, thread
  state, …).

## The handle table

Every object the runtime allocates is registered into a central handle
table (`source/core/handle.c`).  The table maps `HandleIndex` →
`CdoObject *`.  When the GC moves an object — say, promoting it from a
nursery to a tenured space — it updates the handle table entry; live
`CandoValue`s on every thread's stack continue to resolve to the new
location.

This is what makes a real-threading runtime tractable.  Without
indirection, every relocation would have to walk every thread's stack
and rewrite pointers, which is expensive and error-prone.  The handle
indirection trades one cache miss per object access for an enormous
simplification of GC and concurrency.

## The bridge

`source/vm/bridge.c` and `bridge.h` translate between layers.  The two
core operations:

```c
CdoObject *bridge_resolve_object(CandoVM *vm, CandoValue v);
CandoValue bridge_value_for_object(CdoObject *o);
```

`bridge_resolve_object` returns the current `CdoObject *` for the
handle in `v`, or `NULL` if `v` is not an object.  It is cheap (one
table read).  Use it any time you need to *do something* with an
object that arrived as a `CandoValue`.

`bridge_value_for_object` is the inverse: given a freshly-allocated
object, get a `CandoValue` you can return to script.

## The cardinal rule

> **Do not store `CdoObject *` across any call that might trigger GC.**

A "call that might trigger GC" includes:

- Allocating any new object (string concatenation, array push,
  closure creation, class instance, thread spawn).
- Calling a script function.
- Re-entering the VM.
- Calling another native that itself allocates.

The pattern to follow is:

```c
/* GOOD */
CdoObject *o = bridge_resolve_object(vm, argv[0]);
const char *name = cdo_string_cstr(cdo_object_get(o, "name"));
return cando_value_string(vm, name, strlen(name));   /* may GC */

/* If you need to access `o` again afterwards: */
o = bridge_resolve_object(vm, argv[0]);              /* re-resolve */
```

```c
/* BAD — `o` may dangle after the cando_value_string call */
CdoObject *o = bridge_resolve_object(vm, argv[0]);
CandoValue v = cando_value_string(vm, ..., ...);
cdo_object_set(o, "x", v);                           /* o may be stale */
```

If you find yourself wanting to hold an object across allocation,
keep the **handle** (a `HandleIndex`, just a `u32`) and re-resolve.

## Strings

Strings are not heap objects in the `CdoObject` sense — they have their
own type (`CdoString`) and their own intern table.  A `CandoValue` for
a string carries a pointer to the interned `CdoString`; equal strings
share a single `CdoString`.

Because strings are interned, equality is a pointer comparison.  This
also means the GC must coordinate with the intern table on string
death; see `source/core/string.c`.

## Numbers, booleans, null

These fit inline in the `CandoValue` and never go to the heap.  No GC
implications, no bridge needed.

## Threads

Thread handles are `CdoObject`s of `ObjectKind = thread`.  The thread
object owns its OS thread handle, its return values, its error value,
its state machine, and the closure it's executing.  When `await`
unblocks, the calling thread reads the result fields from the thread
object directly.

Thread death does *not* immediately free the `CdoObject` — other
script values may still reference it (for `t:state()` or
`thread.error(t)` queries).  The GC reclaims it once it's unreachable,
like any other object.

## Class machinery

Classes are also just `CdoObject`s.  Their `ObjectKind` is plain
object, but they carry:

- A `__call` metamethod that allocates a fresh instance and runs the
  constructor.
- A `__type` field stamped to the class name (so `type(instance)`
  returns it).
- An `__index` chain, when `EXTENDS` was used.

The `OP_NEW_CLASS`, `OP_BIND_METHOD`, `OP_INHERIT`, and
`OP_BIND_DEFAULT_CALL` opcodes set this up; see [vm.md](vm.md).

## Common patterns when writing natives

### Validate then operate

```c
if (argc < 2 || !cando_is_string(args[0])) {
    cando_vm_error(vm, "first argument must be a string");
    return -1;
}
if (!cando_is_object(args[1])) {
    cando_vm_error(vm, "second argument must be an object");
    return -1;
}
CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(args[1]));
```

### Loop without holding pointers across calls

```c
CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(args[0]));
u32 n = cdo_array_len(arr);

for (u32 i = 0; i < n; i++) {
    CdoValue v;
    if (!cdo_array_rawget_idx(arr, i, &v)) continue;
    /* ... operate on v ... */
    /* if you must call back into the VM, re-resolve `arr` afterwards */
    arr = cando_bridge_resolve(vm, cando_as_handle(args[0]));
}
```

### Multi-return

Push values in order onto the VM stack, then signal the count:

```c
cando_push(vm, cando_value_number(min));
cando_push(vm, cando_value_number(max));
return cando_native_return(vm, 2);
```

## Reading the source

If you take one thing away: when in doubt, look at
`source/lib/array.c` or `source/lib/string.c`.  They are short, they
follow the conventions, and they cover almost every shape of
native-code interaction with the value system.
