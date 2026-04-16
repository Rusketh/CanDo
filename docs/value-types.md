# Value Types

CanDo uses a two-layer value system. `CandoValue` is the VM-level representation used on the stack, in call frames, in the globals table, and in `CandoNativeFn` signatures. `CdoValue` is the object-layer mirror used inside the property system. The bridge layer converts between them.

## CandoValue (`source/core/value.h`)

A small tagged union that carries every script-visible datum:

```c
typedef enum {
    TYPE_NULL   = 0,
    TYPE_BOOL   = 1,
    TYPE_NUMBER = 2,
    TYPE_STRING = 3,
    TYPE_OBJECT = 4,
} TypeTag;

typedef u32 HandleIndex;
#define CANDO_INVALID_HANDLE ((HandleIndex)UINT32_MAX)

typedef struct CandoValue {
    u8 tag;
    union {
        bool         boolean;
        f64          number;
        CandoString *string;
        HandleIndex  handle;
    } as;
} CandoValue;
```

Objects are never raw pointers. `TYPE_OBJECT` stores a `HandleIndex` into `CandoVM.handles`; resolve it through the bridge layer.

### Constructors

```c
CandoValue cando_null(void);
CandoValue cando_bool(bool v);
CandoValue cando_number(f64 v);
CandoValue cando_string_value(CandoString *s);   // takes ownership
CandoValue cando_object_value(HandleIndex h);
```

### Predicates

```c
bool cando_is_null(CandoValue v);
bool cando_is_bool(CandoValue v);
bool cando_is_number(CandoValue v);
bool cando_is_string(CandoValue v);
bool cando_is_object(CandoValue v);
```

### Operations

```c
const char *cando_value_type_name(TypeTag tag);
bool        cando_value_equal(CandoValue a, CandoValue b);
char       *cando_value_tostring(CandoValue v);    // caller frees
CandoValue  cando_value_copy(CandoValue v);         // retains strings
void        cando_value_release(CandoValue v);       // releases strings
```

`cando_value_equal` is structural for primitives. `cando_value_copy` increments `ref_count` on string values; `cando_value_release` decrements it.

## CandoString (`source/core/value.h`)

Ref-counted, immutable heap string. This is the VM layer string type.

```c
typedef struct CandoString {
    u32  ref_count;
    u32  length;    // byte length, excluding NUL
    u32  hash;      // FNV-1a; 0 = not yet computed
    char data[];    // flexible array; NUL-terminated
} CandoString;
```

```c
CandoString *cando_string_new(const char *src, u32 length);
CandoString *cando_string_retain(CandoString *s);
void         cando_string_release(CandoString *s);   // frees at ref_count 0
```

`CandoString` is separate from `CdoString`, the object layer's intern-table string. To use a `CandoString` as a property key on a `CdoObject`, convert it with `cando_bridge_intern_key()`.

## Native function sentinels

Native functions are stored as special `TYPE_NUMBER` values. A negative number encodes the function index:

```
native #0 → -1.0
native #1 → -2.0
native #N → -(N+1).0
```

```c
#define IS_NATIVE_FN(v)  (cando_is_number(v) && (v).as.number < 0.0)
#define NATIVE_INDEX(v)  ((u32)(-(v).as.number - 1.0))
```

`OP_CALL` and `OP_METHOD_CALL` check `IS_NATIVE_FN` before object dispatch.

## CdoValue (`source/object/value.h`)

The object layer's mirror type with its own tag enum:

| Tag | Meaning |
|---|---|
| `CDO_NULL` | nil |
| `CDO_BOOL` | boolean |
| `CDO_NUMBER` | double |
| `CDO_STRING` | `CdoString*` (interned) |
| `CDO_OBJECT` | `CdoObject*` |
| `CDO_ARRAY` | `CdoObject*` (array kind) |
| `CDO_FUNCTION` | `CdoObject*` (function kind) |
| `CDO_NATIVE` | `CdoObject*` (native kind) |

Constructors:

```c
CdoValue cdo_null(void);
CdoValue cdo_bool(bool v);
CdoValue cdo_number(f64 v);
CdoValue cdo_string_value(CdoString *s);
CdoValue cdo_object_value(CdoObject *obj);
```

## Bridge layer (`source/vm/bridge.h`)

All cross-layer conversions go through these functions:

```c
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);
CandoValue cando_bridge_new_object(CandoVM *vm);
CandoValue cando_bridge_new_array(CandoVM *vm);
CdoString *cando_bridge_intern_key(CandoString *cs);
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue   cando_bridge_to_cdo(CandoVM *vm, CandoValue v);
```

### Ownership rules

- `cando_bridge_to_cando` creates a new `CandoString` for string values (`ref_count` = 1). Caller must release.
- `cando_bridge_to_cdo` creates a new `CdoString` (not interned). Use `cdo_string_intern()` if you need the interned version for hash-key lookups.
- Object values round-trip cleanly through the handle table with no copies.

See [object-system.md](object-system.md) for `CdoObject` internals. See [vm-internals.md](vm-internals.md) for how the VM uses values at runtime.
