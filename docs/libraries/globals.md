# Global Builtins

Three core natives are registered by `cando_open(vm)` itself, before
any standard library is opened.  They are always present, regardless
of which `cando_open_*lib` calls run.

## `print(...)`

Writes each argument to standard output, separated by spaces, followed
by a newline.

- Arrays are expanded element-by-element (the array brackets are *not*
  printed; iterate with `inspect()` if you want a literal-looking
  rendering).
- Multi-return calls spread.
- Objects with a `__tostring` metamethod use it; everything else uses
  the canonical representation.

```cdo
print("hello", "world");                  // hello world
print(1, 2, 3);                            // 1 2 3
print([10, 20, 30]);                       // 10 20 30  (array expanded)
print({ name: "Alice" });                  // { name: "Alice" }
print();                                    // (just a newline)
```

`print()` always flushes; you don't need to call any flush function
afterwards.

## `type(v) → string`

Returns a string naming `v`'s type:

| Returned value | When                                        |
|----------------|---------------------------------------------|
| `"null"`       | `v` is `NULL`.                              |
| `"bool"`       | `v` is `TRUE` or `FALSE`.                   |
| `"number"`     | `v` is any numeric value.                   |
| `"string"`     | `v` is a string.                            |
| `"object"`     | `v` is an object with no `__type` field.    |
| any string     | `v` is an object with a `__type` field — that field's value is returned. |

```cdo
print(type(NULL));                          // null
print(type(TRUE));                          // bool
print(type(42));                            // number
print(type("hi"));                          // string
print(type([]));                            // object

CLASS Vector = (self) { }
print(type(Vector()));                      // Vector
```

`type()` is the canonical way to discriminate between value types — it
is significantly cheaper than checking metamethods.

## `toString(v) → string`

Returns the canonical string representation of `v`.

- For numbers, the shortest representation that round-trips (e.g.
  `42`, `3.14`, `1.5e-7`).
- For booleans, `"true"` / `"false"`.
- For `NULL`, `"null"`.
- For strings, `v` itself unchanged.
- For objects with a `__tostring` metamethod, the result of calling
  it.
- For other objects, a class-aware string like `<Vector>` or
  `<object>`.

```cdo
print(toString(42));                        // 42
print(toString(NULL));                      // null
print(toString(3.14));                      // 3.14

CLASS Vec = (self, x, y) { self.x = x; self.y = y; }
Vec.__tostring = FUNCTION(self) { RETURN `(${self.x},${self.y})`; };
print(toString(Vec(3, 4)));                 // (3,4)
```

`toString` is what string concatenation falls back to when one operand
is a string and the other is an object with `__tostring`.

## `inspect(v, depth*) → string`

Returns a debug-friendly string showing the **contents** of arrays and
objects rather than their handle id.  Designed to be passed to `print`:

```cdo
VAR data = { name: "Alice", scores: [10, 20, 30] };
print(inspect(data));
// { name: "Alice", scores: [10, 20, 30] }
```

### Formatting

| Value                          | Rendering |
|--------------------------------|-----------|
| `null` / `true` / `false`      | as-is |
| number                         | same as `toString(n)` |
| string                         | double-quoted, with `\\`, `\"`, `\n`, `\r`, `\t`, `\xNN` escapes |
| array                          | `[v1, v2, ...]` |
| object                         | `{ key: value, ... }` (FIFO insertion order; non-identifier keys are quoted) |
| function / native / thread     | `<function>` / `<native>` / `<thread>` |

### `depth`

`depth` (default `0`) limits how many levels of nested arrays / objects
are expanded:

- `0` (default) — unlimited recursion.
- `N > 0` — nested arrays / objects beyond level `N` are truncated to
  `[...]` / `{...}`.

```cdo
VAR deep = { a: { b: { c: 1 } } };
print(inspect(deep, 1));                    // { a: {...} }
print(inspect(deep, 2));                    // { a: { b: {...} } }
print(inspect(deep, 0));                    // { a: { b: { c: 1 } } }
```

### Cycles

Cycles are detected on the current path and rendered as `<circular>`,
so `inspect` always terminates regardless of `depth`.  Two distinct
sub-trees that happen to reference the same object are *not* flagged
as cycles (they are printed twice).

```cdo
VAR a = [1];
a:push(a);
print(inspect(a));                          // [1, <circular>]
```

`inspect` does not invoke `__tostring`; it always shows raw structure,
which is what you want when debugging.  Use `toString(v)` when you do
want the metamethod.
