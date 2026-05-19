# `array`

The `array` namespace is also the **array prototype**, so the methods
listed below are reachable in two equivalent forms:

```cdo
arr:push(42)               // method form
array.push(arr, 42)         // function form — same body
```

Indices are **0-based**.  Mutating methods typically return a status
value (`TRUE` on success) or the removed element; derived-array methods
(`map`, `filter`, `splice`, `copy`) return a new array and leave the
receiver unchanged.

## Reference

### `a:length() → number`

Number of integer-indexed elements.

```cdo
VAR xs = [10, 20, 30];
print(xs:length());        // 3
print(#xs);                // 3 — same value via the # operator
```

### `a:push(v) → bool`

Append `v` to the array.  Returns `TRUE` on success, `FALSE` if `a`
isn't a writable array.

```cdo
VAR xs = [];
xs:push(1);
xs:push(2);
xs:push(3);
print(inspect(xs));        // [1, 2, 3]
```

### `a:push(i, v) → bool`

Insert `v` at index `i`, shifting existing elements right.  Returns
`TRUE` on success, `FALSE` otherwise.

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

## JS-style methods

For users coming from JavaScript, the array prototype also exposes the
standard `Array.prototype` surface.  All callbacks are invoked as
`fn(value)` (or `fn(value, index)` for `forEach`), matching CanDo's
existing `map` / `filter` / `reduce` semantics.

### Querying

| Method | Description |
|---|---|
| `a:indexOf(v, from?)`     | First index of `v` ≥ `from`, or `-1`. |
| `a:lastIndexOf(v, from?)` | Last index of `v` ≤ `from`, or `-1`. |
| `a:includes(v)` / `a:contains(v)` | True if `v` is present. |
| `a:find(fn)`              | First element where `fn(v)` is truthy, or `null`. |
| `a:findIndex(fn)`         | Index of that element, or `-1`. |
| `a:findLast(fn)` / `a:findLastIndex(fn)` | Same, from the end. |
| `a:some(fn)` / `a:every(fn)` | Short-circuit existential / universal. |
| `a:at(i)`                 | Element at index `i` (supports negative). |

### Iteration & reduction

| Method | Description |
|---|---|
| `a:forEach(fn)`               | Call `fn(v, i)` for each element. |
| `a:reduceRight(fn, init?)`    | Right-to-left fold. |

### Transforming

| Method | Description |
|---|---|
| `a:flat(depth?)`            | Flatten nested arrays, default depth 1. |
| `a:flatMap(fn)`             | `map` then flatten one level. |

### Combining

| Method | Description |
|---|---|
| `a:concat(...arrays)`       | New array of all elements. |
| `a:slice(start?, end?)`     | Subarray; supports negative indices. |
| `a:join(sep?)`              | String of element toStrings joined by `sep` (default `","`). |

### Mutating (chainable; return the receiver)

| Method | Description |
|---|---|
| `a:reverse()`               | In-place reverse. |
| `a:sort(comparator?)`       | In-place sort.  Default is lexicographic on `toString` of each element; comparator returns `-1` / `0` / `+1`. |
| `a:fill(v, start?, end?)`   | Fill range with `v`. |

### Stack / queue

| Method | Returns |
|---|---|
| `a:shift()`                 | Removed head, or `null` if empty. |
| `a:unshift(...vs)`          | New length after prepending. |

### Set-like

| Method | Description |
|---|---|
| `a:unique()`                | New array with duplicates removed. |
| `a:intersection(b)`         | Elements present in both. |
| `a:union(b)`                | Distinct elements from either. |
| `a:difference(b)`           | Elements in `a` not in `b`. |

```cdo
print([1, 2, 3]:includes(2));            // true
print([1, 2, 3]:join("-"));              // "1-2-3"
print([[1, 2], [3, 4]]:flat():join(","));// "1,2,3,4"
print([3, 1, 2]:sort():join(","));        // "1,2,3"
print([1, 2, 2, 3]:unique():join(","));   // "1,2,3"
```

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
