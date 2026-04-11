# Architecture Overview

CanDo is structured as an embeddable library with a thin CLI wrapper.  The
core is `libcando.so` / `libcando.a`; the `cando` executable is ~90 lines
that call into it.

---

## Repository layout

```
include/
  cando.h             Public embedding API — the only header embedders need

source/
  cando_lib.c         High-level API: cando_open, cando_close, cando_dofile…
  natives.c/h         Core globals: print, type, toString, toNumber, …
  main.c              CLI entry point (links against libcando)

  core/               Low-level primitives
    common.h          Platform detection, CANDO_API macro, typedefs, allocators
    value.h           CandoValue, CandoString — the VM's value type
    lock.h            CandoLockHeader — per-object R/W lock
    handle.h          CandoHandleTable — index → CdoObject*
    memory.h          CandoMemCtrl — pluggable allocator
    thread_platform.h Mutex/cond wrappers (pthreads / Win32)

  object/             Object layer (heap types)
    string.h/.c       CdoString — refcounted, interned
    value.h/.c        CdoValue — object-layer value union
    object.h/.c       CdoObject — hash map of fields; prototype chain; meta-keys
    array.h/.c        Array specialisation of CdoObject
    function.h/.c     OBJ_FUNCTION, OBJ_NATIVE
    class.h/.c        Class objects
    thread.h/.c       OBJ_THREAD — thread handle

  parser/
    lexer.h/.c        Tokeniser → CandoToken[]
    parser.h/.c       Recursive-descent + Pratt → CandoChunk (no AST)

  vm/
    opcodes.h/.c      CandoOpcode enum
    chunk.h/.c        CandoChunk — flat bytecode + constant table
    bridge.h/.c       CandoValue ↔ CdoValue conversion
    vm.h/.c           CandoVM — interpreter, call frames, threading
    debug.h/.c        cando_chunk_disasm

  lib/                17 standard library modules
    math.c/h          math.*
    file.c/h          file.*
    string.c/h        string prototype
    array.c/h         array prototype
    object.c/h        object.*
    json.c/h          json.*
    csv.c/h           csv.*
    thread.c/h        thread.*
    os.c/h            os.*
    datetime.c/h      datetime.*
    crypto.c/h        crypto.*
    process.c/h       process.*
    net.c/h           net.*
    eval.c/h          eval()
    include.c/h       include()
    libutil.c/h       Internal helpers for library modules

  compat/
    win_regex.c       POSIX regex shim for Windows
```

---

## End-to-end execution pipeline

```
┌──────────────┐
│  Source text │  (from file, string, or eval)
│   (.cdo)     │
└──────┬───────┘
       │
       ▼
┌──────────────────────────────────────┐
│  Lexer  (source/parser/lexer.c)      │
│  source text → flat token stream     │
│  CandoToken[]                        │
└──────┬───────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────┐
│  Parser / Compiler                   │
│  (source/parser/parser.c)            │
│  Recursive-descent + Pratt precedence│
│  Writes bytecode directly into       │
│  CandoChunk — no AST                 │
└──────┬───────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────┐
│  CandoChunk  (source/vm/chunk.h)     │
│  Flat u8[] bytecode                  │
│  + CandoValue[] constant pool        │
│  + sub-chunks for nested functions   │
└──────┬───────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────┐
│  VM  (source/vm/vm.c)                │
│  Stack-based interpreter             │
│  cando_vm_exec() wraps chunk in      │
│  CandoClosure, pushes call frame,    │
│  dispatches until OP_HALT /          │
│  OP_RETURN at depth 0 / error        │
└──────────────────────────────────────┘
```

---

## The two-layer value system

CanDo has two parallel value representations.

### VM / core layer (`source/core/`)

```c
typedef struct CandoValue {
    TypeTag type;   /* NULL, BOOL, NUMBER, STRING, OBJECT */
    union {
        bool         boolean;
        double       number;
        CandoString *string;    /* ref-counted heap string */
        HandleIndex  handle;    /* index into vm->handles  */
    } as;
} CandoValue;
```

This type is used throughout: the VM stack, call frames, the globals table,
and all `CandoNativeFn` signatures.

Objects are never raw pointers — they are `HandleIndex` values (a `u32`)
into a handle table owned by `CandoVM.handles`.  This indirection isolates
the VM from object movement and simplifies lifecycle tracking.

### Object layer (`source/object/`)

```c
/* CdoValue — object-layer equivalent */
typedef struct CdoValue { uint8_t type; union { ... } as; } CdoValue;

/* CdoObject — the actual heap object */
struct CdoObject {
    CandoLockHeader  lock;       /* per-object R/W lock             */
    CdoObjectField  *fields;     /* open-addressed hash map         */
    u32              field_cap;
    u32              field_count;
    CdoObjectKind    kind;       /* OBJ_PLAIN, OBJ_ARRAY, OBJ_FUNCTION… */
    /* ... */
};
```

The object layer has its own string type (`CdoString`) with a process-global
intern table, its own value tag enum, and its own object representation.

### The bridge (`source/vm/bridge.h`)

```c
/* Resolve a HandleIndex to a CdoObject* */
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);

/* Allocate new objects and register them in the handle table */
CandoValue cando_bridge_new_object(CandoVM *vm);
CandoValue cando_bridge_new_array(CandoVM *vm);

/* Cross-layer value conversion */
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue   cando_bridge_to_cdo  (CandoVM *vm, CandoValue v);
```

All VM–object interaction goes through these bridge functions so the handle
table stays consistent.

---

## Handle table

```c
/* In CandoVM: */
CandoHandleTable *handles;   /* source/core/handle.h */
```

`HandleIndex` is a `u32`.  `cando_handle_alloc(table, ptr)` stores a raw
`CdoObject*` and returns its index.  `cando_handle_get(table, idx)` retrieves
the pointer.  Handles decouple script values from object addresses.

---

## Global singleton — string intern table

`cdo_object_init()` initialises a process-global string intern table and a
set of pre-interned meta-key strings (`__index`, `__newindex`, `__gc`, etc.).
It must be called exactly once per process before any objects are created.

`cando_open()` manages this automatically with an atomic reference counter:
the first `cando_open` initialises it, the last `cando_close` tears it down.

---

## Threading model

`thread expr` in CanDo spawns a real OS thread (pthreads / Win32).  The child
thread gets its own VM (`cando_vm_init_child`) that shares the parent's global
table and handle table via pointer — reads/writes are protected by their
respective R/W locks.

Closures capture upvalues that are accessed under a per-upvalue R/W lock,
allowing safe sharing between parent and child threads.

`cando_vm_wait_all_threads` blocks until all spawned threads have finished,
preventing the process from exiting with live threads.

---

## High-level API (library mode)

```
include/cando.h      ← single public header
source/cando_lib.c   ← implements the high-level functions:

  cando_open()        allocate VM, init global singleton (ref-counted)
  cando_close()       destroy VM, tear down singleton on last close
  cando_openlibs()    register all 17 standard library modules
  cando_dofile()      read file + compile + exec (realpath for include())
  cando_dostring()    compile string + exec
  cando_loadstring()  compile only → CandoChunk* (caller runs/frees)
  cando_errmsg()      human-readable error message after failure
  cando_version()     "1.0.0"
```

---

## Module dependency graph

```
include/cando.h   ← the only header embedders include

source/core/common.h    ← everything (CANDO_API, types, allocators)
source/core/value.h     ← CandoValue, CandoString
source/core/lock.h      ← CandoLockHeader
source/core/handle.h    ← CandoHandleTable
source/core/memory.h    ← CandoMemCtrl

source/object/string.h  ← CdoString + intern table
source/object/value.h   ← CdoValue
source/object/object.h  ← CdoObject, meta-keys
source/object/array.h   ← array specialisation
source/object/function.h, class.h, thread.h

source/parser/lexer.h   ← tokeniser
source/parser/parser.h  ← parser/compiler → CandoChunk

source/vm/opcodes.h     ← CandoOpcode enum
source/vm/chunk.h       ← CandoChunk
source/vm/bridge.h      ← CandoValue ↔ CdoValue
source/vm/vm.h          ← CandoVM, CandoNativeFn, full VM API
source/vm/debug.h       ← cando_chunk_disasm

source/lib/*.h          ← 17 standard library modules
source/natives.h        ← core globals table
source/cando_lib.c      ← cando_open, cando_dofile, cando_openlibs…
source/main.c           ← CLI: thin wrapper over cando_lib.c API
```
