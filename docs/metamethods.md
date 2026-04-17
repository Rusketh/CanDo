# Metamethods and the Prototype System

CanDo's object system is prototype-based: instead of classes and instances,
objects inherit from other objects through the `__index` chain.  A small set
of double-underscore fields — **meta-keys** — let objects customise how the
VM treats them.

---

## Meta-keys reference

All meta-key strings are pre-interned at startup.  The corresponding global
`CdoString *` pointers are valid after `cdo_object_init()`.

| Field name    | C global           | When dispatched                          |
|---            |---                 |---                                       |
| `__index`     | `g_meta_index`     | Field / method lookup (`obj.field`, `obj:method()`) |
| `__call`      | `g_meta_call`      | Reserved — not yet dispatched by `OP_CALL` |
| `__type`      | `g_meta_type`      | `type(obj)` returns this string          |
| `__tostring`  | `g_meta_tostring`  | `toString(obj)` — native callable only   |
| `__equal`     | `g_meta_equal`     | `obj == other` — native callable only    |
| `__greater`   | `g_meta_greater`   | `obj > other` (and `<`, `<=`, `>=`) — native callable only |
| `__is`        | `g_meta_is`        | Truthiness test — native callable only   |
| `__negate`    | `g_meta_negate`    | Unary `-obj` — native callable only      |
| `__not`       | `g_meta_not`       | Reserved — not yet dispatched by `OP_NOT` |
| `__add`       | `g_meta_add`       | Reserved — not yet dispatched by `OP_ADD` |
| `__len`       | `g_meta_len`       | `#obj` — native callable only            |
| `__newindex`  | `g_meta_newindex`  | Reserved — not yet dispatched by `OP_SET_FIELD` |

> **Callable limitation.** The VM's metamethod dispatcher (`cando_vm_call_meta`)
> currently handles only **native** callables (negative-number sentinels and
> `OBJ_NATIVE` objects).  Script functions assigned as metamethods for
> operators (`__equal`, `__negate`, `__len`, etc.) will trigger a runtime
> error.  `__index` and `__type` work unconditionally because they are read
> as plain fields, not called.

---

## `__index` — prototype-chain lookup

Whenever the VM reads a field that does not exist in an object's own hash
table, it checks whether that object has an `__index` field pointing to
another object, and repeats the lookup there.  The chain is followed up to
`CANDO_PROTO_DEPTH_MAX` (32) levels.

```cando
var proto = { x: 10, greet: "hello" };
var child = { y: 20 };
object.setPrototype(child, proto);   // sets child.__index = proto

print(child.x);       // 10  — found on proto
print(child.y);       // 20  — found on child
print(child.greet);   // hello

child.x = 99;         // own field now shadows proto
print(child.x);       // 99
print(proto.x);       // 10  — proto unchanged
```

`object.setPrototype(obj, proto)` is a convenience wrapper that sets
`obj.__index = proto`.  `object.getPrototype(obj)` reads it back.

### Self-loop guard

If the chain loops back to an already-visited object, `cdo_object_get` stops
and returns `false` rather than looping forever.

---

## `__type` — custom type tag

`type(v)` normally returns `"null"`, `"bool"`, `"number"`, `"string"`, or
`"object"`.  If the object has a `__type` field, that string is returned
instead.

```cando
var vec = { __type: "Vec3", x: 1, y: 2, z: 3 };
print(type(vec));    // Vec3
```

`CLASS` sets `__type` automatically as an immutable (`FIELD_STATIC`) field.

---

## `CLASS` — prototype-based class declarations

`CLASS` is syntactic sugar that creates a plain object whose methods are
stored as fields.  Instances are created manually by a factory method that
sets `__index` back to the class so field lookups traverse the method table.

```cando
CLASS Point {
    FUNCTION make(x, y) {
        VAR p = { x: x, y: y };
        object.setPrototype(p, Point);   // p.__index = Point
        RETURN p;
    }
    FUNCTION dist(self) {
        RETURN math.sqrt(self.x * self.x + self.y * self.y);
    }
    FUNCTION sum(self) {
        RETURN self.x + self.y;
    }
}

print(type(Point));           // Point  (__type set by CLASS)

var p = Point.make(3, 4);
print(p:dist());              // 5
print(p:sum());               // 7
```

`STATIC` before a method name is accepted by the parser but has no special
VM effect — static methods are simply regular fields on the class object.

```cando
CLASS MathUtil {
    STATIC FUNCTION square(n) { RETURN n * n; }
}
print(MathUtil.square(7));    // 49
```

### How `CLASS` compiles

The parser emits:

1. `OP_NEW_CLASS name_idx` — allocates a plain object with `__type` set to
   the class name, pushes it onto the stack.
2. For each `FUNCTION`:  `OP_CONST pc_idx` then `OP_BIND_METHOD meth_idx` —
   pops the method PC and rawsets it as a field on the class (class stays on
   top of stack).
3. `OP_DEF_GLOBAL name_idx` — pops the class and stores it as a global.

### Inheritance with `OP_INHERIT`

The `OP_INHERIT` opcode sets `child.__index = parent`, connecting two class
objects.  While the parser does not yet emit `OP_INHERIT` automatically,
you can replicate two-level inheritance manually:

```cando
CLASS Animal {
    FUNCTION speak(self) { RETURN self.name + " makes a sound"; }
}

CLASS Dog {
    FUNCTION make(name) {
        VAR inst = { name: name };
        object.setPrototype(inst, Dog);
        RETURN inst;
    }
    FUNCTION fetch(self) { RETURN self.name + " fetches!"; }
}
object.setPrototype(Dog, Animal);    // Dog.__index = Animal

var d = Dog.make("Rex");
print(d:fetch());    // Rex fetches!
print(d:speak());    // Rex makes a sound  (found on Animal)
```

---

## `__tostring` — custom string conversion

`toString(obj)` checks for a `__tostring` field.  If present and it is a
**native** callable, it is invoked with the object as the sole argument and
must return a string.  If absent, `toString` falls back to the default
representation.

```cando
// Script functions do NOT work as __tostring; native extensions can.
var plain = { x: 1, y: 2 };
print(type(toString(plain)));    // string  (default representation)
```

---

## `__len` — custom length

`#obj` first checks for a `__len` field.  If present and it is a **native**
callable, it is called with the object and its return value is used.
Otherwise, `cdo_object_length` is used directly:

- Arrays: `items_len` (number of dense elements).
- Plain objects: `field_count` (number of live hash-table entries).

```cando
var obj = { a: 1, b: 2, c: 3 };
print(#obj);    // 3

var arr = [10, 20, 30, 40];
print(#arr);    // 4
```

---

## Operator metamethods (native only)

The following metamethods are dispatched at runtime but require the stored
value to be a **native** callable (set up from C extensions):

| Meta-key    | Trigger         | Receives              | Returns         |
|---          |---              |---                    |---              |
| `__equal`   | `a == b`        | `(a, b)`              | bool-like value |
| `__greater` | `a > b`         | `(a, b)`              | bool-like value |
| `__is`      | truthiness test | `(obj)`               | bool-like value |
| `__negate`  | `-obj`          | `(obj)`               | new value       |
| `__len`     | `#obj`          | `(obj)`               | number          |
| `__tostring`| `toString(obj)` | `(obj)`               | string          |

`__equal` is also used for `!=` (result negated), and `__greater` is composed
with `__equal` to implement `<`, `<=`, `>=`.

---

## C-level API

```c
/* Set __index to establish a prototype chain. */
CdoValue proto_val = { .tag = CDO_OBJECT, .as = { .object = proto } };
cdo_object_rawset(child, g_meta_index, proto_val, FIELD_NONE);

/* Traverse the prototype chain when reading a field. */
CdoValue out;
bool found = cdo_object_get(obj, key, &out);   // follows __index chain

/* Raw access bypasses all meta-method dispatch. */
bool found_own = cdo_object_rawget(obj, key, &out);  // own fields only
```

See `source/object/object.h` for the full API and `source/object/class.h`
for `cdo_class_new`.

---

## Further reading

- [object-system.md](object-system.md) — `CdoObject` internals, field flags,
  FIFO ordering, hash table.
- [language-reference.md](language-reference.md) — syntax for `CLASS`,
  method calls, and operator expressions.
- [vm-internals.md](vm-internals.md) — opcode dispatch and the
  `cando_vm_call_meta` helper.
