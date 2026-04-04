# Object System

## CdoObject  (`source/object/object.h`)

`CdoObject` is the heap object type shared by plain objects, arrays, functions,
and native wrappers.

```c
struct CdoObject {
    CandoLockHeader lock;      // thread-safety header — MUST be offset 0
    u8              kind;      // ObjectKind
    bool            readonly;

    // Hash table (open addressing, linear probing)
    ObjSlot        *slots;
    u32             slot_cap;
    u32             field_count;
    u32             tombstone_count;

    // FIFO insertion-order linked list through slots[] indices
    u32             fifo_head;
    u32             fifo_tail;

    // Dense array storage (valid when kind == OBJ_ARRAY)
    CdoValue       *items;
    u32             items_len;
    u32             items_cap;

    // Function data (OBJ_FUNCTION or OBJ_NATIVE)
    union { ... } fn;
};
```

### ObjectKind

| Constant | Description |
|---|---|
| `OBJ_OBJECT` | plain key-value object |
| `OBJ_ARRAY` | numeric-indexed array (dense `items[]` storage) |
| `OBJ_FUNCTION` | script closure |
| `OBJ_NATIVE` | native C function wrapper |

---

## Global initialisation

The object layer maintains a global intern table for `CdoString` keys and
pre-interns all meta-key strings at startup.

```c
void cdo_object_init(void);            // call once before any object code
void cdo_object_destroy_globals(void); // call at shutdown
```

These are called in `main.c` around the VM execution.

---

## Field flags

```c
#define FIELD_NONE    0x00  // normal mutable field
#define FIELD_STATIC  0x01  // immutable after first assignment
#define FIELD_PRIVATE 0x02  // hidden from outside class scope
```

Pass these to `cdo_object_rawset()` when creating library objects or class
method tables.

---

## Raw field access

```c
// Look up key in obj's own hash table only (no prototype chain).
// Returns true and writes *out if found.
bool cdo_object_rawget(const CdoObject *obj, CdoString *key, CdoValue *out);

// Insert or update a field.
// Returns false on: readonly obj, FIELD_STATIC already-set key, or NULL key.
bool cdo_object_rawset(CdoObject *obj, CdoString *key, CdoValue val, u8 flags);

// Delete a field (returns false if not found, readonly, or FIELD_STATIC).
bool cdo_object_rawdelete(CdoObject *obj, CdoString *key);
```

Keys **must** be interned `CdoString*` values (pointer equality is used for
lookup).  Call `cdo_string_intern(data, length)` before using a string as a
key.

---

## Prototype-chain lookup

```c
// Looks up key in obj; if not found traverses __index up to 32 levels.
bool cdo_object_get(CdoObject *obj, CdoString *key, CdoValue *out);
```

Use `cdo_object_get` (not `rawget`) when implementing dot-access lookups that
should respect inheritance.  The chain limit is `CANDO_PROTO_DEPTH_MAX = 32`.

---

## Readonly objects

```c
void cdo_object_set_readonly(CdoObject *obj, bool ro);
bool cdo_object_is_readonly(const CdoObject *obj);
```

A readonly object rejects all `rawset` and `rawdelete` calls.  Useful for
constant library tables that are safe to share across threads without locking.

---

## FIFO iteration

```c
typedef bool (*CdoIterFn)(CdoString *key, CdoValue *val, u8 flags, void *ud);
void cdo_object_foreach(const CdoObject *obj, CdoIterFn fn, void *ud);
```

Objects preserve insertion order.  Iteration calls `fn` for each live field in
the order keys were first inserted.  Return `false` from `fn` to stop early.

---

## Meta-keys

Pre-interned `CdoString*` globals are available after `cdo_object_init()`:

| Global | String | Purpose |
|---|---|---|
| `g_meta_index` | `__index` | prototype chain / property lookup |
| `g_meta_call` | `__call` | make an object callable |
| `g_meta_type` | `__type` | string type name |
| `g_meta_tostring` | `__tostring` | custom string representation |
| `g_meta_equal` | `__equal` | `==` operator override |
| `g_meta_greater` | `__greater` | `>` operator override |
| `g_meta_is` | `__is` | `is` type-check operator |
| `g_meta_negate` | `__negate` | unary `-` override |
| `g_meta_not` | `__not` | unary `!` override |
| `g_meta_add` | `__add` | `+` operator override |
| `g_meta_len` | `__len` | `#` length operator |
| `g_meta_newindex` | `__newindex` | assignment intercept |

Do NOT call `cdo_string_release()` on these pointers — they are owned by the
intern table.

---

## String prototype  (`vm->string_proto`)

The string module (`source/lib/string.c`) sets `vm->string_proto` to a
read-only `CandoValue` (TYPE_OBJECT) that holds all string methods.  This
enables colon-syntax method calls on string literals:

```
"hello":toLower()      -- becomes string.toLower("hello")
"hello":sub(1, 3)      -- becomes string.sub("hello", 1, 3)
```

### How it works

`OP_METHOD_CALL` with a string receiver:

1. Checks `cando_is_string(receiver)`.
2. Resolves `vm->string_proto` as a `CdoObject*`.
3. Looks up the method name in the prototype (full `cdo_object_get` chain).
4. If the result is a native sentinel, calls it with `(arg_count + 1)` args
   starting from the callee slot (so `args[0]` = the receiver string).

`OP_GET_FIELD` with a string receiver:

1. Resolves the field from `vm->string_proto`.
2. Returns the raw method value (the native sentinel) without self-binding.

### Setting the prototype in a module

```c
// In your registration function:
CandoValue proto_val = cando_bridge_new_object(vm); // create the table
// ... add methods via cdo_object_rawset ...
cdo_object_set_readonly(cando_bridge_resolve(vm, proto_val.as.handle), true);
cando_vm_set_global(vm, "string", proto_val, true); // expose as "string"
vm->string_proto = proto_val;                        // enable : syntax
```

---

## Arrays  (`source/object/array.h`)

```c
CdoObject *cdo_array_new(void);
void       cdo_array_push(CdoObject *arr, CdoValue val);
CdoValue   cdo_array_get(const CdoObject *arr, u32 index);
void       cdo_array_set(CdoObject *arr, u32 index, CdoValue val);
u32        cdo_array_length(const CdoObject *arr);
```

Arrays use dense `items[]` storage for integer-indexed access.  They also have
a hash table for string-keyed properties (like methods or meta-keys).

---

## Classes  (`source/object/class.h`)

```c
CdoObject *cdo_class_new(const char *type_name, u32 name_len);
```

Creates a plain `CdoObject` with `__type` pre-set to the given name.  The
`OP_NEW_CLASS` opcode uses this.  The `__call` meta-method acts as the
constructor and is expected to return a new instance with `__index` pointing
to the class object.
