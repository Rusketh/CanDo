# `csv`

RFC 4180 CSV parsing and serialization.  Cells are always treated as
strings — there is no built-in type inference.

## Reference

### `csv.parse(text, delim*, header*) → array`

Parse `text` as CSV.

- `delim` is a single-character delimiter.  Default `","`.
- `header` controls whether the first row is treated as a header.
  Default `TRUE`.

When `header` is `TRUE` (the default), the first row supplies field
names and the result is an **array of objects** keyed by those names:

```cdo
VAR rows = csv.parse("name,age\nalice,30\nbob,25\n");
print(rows[0].name);               // alice
print(rows[1].age);                // 25
```

When `header` is `FALSE`, the result is a plain **array of arrays of
strings**:

```cdo
VAR raw = csv.parse("a,b\n1,2\n", ",", FALSE);
print(raw[1][0]);                  // 1
```

The parser accepts RFC 4180 quoting with doubled `""` escapes:

```cdo
VAR rows = csv.parse('name,quote
alice,"she said ""hi"""');
print(rows[0].quote);              // she said "hi"
```

### `csv.stringify(data, delim*, headers*) → string`

Serialize back to CSV text.  Accepts either an **array of arrays** or
an **array of objects**.

In object mode, the column order is taken from `headers` when supplied,
otherwise from the keys of the first object.

Fields containing the separator, quote, or newline are quoted; embedded
quotes are escaped by doubling.

```cdo
VAR text = csv.stringify([
    { name: "Alice", age: 30 },
    { name: "Bob",   age: 25 },
]);
// name,age
// Alice,30
// Bob,25

VAR text2 = csv.stringify([[1, 2], [3, 4]], ";");
// 1;2
// 3;4
```

## Examples

### Read, transform, write

```cdo
VAR rows = csv.parse(file.read("input.csv"));

VAR processed = rows ~> {
    pipe.amount = math.floor(pipe.amount);
    RETURN pipe;
};

file.write("output.csv", csv.stringify(processed));
```

### Tab-separated values

```cdo
VAR rows = csv.parse(file.read("data.tsv"), "\t");
```

### Loading CSV through `include()`

`include("./data.csv")` calls `csv.parse(text)` (with header mode) and
caches the result keyed by canonical path:

```cdo
VAR users = include("./users.csv");
print(users[0].name);
```
