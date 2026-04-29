# Cando Binary Modules

This directory hosts the source for **C-based binary extension modules**
that scripts load at runtime via `include()`.  Each module lives in its
own subdirectory with its own source, build rules, tests, and docs:

```
modules/
├── README.md                this file
├── ldap/                    LDAP / Active Directory bindings
│   ├── README.md            module documentation
│   ├── ldap_module.c        implementation
│   ├── ldap_helpers.h       pure-C helpers (also used by tests)
│   ├── test_ldap.c          C unit tests
│   ├── test_ldap.cdo        script-level integration tests
│   └── Makefile             per-module build
├── sqlite/                  SQLite bindings
├── sql/                     PostgreSQL + MySQL/MariaDB bindings
├── window/                  GLFW window + OpenGL context
├── draw/                    2D drawing primitives (layered on `window`)
└── forms/                   .NET-Forms-shaped Win32 GUI binding
                             (Windows-only at runtime; loads as a stub
                              on Linux/macOS so feature detection works)
```

A module compiles to a single shared library (`name.so` / `name.dylib` /
`name.dll`) which exports one symbol — `cando_module_init` — that the
runtime calls once when the module is first `include()`d.  See
[`docs/writing-extensions.md`](../docs/writing-extensions.md) for the
binary-module contract.

## Building all modules

From the repository root:

```bash
make modules            # builds every module (Linux / macOS)
make modules-windows    # cross-compiles every module for Windows
```

The artefacts are placed alongside their source (e.g. `modules/ldap/ldap.so`
on Linux) and are also uploaded as CI artefacts on each push.  Download
them from the workflow run page; they are intended to be dropped into
your script's working directory and loaded with `include()`.

## Adding a new module

1. Create `modules/<name>/` and copy the `ldap` layout.
2. Implement `CandoValue cando_module_init(CandoVM *vm)`.
3. Add a Makefile that produces `<name>.so` / `<name>.dll`.
4. Write unit tests in `test_<name>.c` (pure-C helpers) and
   integration tests in `test_<name>.cdo` (script-level surface).
5. Add the module's directory to the top-level Makefile's
   `MODULES =` list and to the `.github/workflows/ci.yml`
   modules build step.
