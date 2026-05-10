# Architecture

CanDo's execution pipeline is end-to-end:

```
   .cdo source ──► lexer ──► parser/compiler ──► CandoChunk
                                                     │
                                                     ▼
                              VM dispatch loop  ◄─ JIT (optional)
                                     │
                                     ▼
                                heap (CdoObject)
                                     │
                                     ▼
                                handle table  ◄── CandoValue (stack)
```

There is **no AST**.  The parser emits bytecode directly into a
`CandoChunk`.  This keeps the front-end small, eliminates a node type
hierarchy, and gives the parser visibility into the bytecode it just
emitted (used for some peephole patches and class sugar).

## Phases

### 1. Lex

`source/parser/lexer.c` consumes the raw source and produces tokens on
demand.  The lexer is hand-written, byte-driven, and handles:

- Identifiers and keywords (case-insensitive but not mixed-case).
- Number literals (decimal, hex `0x`, octal `0o`, binary `0b`).
- Three string kinds (`"…"`, `'…'`, `` `…` ``) with appropriate
  escape and interpolation rules.
- All operator tokens, including the multi-character ones (`->`, `<-`,
  `?.`, `?[`, `~>`, `~!>`, `~&>`, `::`, `=>`).
- Comments (single- and multi-line).

### 2. Parse + compile

`source/parser/parser.c` is a **Pratt parser** that emits bytecode
into a `CandoChunk` as it goes.  Each token has a prefix-handler and an
infix-handler; the algorithm dispatches on token type and operator
binding power.

The compiler tracks:

- **Scopes.**  Each `{ … }` opens a new scope; `VAR`/`CONST`/`GLOBAL`
  bindings, function parameters, and loop induction variables are all
  tracked here.
- **Locals.**  Resolved to slot indices, used by `OP_LOAD_LOCAL` /
  `OP_STORE_LOCAL`.
- **Upvalues.**  Captured locals from enclosing scopes; tracked in the
  closure's upvalue list and emitted as `OP_LOAD_UPVAL` /
  `OP_STORE_UPVAL`.
- **Globals.**  Resolved to constant-pool string indices; emitted as
  `OP_LOAD_GLOBAL` / `OP_STORE_GLOBAL`.
- **Loop and TRY frames.**  Used by `BREAK`, `CONTINUE`, `THROW`, and
  `RERAISE` to know what to unwind.

The output is a `CandoChunk` containing:

- A flat byte array of bytecode.
- A constant pool (numbers, strings, sub-chunks for nested functions).
- Source-position metadata for diagnostics.
- An ordered list of upvalue descriptors per function.

### 3. Execute

`source/vm/vm.c` runs the chunk on a stack-based VM.  Dispatch uses
**GCC computed gotos** when available (every supported compiler) for
tight branch prediction.  The dispatch loop is in `vm_run()`; each
opcode is a `case`/label that:

1. Decodes its operands.
2. Pops / reads stack values.
3. Performs its work (sometimes through the bridge layer for objects).
4. Pushes results.
5. Jumps to the next instruction.

A complete opcode list is in [vm.md](vm.md).

### 4. Optionally JIT

When the `--jit` flag (or `CANDO_JIT=1`, or `jit.on()`) is set, the
runtime tracks back-edges, function entries, and iterator advances.
PCs that exceed a hot-threshold trigger trace recording.  See
[jit.md](jit.md) for the full lifecycle.

## Concurrency model

The VM was designed around **first-class OS threads**.  The pieces:

- **Handle indirection.**  Every `CandoValue` for an object stores a
  `HandleIndex`, not a pointer.  A central handle table maps indices
  to current `CdoObject *` pointers.  The GC can move objects without
  invalidating values held by other threads' stacks — they re-resolve
  through the handle table.
- **Lock primitives.**  `source/core/lock.h` exposes a re-entrant
  pthread mutex wrapper.  `object.lock` / `object.unlock` are the
  script-visible side; `_meta` writes use the same primitive
  internally.
- **Per-thread VMs.**  Each spawned script thread has its own value
  stack and call stack but shares the global table, intern table, and
  handle table with its siblings.
- **GC.**  The collector runs concurrently with script execution.  It
  cooperates with the handle table (relocation is invisible to live
  values) and with the lock primitives (objects under script lock are
  treated as roots until released).

## Boundary between layers

Two value representations exist intentionally — the script side and
the heap side.  The bridge layer (`source/vm/bridge.c`) is the only
code that knows both.  Any new code that needs to operate on objects
goes through the bridge:

```c
CdoObject *o = bridge_resolve_object(vm, value);
if (!o) cando_throw(vm, "expected object");

CandoValue field = cdo_object_get(o, "name");
```

Storing the resolved `CdoObject *` across a call that might trigger
GC is the most common bug in this codebase.  Treat the resolved
pointer as a short-lived temporary.

The full treatment is in [value-system.md](value-system.md).

## File layout cheat-sheet

| Question                                  | Look in |
|-------------------------------------------|---------|
| What does this syntax mean?               | `source/parser/parser.c` |
| What does this opcode do?                 | `source/vm/vm.c` |
| Where is the dispatch table?              | `source/vm/opcodes.h` / `.c` |
| How does a string get interned?           | `source/core/string.c` (intern table) and `source/object/string.c` |
| How do upvalues work?                     | `source/object/closure.c` and `source/vm/vm.c` (`OP_CLOSE_UPVAL`) |
| How does a class instance get built?      | `OP_NEW_CLASS`, `OP_BIND_METHOD` in `source/vm/vm.c` |
| How does a thread get spawned?            | `source/object/thread.c` |
| How does a native get registered?         | `source/lib/libutil.c` |

## Public surface

The single public header is
[`include/cando.h`](../../include/cando.h).  Anything not declared
there is internal: refactor freely, just don't break the symbols that
*are* declared there.
