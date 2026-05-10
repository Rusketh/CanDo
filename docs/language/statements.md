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

## `IF` / `ELSE`

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

## `BREAK` and `CONTINUE`

```cdo
BREAK;                           // exit the innermost loop
BREAK 2;                         // exit two levels of nesting
CONTINUE;                        // skip to the next iteration of the innermost loop
CONTINUE 2;                      // continue the loop two levels out
```

The optional numeric argument is the loop depth to skip (where 0 ==
innermost).

```cdo
FOR row IN 0 -> 9 {
    FOR col IN 0 -> 9 {
        IF grid[row][col] == NULL { BREAK 2; }
    }
}
```

`BREAK` and `CONTINUE` outside a loop produce a runtime error.

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
