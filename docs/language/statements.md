# Statements

This page covers the statement forms — declarations, control flow, and
blocks.  Expressions and operators are in
[expressions.md](expressions.md).

## Statement terminators

CanDo uses **explicit semicolons**.  Newlines are not significant.

```cdo
VAR x = 1;
VAR y = 2;
print(x + y);
```

A `{ … }` block does not require a trailing semicolon.

## Variable declarations

### `VAR`

Binds a new variable in the current scope.  The initializer is required.

```cdo
VAR x = 1;
VAR a, b = 10, 20;             // multi-var parallel assignment
VAR p, q = minmax(7, 3);       // unpack a multi-return
```

### `CONST`

Same as `VAR` but the binding is write-protected.  Reassignment is a
compile-time error.

```cdo
CONST PI = 3.14159;
PI = 3;                         // error: cannot assign to constant 'PI'
```

`CONST` only protects the binding, not the value.  `CONST t = { x: 1 };
t.x = 2` is allowed — `t` still refers to the same object.

### `GLOBAL`

Forces a binding into the global scope, regardless of where the
statement appears.

```cdo
GLOBAL Counter = 0;            // explicit global

FUNCTION tick() {
    GLOBAL Counter = Counter + 1;
}
```

Without `GLOBAL`, an unqualified assignment to an undeclared identifier
inside a function still creates a global.  `GLOBAL` makes that intent
explicit and is the recommended style.

## Block scope

Variables declared with `VAR` or `CONST` are scoped to the enclosing
`{ }` block:

```cdo
IF TRUE {
    VAR inside = 42;
    print(inside);              // 42
}
// `inside` is not visible here
```

Function bodies, loop bodies, and bare `{ … }` blocks all introduce a
new scope.

## Multi-assignment

```cdo
VAR a, b, c = 1, 2, 3;          // declare three at once
a, b = b, a;                    // swap
```

The right-hand side is evaluated **fully** before any assignment, so
swaps work without a temporary.

A multi-return call on the right-hand side spreads:

```cdo
FUNCTION triple() { RETURN 10, 20, 30; }
VAR x, y, z = triple();         // x=10, y=20, z=30
```

If the RHS is shorter than the LHS, missing slots are bound to `NULL`.
If longer, extras are dropped (use a [mask](expressions.md#mask-selectors-) to
select specific positions).

## `IF` / `ELSE` / `ALSO`

```cdo
IF x > 0 {
    print("positive");
} ELSE IF x < 0 {
    print("negative");
} ELSE {
    print("zero");
}
```

Multi-comparison — test one value against several alternatives:

```cdo
IF code == 200, 201, 204 {
    print("success");
}

IF grade > 50, 60, 70 {
    // grade > ALL of (50, 60, 70)  — i.e. grade > 70
    print("solid pass");
}
```

### `ALSO` — inclusive branches

`ALSO` makes a branch *inclusive*: it fires when the immediately
preceding branch in the chain ran (in addition to the standard
`ELSE`-style fallback).  This turns an `IF` chain into a switch/case-style
construct where multiple cases can fire.

```cdo
IF TRUE  { print("A"); }
ALSO IF FALSE { print("B"); }
ALSO          { print("C"); }
//  → A, B, C all run
```

`ALSO` and `ELSE` may be mixed in the same chain.  Each chain has two
runtime flags:

- **`matched`** — true once any branch has fired in this chain.
- **`prev_ran`** — true iff the immediately-preceding branch fired.

| Branch       | Fires when                                                                                   |
|---           |---                                                                                           |
| `IF C`       | `C`                                                                                          |
| `ELSE IF C`  | `!matched && C`                                                                              |
| `ELSE`       | `!matched`                                                                                   |
| `ALSO IF C`  | `prev_ran` **OR** `(!matched && C)` — when `prev_ran`, the body runs and `C` is **ignored**. |
| `ALSO`       | `prev_ran` **OR** `!matched`                                                                 |

Worked examples:

```cdo
// Switch-style fall-through:
FUNCTION describe(n) {
    IF n > 0       { print("positive"); }
    ALSO IF n > 5  { print(">5");  }    // fires if `positive` ran
    ALSO IF n > 10 { print(">10"); }    // fires if previous ran
    ELSE           { print("non-positive"); }
}
describe(20);   //  positive  >5  >10
describe(3);    //  positive  >5  >10   (also-if cond ignored when prev ran)
describe(-1);   //  non-positive

// Mixed else / also:
IF FALSE      { /* skip */ }
ELSE IF TRUE  { print("B"); }    //   matched := true
ALSO IF FALSE { print("C"); }    //   prev_ran true → fires (cond ignored)
ELSE          { /* skip */ }     //   matched still true
//  → B, C
```

Constraints:

- `ALSO` may only follow `IF`, `ELSE IF`, `ELSE`, `ALSO IF`, or `ALSO`.
  A bare `ALSO { … }` at the start of a statement is a parse error.
- `ELSE` / `ELSE IF` after an `ALSO` is still legal and is gated against
  the `matched` flag.

`SETTLE` (described under [`BREAK`, `CONTINUE`,
`SETTLE`](#break-continue-and-settle) below) exits an `IF` chain early
the same way `BREAK` exits a loop.

## `WHILE`

```cdo
VAR i = 0;
WHILE i < 10 {
    print(i);
    i = i + 1;
}
```

`BREAK` and `CONTINUE` (below) work inside `WHILE`.

## `FOR`

CanDo's `FOR` has four shapes:

### Range form

```cdo
FOR i IN 1 -> 10 { … }          // 1, 2, …, 10
FOR i IN 10 <- 1 { … }          // 10, 9, …, 1
```

### Iterate keys

```cdo
FOR k IN obj { print(k); }      // each own key
```

For arrays this iterates the **named** keys; integer indices are
covered by the `OF` form below.

### Iterate values

```cdo
FOR v OF arr { print(v); }      // arrays in index order
FOR v OF obj { print(v); }      // objects in insertion order
```

### Generic iterator (`OVER`)

```cdo
FOR k, v OVER my_pairs(t) { … }
```

The expression after `OVER` must produce three values: an iterator
function, a state, and an initial control value.  Each iteration calls
`iter(state, control)` and binds the named loop variables to its return
values.  Iteration ends when the iterator returns `NULL` as the new
control value.

```cdo
FUNCTION my_pairs(t) {
    RETURN FUNCTION(s, c) {
        IF c >= #s { RETURN NULL; }
        RETURN c + 1, c, s[c];      // new control, then yielded values
    }, t, 0;
}

FOR k, v OVER my_pairs([10, 20, 30]) {
    print(k, v);                     // 0 10 / 1 20 / 2 30
}
```

This is the standard iterator-protocol shape.  See
[../libraries/array.md](../libraries/array.md) for stdlib-supplied
iterators.

## `BREAK`, `CONTINUE`, and `SETTLE`

```cdo
BREAK;                           // exit the innermost loop
BREAK 2;                         // exit two levels of nesting
CONTINUE;                        // skip to the next iteration of the innermost loop
CONTINUE 2;                      // continue the loop two levels out

SETTLE;                          // exit the innermost IF chain
SETTLE 1;                        // exit two nested IF chains
```

The optional numeric argument is the depth to skip (where 0 == innermost).

`BREAK` / `CONTINUE` count enclosing **loops** only; `SETTLE` counts
enclosing **`IF` chains** only.  The two are independent: a loop inside
an `IF` chain is transparent to `SETTLE`, and an `IF` chain inside a
loop is transparent to `BREAK`.

```cdo
FOR row IN 0 -> 9 {
    FOR col IN 0 -> 9 {
        IF grid[row][col] == NULL { BREAK 2; }
    }
}

IF condition {
    WHILE active {
        IF should_stop { SETTLE; }    // exits the OUTER `IF` chain;
                                       // the WHILE is skipped over.
        IF want_break  { BREAK; }     // exits the WHILE; the outer
                                       // `IF` chain continues.
        do_work();
    }
    cleanup();                         // skipped by SETTLE, reached by BREAK.
}
```

`BREAK` / `CONTINUE` outside a loop and `SETTLE` outside an `IF` chain
produce runtime errors.

## `RETURN`

```cdo
FUNCTION twice(x) { RETURN x * 2; }

FUNCTION minmax(a, b) {
    IF a < b { RETURN a, b; }
    RETURN b, a;
}
```

A bare `RETURN` returns no values (callers see `NULL`).

At a chunk's top level, the value of `RETURN <expr>` becomes the
module's exported value when loaded with `include()`; see
[modules.md](modules.md).

## Empty statement

A bare `;` is a no-op.

## Block statement

```cdo
{
    VAR scratch = expensive_compute();
    print(scratch);
}
// scratch is gone
```

A block introduces a fresh lexical scope; the contained variables are
not visible outside it.
