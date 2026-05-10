# Binary Modules

This directory hosts CanDo's **binary extension modules** — C code
compiled into shared libraries that scripts load at runtime via
`include()`.  Each module lives in its own subdirectory with its own
source, build rules, tests, and documentation.

These are distinct from the [standard library](../docs/libraries/README.md),
which is linked into the main `cando` binary and always available.
Binary modules are loaded on demand and may not be available on every
platform — see each module's README for platform notes.

## Index

| Module                  | What it provides | Platforms |
|-------------------------|------------------|-----------|
| [`sqlite/`](sqlite/README.md) | SQLite bindings — prepared statements, transactions, user-defined functions, iteration | All |
| [`sql/`](sql/README.md)       | MySQL / MariaDB / PostgreSQL bindings | All (libs required) |
| [`ldap/`](ldap/README.md)     | LDAP / Active Directory client | All (OpenLDAP required) |
| [`smtp/`](smtp/README.md)     | SMTP, IMAP, POP3, DKIM, SPF, MIME parsing | All |
| [`window/`](window/README.md) | Cross-platform OS window with an OpenGL context | Linux, macOS, Windows |
| [`draw/`](draw/README.md)     | 2D drawing primitives, layered on `window` | Linux, macOS, Windows |
| [`forms/`](forms/README.md)   | Native widget tree (Form, Button, TextBox, …) | Windows (stub elsewhere) |

## How a module loads

Each module compiles to a single shared library:

```
modules/<name>/<name>.so       # Linux
modules/<name>/<name>.dylib    # macOS
modules/<name>/<name>.dll      # Windows
```

The library exports one symbol — `cando_module_init` — that the
runtime calls once when the module is first `include()`d:

```cdo
VAR sqlite = include("./modules/sqlite/sqlite");   // probes .so / .dylib / .dll
VAR db = sqlite.open("data.db");
db:exec("CREATE TABLE foo (id INTEGER PRIMARY KEY)");
db:close();
```

Subsequent `include()` calls with the same canonical path return the
cached module value without re-loading or re-initializing.  See
[`docs/language/modules.md`](../docs/language/modules.md) for the
loader's full path-resolution and caching rules.

## Building all modules

From the repository root:

```bash
make modules            # builds every module (Linux / macOS)
make modules-windows    # cross-compiles every module for Windows
```

Per-module Makefiles are also runnable directly:

```bash
cd modules/sqlite
make
```

The artefacts are placed alongside their source (e.g.
`modules/sqlite/sqlite.so` on Linux) and are also uploaded as CI
artefacts on each push.  Download them from the workflow run page; they
are intended to be dropped into your script's working directory and
loaded with `include()`.

## Adding a new module

The full walk-through is in
[`../docs/api/extensions.md`](../docs/api/extensions.md).  The short
version:

1. Create `modules/<name>/` and copy the layout of an existing module
   (`sqlite/` is the canonical small template).
2. Implement `CandoValue cando_module_init(CandoVM *vm)` in
   `<name>_module.c`.
3. Add a Makefile that produces `<name>.so` / `<name>.dylib` /
   `<name>.dll`.
4. Write unit tests in `test_<name>.c` (pure-C helpers) and
   integration tests in `test_<name>.cdo` (script-level surface).
5. Add the module's directory to the top-level `Makefile`'s
   `MODULES =` list and to the `.github/workflows/ci.yml` modules
   build step.
6. Document the module's script-facing surface in
   `modules/<name>/README.md` and add a row to the index above.

## Layout convention

Every module follows the same shape:

```
modules/<name>/
  README.md            module documentation (script-facing API)
  <name>_module.c      implementation; exports cando_module_init
  <name>_helpers.h     pure-C helpers reusable by the C tests
  <name>_helpers.c     (optional) helpers implementation
  test_<name>.c        C unit tests for the helpers
  test_<name>.cdo      script-level integration tests
  Makefile             builds <name>.so / .dylib / .dll
```

Following this layout means CI, the top-level Makefile, and tooling
all "just work" for the new module.
