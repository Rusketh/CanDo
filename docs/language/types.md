# Types

CanDo has **five value types** at the VM level:

| Type     | Literal forms                          | Notes                                    |
|----------|----------------------------------------|------------------------------------------|
| `null`   | `NULL`                                 | The single absent value.                 |
| `bool`   | `TRUE`, `FALSE`                        | Distinct from `0`/`""`/`NULL`.           |
| `number` | `42`, `3.14`, `1e-5`, `0xff`, `0b101`  | IEEE-754 double, always.                 |
| `string` | `"…"`, `'…'`, `` `…` ``                | Immutable, reference-counted, UTF-8.     |
| `object` | `{…}`, `[…]`, closures, classes, threads | Heap object, referenced via a handle.  |

Arrays, plain objects, classes, closures, and threads are **all
objects** — they differ only in the internal `ObjectKind` tag.

`type(v)` returns one of `"null"`, `"bool"`, `"number"`, `"string"`,
`"object"`.  If an object exposes a `__type` field, `type(v)` returns
that field's value instead.

```cdo
print(type(NULL));            // null
print(type(TRUE));            // bool
print(type(42));              // number
print(type("hi"));            // string
print(type([1, 2]));          // object
print(type({ k: 1 }));        // object

CLASS Foo = (self) { }        // statement-form class
print(type(Foo()));           // Foo  -- via __type
```

## Truthiness

Only `NULL` and `FALSE` are falsy.  Every other value — including `0`,
`""`, the empty array `[]`, and the empty object `{}` — is truthy.

```cdo
IF 0   { print("yes"); }      // prints "yes"
IF ""  { print("yes"); }      // prints "yes"
IF []  { print("yes"); }      // prints "yes"
IF NULL  { /* skipped */ }
IF FALSE { /* skipped */ }
```

In particular this is **not** C's truthiness rule; in CanDo a
zero-valued number, an empty string, and an empty container are all
truthy.

## `null`

`NULL` is the single absent value.  It has type `null`.  Reading a
missing field returns `NULL`:

```cdo
VAR obj = { a: 1 };
print(obj.b);               // null
```

`?.` and `?[]` short-circuit on `NULL` (see
[expressions.md](expressions.md)).

## Booleans

`TRUE` and `FALSE`.  All comparison operators return a `bool`.  Logical
operators (`&&`, `||`, `!`) **do not coerce** their operands to a
boolean — they return one of the operands verbatim:

```cdo
print(NULL || "fallback");     // fallback
print(0    || "fallback");     // 0  (because 0 is truthy)
print("a"  && "b");            // b
print(!NULL);                  // true
```

## Numbers

The single numeric type is `double`.  There is no separate integer type;
operations that conceptually need integers (bitwise ops, `array[i]`,
`s:char(i)`) coerce to `int64_t` internally.

Arithmetic operators: `+ - * / % ^`.  `^` is power, right-associative,
binds tighter than `*`.

Bitwise operators: `& | |&` (XOR) `<< >>` and unary `~` (NOT).  These
are integer-domain operations on `int64_t` representations.

```cdo
print(2 ^ 10);                 // 1024
print(7 / 2);                  // 3.5     -- not integer division
print(7 % 2);                  // 1
print(0xff & 0x0f);            // 15
print(0xff |& 0x0f);           // 240     -- xor
```

Division by zero produces a runtime error:

```cdo
TRY {
    VAR v = 1 / 0;
} CATCH (msg) {
    print(msg);                // "division by zero"
}
```

## Strings

Strings are **immutable**.  Concatenation, slicing, case conversion, and
`replace` all return new strings.

Three quote styles, three different rule sets:

```cdo
VAR a = "double-quoted: \n is a newline, \" is a quote";
VAR b = 'single-quoted: literal newlines
allowed; no escape interpretation';
VAR c = `template-string with ${a:length()} characters`;
```

Indexing into a string is byte-based.  String contents are UTF-8 by
convention but the runtime treats them as opaque byte sequences for
length and indexing — so `#s` returns the byte length, and `s:sub(0, 4)`
takes the first four **bytes**, not codepoints.

The `string` library and string prototype methods are documented in
[../libraries/string.md](../libraries/string.md).

## Arrays

```cdo
VAR xs = [10, 20, 30];
print(xs[0]);                  // 10
print(xs[2]);                  // 30
print(#xs);                    // 3
print(xs:length());            // 3 — same thing via array prototype

xs[3] = 40;                    // append by index
xs:push(50);                   // append by method
xs:pop();                      // remove and return last → 50
```

Indices are 0-based.  Writing past the end of an array extends it (the
intermediate slots are filled with `NULL`).

Arrays have **dense integer-indexed storage** *and* the full object hash
table — you can attach named fields to an array.  This is occasionally
useful (e.g. caching computed properties on a list) but does change how
iteration works:

```cdo
VAR a = [1, 2, 3];
a.label = "primes";

FOR v OF a { print(v); }       // 1 2 3      — integer-indexed values
FOR k IN a { print(k); }       // label      — named keys only
```

The `array` library and prototype methods are documented in
[../libraries/array.md](../libraries/array.md).

## Objects

Plain objects are unordered hash tables that **preserve insertion order**
when iterated.

```cdo
VAR o = { name: "Alice", age: 30 };
o.city = "NYC";
print(inspect(o));             // { name: "Alice", age: 30, city: "NYC" }

FOR k IN o { print(k); }       // name age city
FOR v OF o { print(v); }       // Alice 30 NYC
```

Field access:

```cdo
o.name             // dot access — bare identifier key
o["name"]          // bracket access — any string key
o["two words"]     // bracket access for non-identifier keys
```

Both forms read and write the same hash table.  When an object has a
prototype (`__index`), missing keys fall through (see
[classes.md](classes.md)).

The `object` library is documented in
[../libraries/object.md](../libraries/object.md).

## Type coercion

CanDo does **very little implicit coercion**.

- `+` between two strings concatenates.
- `+` between two numbers adds.
- `+` between a string and a number is a runtime error unless one side
  defines `__add` — call `toString(n)` explicitly.
- Comparison (`==`, `!=`) does not coerce: `1 == "1"` is `FALSE`.
- Equality between objects is **identity-based** (same handle).  Two
  separate objects with the same fields compare unequal.

The `__add`, `__concat`, `__eq`, `__lt`, `__le` metamethods let
user-defined types opt in to operator overloading; see
[classes.md](classes.md).
