# JIT Compiler Plan

This document is a roadmap for replacing CanDo's stack-based bytecode
interpreter with a tracing JIT compiler in the style of LuaJIT 2.x. It
is a *plan* — no code has been written yet. It lays out the final
architecture, the order of work, and the CanDo-specific obstacles that
make the project harder than a textbook LuaJIT clone.

The end goal: keep `libcando` and the embedding API
(`include/cando.h`) bit-compatible, keep the existing interpreter as
the cold path, and gain ≥5–10× speedup on tight numeric loops and
≥2–3× on mixed scripts.

---

## 1. Current state (baseline)

| Component | File | Notes |
|---|---|---|
| Lexer | `source/parser/lexer.c` | 642 LOC |
| Parser/compiler | `source/parser/parser.c` | 2607 LOC, no AST, emits bytecode directly |
| Bytecode chunk | `source/vm/chunk.c`, `chunk.h` | flat `u8[]`, constant pool, line table |
| Opcodes | `source/vm/opcodes.h` | ~80 ops in 19 bands; 1/3/5-byte variable width |
| Interpreter | `source/vm/vm.c` | 4144 LOC, GCC computed-goto dispatch |
| Bridge | `source/vm/bridge.c` | `CandoValue ↔ CdoValue`, handle resolve |
| Object layer | `source/object/*` | `CdoObject` hash map, prototype chain |
| Threading | `source/core/thread_platform.c` | real OS threads, R/W locks per object/upvalue |

Performance characteristics today:

- Every arithmetic op: 1 indirect goto + tag check on both operands.
- Every object access: handle lookup (`HandleIndex → CdoObject*`)
  through `cando_handle_get`, then a hash probe on `CdoObject`.
- Every native call: tag check (negative-number sentinel) + indirect
  function pointer.
- Closures/upvalues access goes through a per-upvalue R/W lock.
- The hottest loop opcode is a 1-byte op + 2-byte signed offset.

These four points are the main targets of the JIT.

---

## 2. JIT strategy choice

Two real options. We pick (A).

**(A) LuaJIT-style tracing JIT (recommended).** Detect hot loops at
runtime, record one execution trace as linear SSA IR, optimise it,
emit native machine code, and side-exit back to the interpreter on
guard failure. Best ROI for CanDo's dynamic, prototype-based,
handle-indirected value system because the recorder *specialises away*
the type tags and handle lookups that dominate the interpreter cost.

**(B) Per-function method JIT (rejected).** Compiles whole functions
ahead of execution. Simpler control flow but worse for dynamically
typed code: every operation has to handle every type, so we'd ship
mostly the same work the interpreter does. Reconsider only if the
trace recorder turns out to be unworkable with CanDo's exception model
(see §9).

Sub-decision: **trace IR shape**. Adopt LuaJIT-style linear SSA with
guards and snapshots, not a CFG. Trace shape == bytecode execution
order, which makes recording almost mechanical.

Sub-decision: **codegen backend**. Three candidates:

| Backend | Pros | Cons |
|---|---|---|
| DynASM (LuaJIT's) | proven, tiny, fast codegen | per-arch templates, x64/arm64 only |
| libgccjit | portable, optimising | huge dep, slow codegen, GPL surface |
| Custom emitter | full control | months of work per arch |

Recommendation: **DynASM port**, x86-64 first, AArch64 second. CanDo
already requires GCC (computed gotos), so the DynASM toolchain
(LuaJIT's preprocessor + Lua) is acceptable as a build-time dep. The
generated `.h` files ship in-tree so end users do not need Lua.

---

## 3. Final architecture

```
                             cando_dofile / cando_dostring
                                          │
                            parser → CandoChunk (unchanged)
                                          │
                                          ▼
                              ┌───────────────────────┐
              cold path  ←──  │     interpreter       │  (existing vm.c)
                              │  + hot-path counters  │
                              └───────────┬───────────┘
                                          │ counter overflow on
                                          │ backedge / call entry
                                          ▼
                              ┌───────────────────────┐
                              │      recorder         │  bytecode → IR
                              │  (trace_record.c)     │  + type guards
                              └───────────┬───────────┘
                                          │
                                          ▼
                              ┌───────────────────────┐
                              │       optimiser       │  fold / CSE / LICM
                              │   (trace_opt.c)       │  alloc-sinking, dse
                              └───────────┬───────────┘
                                          │
                                          ▼
                              ┌───────────────────────┐
                              │       backend         │  IR → mcode (DynASM)
                              │  (trace_asm_x64.dasc) │
                              └───────────┬───────────┘
                                          │
                                          ▼
                              ┌───────────────────────┐
                              │      mcode cache      │  exec-mapped pages
                              │   patched into chunk  │  side-exits → interp
                              └───────────────────────┘
```

New files (proposed):

```
source/jit/
  jit.h                    public JIT API (cando_jit_init, cando_jit_attach)
  jit.c                    lifecycle + glue
  jit_profile.c            backedge counters, trigger thresholds
  jit_ir.h / jit_ir.c      IR opcodes, SSA value table, snapshots
  jit_record.c             bytecode → IR recorder
  jit_opt.c                FOLD / CSE / LICM / sink / dse passes
  jit_asm.h                backend interface
  jit_asm_x64.dasc         x86-64 DynASM template (DynASM-preprocessed)
  jit_asm_arm64.dasc       AArch64 template (phase 2)
  jit_mcode.c              executable page allocator (mmap RWX flip)
  jit_snapshot.c           snapshot/restore for side exits
  jit_dump.c               -Xjit-dump tracing output (optional)
```

The interpreter stays put. A `CandoChunk` gains a `trace_table*`
sidecar (lazy) that maps bytecode offsets to compiled traces; the
interpreter checks this on backedges via a single hot-path opcode.

---

## 4. Bytecode and value-format prerequisites

CanDo's current encoding is fine for the interpreter but inconvenient
for the recorder. Two changes are needed before we touch the JIT:

### 4.1 Stable per-opcode descriptors

The recorder needs metadata for every opcode: input arity, output
arity, side-effect class, may-throw, may-call, type signature. Add a
table next to `cando_opcode_fmt()` in `source/vm/opcodes.c`:

```c
typedef struct CandoOpInfo {
    u8  in_arity;      // values consumed from stack
    u8  out_arity;     // values produced
    u8  effect;        // EFFECT_PURE|LOAD|STORE|CALL|THROW|CONTROL
    u8  may_throw;
    u8  may_recurse;   // calls into VM — abort recording
} CandoOpInfo;
extern const CandoOpInfo cando_opcode_info[OP_COUNT];
```

This is independently useful for the disassembler and for static
analysis.

### 4.2 NaN-boxed `CandoValue`

Today `CandoValue` is a 16-byte tagged union (`u8 tag` + 8-byte
payload). The JIT will move thousands of values through registers per
trace; passing 16 bytes around defeats register allocation. Switch to
NaN-boxing:

```c
typedef union { f64 d; u64 u; } CandoValue;

// 0x7FF0... mantissa-tagged
//   tag bits 51:48 → TYPE_NULL/BOOL/STRING/OBJECT
//   payload bits 47:0 → bool, CandoString*, HandleIndex, …
// real doubles use the regular IEEE encoding
```

This is a large invasive change. It touches:

- `source/core/value.h/.c`
- every native function (the `CandoNativeFn` signature is unchanged
  but accessors like `v.as.number` become `cando_as_number(v)`)
- the bridge layer
- every standard-library module under `source/lib/`

Approach: introduce inline accessors (`cando_as_number`,
`cando_as_handle`, etc.) in a *separate commit* against the current
union representation. Then flip the storage in a follow-up commit. The
accessors hide the change from native code. This is the same trick
LuaJIT used to migrate from Lua 5.1's tagged union to NaN-boxed
`TValue`.

### 4.3 Handle-table fast path

The `HandleIndex → CdoObject*` lookup is on the critical path of every
field access. Today it's a function call through
`cando_handle_get`. Inline it as a macro that does a single bounded
array load:

```c
#define CANDO_HANDLE_DEREF(t, h) ((t)->slots[(h)].ptr)
```

…with the bounds check elided in JIT-emitted code (the recorder
already proved the handle is valid via a guard). Generation count for
slot reuse is already in `CandoHandleTable` — keep it for the
interpreter and skip it on traces by snapshotting the generation at
record time and emitting a guard.

(Phase 1.5 status: the inline `cando_handle_get` and the
`CANDO_HANDLE_DEREF` macro both ship in `source/core/handle.h`. The
macro currently has no call sites; it is consumed by the JIT recorder
in Phase 4.)

---

## 5. Hot-path detection

Add per-bytecode-byte hit counters keyed by `(CandoChunk*, offset)`:

- backedge of `OP_LOOP` (the only backward branch in the ISA).
- function-entry (first byte of every chunk that is the body of a
  closure called via `OP_CALL`).
- `OP_FOR_NEXT` / `OP_FOR_OVER_NEXT` / `OP_PIPE_NEXT` (the iterator
  ops — these are the "hot" loop primitives in idiomatic CanDo).

Counter storage: side table on the chunk, allocated lazily on first
miss (`u16` per byte position would be too much; use a hash table
keyed by offset with `u32` counters, sized to the number of distinct
backedge sites).

Threshold: 56 hits triggers recording (LuaJIT's default — pick a
config knob, not a constant).

To keep the interpreter cheap when the JIT is disabled, the counter
increment lives behind a single branch on `vm->jit_enabled`. On
release builds with JIT on, the counter increment is `add [r], 1` +
`jo rare_path` — same overhead LuaJIT pays.

---

## 6. Recorder

The recorder is the biggest single piece of work. It runs alongside
the interpreter: while a trace is being recorded, every opcode's
handler additionally calls into `jit_record.c` to emit IR.

### 6.1 IR shape

Linear array of SSA-typed instructions. Every IR op has:

```c
typedef struct IRIns {
    u16 op;           // IR_ADD, IR_FLOAD, IR_HREF, IR_GUARD_NUM, …
    u8  type;         // IRT_NUM, IRT_STR, IRT_OBJ, IRT_NIL, IRT_BOOL
    u8  flags;
    u32 op1;          // SSA ref (or constant pool ref) — high bit = const
    u32 op2;
} IRIns;
```

A trace = `IRIns *ir`, a snapshot table for guards, and a constant
table.

### 6.2 IR opcode classes

| Class | Examples | Notes |
|---|---|---|
| Constants | `KNUM`, `KSTR`, `KHANDLE` | folded into operand encoding |
| Loads | `SLOAD`, `FLOAD`, `HLOAD` (handle deref), `ULOAD` (upvalue) | |
| Stores | `SSTORE`, `FSTORE`, `USTORE` | sinkable for dead allocs |
| Arith | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `NEG` | typed `IRT_NUM` |
| Compare | `EQ`, `LT`, `LE` | produce `IRT_BOOL`, fed to guards |
| Guard | `GUARD_NUM`, `GUARD_OBJ`, `GUARD_TRUE`, `GUARD_GEN` | side-exit anchor |
| Memory | `HREF` (object hash probe), `AREF` (array index) | |
| Call | `CALLN` (native), `CALLC` (C runtime helper) | |

`HREF` deserves special treatment because object lookup is the
hottest non-arith op. The recorder folds `OP_GET_FIELD`, the
prototype-chain walk, and the bridge conversion into a single
`HREF` IR op that the optimiser can hoist out of loops if the field
key is loop-invariant — this is the exact trick that gives LuaJIT
constant-time table access in tight loops.

### 6.3 Recording boundaries

Start: backedge counter overflow.
Stop: any of —

1. The same backedge fires again (we closed the loop — emit `LOOP`).
2. We left the function via `OP_RETURN` more than `JIT_MAXFRAMES`
   times.
3. We hit an unrecordable opcode (see §9).
4. Trace length exceeds `JIT_MAXIRINS` (default 4096).

Abort handling: black-list the start PC for `JIT_PENALTY_BACKOFF`
counter ticks before retrying. If a trace is aborted four times at the
same PC, blacklist permanently.

### 6.4 Snapshots

Every guard records a snapshot: a list of "stack slot N currently
holds SSA ref M". On side-exit, the exit stub restores the
interpreter stack from the snapshot, sets `vm->stack_top`, restores
`frame_count` if the trace was inlining a call, and resumes
interpretation at the bytecode PC stored in the snapshot.

---

## 7. Optimiser

LuaJIT runs these passes during recording (forward-flow,
single-pass) so the IR never gets large. Adopt the same pipeline:

1. **FOLD** — peephole + constant folding, table-driven.
2. **CSE** — hash-keyed by `(op, op1, op2)`.
3. **LOAD-FORWARD / STORE-FORWARD** — alias-aware load elimination.
4. **DSE** — dead store elim for sunk allocations.
5. **LICM** — for the loop body, hoist invariants above the LOOP
   marker.
6. **Allocation sinking** — `OP_NEW_OBJECT` followed by stores that
   never escape the trace becomes a register tuple; the actual
   `cando_bridge_new_object` call is emitted only on side-exit.

Allocation sinking is the highest-value pass for CanDo because the
language idiomatically returns small objects (e.g. `{x: 1, y: 2}`)
that today force a `CdoObject` heap allocation through the bridge.

---

## 8. Code generation

DynASM-based, register-allocated linear scan walking the IR backwards
(LuaJIT's approach). Per-arch file emits the prologue, epilogue, and
exit stubs.

### 8.1 Calling convention

A trace gets called from the interpreter through one entry point:

```c
typedef u32 (*TraceFn)(CandoVM *vm);   // returns exit number
```

Inside the trace:

- `vm` lives in a callee-saved register (`%rbx` on x64).
- Stack pointer in `%rbp` mirrors `vm->stack_top`.
- Hot SSA values stay in registers; spills go to a fixed-size
  per-trace spill area in the `vm` struct (`CandoValue spill[64]`).

### 8.2 mcode allocation

Single global mcode area mapped `PROT_READ|PROT_EXEC` after each
write. On Linux/macOS use `mmap` + `mprotect`; on Windows use
`VirtualAlloc` + `VirtualProtect`. Apple Silicon needs the
`MAP_JIT` flag and `pthread_jit_write_protect_np` toggling — handle
in `jit_mcode.c` behind `#ifdef __APPLE__`.

Fixed arena, bump-pointer; on overflow flush *all* traces and start
over (LuaJIT does this; rare in practice).

### 8.3 Side exits

One exit stub per trace, parametrised by exit number. The stub:

1. Spills all live IR registers into the snapshot's stack slots.
2. Loads the bytecode PC from the snapshot.
3. Tail-jumps to the interpreter's dispatch loop with `ip` set.

---

## 9. CanDo-specific obstacles

These are the items that don't exist in LuaJIT and have to be solved.

### 9.1 Two-layer value system

Every script-visible object is a `HandleIndex` that maps to a
`CdoObject*`, and many ops convert between `CandoValue` and
`CdoValue` via the bridge. The bridge layer must become recordable.

Decision: treat `cando_bridge_resolve` and
`cando_bridge_intern_key` as IR primitives (`HREF`, `KINTERN`) so
they're inlined into the trace. Treat `cando_bridge_new_object` and
`cando_bridge_new_array` as IR primitives that participate in
allocation sinking. The conversion functions (`to_cando` /
`to_cdo`) become NOPs at the IR level once values are NaN-boxed —
both layers will share the same encoding.

This means §4.2 (NaN-boxing) is a *prerequisite* for the JIT, not an
optimisation.

### 9.2 Try/catch and error unwinding

`OP_TRY_BEGIN` pushes a `CandoTryFrame` that captures `stack_top`,
`frame_count`, `loop_depth`. The JIT must keep these in sync at
every guard exit, or `THROW` inside a trace will rewind to the
wrong stack height.

Plan: traces refuse to record across `OP_TRY_BEGIN` /
`OP_TRY_END` boundaries in v1. Inside a `try` block we stay in the
interpreter. v2: model try-frames as IR allocations and snapshot
them.

### 9.3 Threading and per-object R/W locks

`CdoObject` and `CandoUpvalue` carry `CandoLockHeader` that's
acquired on every read/write. JIT-compiled code that elides the lock
can race with another OS thread.

Plan: emit a guard `GUARD_OBJ_OWNED` at the top of the trace that
checks the object's lock thread-owner field. If another thread
touched it, side-exit. For shared upvalues, bail out of recording
(rare). Document this as "the JIT specialises for single-thread
hot-paths"; cross-thread sharing transparently falls back to the
interpreter.

### 9.4 Multi-return and spread (`OP_SPREAD_RET`, `OP_*_SPREAD`)

These rely on `vm->last_ret_count`, which is a runtime quantity. The
recorder freezes the count at record time and emits a
`GUARD_LAST_RET_COUNT` so a different return arity at run time
side-exits.

### 9.5 Mask operators (`OP_MASK_PASS`, `OP_MASK_SKIP`,
`OP_MASK_APPLY`)

These adjust the stack based on a bitmask operand. Pure stack
shuffles — the recorder sees them as zero-cost SSA renamings.

### 9.6 Variable-width `OP_CLOSURE`

`OP_CLOSURE` has a tail of capture descriptors. The recorder treats
it as a single IR op `CLOSURE` whose operand list reads the capture
table. Closure creation participates in allocation sinking — most
closures captured by `for`/`pipe` loops never escape and become
register tuples.

### 9.7 Native function sentinels

Natives are encoded as negative `f64`. NaN-boxing changes this — a
native becomes a fourth top-level type tag (`TYPE_NATIVE`). This is a
breaking change to anyone reading values directly. Mitigated by §4.2
adding `IS_NATIVE_FN`/`NATIVE_INDEX` accessors. Native call from a
trace is `CALLN` IR; we inline well-known pure natives
(`math.sqrt`, `string.len`, …) by tagging their entries in the
native table with `CANDO_NATIVE_PURE`.

### 9.8 Eval re-entrancy

`cando_vm_exec_eval` sets `vm->eval_stop_frame`. A trace that
crosses this depth must side-exit at the matching `OP_RETURN`.
Implement by emitting `GUARD_EVAL_BOUNDARY` at every recorded
`OP_RETURN`.

### 9.9 GC and handle invalidation

Today the handle table can compact and reissue indices. If a trace
caches `HandleIndex → CdoObject*` translations, a GC run between
calls invalidates them.

Plan: bump a `vm->handle_generation` counter on any GC compaction.
Every trace entry checks it and, on mismatch, flushes its mcode and
re-records. This is cheaper than per-access generation guards.

---

## 10. Phased implementation

Each phase ends with the project still building, all tests passing,
and (where applicable) measurable performance numbers.

### Phase 0 — Baseline & infrastructure (1 week)

- Add `tests/bench/` with `mandelbrot.cdo`, `nbody.cdo`,
  `fib.cdo`, `forms_event_loop.cdo`. Wire up `make bench`.
- Add `--jit-stats` plumbing to `main.c` (stub for now).
- Add the `CandoOpInfo` table from §4.1.

### Phase 1 — NaN-boxing (2 weeks)

- Introduce `cando_as_number`/`cando_as_string`/… accessors on the
  current union. Convert all 19 stdlib modules + bridge + natives to
  use them. Tests stay green.
- Flip storage to NaN-box. Update `cando_value_tostring`,
  `cando_value_equal`, `cando_value_copy`,
  `cando_value_release`. Update bridge layer. Tests stay green.
- Inline `CANDO_HANDLE_DEREF`. Benchmark — expect 5–15% interpreter
  speedup just from the cache-friendlier `CandoValue`.

### Phase 2 — Profiling hooks (1 week)

- Hot-path counters on backedges + function entry + iterator
  `_NEXT` ops.
- `-Xjit-hotcount` knob + per-PC counter dump for tuning thresholds.
- `vm->jit_enabled` flag with no-op codepath. Behind
  `CANDO_ENABLE_JIT` build flag.

### Phase 3 — IR + recorder, no codegen (3 weeks)

- IR data structures (§6.1).
- Recorder for the arithmetic, comparison, local-variable, and
  jump bands (the "easy" 30 opcodes).
- Per-opcode tests: record one trace, verify IR shape matches a
  golden file.
- No codegen yet — execute by IR-interpreter (a debug mode that's
  also useful long-term).

### Phase 4 — Recorder for objects, calls, closures (3 weeks)

- `HREF` / `AREF` / `CALLN` / `CALLC` recording.
- Allocation sinking shape for `OP_NEW_OBJECT` / `OP_NEW_ARRAY` /
  `OP_CLOSURE`.
- Abort gracefully on `OP_TRY_BEGIN`, `OP_THROW`, `OP_THREAD`,
  `OP_AWAIT`, eval-boundary returns.

#### Phase 4.4 — Allocation sinking sub-plan

Decomposed into commit-sized steps.  Each step is self-contained
(builds + tests + benches green) and individually shippable.  Real
sinking only kicks in at step `j+k` -- earlier steps lay down the
plumbing.

**Status: COMPLETE.**  All 11 sub-steps (a through k) shipped.
Phase 4 is now closed.  Headline result: arrays-in-loop benchmark
went from ~1.9x JIT speedup (codegen with helper calls) to ~10.7x
(sinking with stack buffers).  Existing benches unchanged.

v1 follow-ups:
  - **v1a (DONE)** Sinking for IR_NEW_OBJECT.  Codegen tracks
    field name -> stack slot index in `CGSunk.field_kref[]`;
    `IR_FIELD_SET` writes to `[rbp + base - 8*field_idx]`,
    `IR_FIELD_GET` reads symmetrically.  Bail if a field name
    isn't a constant string KIDX.  Object literal allocation
    benchmark now ~9x faster.
  - **v1c (DONE)** Materialise-on-side-exit.  Escape analysis
    no longer NOPs the dead SSTORE; it marks it `IRF_SUNK`.
    Codegen records each sunk SSTORE in `cg.sink_recs[]`
    (slot, stack offset, capacity, is_array, field names).
    The side-exit common stub now calls
    `cando_jit_materialize_sunk_for_mcode` BEFORE the snapshot
    replay helper.  That helper allocates a real heap
    array/object from the stack buffer and writes it to
    `frame_slots[slot]`.  Fixes the inline-fn bug where a local
    declared outside the loop and read after the loop saw
    stale data after a side-exit (e.g. `FUNCTION foo() { VAR a
    = [0]; FOR k IN 1->200 { a = [k]; } RETURN a[0]; }` now
    returns 200 with JIT, matching bytecode).
  - **v1b (open)** Sinking for IR_RANGE_* (size known only at
    runtime; deferred -- low value without OP_FOR_INIT
    recording).
  - **(open)** Smarter escape analysis (alias-aware across
    SSTORE/SLOAD chains).

| step | scope | recorder | IR-interp | codegen | sinking | bench impact |
|------|-------|----------|-----------|---------|---------|--------------|
| **a** | IR shape: `IR_NEW_ARRAY` (op1=count) + `IR_ARRAY_APPEND` (array, value).  Recorder emits both for `OP_NEW_ARRAY <count>`.  Result `IRT_OBJ` lives in `vals[i].u`. | yes | yes (call `cando_bridge_new_array` + `cdo_array_push`) | bail | no | none yet -- traces still abort downstream |
| **b** | Allow `IRT_OBJ` values through `IR_SLOAD` / `IR_SSTORE`.  Per-slot type tracking on the recorder; `IR_SSTORE` dispatches on source type for the wrap (`cando_number(d)` for nums, raw `.u` for objects).  `IR_SLOAD` honours its `type` field at runtime. | yes | yes | bail | no | scripts that store an array literal to a local now compile traces |
| **c** | `IR_INDEX_GET` (array, idx) for `OP_GET_INDEX`.  Result `IRT_NUM` initially (assumes array of numbers; bail otherwise). | yes | yes | bail | no | `arr[k]` reads inside a hot loop trace |
| **d** | `IR_INDEX_SET` for `OP_SET_INDEX` -- 3 operands need either a packed encoding or a 2-op pair (`IR_INDEX_PREP` + `IR_INDEX_STORE`). | yes | yes | bail | no | `arr[k] = v` writes inside a hot loop trace |
| **e** | `IR_NEW_OBJECT` + `IR_FIELD_GET` / `IR_FIELD_SET` for empty + small object literals. | yes | yes | bail | no | object literals trace |
| **f** | `IR_RANGE_ASC` for `OP_RANGE_ASC` (and DESC variant).  Pops 2 numerics, allocates the range array.  Same shape as `IR_NEW_ARRAY` + N `IR_ARRAY_APPEND` but lowered specially for the typical contiguous case. | yes | yes | bail | no | nbody-class scripts trace via `FOR i IN 0 -> N - 1` |
| **g** | Codegen for `IR_NEW_ARRAY` / `IR_ARRAY_APPEND` -- C calls to the bridge helpers from mcode.  `IR_INDEX_GET/SET` codegen too. | done above | done above | yes | no | array-allocating traces fully native |
| **h** | Codegen for `IR_NEW_OBJECT` + field ops. | done above | done above | yes | no | object literal traces fully native |
| **i** | Codegen for `IR_RANGE_ASC`. | done above | done above | yes | no | range-FOR traces fully native (still allocates per iter) |
| **j** | Escape analysis pass that runs after DSE/DCE/LICM in `cando_recorder_finish`.  For each `IR_NEW_*` op, follow the result IRRef forward; mark sinkable iff every use is one of `IR_INDEX_GET/SET`, `IR_FIELD_GET/SET`, or another sinkable allocation -- i.e. the value never reaches an `IR_SSTORE` to a slot, an `IR_GSTORE` to a global, an `IR_CALL_*`, or `IR_RETURN`.  Bit per IR op marking sinkable. | -- | -- | -- | partly | no direct bench (groundwork) |
| **k** | Allocation sinking codegen.  For sunk allocations: emit no allocation in the body (or pre-allocate a stack-local buffer for indexed access).  Lower `IR_INDEX_GET/SET` on sunk arrays to direct `[rsp+8*idx]` reads/writes.  At every guard's side-exit stub: emit a "materialize" trampoline that `cando_bridge_new_array` + populates from the buffer + writes the resulting handle into whatever slot would have held it -- so bytecode resuming sees the right state.  Update snapshots to know about sunk-allocation handles too. | -- | -- | yes | yes | nbody hot loop allocates 0 ranges per iter; modest -> big speedup depending on workload |

Step c-d-e-f open the door to many more recordable shapes; once
those are in (even with no codegen), the bench surface widens.
Step j+k is where the actual perf payoff lands -- before that, every
step is correctness-and-coverage groundwork.

Tracking notes will live in commit messages.  When all eleven are
shipped Phase 4.4 is done and Phase 4 is closed.

### Phase 5 — Optimiser (2 weeks)

- FOLD table + CSE + LOAD/STORE-forward + DSE + LICM +
  allocation-sinking, in that order. Each pass behind a debug toggle
  so we can A/B IR quality.

### Phase 6 — DynASM x64 backend (4 weeks)

- mcode arena (§8.2).
- Linear-scan register allocator over IR.
- Snapshot/restore exit stub.
- `jit_compile_trace()` end-to-end. Tests: every benchmark in
  Phase 0 must produce identical output between interpreter and JIT
  modes.

### Phase 7 — Side traces + trace linking (DEFERRED post-v1)

- Record side traces from hot exit stubs, link back into parent
  trace.
- Trace-tree shape (chains, no joins) — match LuaJIT.

**Status: deferred.**  Implementing this requires:
  1. Per-snapshot exit-hit counters in CandoTrace.
  2. Recorder armed-from-side-exit mode (start recording at the
     PC the snapshot replay landed on, not at OP_LOOP).
  3. Side trace gets its own mcode body with state matching the
     parent trace's snapshot at the exit point.
  4. Patch parent trace's exit stub to jump directly into the
     side trace's entry instead of running materialise + replay.

Each step is non-trivial.  The current v1 routes side-exits
through bytecode and re-enters the parent trace via `find_trace`
on the next OP_LOOP.  This is correct but pays full bytecode
cost during the "tail" of a side-exit-heavy loop.  Real-world
impact on the v1 workload mix is small (most loops have <5%
guard-fail rate).  Revisit when a benchmark demonstrably needs
it.

### Phase 8 — Stabilisation

**Status: DONE for v1.**

- ✅ Full benchmark sweep vs interpreter.  Mandelbrot ~10x,
  loops ~2x, fib/nbody marginal (recursive / few hot loops).
  Target ≥5x met for the mandelbrot-class numeric loop, not
  for global-variable-heavy loops -- those are bottlenecked on
  IR_GLOAD/IR_GSTORE hash lookups (no alias analysis yet).
- ✅ `--jit-dump` IR dumper for debugging (per-trace IR + per-op
  flags incl. [INV] [GUARD] [SUNK]).  mcode dumper deferred --
  use objdump on the trace body if needed.
- ✅ Stress tests: all 56 integration tests + 2149 JIT unit
  tests pass with `CANDO_JIT=1`.  4/4 benchmarks produce
  byte-identical output between JIT and bytecode modes.
  Valgrind clean (0 errors, 0 leaks) on case2 / sink_alias /
  jit_recorder / mandelbrot / loops.
- ✅ `--jit-dump` IR dumper extended with mcode hexdump and
  sink_recs listing.  Per-op flags now include `[GUARD] [INV]
  [PIN] [SUNK]`.
- Documentation: `docs/jit-plan.md` (this file) covers v1 design
  + decisions + deferred work.  User-facing `docs/jit.md` and
  contributor-facing `docs/jit-internals.md` deferred.

### Phase 8.1 — Outstanding optimisations (DEFERRED)

Items investigated and intentionally deferred during the post-v1
audit pass:

  - **OP_BREAK recorder support.**  Mandelbrot already gets ~8.6x
    so the remaining trace_aborts are low-value (the resulting
    traces would be one-shot, exiting at the BREAK guard).
    Skipped.
  - **OP_FOR_INIT recorder support.**  Required to JIT nbody's
    nested FOR loops.  Needs new IR ops to model the FOR-state
    initialisation (3-slot push + length compute) plus a runtime
    type guard on the iterable.  ~200-300 LOC; low ROI for v1
    workload mix.  Deferred.
  - **Global scalar promotion** (skip GLOAD's hash-lookup C call
    on iter 2+).  Implemented but reverted: the GLOAD's IRRef is
    captured by snapshots as the "pre-iter" value to restore on
    side-exit.  IRF_INVARIANT-skipping the GLOAD plus a GSTORE-
    side fixup leaves vals[gload_ref] holding the POST-GSTORE
    value at side-exit, defeating rollback semantics
    (Phase 4.1 div_rollback regression: iter-100 catch saw
    `committed=101` instead of 100).  A correct version needs
    a separate snapshot capture path for promoted GLOADs --
    deferred to a future pass.
  - **Phase 4.4 v1b range sinking.**  Would require IR_RANGE_*
    handling in `cg_assign_sunk_offsets`, but the dominant range
    use-case is `FOR x IN 1->N` which goes through OP_FOR_INIT
    (not yet recorded).  Standalone `VAR r = 1->10; r[5]` is
    rare.  Deferred.
  - **Audit LOW #5 / #8 / #10** (IRT_PTR consistency, const-cast
    on sunk-overflow, cur_snap=0 for early-trace side-exits).
    Stylistic / matches IR-interp behaviour; documented in code.

### Phase 9 — AArch64 backend (3 weeks, post-v1)

- Port `jit_asm_x64.dasc` to `jit_asm_arm64.dasc`.
- macOS Apple Silicon — `MAP_JIT` + write-protect dance.
- Reuse all of Phases 3–5 unchanged.

Total: **~22 weeks** for x64-only v1. Realistic with one engineer.

---

## 11. User interface

Three layers, ordered from "what a beginner types" to "what a JIT
hacker types".

### 11.1 CLI flags (`source/main.c`)

User-facing — short and obvious:

| Flag | Effect |
|---|---|
| `--jit` | enable the JIT (default in v2; opt-in in v1) |
| `--no-jit` | disable the JIT for this run |
| `--jit-stats` | print a one-line summary at exit: traces compiled, side exits, mcode bytes used |

Power-user — namespaced under `-X` so we never collide with user
flags, and so the surface can grow without polluting `--help`:

| Flag | Effect |
|---|---|
| `-Xjit-hotcount=N` | set the trace-trigger threshold (default 56) |
| `-Xjit-maxmcode=N` | mcode arena cap in MiB (default 64) |
| `-Xjit-maxtrace=N` | per-trace IR instruction cap (default 4096) |
| `-Xjit-force` | force-record on first backedge (test harness) |
| `-Xjit-dump=ir,mcode,exits` | dump compiled traces (debug builds) |
| `-Xjit-blacklist=PC` | prevent recording at a specific bytecode site |

`cando --help` lists only the four `--jit*` flags. The `-X*` flags
are documented in `docs/jit.md` for advanced users.

### 11.2 Environment variables

Mirror the CLI for embedders who don't go through `main.c`:

| Var | Equivalent |
|---|---|
| `CANDO_JIT=1` / `CANDO_JIT=0` | `--jit` / `--no-jit` |
| `CANDO_JIT_HOTCOUNT=N` | `-Xjit-hotcount=N` |
| `CANDO_JIT_DUMP=ir,mcode` | `-Xjit-dump=...` |

Precedence: explicit CLI flag > env var > built-in default.

### 11.3 Script-level API: the `jit` stdlib module

Modeled on the existing `gc` module (`source/lib/gc.c`), registered
by `cando_openlibs()` and exposed as a global table named `jit`.
Scripts can introspect and toggle the JIT on the fly — useful for
benchmarking, for hot-loading code that the JIT shouldn't touch, and
for self-tests.

Final API surface:

```cdo
// --- on/off -----------------------------------------------------
jit.on()                  // enable globally; same as --jit
jit.off()                 // disable globally; flushes existing traces
jit.toggle()              // flip current state, return new state
jit.status()              // → "on" | "off" | "unavailable"
jit.isAvailable()         // → TRUE if libcando was built with JIT

// --- scoped control --------------------------------------------
jit.with(FALSE, fn() {    // run fn with JIT temporarily off
    benchmarkInterpreter()
})

jit.noTrace(fn() {        // mark the function so it never records;
    debugSensitiveStuff() // calls inside still trace normally
})

// --- introspection ---------------------------------------------
jit.stats()               // → { traces, aborts, side_exits, mcode_used, mcode_cap }
jit.resetStats()
jit.tracesFor(fn)         // → array of trace IDs that cover fn

// --- tuning ----------------------------------------------------
jit.hotcount(56)          // set/get trigger threshold
jit.maxmcode(64)          // MiB cap on the mcode arena
jit.flush()               // throw away every compiled trace; counters reset

// --- diagnostics (debug builds only) ---------------------------
jit.dump("ir", traceId)   // → string IR listing
jit.dump("mcode", traceId)// → string disassembly
jit.onTrace(fn(traceId, kind) { ... })  // callback per trace event
```

Implementation sketch:

- New file `source/lib/jit.c` + header, exporting
  `cando_lib_jit_register(CandoVM *vm)` to match the pattern in
  `source/lib/gc.c`.
- The natives are thin wrappers around a new public C API in
  `source/jit/jit.h`:

  ```c
  CANDO_API bool cando_jit_is_available(void);          // build-flag
  CANDO_API bool cando_jit_is_enabled(CandoVM *vm);
  CANDO_API void cando_jit_set_enabled(CandoVM *vm, bool on);
  CANDO_API void cando_jit_flush(CandoVM *vm);
  CANDO_API void cando_jit_get_stats(CandoVM *vm, CandoJitStats *out);
  CANDO_API void cando_jit_reset_stats(CandoVM *vm);
  CANDO_API void cando_jit_set_hotcount(CandoVM *vm, u32 n);
  CANDO_API u32  cando_jit_get_hotcount(const CandoVM *vm);
  ```
- `cando_lib_jit_register` is added to `cando_openlibs()` in
  `source/cando_lib.c` so embedders get it for free.
- When CanDo is built with `-DCANDO_ENABLE_JIT=OFF`, the same module
  is still registered but every function returns
  `"unavailable"` / `FALSE` and is a no-op. Scripts written for
  the JIT-enabled build still load and run.

Concurrency: `jit.on()` / `jit.off()` mutate `vm->jit_enabled`
under the VM lock. Calling them from inside a JIT trace
side-exits cleanly because `jit.off()` is a `CALLN` to a
non-pure native, which always triggers a snapshot. `jit.flush()`
walks the chunk trace tables, drops the mcode arena, and continues
in the interpreter — currently-executing traces finish, future
calls re-enter the interpreter.

### 11.4 Build system

`CMakeLists.txt` and `Makefile` both gain:

```
-DCANDO_ENABLE_JIT=ON       (default OFF in phase 0–2, ON from phase 6)
```

DynASM preprocessing: a `dasc → h` step using LuaJIT's
`dynasm.lua`. We *do not* require Lua at runtime — only at build
time for contributors. Pre-generated `.h` files ship in the source
tree (`source/jit/gen/jit_asm_x64.h`) so end users build with plain
`make`. CI regenerates them and fails on drift.

Cross-compilation: the generated `.h` is host-arch-independent (it's
just C with embedded byte sequences), so cross-builds work normally.

When `CANDO_ENABLE_JIT=OFF`: `source/jit/` is excluded, the public
`cando_jit_*` API in `jit.h` provides stubs (so embedders link
unchanged), and the script-level `jit` module reports
`"unavailable"`.

---

## 12. Testing strategy

1. **Differential testing.** Every test in `tests/` runs under both
   `--no-jit` and `-Xjit-force` (force-trace every backedge once).
   Outputs must match byte-for-byte.
2. **Trace replay.** A debug build records every IR trace to disk;
   a separate harness loads the IR back and runs the
   IR-interpreter. Catches optimiser-vs-codegen divergences.
3. **Fuzzing.** Existing `tests/fuzz/` corpus runs under JIT for
   1 hour in CI on every PR.
4. **Stress.** A long-running scripts (`forms` event loops, the
   stdlib `http` server) for 30 minutes under JIT to shake out mcode
   leaks and lock interactions.
5. **Benchmarks.** `tests/bench/` runs nightly; results posted as
   PR comments by an existing CI job.

---

## 13. Risks and open questions

| Risk | Mitigation |
|---|---|
| NaN-box migration breaks every stdlib module at once | Two-step accessor migration (§4.2) |
| Per-object R/W locks defeat trace optimisation | Owner-thread guard at trace entry; cross-thread paths fall back to interpreter |
| Try/catch model can't be recorded | v1 simply doesn't record across try blocks |
| `OP_CLOSURE`'s variable-width tail complicates IR | Treat as opaque; sink whole closure if unused |
| DynASM is unmaintained upstream | Vendor a copy; we only need the preprocessor + a stable runtime |
| Apple Silicon `MAP_JIT` semantics | Isolated to `jit_mcode.c`; covered by macOS CI |
| GC handle invalidation | Generation counter at trace entry (§9.9) |
| Bytecode format must stay stable for serialised chunks | The JIT consumes bytecode; format is unchanged |

Open questions to resolve before Phase 1:

- Do we want NaN-boxing on 32-bit hosts? (LuaJIT splits the
  representation. Simpler answer: drop 32-bit support and document
  it. Currently CanDo claims to compile on 32-bit; check
  `tests/ci/` for actual 32-bit coverage before committing.)
- Should `CANDO_ENABLE_JIT` be on by default in release builds, or
  opt-in until the AArch64 backend lands? Recommendation: opt-in
  via env var `CANDO_JIT=1` for v1, default-on in v2.

---

## 14. Out of scope (intentionally)

- AOT compilation of `.cdo` to native binaries. The bytecode
  serialiser is a separate project.
- LLVM-based JIT. Considered and rejected (§2).
- Inline caches for the interpreter. The JIT replaces this concern.
- Rewriting the parser. The bytecode is the JIT's input; the parser
  doesn't change.
- WASM backend. Possible long-term, but the IR is designed
  register-machine-friendly so it's not foreclosed.

---

## 15. Success criteria for v1 merge

1. `--jit` matches `--no-jit` on every test in `tests/`, including
   the `forms` UI scripts.
2. Geomean ≥5× speedup on `tests/bench/` numeric loops, ≥2× on
   mixed scripts.
3. No measurable regression (>2%) when JIT is disabled.
4. mcode arena bounded at 64 MiB by default; flushable at runtime
   via `jit.flush()` from CanDo or `cando_jit_flush()` from C.
5. `jit.on()`, `jit.off()`, `jit.stats()`, and `jit.flush()` work
   from a script and round-trip through `--no-jit` builds as
   no-ops returning `"unavailable"`.
6. The new `docs/jit.md` documents the four user-facing CLI flags,
   the `jit` script module, and how to read `-Xjit-dump` output.

When all five hold, this plan file is deleted and replaced with the
real `docs/jit.md` + `docs/jit-internals.md` (matching the
"replace planning docs with full module documentation" pattern used
for the `forms` module).
