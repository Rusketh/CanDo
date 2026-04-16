# Object System

## CdoObject (`source/object/object.h`)

`CdoObject` is the single heap-allocated type backing plain objects, arrays,
script closures, native function wrappers, and thread handles.

```c
struct CdoObject {
    CandoLockHeader lock;      // MUST be offset 0
    u8              kind;      // ObjectKind
    bool            readonly;

    ObjSlot        *slots;     // open-addressed hash table, linear probing
    u32             slot_cap;
    u32             field_count;
    u32             tombstone_count;

    u32             fifo_head; // FIFO insertion-order linked list through slots[]
    u32             fifo_tail;

    CdoValue       *items;    // dense array storage (OBJ_ARRAY)
    u32             items_len;
    u32             items_cap;

    union { ... } fn;         // function data (OBJ_FUNCTION / OBJ_NATIVE)
};
```

### ObjectKind

`OBJ_OBJECT` -- plain key-value object.
`OBJ_ARRAY` -- dense `items[]` storage plus hash table for string-keyed properties.
`OBJ_FUNCTION` -- script closure.
`OBJ_NATIVE` -- C function wrapper.
`OBJ_THREAD` -- thread handle.

## Global initialisation

```c
void cdo_object_init(void);
void cdo_object_destroy_globals(void);
```

`cdo_object_init()` creates the process-global intern table and pre-interns
every meta-key string. `cdo_object_destroy_globals()` tears it down at
shutdown. `cando_open()` manages both calls through an atomic refcount, so
embedders do not call these directly.

## Field flags

```c
#define FIELD_NONE    0x00   // normal mutable field
#define FIELD_STATIC  0x01   // immutable after first assignment
#define FIELD_PRIVATE 0x02   // hidden from outside class scope
```

## Raw field access

```c
bool cdo_object_rawget(const CdoObject *obj, CdoString *key, CdoValue *out);
bool cdo_object_rawset(CdoObject *obj, CdoString *key, CdoValue val, u8 flags);
bool cdo_object_rawdelete(CdoObject *obj, CdoString *key);
```

All three operate on the object's own hash table only -- no prototype chain.
Keys **must** be interned `CdoString*` values; lookup uses pointer equality.

`rawget` returns `true` and writes `*out` when the field exists.

`rawset` returns `false` when: the object is readonly, the key is `NULL`, or
the field already exists with `FIELD_STATIC`.

`rawdelete` returns `false` when: the field is not found, the object is
readonly, or the field has `FIELD_STATIC`.

## Prototype-chain lookup

```c
bool cdo_object_get(CdoObject *obj, CdoString *key, CdoValue *out);
```

Looks up `key` in the object's own fields first, then follows the `__index`
chain. Stops after `CANDO_PROTO_DEPTH_MAX` (32) levels. Use this instead of
`rawget` whenever dot-access should respect inheritance.

## Readonly objects

```c
void cdo_object_set_readonly(CdoObject *obj, bool ro);
bool cdo_object_is_readonly(const CdoObject *obj);
```

A readonly object rejects all `rawset` and `rawdelete` calls. Useful for
constant library tables shared across threads without locking.

## FIFO iteration

```c
typedef bool (*CdoIterFn)(CdoString *key, CdoValue *val, u8 flags, void *ud);
void cdo_object_foreach(const CdoObject *obj, CdoIterFn fn, void *ud);
```

Fields are visited in insertion order. Return `false` from the callback to
stop early.

## Meta-keys

Pre-interned `CdoString*` globals, available after `cdo_object_init()`:

| Global | Key | Purpose |
|---|---|---|
| `g_meta_index` | `__index` | prototype chain / property lookup |
| `g_meta_call` | `__call` | make an object callable |
| `g_meta_type` | `__type` | string type name |
| `g_meta_tostring` | `__tostring` | custom string representation |
| `g_meta_equal` | `__equal` | `==` override |
| `g_meta_greater` | `__greater` | `>` override |
| `g_meta_is` | `__is` | type-check |
| `g_meta_negate` | `__negate` | unary `-` |
| `g_meta_not` | `__not` | unary `!` |
| `g_meta_add` | `__add` | `+` override |
| `g_meta_len` | `__len` | `#` length |
| `g_meta_newindex` | `__newindex` | assignment intercept |

These are owned by the intern table. Never call `cdo_string_release()` on them.

## String and array prototypes

`source/lib/string.c` sets `vm->string_proto` to a readonly `CandoValue`
(`TYPE_OBJECT`) containing all string methods. This enables colon-syntax on
string literals:

```
"hello":toUpper()
```

When `OP_METHOD_CALL` or `OP_FLUENT_CALL` receives a string receiver, the VM:

1. Checks `cando_is_string(receiver)`.
2. Resolves `vm->string_proto` as a `CdoObject*`.
3. Looks up the method name via `cdo_object_get` (full prototype chain).
4. Shifts the stack so the receiver becomes `arg[0]` and the method becomes the callee at slot 0.
5. Dispatches. For fluent calls (`::`) the receiver is returned instead of the method result.

The same pattern applies to `vm->array_proto` for arrays.

## Arrays (`source/object/array.h`)

```c
CdoObject *cdo_array_new(void);
bool       cdo_array_push(CdoObject *arr, CdoValue val);
bool       cdo_array_rawget_idx(const CdoObject *arr, u32 idx, CdoValue *out);
bool       cdo_array_rawset_idx(CdoObject *arr, u32 idx, CdoValue val);
u32        cdo_array_len(const CdoObject *arr);
bool       cdo_array_insert(CdoObject *arr, u32 idx, CdoValue val);
bool       cdo_array_remove(CdoObject *arr, u32 idx, CdoValue *out);
```

Dense `items[]` storage for integer-indexed access. String-keyed properties
(methods, meta-keys) go through the normal hash table on the same `CdoObject`.

## Classes (`source/object/class.h`)

```c
CdoObject *cdo_class_new(const char *type_name, u32 name_len);
```

Creates a `CdoObject` with `__type` pre-set to the given name. Used by the
`OP_NEW_CLASS` opcode. The `__call` meta-method serves as the constructor and
is expected to return a new instance whose `__index` points back to the class
object.

## CdoString (`source/object/string.h`)

```c
CdoString *cdo_string_new(const char *src, u32 length);
CdoString *cdo_string_retain(CdoString *s);
void       cdo_string_release(CdoString *s);
u32        cdo_string_hash(CdoString *s);
CdoString *cdo_string_intern(const char *src, u32 length);
```

`cdo_string_intern` returns the canonical `CdoString*` from the process-global
intern table. All object field keys must be interned so that lookups reduce to
pointer equality.

---

See `value-types.md` for `CandoValue`/`CdoValue` conversion and `vm-internals.md`
for how the VM uses objects at runtime.
