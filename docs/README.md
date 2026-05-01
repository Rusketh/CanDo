# CanDo Documentation

CanDo is a small C11 scripting language designed to be embedded in host
applications — games, tools, platform frameworks, CLIs — in the same way
Lua is.  Scripts are plain UTF-8 text with the extension `.cdo` and are
compiled to bytecode for a stack-based VM.

## For script authors

**Start here:**

| Document | What's in it |
|---|---|
| [getting-started.md](getting-started.md) | Build the interpreter and run your first script |
| [user-guide.md](user-guide.md) | Hands-on tutorial: learn CanDo by example |
| [language-reference.md](language-reference.md) | Complete syntax: types, expressions, statements, functions, classes |
| [standard-library.md](standard-library.md) | Every built-in library module, function-by-function |
| [patterns.md](patterns.md) | Common idioms — error handling, FP style, classes, modules, threads |
| [cli.md](cli.md) | The `cando` executable: flags, `args[]`, exit codes, environment |

**Concurrency:**

| Document | What's in it |
|---|---|
| [threading.md](threading.md) | `thread` / `await`, the `thread` library, shared state |
| [streaming.md](streaming.md) | Unified `stream` API: pipe between files, sockets, HTTP, processes, threads |

**Networking:**

| Document | What's in it |
|---|---|
| [socket.md](socket.md) | Long-form guide to the `socket` and `secure_socket` libraries |

**Data formats:**

| Document | What's in it |
|---|---|
| [yaml.md](yaml.md) | YAML parse / stringify and `include()`-time loading |
| [sql.md](sql.md) | SQLite-backed `sql` module: queries, parameter binding, transactions |

**Graphics & UI** (require platform binary modules):

| Document | What's in it |
|---|---|
| [window.md](window.md) | Window creation and event loop |
| [draw.md](draw.md) | 2D drawing primitives, sprites, text |
| [forms.md](forms.md) | Declarative widget tree for in-window UI |

## For C embedders

| Document | What's in it |
|---|---|
| [embedding.md](embedding.md) | Lifecycle, running scripts, exchanging values, error handling |
| [c-api.md](c-api.md) | Reference for every public symbol in `cando.h` |
| [writing-extensions.md](writing-extensions.md) | Native functions and in-process library modules |
| [creating-platforms.md](creating-platforms.md) | Building a love2d-style framework on top of CanDo |

## Internals (contributors)

| Document | What's in it |
|---|---|
| [architecture.md](architecture.md) | End-to-end pipeline and module layout |
| [value-types.md](value-types.md) | `CandoValue`, `CdoValue`, handles, the bridge layer |
| [object-system.md](object-system.md) | `CdoObject`, prototype chains, meta-methods, arrays |
| [metamethods.md](metamethods.md) | All meta-keys, `CLASS`, prototype-based inheritance, operator overloading |
| [parser-compiler.md](parser-compiler.md) | Lexer, Pratt parser, scope system, bytecode emission |
| [vm-internals.md](vm-internals.md) | Dispatch loop, call frames, upvalues, opcode reference |

---

## Build

CMake is the supported build system.  A GNU `Makefile` is provided as a
fallback.

```bash
cmake -B build && cmake --build build
./build/cando tests/scripts/hello.cdo
```

Outputs:

- `libcando.so` / `libcando.dll` — shared library for embedding
- `libcando.a` / `libcando_static.lib` — static library
- `cando` / `cando.exe` — standalone interpreter

OpenSSL and pthreads are required (used by the `http`/`https` and
`thread` libraries).

## Hello world — embedding

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    if (cando_dofile(vm, "game/main.cdo") != CANDO_OK)
        fprintf(stderr, "%s\n", cando_errmsg(vm));

    cando_close(vm);
    return 0;
}
```

```
gcc myapp.c -lcando -Iinclude -o myapp
```

See [embedding.md](embedding.md) for a complete walk-through.

## Repository layout

```
include/cando.h      single public header for embedders
source/
  core/              value, memory, handle table, lock primitives
  object/            heap objects: CdoObject, array, function, thread
  parser/            lexer + Pratt parser → bytecode
  vm/                bytecode, dispatch loop, bridge layer, disassembler
  lib/               19 standard library modules
  natives.c          core natives: print, type, toString
  cando_lib.c        public embedding API (cando_open, cando_dofile, …)
  main.c             `cando` CLI entry point
tests/
  scripts/           .cdo scripts run by the integration suite
  integration/       shell runner that executes every script
  test_*.c           unit tests for core, object, lexer, parser, vm, thread
```

## Version

This documentation tracks CanDo 1.0.0 — the version defined in
`include/cando.h` (`CANDO_VERSION`).  The numeric form `CANDO_VERSION_NUM`
is `10000` (major × 10000 + minor × 100 + patch).
