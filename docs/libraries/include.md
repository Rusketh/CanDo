# `include`

The `include()` global loads and caches a script module.  Full
semantics — path resolution, file-extension dispatch, caching — are
documented in [../language/modules.md](../language/modules.md); this
page summarizes the function reference.

## Reference

### `include(path) → any`

Load `path` and return its module value.  The file extension selects
the loader:

| Extension                  | Loader |
|----------------------------|--------|
| `.cdo`                     | Parsed and executed.  Top-level `RETURN` (or last expression) is the module value. |
| `.so` / `.dylib` / `.dll`  | Loaded with `dlopen`/`LoadLibrary`.  `cando_module_init(CandoVM *)` is called once. |
| `.json`                    | Parsed as JSON. |
| `.csv`                     | Parsed as CSV (header mode). |
| `.yaml` / `.yml`           | Parsed as YAML. |

If `path` has no extension, `include` probes `<path>.so`, `.dylib`,
`.dll`, `.cdo` in that order.

```cdo
VAR my   = include("./mylib.cdo");
VAR cfg  = include("./config.json");
VAR rows = include("./data.csv");
VAR yml  = include("./settings.yaml");
VAR sql  = include("./modules/sqlite/sqlite");    // probes .so/.dylib/.dll
```

Identical canonical paths share **one cached value** — Node-style
`require()` semantics.  Mutating the returned value mutates every other
holder of it.

## Authoring a `.cdo` module

```cdo
// counter.cdo
VAR n = 0;

VAR exports = {};
exports.next = FUNCTION() { n = n + 1; RETURN n; };
exports.peek = FUNCTION() { RETURN n; };
RETURN exports;
```

```cdo
// main.cdo
VAR c = include("./counter.cdo");
print(c.next(), c.next(), c.peek());     // 1 2 2
```

## Path resolution

- **Absolute paths** are canonicalised with `realpath()` and used
  directly.
- **Relative paths** are resolved relative to the **directory of the
  file doing the including**, not the process's cwd.

## Authoring a binary module

See [../api/extensions.md](../api/extensions.md) and the existing
modules in [`../../modules/`](../../modules/README.md).

## See also

- [../language/modules.md](../language/modules.md) — full discussion of
  the loader, cache, and resolution rules.
- [eval.md](eval.md) — running source from a string instead of a file.
