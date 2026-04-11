# CanDo Documentation

CanDo is an embeddable C11 scripting language — a lightweight, fast runtime
designed to be integrated into games, tools, and application frameworks the
same way Lua is.  Scripts use the `.cdo` extension.

## What CanDo is for

| Use case | How |
|---|---|
| Game scripting (mods, quests, events) | `cando_dofile(vm, "mods/quest.cdo")` |
| Application automation | Expose your app's API as native functions |
| Platform frameworks (love2d-style) | Register `load`, `update`, `draw` callbacks in C |
| CLI scripting | Ship the `cando` executable |
| Config and data files | `cando_dostring(vm, config_str, "config")` |

---

## Quick links

### For script authors
| Document | Contents |
|---|---|
| [getting-started.md](getting-started.md) | Install, build, run your first script |
| [language-reference.md](language-reference.md) | Complete syntax and built-in functions |
| [standard-library.md](standard-library.md) | All 17 standard library modules |
| [threading.md](threading.md) | `thread` / `await`, the thread library, patterns |

### For C embedders
| Document | Contents |
|---|---|
| [embedding.md](embedding.md) | VM lifecycle, `dofile`/`dostring`, registering natives, error handling |
| [creating-platforms.md](creating-platforms.md) | Building a game/app framework (love2d-style) |
| [c-api.md](c-api.md) | Complete `cando.h` API reference |
| [writing-extensions.md](writing-extensions.md) | Writing native functions and library modules in C |

### Internals
| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | End-to-end data flow, two-layer value system, module layout |
| [vm-internals.md](vm-internals.md) | VM struct, call frames, dispatch loop, opcodes |
| [object-system.md](object-system.md) | `CdoObject`, prototype chains, field flags, meta-keys |
| [parser-compiler.md](parser-compiler.md) | Lexer, Pratt parser, scope system, bytecode |
| [value-types.md](value-types.md) | `CandoValue`, `CdoValue`, `TypeTag`, bridge layer |

---

## Build quick-start

```bash
# Clone and build (CMake — recommended)
git clone https://github.com/rusketh/cando
cd cando
cmake -B build && cmake --build build
./build/cando script.cdo

# Or with GNU Make
make
./cando script.cdo
```

Outputs:
- `libcando.so` / `libcando.dll` — shared library
- `libcando.a` — static library
- `cando` / `cando.exe` — CLI interpreter

---

## Embedding in 10 lines

```c
#include <cando.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);
    if (cando_dofile(vm, "game/main.cdo") != CANDO_OK)
        fprintf(stderr, "error: %s\n", cando_errmsg(vm));
    cando_close(vm);
}
```

Compile and link:
```bash
gcc -o myapp myapp.c -lcando -Iinclude
```

See [embedding.md](embedding.md) for the full guide.

---

## Repository layout

```
include/
  cando.h              single public header for embedders
source/
  core/                low-level primitives (value, memory, lock, handle)
  object/              object-layer types (CdoObject, array, function, class)
  parser/              lexer + Pratt parser → bytecode
  vm/                  stack-based VM (opcodes, chunk, vm, bridge)
  lib/                 17 standard library modules
  natives.c/h          core native functions (print, type, toString)
  cando_lib.c          high-level embedding API (cando_open, cando_dofile…)
  main.c               cando CLI entry point
assets/
  icon.png / icon.ico  application icon
  gen_icon.py          icon generation script
tests/                 unit tests and integration test suite
docs/                  this documentation
```
