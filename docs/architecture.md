# Architecture Overview

CanDo is a C11 embeddable scripting language. The core ships as
`libcando.so` / `libcando.a`; the `cando` executable is a ~90-line CLI
wrapper that calls into the library API.

---

## Repository layout

```
include/
  cando.h             Public embedding API

source/
  cando_lib.c         High-level API: cando_open, cando_close, cando_dofile...
  natives.c/h         Core globals: print, type, toString
  main.c              CLI entry point (~90 lines)

  core/
    common.h          Platform detection, CANDO_API, typedefs, allocators
    value.h           CandoValue, CandoString
    lock.h            CandoLockHeader (per-object R/W lock)
    handle.h          CandoHandleTable (HandleIndex -> CdoObject*)
    memory.h          CandoMemCtrl (pluggable allocator)
    thread_platform.h Mutex/cond wrappers (pthreads / Win32)

  object/
    string.h/.c       CdoString -- refcounted, interned
    value.h/.c        CdoValue -- object-layer value union
    object.h/.c       CdoObject -- hash map, prototype chain, meta-keys
    array.h/.c        Array specialisation of CdoObject
    function.h/.c     OBJ_FUNCTION, OBJ_NATIVE
    class.h/.c        Class objects
    thread.h/.c       OBJ_THREAD -- thread handle

  parser/
    lexer.h/.c        Tokeniser -> CandoToken stream
    parser.h/.c       Recursive-descent + Pratt -> CandoChunk (no AST)

  vm/
    opcodes.h/.c      CandoOpcode enum (~80 opcodes in bands)
    chunk.h/.c        CandoChunk -- flat bytecode + constant table
    bridge.h/.c       CandoValue <-> CdoValue conversion
    vm.h/.c           CandoVM -- interpreter, call frames, threading
    debug.h/.c        cando_chunk_disasm

  lib/                17 standard library modules
    math, file, string, array, object, json, csv,
    thread, os, datetime, crypto, process, net,
    eval, include, http, https
    libutil.c/h       Internal helpers

  compat/
    win_regex.c       POSIX regex shim for Windows
```

---

## Execution pipeline

```
Source text (.cdo)
       |
       v
Lexer  (source/parser/lexer.c)
  source text -> flat CandoToken stream
       |
       v
Parser/Compiler  (source/parser/parser.c)
  recursive-descent + Pratt precedence
  writes bytecode directly -- no AST
       |
       v
CandoChunk  (source/vm/chunk.h)
  flat u8[] bytecode
  + CandoValue[] constant pool
  + sub-chunks for nested functions
       |
       v
VM  (source/vm/vm.c)
  stack-based interpreter
  GCC computed gotos for dispatch
  runs until OP_HALT / OP_RETURN at depth 0 / error
```

The parser/compiler is a single pass: it consumes tokens and emits bytecode
into a `CandoChunk` directly, with no intermediate AST. Nested function
bodies produce sub-chunks attached to the parent chunk.

See [parser-compiler.md](parser-compiler.md) and
[vm-internals.md](vm-internals.md) for details.

---

## Two-layer value system

CanDo maintains two parallel value representations connected by a bridge
layer.

**VM layer** (`source/core/value.h`): `CandoValue` is a tagged union with a
`u8` type tag and a union of `bool`, `double`, `CandoString*`, or
`HandleIndex`. This type appears on the VM stack, in call frames, in the
globals table, and in all `CandoNativeFn` signatures. Objects are never raw
pointers at this layer -- they are `HandleIndex` values into a handle table
owned by `CandoVM.handles`.

**Object layer** (`source/object/`): `CdoValue` has its own tag enum and
union. `CdoString` is refcounted with a process-global intern table.
`CdoObject` is an open-addressed hash map of fields with a prototype chain
via the `__index` meta-key.

**Bridge** (`source/vm/bridge.h`): All VM-to-object interaction goes through
bridge functions so the handle table stays consistent:

- `cando_bridge_resolve` -- resolve `HandleIndex` to `CdoObject*`
- `cando_bridge_new_object` -- allocate a plain object, register in handles
- `cando_bridge_new_array` -- allocate an array object, register in handles
- `cando_bridge_to_cando` -- convert `CdoValue` to `CandoValue`
- `cando_bridge_to_cdo` -- convert `CandoValue` to `CdoValue`
- `cando_bridge_intern_key` -- intern a string for use as a field key

See [value-types.md](value-types.md) and
[object-system.md](object-system.md) for the full type semantics.

---

## Handle table

`HandleIndex` is a `u32`. It decouples script-visible values from raw object
addresses. `cando_handle_alloc(table, ptr)` stores a `CdoObject*` and
returns its index; `cando_handle_get(table, idx)` retrieves the pointer. The
table is owned by `CandoVM.handles` (defined in `source/core/handle.h`).

---

## Global singleton

`cdo_object_init()` initialises a process-global string intern table and a
set of pre-interned meta-key strings: `__index`, `__call`, `__type`,
`__tostring`, `__equal`, `__greater`, `__add`, `__len`, `__negate`,
`__not`, `__newindex`, `__is`.

`cando_open()` manages this automatically with an atomic reference counter:
the first `cando_open` call initialises the singleton, the last
`cando_close` tears it down.

---

## Threading model

`thread expr` in CanDo spawns a real OS thread (pthreads on POSIX, Win32
threads on Windows). The child thread gets its own VM via
`cando_vm_init_child`, which shares the parent's global table and handle
table via pointer. Reads and writes to shared state are protected by R/W
locks.

Closures capture upvalues that are accessed under a per-upvalue R/W lock,
allowing safe sharing between parent and child threads.

`cando_vm_wait_all_threads` blocks until every spawned thread has finished,
preventing the process from exiting with live threads.

---

## High-level API

All public entry points live in `include/cando.h` and are implemented in
`source/cando_lib.c`:

| Function           | Purpose                                              |
|--------------------|------------------------------------------------------|
| `cando_open()`     | Allocate VM, init global singleton (ref-counted)     |
| `cando_close()`    | Destroy VM, tear down singleton on last close        |
| `cando_openlibs()` | Register all 17 standard library modules             |
| `cando_dofile()`   | Read file + compile + execute                        |
| `cando_dostring()` | Compile string + execute                             |
| `cando_loadstring()`| Compile only, returns `CandoChunk*`                 |
| `cando_errmsg()`   | Human-readable error message after failure           |
| `cando_version()`  | Version string                                       |

See [embedding.md](embedding.md) for usage examples and integration
guidance.
