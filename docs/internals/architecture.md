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
if (!cando_is_object(value)) {
    cando_vm_error(vm, "expected object");
    return -1;
}
CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(value));

CdoString *key = cdo_string_intern("name", 4);
CdoValue   field;
bool       found = cdo_object_get(o, key, &field);
cdo_string_release(key);
```

Storing the resolved `CdoObject *` across a call that might trigger
GC is the most common bug in this codebase.  Treat the resolved
pointer as a short-lived temporary.

The full treatment is in [value-system.md](value-system.md).

## Runtime substructures

A handful of runtime structures don't fit neatly into either the
parser, VM, or library category, but a contributor needs to know they
exist:

- **IF-chain frames** (`CandoIfFrame` in `source/vm/vm.h`).  Every
  active `IF` chain pushes a frame recording the post-chain
  settlement IP and the stack depth at chain entry.  `OP_IF_MARK`
  (vm.c:3242) pushes one, `OP_IF_END` (vm.c:3257) pops it, and
  `OP_SETTLE n` (vm.c:3261) walks `n` levels up the stack, restores
  the saved depth (releasing temporaries), clears `spread_extra`, and
  jumps to the recorded IP.

- **Module cache** (`CandoModuleEntry` in `source/vm/vm.h`,
  `source/lib/include.c:277-299`).  `include(path)` canonicalises via
  `realpath()` and looks up in the cache before parsing.  Entries
  hold the canonical path, the cached export values, an optional
  `dlopen` handle (for `.so` / `.dylib` / `.dll` modules), and the
  chunk + closure keeping a compiled script alive.  Repeated
  `include()` of the same absolute path returns the cached value.

- **Multi-return spread state** (`spread_extra` / `array_extra` /
  `last_ret_count` in `CandoVM`, `source/vm/vm.h:367-370`).
  `OP_SPREAD_RET` (vm.c:4716) accumulates `last_ret_count - 1` into
  `spread_extra`, used by `OP_CALL` to widen a call site.
  `OP_ARRAY_SPREAD` (vm.c:4720) uses `array_extra` for
  array-literal construction so the two spread paths don't interfere.
  `OP_TRUNCATE_RET` (vm.c:4730) discards all but the first return
  value when a multi-call is used in a single-value context.

- **Thread registry** (`CandoThreadRegistry` in
  `source/vm/vm.h:243-250`).  A root-owned struct shared by every
  spawned thread.  `OP_THREAD` (vm.c:4526) increments `count` under
  mutex before creating an OS thread; the trampoline decrements and
  broadcasts on exit.  `cando_vm_request_quit()` sets
  `quit_requested` to signal cooperative shutdown; running threads
  poll the flag to drain cleanly.  `cando_vm_wait_all_threads()`
  blocks the main thread until `count` returns to zero (and the
  companion `cando_vm_wait_all_lifelines()` also waits on
  subsystem-owned lifelines such as open HTTP servers).

- **JIT hot-PC table** (`source/jit/hot.c`, `source/jit/hot.h`).  A
  chained-hash table mapping bytecode PC → hit counter.
  `cando_hot_hit(pc)` is called from loop back-edges, function
  entries, and iterator advances; it bumps the counter and returns
  `true` exactly once when the PC crosses
  `CANDO_HOT_DEFAULT_THRESHOLD` (56 hits) — that's the signal for the
  recorder to start a trace.  Triggered PCs are auto-blacklisted; the
  recorder un-blacklists on successful compile or re-blacklists on
  abort.

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
