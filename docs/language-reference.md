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

Keywords are case-insensitive but **reject mixed-case** spellings.  `VAR`,
`var`, `IF`, `if` all work; `Var` and `iF` are ordinary identifiers.  The
canonical CanDo style is **all-uppercase** for keywords; the all-lowercase
form is supported for users coming from other languages.

```
IF   ELSE   ALSO   WHILE   FOR   IN   OF   OVER
FUNCTION   RETURN   CLASS   EXTENDS
TRY   CATCH   FINALY   THROW
VAR   CONST   GLOBAL   STATIC   PRIVATE
THREAD   ASYNC   AWAIT
TRUE   FALSE   NULL
BREAK   CONTINUE   SETTLE
pipe
```

The lower-case `pipe` is the implicit iteration variable in `~>` and
`~!>` expressions (see *Pipe and filter* below); like other keywords its
spelling can be either all-lower or all-upper, but the convention is
lowercase to make it visually distinct from statement keywords.

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
| Safe indexing | `obj?.field   obj?[expr]`  (null short-circuits) |
| Call | `f(...)   obj:method(...)   obj::method(...)` |
| Range | `1 -> 10`  (ascending)  `10 <- 1`  (descending) |
| Length | `#x` |
| Pipe | `arr ~> pipe * 2`  (map)  `arr ~!> { … }`  (filter)  `arr ~&> { … }`  (conditional filter) |
| Conditional | `cond ? then_expr : else_expr` |
| Mask | `(~.~) expr`  (selector, see below) |
| Vararg/spread | `...args` in parameters;  `...expr` in call-site |

Precedence follows C conventions.  `^` is right-associative and binds
tighter than `*`; `&&` short-circuits before `||`.  The ternary `?:` is
right-associative and binds looser than `||`, so
`a || b ? c : d` parses as `(a || b) ? c : d` and
`a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`.

### Logical `||` and `&&` (Lua-style short-circuit)

`||` returns its **left operand** if that value is truthy; otherwise it
returns its **right operand** verbatim — the result is *not* coerced to a
boolean.  `&&` mirrors the rule: it returns the left operand if falsy,
otherwise the right operand.

```cando
print(FALSE || 0);          // 0
print(NULL  || "fallback"); // fallback
print(FALSE || FALSE);      // false
print("first" || "second"); // first
```

Only `NULL` and `FALSE` are falsy.  `0`, `""`, and empty
arrays/objects are all truthy.

### Ternary conditional `? :`

```cando
VAR label = score >= 50 ? "pass" : "fail";
```

The condition is evaluated once.  Only the chosen branch is evaluated, so
`?:` short-circuits like `IF`/`ELSE`.  Right-associativity makes
chains read top-to-bottom:

```cando
VAR grade = pct >= 90 ? "A"
          : pct >= 80 ? "B"
          : pct >= 70 ? "C" : "F";
```

### Safe access `?.` and `?[]`

`obj?.field` and `obj?[expr]` evaluate the receiver first; if it is `NULL`
the whole chain short-circuits to `NULL` without evaluating the rest of
the access.  Once the chain is "safe", every subsequent `.`, `[`, `:`, or
`(` in the same expression also short-circuits if it sees `NULL`, so a
gap anywhere in the chain propagates cleanly:

```cando
VAR obj = { a: { b: 42 } };
print(obj?.a.b);      // 42
print(obj?.x.y.z);    // null  -- stops at obj.x
print(obj?["a"].b);   // 42
print(NULL?.a.b);     // null  -- never dereferences NULL
```

Use `?.` to safely walk possibly-missing nested data without ringing the
runtime "field access on non-object" error.

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

### `IF` / `ELSE` / `ALSO`

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

#### `ALSO` — inclusive branches

`ALSO` makes a branch *inclusive*: it fires when the immediately-preceding
branch in the chain ran (in addition to the standard `else`-style
fallback).  This turns an `IF` chain into a switch/case-style construct
where multiple cases can fire.

```cando
IF true { print("A"); }
ALSO IF false { print("B"); }
ALSO         { print("C"); }
//  → A, B, C all run
```

`ALSO` and `ELSE` may be mixed in the same chain.  Each branch in a chain
sees two runtime flags:

- **`matched`** — true once any branch has fired in this chain.
- **`prev_ran`** — true iff the immediately-preceding branch fired.

| Branch | Fires when |
|---|---|
| `IF C` | `C` |
| `ELSE IF C` | `!matched && C` |
| `ELSE` | `!matched` |
| `ALSO IF C` | `prev_ran` **OR** `(!matched && C)` &nbsp; *(when `prev_ran`, the body runs unconditionally and `C` is ignored)* |
| `ALSO` | `prev_ran` **OR** `!matched` |

Worked examples:

```cando
// Switch-style fall-through:
IF n > 0   { print("positive"); }
ALSO IF n > 5  { print(">5");  }   // fires if `positive` ran
ALSO IF n > 10 { print(">10"); }   // fires if previous ran
ELSE       { print("non-positive"); }

describe(20);   //  positive  >5  >10
describe(3);    //  positive  >5  >10   (also-if cond ignored when prev ran)
describe(-1);   //  non-positive

// Mixed else / also:
IF false      { /* skip */ }
ELSE IF true  { print("B"); }   //   matched := true
ALSO IF false { print("C"); }   //   prev_ran true → fires (cond ignored)
ELSE          { /* skip */ }    //   matched still true
//  → B, C
```

Constraints:

- `ALSO` may only follow `IF`, `ELSE IF`, `ELSE`, `ALSO IF`, or `ALSO`.
  A bare `ALSO { … }` at the start of a statement is a parse error.
- `ELSE` / `ELSE IF` after an `ALSO` is still legal and is gated against
  the `matched` flag exactly as before.

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

### `BREAK`, `CONTINUE`, `SETTLE`

```cando
BREAK;                // exit the innermost loop
BREAK 2;              // exit two levels of nesting
CONTINUE;             // next iteration of the innermost loop
CONTINUE 1;           // skip to the next iteration of the loop one level out

SETTLE;               // exit the innermost IF chain
SETTLE 1;             // exit two nested IF chains
```

The optional numeric argument is the depth to skip (0 = innermost).

`BREAK` / `CONTINUE` count enclosing **loops** only; `SETTLE` counts
enclosing **`IF` chains** only.  The two are independent: a loop inside
an `IF` chain is transparent to `SETTLE`, and an `IF` chain inside a
loop is transparent to `BREAK`.

```cando
IF condition {
    WHILE active {
        IF should_stop { SETTLE; }   // exits the OUTER `IF` chain;
                                      // the WHILE is skipped over.
        IF want_break  { BREAK; }    // exits the WHILE; the outer
                                      // `IF` chain continues.
        do_work();
    }
    cleanup();                        // skipped by SETTLE, reached by BREAK.
}
```

Errors:

- `BREAK outside loop` / `CONTINUE outside loop` — `BREAK n` / `CONTINUE n`
  used where there is no loop at depth `n`.
- `SETTLE outside IF` — `SETTLE n` used where there is no `IF` chain at
  depth `n`.

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
VAR evens2 = nums ~&> pipe % 2 == 0;                 // [2, 4]
VAR small  = nums ~&> { RETURN pipe < 3; };          // [1, 2]
```

- `~>` (pipe) always produces an array of the same length — the body's
  last expression, or explicit `RETURN` value, is the mapped element.
- `~!>` (filter) produces an array containing only the elements for
  which the body returns a non-null value.  The body's *return value*
  is what ends up in the output, so `~!>` doubles as a map+filter.
- `~&>` (conditional filter) treats the body as a boolean predicate.
  When the body returns a truthy value the **original element** is
  kept (the body's return value is discarded); when it returns a
  falsy value the element is dropped.  Use `~&>` when you want a
  pure filter without the map.

`pipe` is lexically scoped to the body.  Nested pipes work, each with
their own `pipe` binding.

## Classes

`CLASS` defines a callable table.  Calling the class
(`Vector(1, 2, 3)`) produces a fresh instance whose `__index` points back
at the class so methods are reachable via the prototype chain.  The body
between the braces is the **constructor body**; the parameter list comes
right after the `=` sign:

```cando
CLASS Vector = (self, x, y, z) {
    self.x = x;
    self.y = y;
    self.z = z;
}

VAR v = Vector(1, 2, 3);
print(type(v));            // Vector
print(v.x, v.y, v.z);      // 1 2 3
```

Methods, including operator metamethods, are added afterwards as ordinary
field assignments on the class:

```cando
Vector.__add = FUNCTION(a, b) {
    RETURN Vector(a.x + b.x, a.y + b.y, a.z + b.z);
};
Vector.length = FUNCTION(self) {
    RETURN math.sqrt(self.x * self.x +
                     self.y * self.y +
                     self.z * self.z);
};

VAR sum = Vector(1, 2, 3) + Vector(4, 5, 6);
print(sum:length());       // ~10.49
```

### Three forms

```cando
// Statement form -- declares a global named after the class.
//   - The leading `=` is required.
//   - __type is set to the class name.
class Vector = (self, x, y, z) { ... }

// Anonymous expression form -- no __type is set.
var Vector = class (self, x, y, z) { ... };

// Named expression form -- __type = "Vector".
var Vector = class Vector (self, x, y, z) { ... };
```

The parameter list is optional; `class Foo = { }` declares an empty
class with no constructor arguments.

### Inheritance

`EXTENDS` records a parent class so that field lookups fall through the
parent when a key is not present on the child or its instance.  Use the
parent's class object directly (via `Child.__index`) to call a parent
method explicitly -- there is no `super` keyword:

```cando
class Animal = (self, name) { self.name = name; }
Animal.speak = FUNCTION(self) { RETURN self.name + " says hello"; };

class Dog extends Animal = (self, name, breed) {
    Animal.__constructor(self, name);   // call the parent constructor
    self.breed = breed;
}
Dog.bark = FUNCTION(self) {
    // Dog.__index points at Animal.
    RETURN Dog.__index.speak(self) + " (woof, " + self.breed + ")";
};

VAR rex = Dog("Rex", "labrador");
print(rex:bark());     // Rex says hello (woof, labrador)
```

See [metamethods.md](metamethods.md) for the full prototype system,
the desugaring of `class`, and all available meta-keys.

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

Runtime errors are catchable with `TRY` / `CATCH`.  The first `CATCH`
parameter receives the message string.  If uncaught, the host embedder
sees `CANDO_ERR_RUNTIME` and `cando_errmsg(vm)` returns the formatted
message; the `cando` CLI prints it to stderr and exits with code 1.

### Arithmetic and operator errors

| Message                                                    | Cause                                                |
|---                                                         |---                                                   |
| `division by zero`                                         | `a / 0` or `a % 0`.                                  |
| `operands must be numbers (got <T> and <T>)`               | `a + b` where neither side has a `__add` metamethod and at least one side isn't a number / string. |
| `operands must be numbers`                                 | A binary operator (`-`, `*`, `/`, etc.) received a non-numeric operand. |
| `comparison requires numbers`                              | `<`, `<=`, `>`, `>=` between non-numeric values without a `__lt` / `__le` metamethod. |
| `range requires numbers`                                   | `a -> b` or `a <- b` outside a `FOR` (the range-list form). |
| `range check requires numbers`                             | A `FOR i IN a -> b { … }` where `a` or `b` isn't a number. |
| `unary '+' requires a number`                              | `+x` on a non-string non-number.                     |
| `unary '-' requires a number`                              | `-x` on a non-numeric value without `__unm`.         |
| `'++' requires a number`, `'--' requires a number`         | Increment/decrement on a non-number.                 |
| `# operator requires a string or object`                   | `#x` on `null`, `bool`, or `number` without `__len`. |

### Type errors on access

| Message                                          | Cause                                              |
|---                                               |---                                                 |
| `field access on non-object (got <T>)`           | `v.name` when `v` is not an object or string.      |
| `field assignment on non-object`                 | `v.name = …` when `v` is not an object.            |
| `index access on non-object`                     | `v[k]` on a non-object (and non-string).           |
| `index assignment on non-object`                 | `v[k] = …` on a non-object.                        |
| `index must be a number or string`               | `arr[obj]` and similar.                            |
| `IN operator requires an object`                 | `FOR k IN v` where `v` isn't an iterable object.   |
| `OF operator requires an object`                 | `FOR x OF v` where `v` isn't an array.             |
| `pipe/filter (~>/~!>) requires an array source`  | `arr ~> body` where `arr` isn't an array.          |

### Calls and methods

| Message                                          | Cause                                              |
|---                                               |---                                                 |
| `can only call functions (got <T>)`              | Called a non-callable value.  An object with `__call` would have been dispatched instead. |
| `method call on non-object (got <T>)`            | `v:m()` on a non-object value.                     |
| `method is not callable`                         | `v:m()` where `m` resolved but isn't a function.   |
| `meta-method is not callable`                    | A meta-method field (`__add`, `__lt`, …) resolved to a non-callable value. |
| `call stack overflow`                            | Recursion depth exceeds `CANDO_FRAMES_MAX` (256).  |
| `stack overflow in method call`                  | Value stack exhausted while building the call frame (default 2048). |
| `undefined variable '<name>'`                    | Read of a global that was never assigned.          |

### Control flow and bindings

| Message                                  | Cause                                               |
|---                                       |---                                                  |
| `cannot assign to constant '<name>'`     | Reassigning a `CONST` binding.                      |
| `BREAK outside loop`                     | `BREAK` (or `BREAK n`) used where no loop is active. |
| `CONTINUE outside loop`                  | `CONTINUE` used where no loop is active.            |
| `SETTLE outside IF`                      | `SETTLE` (or `SETTLE n`) used outside an `IF` chain. |
| `RERAISE outside of catch block`         | `RERAISE` used outside a `CATCH` body.              |

### Threads and concurrency

| Message                                  | Cause                                               |
|---                                       |---                                                  |
| `thread: expected a function`            | `thread.spawn(v)` with a non-function `v`.          |
| `thread: failed to create OS thread`     | `pthread_create` / Windows equivalent failed.       |
| `await: expected a thread handle`        | `await v` with no operand, or wrong type.           |
| `await: value is not a thread`           | `await v` where `v` is not a thread object.         |

### Class machinery

These come from class compilation primitives and only appear if the
bytecode was hand-crafted or corrupted; ordinary scripts never see them.

| Message                                       | Notes                                |
|---                                            |---                                   |
| `INHERIT: expected class objects`             | `OP_INHERIT` saw bad operands.       |
| `BIND_METHOD: expected class object`          | Method-binding failed.               |
| `BIND_DEFAULT_CALL: expected class object`    | Default `__call` setup failed.       |
| `class __call: missing class receiver`        | Default class `__call` invoked with no class. |
| `class __call: invalid class handle`          | Default class `__call` saw an invalid handle. |

### Not-yet-implemented

| Message                                  | Notes                                                |
|---                                       |---                                                   |
| `tail call not yet implemented`          | The `OP_TAIL_CALL` opcode is reserved.               |
| `ASYNC not implemented (use 'thread' instead)` | Reserved keyword; use `thread` for concurrency. |
| `YIELD not implemented (use 'thread' instead)` | Same.                                          |

### Catching a runtime error

```cando
TRY {
    VAR v = 1 / 0;
} CATCH (msg) {
    print("caught:", msg);    // caught: division by zero
}
```

`THROW` produces an error whose value is whatever you threw — typically
a string but any value is accepted.  `THROW` with multiple values
unpacks into the `CATCH` parameter list:

```cando
TRY {
    THROW "validation", 422, "missing 'name'";
} CATCH (kind, code, detail) {
    print(kind, code, detail);
}
```

If the `CATCH` parameter list is shorter than the throw's value list,
the extras are dropped.  If longer, the extras are `NULL`.
