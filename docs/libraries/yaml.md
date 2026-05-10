# `yaml`

YAML 1.2 parsing and serialization.

## Reference

### `yaml.parse(text) → any`

Decode a YAML document into CanDo values.  **Throws** on malformed
input.

```cdo
VAR cfg = yaml.parse("name: cando
version: 1
tags:
  - alpha
  - beta
");
print(cfg.name);                   // cando
print(cfg.tags[0]);                // alpha
```

### `yaml.stringify(value, indent*) → string`

Encode a CanDo value as block-style YAML text.  `indent` (default `2`,
range `1..16`) controls per-level indentation.

```cdo
print(yaml.stringify({ host: "localhost", port: 8080 }));
// host: localhost
// port: 8080
```

## Supported features

- **Block mappings** (`key: value`), **block sequences** (`- item`), and
  any nested combination — including the common `- key: value` form for
  sequences of mappings.
- **Flow mappings** (`{a: 1, b: 2}`) and **flow sequences** (`[1, 2, 3]`),
  one-line only.
- **Scalar styles**:
  - Plain (`hello`)
  - Single-quoted (`'…'` with `''` escape).
  - Double-quoted (`"…"` with JSON-style escapes plus `\xNN`,
    `\uNNNN`, `\UNNNNNNNN`).
  - Literal block (`|`) and folded block (`>`) with default chomping
    (single trailing newline).
- **Core-schema typing** — unquoted scalars become `null`, `true`,
  `false`, integers (decimal / `0x…` / `0o…`), or floats (`1.5`,
  `.inf`, `.nan`) when they match the corresponding grammar; otherwise
  they remain strings.  `yes`/`no`/`on`/`off` are accepted as booleans
  for compatibility.
- **Comments** — `# …` outside a quoted string runs to end of line, as
  long as the `#` is at start-of-line or preceded by whitespace.
- **Document marker** — an optional leading `---`.

`yaml.stringify` quotes scalars only when ambiguity rules require it
(empty strings, reserved scalars like `null`/`true`/`yes`, leading
indicator characters, embedded control characters, or strings that
would otherwise round-trip back to a non-string type).  Functions and
natives serialise as `null`.

## Examples

### Loading config files

```cdo
VAR cfg = yaml.parse(file.read("config.yaml"));
VAR port = cfg.server.port || 8080;
```

For module-style loading, `include("./config.yaml")` works directly:

```cdo
VAR cfg = include("./config.yaml");
```

### Round-tripping

```cdo
VAR original = { name: "cando", tags: ["alpha", "beta"], debug: TRUE };
VAR text     = yaml.stringify(original);
VAR parsed   = yaml.parse(text);
print(inspect(parsed));
```

### Different indent

```cdo
print(yaml.stringify({ a: { b: { c: 1 } } }, 4));
// a:
//     b:
//         c: 1
```
