# Modules

Module loading is provided by the `include()` global.  It loads a file,
caches the result, and returns a value the caller can use.

## `include(path) → any`

```cdo
VAR my = include("./mylib.cdo");
print(my.greet("world"));
```

`include()` returns whatever the loaded module produced (see below).
Subsequent calls with the same canonical path return the **cached
value** without re-executing — Node.js `require()` semantics.

## Path resolution

- **Absolute paths** are canonicalised with `realpath()` and used
  directly.
- **Relative paths** are resolved relative to the **script's directory**
  — the directory of the nearest enclosing frame whose chunk name is an
  absolute path.  In practice that means relative `include()`s work the
  same way they would in a normal Unix tool: relative to the file doing
  the including, not the process's cwd.

```cdo
// In /home/me/project/main.cdo:
VAR utils = include("./utils.cdo");          // /home/me/project/utils.cdo
VAR cfg   = include("./config/dev.json");    // /home/me/project/config/dev.json
```

## File extensions

The extension on `path` selects the loader:

| Extension                | Loader |
|--------------------------|--------|
| `.cdo`                   | Parsed and executed.  Top-level `RETURN` (or the last expression) is the module value. |
| `.so` / `.dylib` / `.dll`| Loaded with `dlopen` (POSIX) or `LoadLibrary` (Windows).  The exported symbol `cando_module_init(CandoVM *) → CandoValue` is called once and its return value becomes the module value. |
| `.json`                  | Parsed as JSON.  The resulting CanDo value (object/array/string/number/bool/null) is returned. |
| `.csv`                   | Parsed as CSV with the default `,` delimiter; the first row is treated as the header row.  The result is an array of objects keyed by header names. |
| `.yaml` / `.yml`         | Parsed as YAML.  The resulting CanDo value is returned. |

If `path` has **no extension at all**, `include()` probes the filesystem
in this order and uses the first match it finds:

1. `<path>.so`
2. `<path>.dylib`
3. `<path>.dll`
4. `<path>.cdo`

If a path is supplied with one of the recognised extensions but the
file does not exist, `include()` raises an error rather than probing
alternatives.

## Authoring a `.cdo` module

A module is just a `.cdo` file whose last `RETURN` value is what
callers receive:

```cdo
// mylib.cdo
VAR exports = {};

exports.greet = FUNCTION(name) { RETURN `hello ${name}`; };
exports.farewell = FUNCTION(name) { RETURN `bye ${name}`; };

CONST PI = 3.14159;
exports.PI = PI;

RETURN exports;
```

```cdo
// main.cdo
VAR my = include("./mylib.cdo");
print(my.greet("world"));         // hello world
print(my.PI);                     // 3.14159
```

If a module has no top-level `RETURN`, it returns the value of its last
expression.  If it has neither, it returns `NULL`.

## Authoring a binary module

The contract is one C entry point — `CandoValue cando_module_init(CandoVM
*vm)` — packaged into a shared library.  The full walk-through is in
[../api/extensions.md](../api/extensions.md); the existing modules in
[`../../modules/`](../../modules/README.md) serve as templates.

## Caching

Identical canonical paths share **one cached value** across the whole
VM.  This applies to every loader — JSON, CSV, YAML, `.cdo`, and binary
modules.

The cache is keyed by the result of `realpath()`, so two paths that
resolve to the same file (e.g. `./lib.cdo` and `lib.cdo`) share the
same cache entry.

The cache is **not** automatically invalidated when the file changes on
disk.  Restart the VM to pick up new versions.

Because results are cached, mutating a returned value mutates every
other holder of it:

```cdo
// First load
VAR cfg = include("./config.json");
cfg.token = "foo";

// Second load — same object
VAR also_cfg = include("./config.json");
print(also_cfg.token);          // foo
```

If you need a private copy, use `object.copy(cfg)` (shallow) or
re-parse from disk.

## Examples

### Library with private state

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
print(c.next(), c.next(), c.next(), c.peek());    // 1 2 3 3
```

`n` is a local in the module's top-level frame; it stays alive as long
as `next` and `peek` reference it.

### Layered configuration

```cdo
VAR base = include("./config/base.yaml");
VAR env  = include(`./config/${os.getenv("ENV") || "dev"}.yaml`);
VAR cfg  = object.assign({}, base, env);
```

### Conditionally-loaded native module

```cdo
TRY {
    VAR sql = include("./modules/sql/sql");      // probes .so/.dylib/.dll/.cdo
    print("sql module loaded");
} CATCH (e) {
    print("sql module unavailable:", e);
}
```
