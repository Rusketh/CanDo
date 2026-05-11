# Syntax

This page covers the lexical structure of CanDo source files.

## Source encoding

CanDo source is **UTF-8**.  Only ASCII characters carry syntactic
meaning; non-ASCII bytes inside string literals are preserved verbatim.

The conventional file extension is `.cdo`.  The `cando` CLI accepts any
extension; `include()` uses the extension to choose a loader (see
[modules.md](modules.md)).

## Comments

```cdo
// single-line — to end of line

/* multi-line —
   nest is not supported,
   the first */ closes the comment */
```

Comments are stripped during lexing and never appear in the bytecode.

## Whitespace

Spaces, tabs, carriage returns, and newlines are insignificant except as
token separators.  Indentation has no syntactic meaning.

## Identifiers

```
[A-Za-z_][A-Za-z0-9_]*
```

Identifiers are **case-sensitive**.  `count` and `Count` are different
names.  Reserved keywords cannot be used as identifiers.

## Keywords

Keywords are **case-insensitive but reject mixed-case spellings**.
`VAR`, `var`, `IF`, `if` all work.  `Var`, `iF`, and `IfElse` are
ordinary identifiers.

The canonical CanDo style is **all-uppercase**; the all-lowercase form
exists for users coming from other languages.

```
IF       ELSE      ALSO       WHILE      FOR        IN         OF      OVER
FUNCTION RETURN    CLASS      EXTENDS
TRY      CATCH     FINALY     THROW
VAR      CONST     GLOBAL     STATIC     PRIVATE
THREAD   ASYNC     AWAIT
TRUE     FALSE     NULL
BREAK    CONTINUE  SETTLE
pipe
```

A few notes:

- The lower-case `pipe` is the **implicit iteration variable** in `~>`,
  `~!>`, and `~&>` expressions; like other keywords its spelling can be
  either all-lower or all-upper, but lowercase is the convention so it
  reads as a value, not a statement keyword.
- `FINALY` is spelled with **one L**.  `FINALLY` is *not* accepted.
- `ASYNC`, `STATIC`, and `PRIVATE` are reserved but currently used only
  as modifiers on class members; see [classes.md](classes.md).

## Literals

### Number literals

All numbers are IEEE-754 double-precision floats.

```cdo
42         3.14
```

The lexer accepts a run of decimal digits, optionally followed by a
single `.` and another run of digits.  Hexadecimal (`0xff`), octal
(`0o77`), binary (`0b1010`), scientific notation (`1e6`, `1.5e-3`),
and underscore digit separators are **not** currently recognised — the
lexer stops at the first non-digit and the rest of the literal would
be re-lexed as identifier tokens.

### Boolean literals

```cdo
TRUE      FALSE
```

### Null

```cdo
NULL
```

### String literals

Three quote styles:

| Delimiter | Multiline? | Interpolation? | Notes                       |
|---|---|---|---|
| `"…"`         | no  | no              | single-line; backslash before a `"` keeps the quote inside the string, but escape sequences such as `\n`, `\t`, `\xNN`, `\uNNNN` are **not** decoded — they are stored as the literal bytes. |
| `'…'`         | yes | no              | newlines are preserved literally. |
| `` `…` ``     | yes | yes (`${expr}`) | template strings; interpolated expressions go through `toString`. |

```cdo
VAR plain    = "hello world";           // literal text
VAR raw      = 'multi
line literal';
VAR template = `2 + 2 = ${2 + 2}`;
```

To embed a real newline, use the `'…'` form or splice one in via
template interpolation.  `#s` returns the byte length of `s`.  Strings
are immutable; methods that "transform" a string return a new string.

### Array literals

```cdo
[]
[10, 20, 30]
[1, "two", TRUE, NULL, [4, 5]]    // mixed types are fine
```

A trailing comma is allowed.

### Object literals

```cdo
{}
{ name: "Alice", age: 30 }
{ "two words": "ok", 0: "numeric keys are stringified" }
```

A trailing comma is allowed.  Bare-identifier keys are stored as
strings.  Insertion order is preserved when iterating with `FOR k IN
obj`.

### Range literals

`a -> b` is the **ascending** inclusive integer range; `b <- a` is the
**descending** inclusive integer range.  Most often appears inside
`FOR`:

```cdo
FOR i IN 1 -> 5  { print(i); }       // 1 2 3 4 5
FOR i IN 5 <- 1  { print(i); }       // 5 4 3 2 1
```

## Operator characters

The complete set of multi-character operators (see
[expressions.md](expressions.md) for semantics):

```
+    -    *    /    %    ^
&&   ||   !
&    |    |&   ~     <<   >>
==   !=   <    >    <=   >=
=    +=   -=   *=   /=   %=   ^=
++   --
->   <-           range, ascending / descending
?.   ?[           safe member / safe index
:    ::           method call / fluent method call
=>                lambda / anonymous function arrow
~>   ~!>   ~&>    pipe / filter+map / predicate filter
...               varargs / spread
#                 length prefix
~                 mask "keep" marker
.                 mask "skip" marker, also member access
?                 ternary "then" marker
```

## Statement terminators

CanDo uses **explicit semicolons** to terminate statements.  Newlines
are not significant.

```cdo
VAR x = 1;
VAR y = 2;
print(x + y);
```

A `{ … }` block does not require a trailing semicolon.

## Source files and chunks

Each parsed source file becomes a `CandoChunk` — the unit the VM
executes.  Top-level statements run in order; the chunk's last
`RETURN <expr>` (if any) is the file's value when loaded with
`include()`.

```cdo
// mylib.cdo
VAR exports = {};
exports.greet = FUNCTION(name) { RETURN `hello ${name}`; };
RETURN exports;
```

```cdo
// main.cdo
VAR my = include("./mylib.cdo");
print(my.greet("world"));
```

Module-loading details are in [modules.md](modules.md).
