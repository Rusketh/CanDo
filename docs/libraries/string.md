# `string`

The `string` namespace is also the **string prototype**, so every string
value answers to these methods via `:` as well as the `string.*`
function form:

```cdo
"hello":toUpper()           // method form
string.toUpper("hello")      // function form — same body
```

String contents are UTF-8 by convention but the runtime treats them as
opaque byte sequences for length and indexing — `#s` and `s:length()`
return **byte counts**.

Strings are **immutable**.  Methods that "transform" a string always
return a new string.

## Reference

### `s:length() → number`

Byte length of `s`.

```cdo
print("hello":length());          // 5
print(#"hello");                  // 5
```

### `s:sub(start, end*) → string`

Substring spanning `[start, end)`.  Indices are 0-based.  `end` defaults
to `s:length()`.  Negative indices count from the end.

```cdo
print("hello":sub(0, 3));         // hel
print("hello":sub(2));             // llo
print("hello":sub(-2));            // lo
```

### `s:char(n) → string`

One-byte string at index `n` (0-based).  Returns `""` if out of range.

```cdo
print("hello":char(1));            // e
```

### `s:chars() → array`

Array of single-byte strings, one per byte.

```cdo
print(inspect("hi":chars()));      // ["h", "i"]
```

### `s:toLower() → string`, `s:toUpper() → string`

ASCII case conversion.  Non-ASCII bytes pass through unchanged.

```cdo
print("Hello, World!":toLower());  // hello, world!
print("Hello, World!":toUpper());  // HELLO, WORLD!
```

### `s:trim() → string`

Strip ASCII whitespace from both ends.

```cdo
print("  hi  ":trim());            // hi
```

### `s:left(n) → string`, `s:right(n) → string`

First or last `n` bytes.  Clamped to the string's length.

```cdo
print("hello":left(3));            // hel
print("hello":right(3));           // llo
```

### `s:repeat(n) → string`

`s` concatenated with itself `n` times.

```cdo
print("ab":repeat(3));             // ababab
print("-":repeat(10));             // ----------
```

### `s:find(needle) → number`

Byte index of the first occurrence of `needle`, or `-1` if not found.

```cdo
print("hello":find("ll"));         // 2
print("hello":find("xx"));         // -1
```

### `s:split(sep) → array`

Split `s` on `sep`.  Returns an array of parts (the separator itself is
excluded).  An empty `sep` splits into individual characters.

```cdo
print(inspect("a,b,c":split(",")));   // ["a", "b", "c"]
print(inspect("hello":split("")));    // ["h", "e", "l", "l", "o"]
print(inspect(",a,":split(",")));     // ["", "a", ""]
```

### `s:join(parts) → string`

Concatenate the `parts` array using `s` as a separator between elements.

```cdo
print(", ":join(["a", "b", "c"])); // a, b, c
print("/":join(["usr", "local"])); // usr/local
```

### `s:replace(old, new) → string`

Replace **every** occurrence of `old` with `new`.

```cdo
print("hello world":replace("o", "0"));   // hell0 w0rld
print("a b c":replace(" ", "-"));         // a-b-c
```

### `s:startsWith(prefix) → bool`, `s:endsWith(suffix) → bool`

```cdo
print("hello":startsWith("he"));   // true
print("hello":endsWith("lo"));     // true
print("hello":startsWith("xx"));   // false
```

### `s:format(...) → string`

`%s`, `%d`, `%f`, `%%` substitution.  Each `%`-token consumes one
argument from the right.

```cdo
print("hello, %s!":format("world"));            // hello, world!
print("%d items, $%.2f total":format(3, 9.5));  // 3 items, $9.50
print("100%% done":format());                   // 100% done
```

Supported conversions:

- `%s` — `toString(arg)`
- `%d` / `%i` — integer (truncates non-integer numeric input)
- `%f` — float; precision via `%.Nf`
- `%x` / `%X` — hexadecimal integer (lower / upper case)
- `%c` — single byte from an integer code point
- `%%` — literal `%`

Width and precision flags use the host's `printf` semantics (left-align
with `-`, zero-pad with `0`, etc.).

### `s:match(pattern, start*, end*) → bool, array`

POSIX extended regex match.  Returns two values:

1. A boolean — whether the pattern matched.
2. An array of capture groups (group 0 is the entire match).

`start` and `end` (both 0-based) restrict the search range.

```cdo
VAR ok, m = "phone: 555-1234":match("([0-9]+)-([0-9]+)");
print(ok);                         // true
print(inspect(m));                 // ["555-1234", "555", "1234"]

VAR no, _ = "no number here":match("([0-9]+)");
print(no);                         // false
```

## `string.format(fmt, ...) → string`

Standalone function form of `:format`.  Most code uses the method form;
the standalone version is convenient when you have the format string in
a variable:

```cdo
VAR fmt = "name=%s age=%d";
print(string.format(fmt, "Alice", 30));    // name=Alice age=30
```

## Examples

### Stripping a trailing newline

```cdo
VAR line = file.read("/etc/hostname");
IF line:endsWith("\n") {
    line = line:sub(0, -1);
}
```

### Building a CSV row

```cdo
FUNCTION csv_row(fields) {
    RETURN ",":join(fields ~> {
        IF pipe:find(",") >= 0 OR pipe:find("\"") >= 0 {
            RETURN `"${pipe:replace("\"", "\"\"")}"`;
        }
        RETURN pipe;
    });
}

print(csv_row(["a", "b,c", "she said \"hi\""]));
// a,"b,c","she said ""hi"""
```

### Tokenizing on whitespace runs

```cdo
FUNCTION words(s) {
    VAR parts = s:split(" ") ~&> #pipe > 0;
    RETURN parts;
}

print(inspect(words("  hello   world  ")));    // ["hello", "world"]
```
