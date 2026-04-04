# Value Types

## CandoValue  (`source/core/value.h`)

Every script-visible datum is a `CandoValue` — a small tagged union:

```c
typedef struct CandoValue {
    u8 tag;   // TypeTag discriminant
    union {
        bool         boolean;  // TYPE_BOOL
        f64          number;   // TYPE_NUMBER
        CandoString *string;   // TYPE_STRING
        HandleIndex  handle;   // TYPE_OBJECT
    } as;
} CandoValue;
```

### TypeTag

| Constant | Value | Meaning |
|---|---|---|
| `TYPE_NULL` | 0 | nil / absent value |
| `TYPE_BOOL` | 1 | `true` / `false` |
| `TYPE_NUMBER` | 2 | IEEE 754 `double` |
| `TYPE_STRING` | 3 | immutable heap string |
| `TYPE_OBJECT` | 4 | heap object via `HandleIndex` |

### Constructors

```c
CandoValue cando_null()                     // TYPE_NULL
CandoValue cando_bool(bool v)               // TYPE_BOOL
CandoValue cando_number(f64 v)              // TYPE_NUMBER
CandoValue cando_string_value(CandoString*) // TYPE_STRING (takes ownership)
CandoValue cando_object_value(HandleIndex)  // TYPE_OBJECT
```

### Type predicates

```c
bool cando_is_null(CandoValue v)
bool cando_is_bool(CandoValue v)
bool cando_is_number(CandoValue v)
bool cando_is_string(CandoValue v)
bool cando_is_object(CandoValue v)
```

### Value operations

```c
bool  cando_value_equal(CandoValue a, CandoValue b); // structural for primitives
char *cando_value_tostring(CandoValue v);            // heap-alloc; caller must free
CandoValue cando_value_copy(CandoValue v);           // increments string ref_count
void  cando_value_release(CandoValue v);             // decrements string ref_count
```

---

## CandoString  (`source/core/value.h`)

```c
typedef struct CandoString {
    u32  ref_count;
    u32  length;    // byte length, excluding NUL
    u32  hash;      // FNV-1a; 0 = not yet computed
    char data[];    // flexible array; NUL-terminated
} CandoString;
```

`CandoString` is a ref-counted, immutable heap string.

```c
CandoString *cando_string_new(const char *src, u32 length);
CandoString *cando_string_retain(CandoString *s);  // increments ref_count
void         cando_string_release(CandoString *s); // decrements; frees at 0
```

`CandoString` lives in the **VM layer**.  It is separate from `CdoString` (the
object layer's intern-table string).  To use a `CandoString` as a hash key for
a `CdoObject`, convert it with `cando_bridge_intern_key()`.

---

## Native function sentinels

Native functions are stored as special `TYPE_NUMBER` values with a negative
number that encodes the function's index:

```
native #0 → -1.0
native #1 → -2.0
native #N → -(N+1).0
```

Two macros expose this convention:

```c
#define IS_NATIVE_FN(v)  (cando_is_number(v) && (v).as.number < 0.0)
#define NATIVE_INDEX(v)  ((u32)(-(v).as.number - 1.0))
```

The VM's `OP_CALL` and `OP_METHOD_CALL` handlers check `IS_NATIVE_FN` before
attempting object-level dispatch.

---

## CdoValue  (`source/object/value.h`)

The object layer has a mirror type `CdoValue` with its own tag enum:

| Tag | Meaning |
|---|---|
| `CDO_NULL` | nil |
| `CDO_BOOL` | boolean |
| `CDO_NUMBER` | double |
| `CDO_STRING` | `CdoString*` (interned) |
| `CDO_OBJECT` | `CdoObject*` (plain object) |
| `CDO_ARRAY` | `CdoObject*` (array kind) |
| `CDO_FUNCTION` | `CdoObject*` (function kind) |
| `CDO_NATIVE` | `CdoObject*` (native kind) |

Constructors follow the same pattern:

```c
CdoValue cdo_null()
CdoValue cdo_bool(bool v)
CdoValue cdo_number(f64 v)
CdoValue cdo_string_value(CdoString *s)
CdoValue cdo_object_value(CdoObject *obj)
```

---

## Bridge layer  (`source/vm/bridge.h`)

Cross-layer helpers — always use these when crossing between VM and object code:

```c
// Retrieve the CdoObject* behind a HandleIndex
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);

// Create a new empty object/array registered in the handle table
CandoValue cando_bridge_new_object(CandoVM *vm);
CandoValue cando_bridge_new_array(CandoVM *vm);

// Intern a VM-layer key string into the object-layer intern table
CdoString *cando_bridge_intern_key(CandoString *cs);

// Convert in either direction
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue   cando_bridge_to_cdo  (CandoVM *vm, CandoValue v);
```

### Ownership notes

- `cando_bridge_to_cando` creates a **new** `CandoString` for string values
  (ref_count = 1).  The caller is responsible for releasing it when done.
- `cando_bridge_to_cdo` creates a **new** `CdoString` (not interned) for
  string values.  Use `cdo_string_intern()` afterwards if you need the
  interned version for hash-key lookups.
- Object values round-trip cleanly through the handle table with no copies.
