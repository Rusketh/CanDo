# Language Reference

This is the normative reference for the CanDo surface syntax, evaluation
rules, and built-in types.  For each standard-library function see
[standard-library.md](standard-library.md); for `thread`/`await` see
[threading.md](threading.md).

## Lexical structure

### Source encoding

CanDo source is UTF-8.  The lexer only gives special meaning to ASCII
characters; non-ASCII bytes inside string literals are preserved
verbatim.

### Comments

```cando
// single line
/* multi-
   line  */
```

### Identifiers

Match `[A-Za-z_][A-Za-z0-9_]*`.  Case-sensitive.  Keywords are reserved
and cannot be used as identifiers.

### Keywords

Keywords are case-sensitive and — with one exception — upper-case.

```
IF   ELSE   WHILE   FOR   IN   OF   OVER
FUNCTION   RETURN   CLASS
TRY   CATCH   FINALY   THROW
VAR   CONST   GLOBAL   STATIC   PRIVATE
THREAD   ASYNC   AWAIT
TRUE   FALSE   NULL
BREAK   CONTINUE
pipe
```

The lower-case `pipe` is the implicit iteration variable in `~>` and
`~!>` expressions (see *Pipe and filter* below).

> `FINALY` is spelled with one `L`.  `FINALLY` is **not** accepted.

## Types

CanDo has five value types at the VM level:

| Type | Literal | Notes |
|---|---|---|
| `null` | `NULL` | The single absent value |
| `bool` | `TRUE`, `FALSE` | |
| `number` | `42`, `3.14`, `1e-5` | IEEE 754 double, always |
| `string` | `"…"`, `'…'`, `` `…` `` | Immutable, ref-counted |
| `object` | `{…}`, `[…]`, closures | Heap object, referenced via a handle |

Arrays, plain objects, classes, closures and threads are all objects —
they differ only in the internal `ObjectKind` tag.

`type(v)` returns a string name: `"null"`, `"bool"`, `"number"`,
`"string"`, or `"object"`.  If an object exposes a `__type` field, that
value is returned instead.

### Strings

Three quote styles:

| Delimiter | Escapes? | Multiline? | Interpolation? |
|---|---|---|---|
| `"…"` | yes (`\n \t \" \\` …) | no | no |
| `'…'` | no | yes (literal newlines) | no |
| `` `…` `` | yes | yes | yes, `${EXPR}` |

```cando
VAR name = "World";
print(`hello, ${name}!`);          // hello, World!

VAR paragraph = 'line 1
line 2
line 3';
```

`#s` returns the byte length of a string.  Strings are immutable;
methods that "transform" return new strings.

### Arrays and objects

```cando
VAR arr = [10, 20, 30];            // array literal, 0-indexed
VAR obj = { name: "Alice", age: 30 };

arr[1] = 99;                       // index assignment extends as needed
obj.city = "NYC";                  // field assignment adds a field

print(#arr);                       // 3  (length prefix)
print(arr:length());               // 3  (via array prototype)
print(obj.name);                   // dot access
print(obj["name"]);                // bracket access
```

Arrays have dense integer-indexed storage *and* the full object hash
table — you can attach named fields to an array if you want to.  Plain
objects preserve FIFO insertion order when iterated.

## Variables

```cando
VAR x = 1;
CONST PI = 3.14159;                // can never be reassigned
VAR a, b = 10, 20;                 // multi-var parallel assignment
```

`VAR` binds a new variable in the current scope.  `CONST` does the same
but marks the binding as write-protected.

Block scope is lexical and follows `{ }`:

```cando
IF TRUE {
    VAR inside = 42;
    print(inside);
}
// `inside` is not visible here
```

Assignment to an undeclared identifier creates a **global** binding.
Inside functions you should always use `VAR` to keep a variable local.

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+  -  *  /  %  ^` (power) |
| Unary | `-x  +x  !x  ~x  #x  ++x  --x` |
| Comparison | `==  !=  <  >  <=  >=` |
| Logical | `&&  \|\|  !` |
| Bitwise | `&  \|  \|&` (xor) `~` (not) `<<  >>` |
| Assignment | `=  +=  -=  *=  /=  %=  ^=` |
| Indexing | `a[i]   obj.field   obj["field"]` |
| Call | `f(...)   obj:method(...)   obj::method(...)` |
| Range | `1 -> 10`  (ascending)  `10 <- 1`  (descending) |
| Length | `#x` |
| Pipe | `arr ~> pipe * 2`  (map)  `arr ~!> { … }`  (filter) |
| Mask | `(~.~) expr`  (selector, see below) |
| Vararg/spread | `...args` in parameters;  `...expr` in call-site |

Precedence follows C conventions.  `^` is right-associative and binds
tighter than `*`; `&&` short-circuits before `||`.

### Method call: `:` and `::`

```cando
"hello":toUpper()          // -> "HELLO"    (normal method call)
obj:method(a, b)           // obj is passed as the first argument (self)
```

The double-colon variant returns the receiver instead of the method's
result, so calls chain:

```cando
obj::set_x(3)::set_y(4)::set_z(5);
```

### Range generators

`a -> b` and `b <- a` both produce the inclusive integer range
`[a, b]`.  They are most often used inside `FOR … IN`:

```cando
FOR i IN 1 -> 5 { print(i); }      // 1 2 3 4 5
FOR i IN 5 <- 1 { print(i); }      // 5 4 3 2 1
```

A range used in a value context expands to a stack of numbers.

## Control flow

### `IF` / `ELSE`

```cando
IF x > 0 {
    print("positive");
} ELSE IF x < 0 {
    print("negative");
} ELSE {
    print("zero");
}
```

Multi-comparison — test one value against a list of alternatives:

```cando
IF code == 200, 201, 204 {
    print("success");
}

IF grade > 50, 60, 70 {
    // grade is greater than ALL of 50, 60, 70  (i.e. grade > 70)
    print("solid pass");
}
```

`== …` and `!= …` use *any* / *none* semantics; the ordering operators
(`<`, `<=`, `>`, `>=`) require the relation against *all* right-hand
values.

### `WHILE`

```cando
WHILE condition {
    …
}
```

### `FOR`

```cando
FOR i IN 1 -> 10 { … }             // ascending inclusive range
FOR i IN 10 <- 1 { … }             // descending inclusive range

FOR k IN object { … }              // keys
FOR v OF array  { … }              // values (arrays iterate in index order,
                                   //         objects in FIFO insertion order)
```

`FOR … OVER` implements the Lua-style generic iterator protocol.  The
expression to the right of `OVER` must evaluate to three values — an
iterator function, a state, and an initial control value.  Each
iteration calls `iter(state, control)` and binds the named loop
variables to the returned values.  Iteration ends when the iterator
returns `NULL` as the new control value.

```cando
FUNCTION my_pairs(t) {
    RETURN FUNCTION(s, c) {
        IF c >= #s { RETURN NULL; }
        RETURN c + 1, c, s[c];           // new control, then yielded values
    }, t, 0;
}

FOR k, v OVER my_pairs([10, 20, 30]) {
    print(k, v);                          // 0 10 / 1 20 / 2 30
}
```

### `BREAK`, `CONTINUE`

```cando
BREAK;                // exit the innermost loop
BREAK 2;              // exit two levels of nesting
CONTINUE;             // next iteration of the innermost loop
```

The optional numeric argument is the loop depth to skip (0 =
innermost).

### `TRY` / `CATCH` / `FINALY` / `THROW`

```cando
TRY {
    risky_work();
} CATCH (err) {
    print("failed:", err);
} FINALY {
    cleanup();
}
```

`THROW` accepts one or more values; `CATCH` accepts one or more
parameters:

```cando
TRY {
    THROW 404, "not found";
} CATCH (code, msg) {
    print(code, msg);
}
```

Excess thrown values are dropped; missing ones arrive as `NULL`.
Runtime errors (like division by zero or calling a non-function) are
catchable the same way — the error message is thrown as a string.

## Functions

### Definition

```cando
FUNCTION add(a, b) {
    RETURN a + b;
}
```

Anonymous / expression form:

```cando
VAR add = FUNCTION(a, b) { RETURN a + b; };
arr:map(FUNCTION(x) { RETURN x * x; });
```

### Multiple return values

```cando
FUNCTION minmax(a, b) {
    IF a < b { RETURN a, b; }
    RETURN b, a;
}

VAR lo, hi = minmax(7, 3);
```

When a multi-return call is used inside a larger expression list it
spreads — for example, `f(g())` passes all of `g`'s return values as
separate arguments to `f`.

### Closures

Inner functions capture enclosing local variables:

```cando
FUNCTION make_counter() {
    VAR n = 0;
    RETURN FUNCTION() {
        n = n + 1;
        RETURN n;
    };
}

VAR c = make_counter();
print(c(), c(), c());             // 1 2 3
```

Captured variables are promoted to the heap when their defining frame
returns.  They remain shared across every closure that captured them.

### Varargs and spreading

```cando
FUNCTION log(tag, ...rest) {
    print(tag, ...rest);
}

log("INFO", "started", 42, TRUE);
```

At the call site, `...expr` spreads a multi-valued expression (array,
multi-return call, or `...rest`) into the argument list.

### Mask syntax

Masks select positions out of a multi-value expression.  `~` keeps a
value, `.` skips it:

```cando
// (~.~) means "keep, skip, keep"
VAR x, z = (~.~) 1, 2, 3;          // x = 1, z = 3

FUNCTION triple() { RETURN 10, 20, 30; }

VAR first, third = (~.~) triple(); // first = 10, third = 30
```

Pure-`~` masks consume exactly their width and ignore anything past it;
pure-`.` masks skip exactly their width and pass everything past it
through unchanged.  Mixed masks (`~`/`.` interleaved) apply strictly
per position.

## Pipe and filter

Both operators iterate the array on the left and evaluate the block on
the right with the special variable `pipe` bound to the current element.

```cando
VAR nums = [1, 2, 3, 4, 5];

VAR tens   = nums ~> pipe * 10;                      // [10,20,30,40,50]
VAR evens  = nums ~!> { IF pipe % 2 == 0 { RETURN pipe; } };
VAR big    = nums ~!> { IF pipe > 3 { RETURN pipe; } };
```

- `~>` (pipe) always produces an array of the same length — the body's
  last expression, or explicit `RETURN` value, is the mapped element.
- `~!>` (filter) produces an array containing only the elements for
  which the body returns a non-null value.

`pipe` is lexically scoped to the body.  Nested pipes work, each with
their own `pipe` binding.

## Classes

`CLASS` defines an object whose methods are stored as fields and
accessible via the `__index` prototype chain.  Method calls with `:`
pass the receiver as the first argument (`self`).

A factory method should call `object.setPrototype(inst, ClassName)` so
that instances inherit the class methods:

```cando
CLASS Point {
    FUNCTION make(x, y) {
        VAR p = { x: x, y: y };
        object.setPrototype(p, Point);    // p.__index = Point
        RETURN p;
    }
    FUNCTION dist(self) {
        RETURN math.sqrt(self.x * self.x + self.y * self.y);
    }
}

print(type(Point));        // Point  (__type set by CLASS)
VAR p = Point.make(3, 4);
print(p:dist());           // 5
```

`CLASS` automatically sets `__type` to the class name (immutable).
Method declarations inside a class body may be preceded by `STATIC`
and/or `PRIVATE`, which are accepted by the parser as field-flag hints.
See [metamethods.md](metamethods.md) for the full prototype system and
all available meta-keys.

## Threads

A full treatment lives in [threading.md](threading.md).  At a glance:

```cando
VAR t = thread {                   // spawn an OS thread
    thread.sleep(10);
    RETURN "hi";
};

VAR msg = await t;                 // join and receive the return value
print(msg);                        // hi
```

`thread` accepts either a block (`thread { … }`) or a plain call
expression (`thread f(x)`).  `await` blocks the current thread until
the target finishes and yields its return values.

## Modules

`include("./lib.cdo")` loads a script module.  The module's top-level
`RETURN` value (or the last expression in eval mode) is cached keyed by
the canonical path.  Subsequent `include()` calls with the same path —
relative or absolute — return the cached value without re-executing.

Binary extension modules (`.so` / `.dylib` / `.dll`) are loaded the
same way; see [writing-extensions.md](writing-extensions.md).

## Errors raised by the runtime

The following operations raise catchable errors:

- Division or modulo by zero on numbers
- Calling a non-callable value (`v()`)
- Indexing `NULL` with a field or `[idx]`
- Assigning to a `CONST` variable
- `RETURN` or `BREAK` outside a valid target (parse error)
- Stack overflow (call depth > 256, value stack > 2048)

The error message is the value passed to the `CATCH` parameter.  If
uncaught, the host embedder sees `CANDO_ERR_RUNTIME` and
`cando_errmsg(vm)` returns the formatted message.
