# `array`

The `array` namespace is also the **array prototype**, so the methods
listed below are reachable in two equivalent forms:

```cdo
arr:push(42)               // method form
array.push(arr, 42)         // function form — same body
```

Indices are **0-based**.  Methods that mutate `arr` return either the
new length or the receiver itself; methods that derive a new array
return that array, leaving the receiver unchanged.

## Reference

### `a:length() → number`

Number of integer-indexed elements.

```cdo
VAR xs = [10, 20, 30];
print(xs:length());        // 3
print(#xs);                // 3 — same value via the # operator
```

### `a:push(v) → number`

Append `v` to the array; return the new length.

```cdo
VAR xs = [];
xs:push(1);
xs:push(2);
xs:push(3);
print(inspect(xs));        // [1, 2, 3]
```

### `a:push(i, v) → number`

Insert `v` at index `i`, shifting existing elements right.  Return the
new length.

```cdo
VAR xs = [1, 2, 4];
xs:push(2, 3);
print(inspect(xs));        // [1, 2, 3, 4]
```

### `a:pop() → any`

Remove and return the last element.  Returns `NULL` on an empty array.

```cdo
VAR xs = [1, 2, 3];
print(xs:pop());           // 3
print(inspect(xs));        // [1, 2]
```

### `a:remove(i) → any`

Remove and return the element at index `i`, shifting later elements
left.  Returns `NULL` if `i` is out of range.

```cdo
VAR xs = ["a", "b", "c", "d"];
print(xs:remove(1));       // b
print(inspect(xs));        // ["a", "c", "d"]
```

### `a:splice(start, count) → array`

Remove `count` elements starting at `start`.  Return the removed
elements as a new array.  Equivalent to ECMAScript's
`Array.prototype.splice(start, count)`.

```cdo
VAR xs = [10, 20, 30, 40, 50];
VAR removed = xs:splice(1, 3);
print(inspect(removed));   // [20, 30, 40]
print(inspect(xs));        // [10, 50]
```

### `a:copy() → array`

Shallow copy.  Nested arrays/objects are shared with the original.

```cdo
VAR src  = [1, 2, [3, 4]];
VAR dst  = src:copy();
dst:push(99);
print(inspect(src));       // [1, 2, [3, 4]]
print(inspect(dst));       // [1, 2, [3, 4], 99]

dst[2]:push(5);            // mutating the nested array affects both
print(inspect(src));       // [1, 2, [3, 4, 5]]
```

### `a:map(fn) → array`

New array of the same length, with each element replaced by
`fn(element)`.

```cdo
VAR squared = [1, 2, 3, 4]:map((x) => x * x);
print(inspect(squared));   // [1, 4, 9, 16]
```

For one-liners, the `~>` pipe operator is shorter:

```cdo
VAR squared = [1, 2, 3, 4] ~> pipe * pipe;
```

### `a:filter(fn) → array`

New array containing only the elements for which `fn(element)` is
truthy.

```cdo
VAR positives = [-2, -1, 0, 1, 2]:filter((x) => x > 0);
print(inspect(positives)); // [1, 2]
```

The pipe equivalent is `~&>`:

```cdo
VAR positives = [-2, -1, 0, 1, 2] ~&> pipe > 0;
```

### `a:reduce(fn, init) → any`

Left fold.  Calls `fn(acc, element)` for each element, threading the
accumulator through; returns the final accumulator.

```cdo
VAR sum = [1, 2, 3, 4]:reduce((acc, x) => acc + x, 0);
print(sum);                // 10

VAR product = [1, 2, 3, 4]:reduce((acc, x) => acc * x, 1);
print(product);            // 24

// Building an index by id
VAR users = [
    { id: 1, name: "Alice" },
    { id: 7, name: "Bob"   },
];
VAR by_id = users:reduce(FUNCTION(acc, u) {
    acc[u.id] = u;
    RETURN acc;
}, {});
print(by_id[7].name);      // Bob
```

## Iteration patterns

A plain array iterates by value or by index:

```cdo
VAR xs = [10, 20, 30];

FOR v OF xs { print(v); }              // 10 20 30
FOR i, v OVER ipairs(xs) { … }         // 0,10  1,20  2,30
```

(`ipairs` is not a built-in; the `OVER` form needs an iterator
protocol — see [`../language/statements.md`](../language/statements.md)
for the protocol.)

## Iteration order

Arrays iterate in **index order**.  If you've poked named keys onto an
array (which is allowed but unusual), `FOR k IN arr` walks those named
keys, while `FOR v OF arr` walks the dense integer-indexed values.

## Errors

| Message                          | Cause                                      |
|---|---|
| `array.push: array is locked`    | The receiver was locked with `object.lock` by another thread. |
| `OF operator requires an object` | `FOR v OF arr` where `arr` isn't an array. |

Most other failures (`pop()` on empty, `remove()` out of range) return
`NULL` instead of throwing.
