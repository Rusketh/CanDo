# Classes

CanDo's `CLASS` is sugar over a prototype-based object system.  A class
is just a regular object whose `__call` metamethod creates fresh
instances and whose `__index` gives the instances method dispatch.

## Defining a class

The body between the braces is the **constructor body**; the parameter
list comes right after the `=` sign:

```cdo
CLASS Vector = (self, x, y, z) {
    self.x = x;
    self.y = y;
    self.z = z;
}

VAR v = Vector(1, 2, 3);
print(type(v));              // Vector
print(v.x, v.y, v.z);        // 1 2 3
```

Methods (including operator metamethods) are added afterwards as
ordinary field assignments on the class:

```cdo
Vector.length = FUNCTION(self) {
    RETURN math.sqrt(self.x * self.x +
                     self.y * self.y +
                     self.z * self.z);
};

Vector.__add = FUNCTION(a, b) {
    RETURN Vector(a.x + b.x, a.y + b.y, a.z + b.z);
};

VAR sum = Vector(1, 2, 3) + Vector(4, 5, 6);
print(sum:length());         // ~10.49
```

When you call `Vector(1, 2, 3)` the runtime:

1. Allocates a new plain object.
2. Sets `instance.__index = Vector`, so methods on the class are
   reachable from the instance.
3. Sets `instance.__type = "Vector"` (so `type(instance)` returns
   `"Vector"`).
4. Invokes the constructor body with the new object as `self`.

## The three forms

```cdo
// 1. Statement form — declares a global named after the class.
//    The leading `=` is required.  __type is set to the class name.
CLASS Vector = (self, x, y, z) { … }

// 2. Anonymous expression form — no __type is set.
VAR Vector = CLASS (self, x, y, z) { … };

// 3. Named expression form — __type = "Vector".
VAR Vector = CLASS Vector (self, x, y, z) { … };
```

The parameter list is optional; `CLASS Foo = { }` declares an empty
class with no constructor arguments.

## Inheritance

`EXTENDS` records a parent class so that field lookups fall through the
parent when a key is not present on the child or its instance:

```cdo
CLASS Animal = (self, name) { self.name = name; }
Animal.speak = FUNCTION(self) { RETURN self.name + " says hello"; };

CLASS Dog EXTENDS Animal = (self, name, breed) {
    Animal.__constructor(self, name);    // call the parent constructor
    self.breed = breed;
}
Dog.bark = FUNCTION(self) {
    // Dog.__index points at Animal.
    RETURN Dog.__index.speak(self) + " (woof, " + self.breed + ")";
};

VAR rex = Dog("Rex", "labrador");
print(rex:bark());           // Rex says hello (woof, labrador)
```

There is **no `super` keyword**.  Use the parent class object directly
to call a parent method; in single-inheritance scenarios `Dog.__index`
*is* the parent class.

`EXTENDS` only affects method *lookup*; the parent's constructor body
is **not** called automatically.  Call it explicitly through
`Animal.__constructor(self, ...)` if you want parent initialization.

## Metamethods

Operator overloads, `__call`, `__tostring`, `__index`, `__len`, and the
rest are described in detail below.  Set them by ordinary field
assignment on the class:

```cdo
CLASS Counter = (self, start) { self.n = start; }
Counter.__call = FUNCTION(self) { self.n = self.n + 1; RETURN self.n; };

VAR c = Counter(0);
print(c());                  // 1
print(c());                  // 2
print(c());                  // 3
```

### Operator metamethods

When the runtime encounters an operator on a non-numeric receiver, it
walks the receiver's `__index` chain looking for the corresponding
metamethod.  If found, the metamethod is called with the receiver and
the right-hand operand (left-hand for unary).

| Operator       | Metamethod   | Signature                      |
|----------------|--------------|--------------------------------|
| `a + b`        | `__add`      | `(a, b) → result`              |
| `a - b`        | `__sub`      | `(a, b) → result`              |
| `a * b`        | `__mul`      | `(a, b) → result`              |
| `a / b`        | `__div`      | `(a, b) → result`              |
| `a % b`        | `__mod`      | `(a, b) → result`              |
| `a ^ b`        | `__pow`      | `(a, b) → result`              |
| `-a`           | `__unm`      | `(a) → result`                 |
| `#a`           | `__len`      | `(a) → number`                 |
| `a == b`       | `__eq`       | `(a, b) → bool` (only when both sides are objects of the same class) |
| `a < b`        | `__lt`       | `(a, b) → bool`                |
| `a <= b`       | `__le`       | `(a, b) → bool`                |
| `a..b`         | `__concat`   | (string concat fallback)       |
| `a(args)`      | `__call`     | `(a, ...args) → ...result`     |

For the binary operators, if the left operand has no metamethod, the
right operand's metamethod is consulted.

### Lookup metamethods

| Metamethod | When it fires                                               |
|------------|-------------------------------------------------------------|
| `__index`  | Field read on the receiver missed.  May be an object (its keys are searched) or a function (called with the receiver and key, return value becomes the read result). |
| `__newindex` | Field write on the receiver missed.  Function form is called with `(receiver, key, value)`. |

```cdo
CLASS LazyDict = (self) { self._loaded = {}; }
LazyDict.__index = FUNCTION(self, key) {
    IF self._loaded[key] == NULL {
        self._loaded[key] = expensive_load(key);
    }
    RETURN self._loaded[key];
};
```

### Display metamethods

| Metamethod    | Used by                                                  |
|---------------|----------------------------------------------------------|
| `__tostring`  | `toString(v)`, string concatenation fallback, `print`.   |
| `__type`      | Reported by `type(v)`.  Strings only; no fallback.       |

```cdo
CLASS Money = (self, cents) { self.cents = cents; }
Money.__tostring = FUNCTION(self) {
    RETURN string.format("$%d.%02d", self.cents / 100, self.cents % 100);
};
print(Money(1234));          // $12.34
```

### Truthiness metamethod

| Metamethod | Used by                                                       |
|------------|---------------------------------------------------------------|
| `__is`     | `IF`, `ELSE IF`, `&&`, `\|\|`, `!`, and any boolean context.  |

`__is(self)` is invoked whenever the object appears in a boolean
context.  Its return value is itself tested for truthiness using the
default rules (`NULL`, `FALSE`, and `0` are falsy; everything else is
truthy).

```cdo
CLASS Box = (self, n) { self.n = n; }
Box.__is = FUNCTION(self) { RETURN self.n > 0; };

IF Box(0) { /* skipped */ }
IF Box(5) { print("non-empty"); }    // non-empty
```

### Field flags (advanced)

Fields can carry flags that change how they behave under writes and
serialization.  These are exposed through the `object` and `_meta`
libraries — see [../libraries/object.md](../libraries/object.md) and
[../libraries/meta.md](../libraries/meta.md).

## Member modifiers

The reserved words `STATIC` and `PRIVATE` mark class members.

```cdo
CLASS Counter = (self) { self.n = 0; }

STATIC Counter.kind = "counter";    // shared across all instances
PRIVATE Counter.tick = FUNCTION(self) { self.n = self.n + 1; };
```

- `STATIC` means the field lives on the class object itself, not the
  instance — every instance reads through `__index` and sees the same
  value.
- `PRIVATE` is a soft contract: it suppresses the member from
  `inspect()` output and from `object.keys()` enumeration.  It does
  **not** enforce access control.

## Default `__call` for classes

Every class object gets a default `__call` that builds a new instance
and runs the constructor body.  You can override it — but doing so
removes the constructor sugar; you have to allocate and initialize the
instance yourself.

```cdo
CLASS Singleton = (self) { self.created = datetime.now(); }
VAR _instance = NULL;
Singleton.__call = FUNCTION(class) {
    IF _instance == NULL {
        _instance = { __index: class, __type: "Singleton" };
        class.__constructor(_instance);
    }
    RETURN _instance;
};
```

## Examples — common patterns

### Value type with operator overloading

```cdo
CLASS Color = (self, r, g, b) { self.r = r; self.g = g; self.b = b; }
Color.__add      = FUNCTION(a, b) {
    RETURN Color(a.r + b.r, a.g + b.g, a.b + b.b);
};
Color.__tostring = FUNCTION(self) {
    RETURN `rgb(${self.r},${self.g},${self.b})`;
};

print(Color(10, 20, 30) + Color(1, 2, 3));    // rgb(11,22,33)
```

### Inheritance with shared utility methods

```cdo
CLASS Shape = (self) { }
Shape.area      = FUNCTION(self) { THROW "not implemented"; };
Shape.describe  = FUNCTION(self) {
    RETURN type(self) + " of area " + toString(self:area());
};

CLASS Circle EXTENDS Shape = (self, r) { self.r = r; }
Circle.area = FUNCTION(self) { RETURN math.pi * self.r ^ 2; };

CLASS Square EXTENDS Shape = (self, s) { self.s = s; }
Square.area = FUNCTION(self) { RETURN self.s ^ 2; };

VAR shapes = [Circle(3), Square(4)];
FOR s OF shapes { print(s:describe()); }
// Circle of area 28.27...
// Square of area 16
```

### Mixin via metamethod chaining

```cdo
VAR Stringable = {};
Stringable.toString = FUNCTION(self) {
    RETURN inspect(self);
};

CLASS Foo = (self, x) { self.x = x; }
object.assign(Foo, Stringable);     // copy the methods over
print(Foo(7):toString());           // { x: 7, __type: "Foo" }
```

## Internals

The `CLASS` keyword desugars to a sequence of bytecode instructions
(`OP_NEW_CLASS`, `OP_BIND_METHOD`, `OP_INHERIT`,
`OP_BIND_DEFAULT_CALL`).  The full sequence and the resulting object
shape are documented in
[../internals/vm.md](../internals/vm.md).
