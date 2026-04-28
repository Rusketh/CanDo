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
| `__newindex`  | `g_meta_newindex`  | Field assignment when the key is new     |
| `__type`      | `g_meta_type`      | `type(obj)` returns this string          |
| `__tostring`  | `g_meta_tostring`  | `toString(obj)` — calls the metamethod   |
| `__len`       | `g_meta_len`       | `#obj` — custom length                   |
| `__eq`        | `g_meta_eq`        | `obj == other` and `obj != other`        |
| `__lt`        | `g_meta_lt`        | `a < b` and `a > b` (swapped args)      |
| `__le`        | `g_meta_le`        | `a <= b` and `a >= b` (swapped args)    |
| `__add`       | `g_meta_add`       | `a + b`                                  |
| `__sub`       | `g_meta_sub`       | `a - b`                                  |
| `__mul`       | `g_meta_mul`       | `a * b`                                  |
| `__div`       | `g_meta_div`       | `a / b`                                  |
| `__mod`       | `g_meta_mod`       | `a % b`                                  |
| `__pow`       | `g_meta_pow`       | `a ^ b`                                  |
| `__unm`       | `g_meta_unm`       | Unary `-obj`                             |
| `__idiv`      | `g_meta_idiv`      | Integer division (reserved)              |
| `__call`      | `g_meta_call`      | `obj(...)` when `obj` is not a function  |
| `__constructor` | `g_meta_constructor` | Run by the default class `__call`       |

Metamethods can be **native** C functions, inline **script** functions
(number PC offsets), or **OBJ_FUNCTION** closures.  The VM dispatcher
(`cando_vm_call_meta`) handles all three.

---

## `__index` — prototype-chain lookup

Whenever the VM reads a field that does not exist in an object's own hash
table, it checks whether that object has an `__index` field pointing to
another object, and repeats the lookup there.  The chain is followed up to
`CANDO_PROTO_DEPTH_MAX` (32) levels.

```cando
VAR proto = { x: 10, greet: "hello" };
VAR child = { y: 20 };
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

## `__newindex` — intercept field assignment

When assigning a key that does **not** already exist in the object's own
fields, the VM checks the prototype chain for a `__newindex` metamethod.  If
found, it is called with `(self, key, value)` instead of performing the raw
set.  If the key already exists on the object, the assignment proceeds
directly (no metamethod call).

```cando
VAR log = [];
VAR proxy_proto = {
    __newindex: FUNCTION(self, key, val) {
        log:push("set:" + key);
    }
};

VAR obj = {};
object.setPrototype(obj, proxy_proto);
obj.foo = 42;
print(log[0]);       // set:foo
```

---

## `__type` — custom type tag

`type(v)` normally returns `"null"`, `"bool"`, `"number"`, `"string"`, or
`"object"`.  If the object has a `__type` field, that string is returned
instead.

```cando
VAR vec = { __type: "Vec3", x: 1, y: 2, z: 3 };
print(type(vec));    // Vec3
```

`CLASS` sets `__type` automatically as an immutable (`FIELD_STATIC`) field.

---

## `CLASS` — callable class objects

`CLASS` produces a plain object that is **callable**.  Calling the class
runs a default `__call` wrapper that allocates a fresh instance, links it
to the class via `__index`, and runs the class's `__constructor` on the
new instance.  The body between the braces is the constructor body; its
parameters come right after the `=` sign.

```cando
CLASS Point = (self, x, y) {
    self.x = x;
    self.y = y;
}
Point.dist = FUNCTION(self) {
    RETURN math.sqrt(self.x * self.x + self.y * self.y);
};
Point.sum = FUNCTION(self) {
    RETURN self.x + self.y;
};

print(type(Point));           // Point  (__type set by CLASS)
VAR p = Point(3, 4);
print(p:dist());              // 5
print(p:sum());               // 7
```

### What a class is, exactly

`class Vector = (self, x, y, z) { BODY }` desugars to roughly:

```cando
VAR Vector = { __type: "Vector" };
Vector.__constructor = FUNCTION(self, x, y, z) { BODY };
Vector.__call = <vm-supplied default-call wrapper>;
```

When the class is invoked --  `Vector(1, 2, 3)`  --  `OP_CALL` follows
`__call` to the default wrapper, which:

1. Allocates a new object (the instance).
2. Sets `instance.__index = Vector` so methods on the class are reachable
   from the instance via the prototype chain.
3. If the class has a `__constructor` field, calls it with
   `(instance, args…)`.  The constructor's return value is discarded.
4. Returns the instance.

This means an instance is just a plain object whose `__index` points back
at the class.  All method lookups (`obj:method()`, operator metamethods,
`__type`, `__tostring`) flow through that link.

### Three forms

```cando
class Vector = (self, x, y, z) { ... }                  // statement
var Vector = class (self, x, y, z) { ... };             // anonymous expr
var Vector = class Vector (self, x, y, z) { ... };      // named expr
class Empty = { }                                       // params optional
```

The statement form requires the leading `=`; both expression forms drop
it.  Naming is optional in the expression form -- if absent, the class
has no `__type`.

### How `CLASS` compiles

For `class Vector = (self, x, y, z) { BODY }` the parser emits:

1. `OP_NEW_CLASS Vector` (or `OP_NEW_OBJECT` for the anonymous form) --
   allocates the class object, sets `__type` to the class name when
   present, and pushes it onto the stack.
2. (If `extends Parent` was given) `OP_LOAD_GLOBAL Parent` is emitted
   *before* the class object so the stack reads `[..., parent, class]`,
   followed by `OP_INHERIT` which sets `class.__index = parent` and pops
   the parent.
3. The constructor body is compiled inline as a closure: a forward
   `OP_JUMP` over the body, the body itself ending in `OP_RETURN`, then
   `OP_CLOSURE` to push the closure value, then
   `OP_BIND_METHOD __constructor`.
4. `OP_BIND_DEFAULT_CALL` -- sets `class.__call` to the VM's
   built-in default-constructor native.
5. (Statement form only) `OP_DEF_GLOBAL Vector` binds the class as a
   global.

### Inheritance with `EXTENDS`

`EXTENDS` chains two classes together by setting the child class's
`__index` to the parent.  Inside a method, parent dispatch is done
directly through `Child.__index` -- there is no `super` keyword.

```cando
class Animal = (self, name) { self.name = name; }
Animal.speak = FUNCTION(self) { RETURN self.name + " says hello"; };

class Dog extends Animal = (self, name, breed) {
    Animal.__constructor(self, name);   // delegate to parent ctor
    self.breed = breed;
}
Dog.bark = FUNCTION(self) {
    RETURN Dog.__index.speak(self) +     // -> Animal.speak
           " (woof, " + self.breed + ")";
};

VAR rex = Dog("Rex", "labrador");
print(type(rex));           // Dog
print(rex:speak());         // Rex says hello   (inherited from Animal)
print(rex:bark());          // Rex says hello (woof, labrador)
```

---

## `__tostring` — custom string conversion

`toString(obj)` checks for a `__tostring` metamethod.  If present, it is
called with the object as the sole argument and should return a string.  If
absent, `toString` falls back to the default representation.

```cando
CLASS Vec = (self, x, y) {
    self.x = x;
    self.y = y;
}
Vec.__tostring = FUNCTION(v) {
    RETURN "Vec(" + toString(v.x) + "," + toString(v.y) + ")";
};
print(toString(Vec(3, 4)));   // Vec(3,4)
```

---

## `__len` — custom length

`#obj` first checks for a `__len` metamethod.  If present, it is called
with the object and its return value is used.  Otherwise,
`cdo_object_length` is used directly:

- Arrays: `items_len` (number of dense elements).
- Plain objects: `field_count` (number of live hash-table entries).

```cando
Vec.__len = FUNCTION(v) { RETURN v.x + v.y; };
VAR v = Vec.make(10, 25);
print(#v);    // 35
```

---

## Comparison metamethods

### `__eq` — equality

Dispatched for `==` and `!=`.  Receives `(a, b)` and should return a
boolean.  For `!=`, the VM negates the result.

```cando
Vec.__eq = FUNCTION(a, b) { RETURN a.x == b.x && a.y == b.y; };
print(Vec.make(1, 2) == Vec.make(1, 2));   // true
print(Vec.make(1, 2) != Vec.make(3, 4));   // true
```

### `__lt` — less than

Dispatched for `<` directly and for `>` with swapped arguments.

```cando
Vec.__lt = FUNCTION(a, b) {
    RETURN (a.x*a.x + a.y*a.y) < (b.x*b.x + b.y*b.y);
};
VAR small = Vec.make(1, 1);
VAR big   = Vec.make(3, 4);
print(small < big);    // true
print(big > small);    // true  (dispatches __lt(small, big))
```

### `__le` — less than or equal

Dispatched for `<=` directly and for `>=` with swapped arguments.

```cando
Vec.__le = FUNCTION(a, b) {
    RETURN (a.x*a.x + a.y*a.y) <= (b.x*b.x + b.y*b.y);
};
print(Vec.make(3,4) <= Vec.make(3,4));   // true
print(Vec.make(3,4) >= Vec.make(1,1));   // true
```

---

## Arithmetic metamethods

All binary arithmetic metamethods receive `(a, b)` and should return the
computed result.  They are dispatched when at least one operand is an object.

| Meta-key | Operator | Example |
|---|---|---|
| `__add` | `a + b` | Vector addition |
| `__sub` | `a - b` | Vector subtraction |
| `__mul` | `a * b` | Component-wise multiplication |
| `__div` | `a / b` | Component-wise division |
| `__mod` | `a % b` | Component-wise modulo |
| `__pow` | `a ^ b` | Component-wise power |

```cando
Vec.__add = FUNCTION(a, b) { RETURN Vec.make(a.x + b.x, a.y + b.y); };
Vec.__sub = FUNCTION(a, b) { RETURN Vec.make(a.x - b.x, a.y - b.y); };
Vec.__mul = FUNCTION(a, b) { RETURN Vec.make(a.x * b.x, a.y * b.y); };
Vec.__div = FUNCTION(a, b) { RETURN Vec.make(a.x / b.x, a.y / b.y); };
Vec.__mod = FUNCTION(a, b) { RETURN Vec.make(a.x % b.x, a.y % b.y); };
Vec.__pow = FUNCTION(a, b) { RETURN Vec.make(a.x ^ b.x, a.y ^ b.y); };

VAR result = Vec.make(1, 2) + Vec.make(3, 4);
print(result.x);    // 4
print(result.y);    // 6
```

### `__unm` — unary minus

Receives a single argument `(obj)` and returns the negated value.

```cando
Vec.__unm = FUNCTION(v) { RETURN Vec.make(-v.x, -v.y); };
VAR neg = -Vec.make(5, -3);
print(neg.x);       // -5
print(neg.y);       // 3
```

### `__idiv` — integer division

The `__idiv` meta-key is defined and interned but no `//` operator exists in
the language yet.  It is reserved for future use.

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

## The `_meta` global registry

Native libraries that hand out instance objects (HTTP request/response,
servers, etc.) wire them up to a shared prototype stored at `_meta.<name>`.
Because `_meta` is an ordinary writable global, user code can attach new
methods that propagate to every instance through the `__index` chain:

```cando
_meta.http_response.write = FUNCTION(self, data) {
    self.body = self.body + data;
};
```

Each subtable is created lazily; its `__type` is stamped as a `FIELD_STATIC`
constant equal to the type name (`"http_response"` for example), so
`type(instance)` reflects the tag.  Default native methods are inserted with
`FIELD_NONE` flags so user code may override them.

The same pattern is used by the [`socket`](standard-library.md#socket)
and [`secure_socket`](standard-library.md#secure_socket) libraries.
Adding a method once on `_meta.tcp_socket` makes it visible on every
plain-TCP connection (including those created in a child VM by
`createServer`):

```cando
VAR LF = '
';
_meta.tcp_socket.writeLine = FUNCTION(self, line) { self:sendAll(line + LF); };
```

`_meta.tls_socket` is a separate table — it does *not* chain to
`_meta.tcp_socket` via `__index`, because doing so would make
`type(s)` return `"tcp_socket"` for TLS connections.  Alias methods
explicitly when you want them on both:

```cando
_meta.tls_socket.writeLine = _meta.tcp_socket.writeLine;
```

You can register your own meta tables and use them as prototypes with
`object.setPrototype` for any type you control.

See [standard-library.md `#_meta`](standard-library.md#meta-global-meta-registry)
for the list of built-in subtables and their default methods.

---

## Further reading

- [object-system.md](object-system.md) — `CdoObject` internals, field flags,
  FIFO ordering, hash table.
- [language-reference.md](language-reference.md) — syntax for `CLASS`,
  method calls, and operator expressions.
- [vm-internals.md](vm-internals.md) — opcode dispatch and the
  `cando_vm_call_meta` helper.
