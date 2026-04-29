# YAML Library

The `yaml` library is part of the CanDo standard library — it is
registered by `cando_openlibs()` (or `cando_open_yamllib()`) and
exposes a single global `yaml` object.

```cdo
print(yaml.parse("a: 1\nb: 2"));
```

The implementation is self-contained C — no external `libyaml` runtime
dependency.  It targets the YAML 1.2 *core schema*, with a small set of
YAML 1.1 conveniences kept for compatibility (`yes`/`no`/`on`/`off`
booleans).

## API

### `yaml.parse(text) → value`

Decode a YAML document into native CanDo values.

| YAML construct                      | CanDo value                                                |
| ----------------------------------- | ---------------------------------------------------------- |
| Block mapping / flow mapping        | object (insertion-ordered)                                 |
| Block sequence / flow sequence      | array                                                      |
| Plain scalar                        | core-schema typed: `null`/`true`/`false`/integer/float/string |
| Single-quoted (`'…'`) scalar        | string (only `''` escape; preserves everything else)       |
| Double-quoted (`"…"`) scalar        | string with JSON-style escapes plus `\xNN`, `\uNNNN`, `\UNNNNNNNN` |
| Literal block scalar (`\|`)          | string (newlines preserved)                                |
| Folded block scalar (`>`)           | string (line breaks become spaces; blank line → newline)   |

Throws an error on malformed input — for example, unterminated quoted
strings, unterminated flow containers, tab-indented block content, or
trailing junk after the document.

### `yaml.stringify(value, indent?) → string`

Serialise a CanDo value as a block-style YAML document.

- `indent` (default 2, range 1..16) controls the per-level indentation
  in spaces.
- Objects emit as block mappings, arrays as block sequences, with
  empty containers rendered inline as `{}` / `[]`.
- Scalars are quoted only when ambiguity rules would otherwise change
  their type on round-trip — strings that look like numbers, booleans,
  or `null`; strings starting with a YAML indicator character; strings
  with control characters or trailing whitespace.
- Functions and natives are not representable in YAML and serialise
  as `null`.

The output always ends with a single trailing newline.

## Examples

### Parsing a configuration file

```cdo
VAR cfg = yaml.parse('
service: api
port: 8080
features:
  - login
  - billing
db:
  host: db.local
  pool: 5
');

print(cfg.service);          // api
print(cfg.port);             // 8080
print(cfg.features[0]);      // login
print(cfg.db.host);          // db.local
```

### Round-tripping

```cdo
VAR doc = { name: "cando", tags: ["alpha", "beta"], stable: true };
VAR text = yaml.stringify(doc);
VAR back = yaml.parse(text);

print(back.name);            // cando
print(back.tags[1]);         // beta
print(back.stable);          // true
```

### Loading via `include()`

`include()` recognises `.yaml` and `.yml` extensions and returns the
parsed YAML document, with the same module caching rules as
JSON / CSV / native modules:

```cdo
VAR settings = include("./settings.yaml");
print(settings.host);
```

## Supported subset

This is the supported feature set, in roughly the order you are likely
to need it:

- Block mappings: `key: value`, with the value either inline or on a
  child indent.
- Block sequences: `- item`, including the inline `- key: value` form
  for sequences of mappings.
- Flow containers: `[a, b]`, `{a: 1, b: 2}` — single-line only.
- Plain, single-quoted, and double-quoted scalars.
- Literal (`|`) and folded (`>`) block scalars with default chomping.
- Line comments: `# …`.
- An optional leading `---` document marker.
- Core-schema scalar typing (`null`, `~`, `true`, `false`, integers,
  floats, strings) plus YAML 1.1 booleans (`yes`/`no`/`on`/`off`).

The following YAML features are intentionally not supported (they are
rare in practice and add a lot of complexity to a single-file
implementation):

- Anchors (`&id`), aliases (`*id`), merge keys (`<<:`).
- Explicit type tags (`!!str`, `!!float`, etc.).
- Multi-document streams beyond a single optional leading `---`.
- Complex / non-string mapping keys (`? key` syntax).
- Tab characters in block-structural indentation (an explicit error).

When the parser hits an unsupported or malformed construct it throws an
error rather than silently producing the wrong shape, so scripts
relying on `yaml.parse` can `try { … } catch (e) { … }` around the call
and surface a useful diagnostic.

## C API

For embedders and other library code there is a single re-usable
parsing entry point:

```c
#include <cando/lib/yaml.h>

CANDO_API bool cando_lib_yaml_parse_buffer(CandoVM *vm,
                                           const char *src, usize len,
                                           const char *where,
                                           CandoValue *out);
```

It is the same function used by the `include()` loader to parse
`.yaml`/`.yml` files; on failure it sets a VM error prefixed with
`where` and returns `false`.
