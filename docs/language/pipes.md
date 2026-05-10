# Pipes

CanDo has three pipe-style operators that iterate the array on their
left and run a body on the right with the special variable `pipe` bound
to the current element:

| Operator | Purpose                                           | Result          |
|----------|---------------------------------------------------|-----------------|
| `~>`     | Map.  Body's value is the new element.             | Same length.    |
| `~!>`    | Map + filter.  Non-`NULL` body values are kept.    | ≤ source length.|
| `~&>`    | Predicate filter.  Truthy body keeps the original. | ≤ source length.|

All three return a new array; the source array is never modified.

## `~>` (map)

The body's value (last expression, or explicit `RETURN`) becomes the new
element.  The result has the same length as the source.

```cdo
VAR ns = [1, 2, 3, 4, 5];

VAR doubled = ns ~> pipe * 2;             // [2, 4, 6, 8, 10]
VAR named   = ns ~> `n${pipe}`;            // ["n1", "n2", "n3", "n4", "n5"]

VAR shifted = ns ~> {
    VAR offset = 100;
    RETURN pipe + offset;
};                                         // [101, 102, 103, 104, 105]
```

## `~!>` (filter+map)

The body's *return value* is what ends up in the output, but only if it
is non-`NULL`.  This makes `~!>` a combined filter and map — drop with
`NULL`, transform with anything else.

```cdo
VAR ns = [1, 2, 3, 4, 5];

// Keep only evens, doubled.
VAR doubled_evens = ns ~!> {
    IF pipe % 2 == 0 { RETURN pipe * 2; }
};                                         // [4, 8]
```

If the body falls off the end without a return, the result is `NULL` —
and the element is dropped.  An explicit `RETURN NULL` works too.

## `~&>` (predicate filter)

The body is treated as a boolean predicate.  When the body returns a
truthy value the **original element** is kept; when it returns falsy the
element is dropped.  The body's value is otherwise discarded.

```cdo
VAR ns = [1, 2, 3, 4, 5];

VAR evens = ns ~&> pipe % 2 == 0;          // [2, 4]
VAR small = ns ~&> { RETURN pipe < 3; };   // [1, 2]
```

Use `~&>` when you want a pure filter without the map.

## The `pipe` keyword

Inside the body, `pipe` is bound to the current element.  It's
**lexically scoped**: a nested pipe gets its own `pipe`.

```cdo
VAR matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]];

VAR doubled_matrix = matrix ~> {
    RETURN pipe ~> pipe * 2;     // inner `pipe` is a number; outer is a row
};
// [[2, 4, 6], [8, 10, 12], [14, 16, 18]]
```

`pipe` is read-only inside the body.  Mutating it would not change the
source array; if you find yourself wanting that, write a regular
`FOR … OF` loop.

## Body forms

Each operator accepts one of two body forms:

### Single expression

```cdo
arr ~> pipe * 2
arr ~&> pipe > 0
```

The expression's value is used directly.

### Block

```cdo
arr ~> {
    VAR scaled = pipe * 100;
    RETURN scaled - 1;
}
```

Inside a block you can introduce locals, `IF`, etc.  An explicit
`RETURN` provides the value; otherwise the value is `NULL`, which
matters for `~!>` and `~&>`.

## Composition

Pipes return arrays, so they chain naturally:

```cdo
VAR result = data
    ~&> pipe.active             // keep only active
    ~> pipe.amount * 1.10        // apply 10% markup
    ~!> {                        // log + clip negatives
        IF pipe < 0 { RETURN; }
        log(pipe);
        RETURN math.min(pipe, 1000);
    };
```

The pipe operators bind tighter than assignment but looser than the
arithmetic operators, so most of the time you can write linear chains
without parentheses.

## Errors

| Message                                         | Cause |
|---|---|
| `pipe/filter (~>/~!>) requires an array source` | The left side wasn't an array (the same message covers `~&>`). |

A `THROW` inside a pipe body unwinds the way it would in any other
expression — the surrounding `TRY` (if any) catches it.

## When *not* to use pipes

Pipes are great for short, declarative transforms.  When the
transformation is long, has multiple intermediate locals, or builds
state across iterations, an ordinary `FOR` is clearer:

```cdo
// fine, but reaching the limits of "declarative"
VAR totals = orders ~&> pipe.paid ~> pipe.amount;

// Probably clearer as a loop.
VAR total = 0;
VAR rejected = 0;
FOR o OF orders {
    IF !o.paid { rejected = rejected + 1; CONTINUE; }
    total = total + o.amount;
}
```
