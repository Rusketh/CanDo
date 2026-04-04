# Architecture Overview

## End-to-end execution pipeline

```
source text (.cdo)
      │
      ▼
  [Lexer]  source/parser/lexer.c
  Converts raw text into a flat token stream (CandoToken[]).
      │
      ▼
  [Parser / Compiler]  source/parser/parser.c
  Recursive-descent + Pratt expression parser.
  Writes bytecode directly into a CandoChunk; no intermediate AST.
      │
      ▼
  [CandoChunk]  source/vm/chunk.h
  A flat byte array of instructions + a constants[] table of CandoValue.
  Nested functions are stored as sub-chunks referenced from constants[].
      │
      ▼
  [VM]  source/vm/vm.c
  Stack-based interpreter.  cando_vm_exec() wraps the chunk in a
  CandoClosure, pushes the first call frame, then dispatches in a tight
  loop until OP_HALT, OP_RETURN at frame 0, or an unhandled error.
```

## The two-layer value system

Cando has two separate value representations that exist in parallel:

### VM / core layer  (`source/core/`)

```c
typedef struct CandoValue {
    u8 tag;      // TypeTag: NULL, BOOL, NUMBER, STRING, OBJECT
    union {
        bool         boolean;
        f64          number;
        CandoString *string;   // ref-counted heap string
        HandleIndex  handle;   // index into vm->handles
    } as;
} CandoValue;
```

This is the type the VM stack, call frames, globals table, and all native
function signatures use.  Primitive copies are cheap; objects are never raw
pointers — they are `HandleIndex` values (a `u32`) into a handle table owned
by `CandoVM.handles`.

### Object layer  (`source/object/`)

```c
// CdoValue — the object-layer equivalent
typedef struct CdoValue { ... } CdoValue;   // object/value.h

// CdoObject — the actual heap object
struct CdoObject { ... };                   // object/object.h
```

The object layer has its own string type (`CdoString`) with an intern table,
its own value tag enum (`CDO_NULL`, `CDO_BOOL`, `CDO_NUMBER`, `CDO_STRING`,
`CDO_OBJECT`, …), and its own object representation.

### The bridge  (`source/vm/bridge.h`)

```c
// Resolve a HandleIndex to a CdoObject*
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);

// Allocate a new CdoObject/CdoArray, register in handle table, return CandoValue
CandoValue cando_bridge_new_object(CandoVM *vm);
CandoValue cando_bridge_new_array(CandoVM *vm);

// Cross-layer value conversion
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue   cando_bridge_to_cdo  (CandoVM *vm, CandoValue v);

// Intern a VM-layer CandoString into the object-layer intern table
CdoString *cando_bridge_intern_key(CandoString *cs);
```

Whenever the VM needs to manipulate a `CdoObject` (field get/set, prototype
traversal, meta-method dispatch) it calls bridge functions.  Native library
code that creates objects must go through `cando_bridge_new_object()` so the
handle table is kept in sync.

## Handle table

```c
// In CandoVM:
CandoHandleTable handles;   // source/core/handle.h
```

`HandleIndex` is a `u32`.  `cando_handle_alloc(table, ptr)` registers a raw
`CdoObject*` and returns its index.  `cando_handle_get(table, idx)` retrieves
the pointer.  Using indices rather than raw pointers means the GC can
relocate objects without breaking script values.

## Module dependency graph

```
core/common.h   ← everything (types, cando_alloc/free)
core/value.h    ← CandoValue, CandoString
core/lock.h     ← CandoLockHeader (embedded in every CdoObject)
core/handle.h   ← CandoHandleTable
core/memory.h   ← CandoMemCtrl

object/string.h ← CdoString + intern table
object/value.h  ← CdoValue
object/object.h ← CdoObject, meta-keys
object/array.h  ← array specialization
object/function.h, object/class.h

parser/lexer.h  ← tokenizer
parser/parser.h ← parser/compiler → CandoChunk

vm/opcodes.h    ← CandoOpcode enum + helpers
vm/chunk.h      ← CandoChunk
vm/bridge.h     ← CandoValue ↔ CdoValue
vm/vm.h         ← CandoVM, CandoNativeFn, full API

lib/math.h      ← math module
lib/file.h      ← file module
lib/eval.h      ← eval() global function
lib/string.h    ← string module + string prototype

natives.h       ← core globals (print, type, tostring, …)
main.c          ← ties everything together
```
