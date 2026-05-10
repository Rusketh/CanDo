# JIT Internals

Contributor-facing reference for the JIT compiler.  Pairs with
[docs/jit.md](jit.md) (user-facing) and [docs/jit-plan.md](jit-plan.md)
(historical design notes / deferred phases).

## Pipeline

```
bytecode -> hot detection -> recorder (-> abort) -> IR
       (cando_jit_hot_hit)  (cando_recorder_observe)
                              |
                              v
                       passes: DSE -> DCE -> CSE -> LICM -> escape
                       analysis -> mark_promoted_globals
                              |
                              v
                       codegen (cando_jit_codegen_trace)
                              |
                              v
                  trace cache (vm->jit->traces[])
                              |
                              v
       cando_jit_find_traces / cando_trace_run dispatch
                  (mcode_fn or IR-interp fallback)
                              |
                              v
                       side-exit -> snapshot replay -> bytecode
```

## Files

| File | Purpose |
|---|---|
| `source/jit/ir.h` | `IRIns` (SSA instruction, 12 bytes), `IROp` enum, flag bits (`IRF_GUARD/INVARIANT/PINNED/SUNK`) |
| `source/jit/ir.c` | Constant pool dedup, IR dump (`cando_ir_dump`), op name table |
| `source/jit/jit.h` | `CandoTrace`, `CandoSnapshot`, `CandoSnapEntry`, `CandoRecorder`, public API |
| `source/jit/jit.c` | Recorder, all IR passes, IR-interpreter, runtime helpers |
| `source/jit/codegen.c` | x86_64 SysV codegen: emit primitives, per-IR-op emitters, trace prologue/epilogue, side-exit common stub |
| `source/jit/hot.c/.h` | Per-PC hot counter table |
| `source/jit/mcode.c/.h` | Page-aligned executable mapping (mmap RW -> RX) |

## Key data structures

### `IRIns` (ir.h)

```c
typedef struct IRIns {
    u8  op;     /* IROp */
    u8  type;   /* IRType: NUM/BOOL/STR/OBJ/PTR/VOID */
    u8  flags;  /* IRF_GUARD | IRF_INVARIANT | IRF_PINNED | IRF_SUNK */
    u8  reserved;
    u32 op1;    /* IRRef OR raw integer (slot, KIDX) -- see ir_op_uses_irref */
    u32 op2;    /* IRRef OR snap idx (for guards) */
} IRIns;
```

`IRRef` is a `u32` index into `ir->ir[]`.  When the high bit
(`IRREF_KFLAG`) is set, the value is a constant-pool index into
`ir->constants[]` instead.  Always check `IRREF_IS_K()` before
treating as an op-index.

### `CandoSnapEntry` (jit.h)

```c
typedef struct CandoSnapEntry {
    u8    kind;    /* SNAP_SLOT | SNAP_GLOBAL | SNAP_INDEX */
    u32   key;     /* slot / kidx / array-IRRef */
    IRRef irref;   /* pre-iter value's IRRef (vals[irref].d) */
    IRRef irref2;  /* SNAP_INDEX: idx-IRRef.  Else unused. */
} CandoSnapEntry;
```

Built from `CandoRecorder.pending_snap` at each guard emit.  Side-
exit replay walks the entries and writes back:
  - `SNAP_SLOT`: `frame_slots[key] = cando_number(vals[irref].d)`
  - `SNAP_GLOBAL`: `vm->globals[name(key)] = cando_number(vals[irref].d)`
  - `SNAP_INDEX`: `arr(key)->items[idx(vals[irref2].d)] = cando_number(vals[irref].d)`

### `CandoTrace` (jit.h)

Holds compiled trace state: IR, snapshots, mcode, sunk shadow buffer,
gload cache.  See the field comments for details.

## Op classification cheatsheet

| Op family | IRRef? op1 | IRRef? op2 |
|---|---|---|
| `IR_KNUM/KSTR/KOBJ/KBOOL/KNULL` | no (raw / kidx) | no |
| `IR_SLOAD/HLOAD_SLOT` | no (slot) | no |
| `IR_SSTORE` | no (slot) | yes |
| `IR_GLOAD` | no (KIDX) | no |
| `IR_GSTORE` | no (KIDX) | yes |
| `IR_CALL_F1` | no (native idx) | yes |
| `IR_HLEN` | yes | no |
| All others (default) | yes | yes |

`ir_op_uses_irref` (jit.c) is the canonical classifier.  CSE / DCE /
LICM all rely on it.

## Codegen ABI

Trace function signature:

```c
CandoTraceStatus mcode_fn(CandoVM *vm, CandoTrace *t, bool skip_inv,
                           CandoValue *frame_slots, TraceVal *vals);
```

Register assignments after prologue:
| Reg | Holds |
|---|---|
| `rbx` | `vm` |
| `r12` | `t` |
| `r13b` | `skip_inv` (LICM flag) |
| `r14` | `vals` (scratch table) |
| `r15` | `frame_slots` |

Stack layout after prologue:
```
[rbp + 8]            return addr
[rbp]                saved rbp
[rbp - 8]            saved rbx
...                  saved r12-r15
[rbp - 48]           sub rsp +8 alignment pad
[rbp - 56]           sunk-allocation slot 0 (first sunk's element 0)
[rbp - 56 - 8N]      sunk-allocation slot N
```

Side-exit common stub (one per trace, at end of mcode):
1. Save snap_idx in `r13` (across the materialise call).
2. If trace has sunk allocations (`sink_count > 0`), call
   `cando_jit_materialize_sunk_for_mcode(vm, t, rbp, frame_slots)`.
3. Call `cando_jit_replay_snapshot_for_mcode(vm, t, vals, frame_slots, snap_idx)`.
4. Return `TRACE_GUARD_FAILED`.

## Pass order

```c
eliminate_dead_stores(&r->ir);      // SSTORE dedup (last write wins)
eliminate_dead_code(&r->ir);        // backward liveness sweep
common_subexpression_elimination(&r->ir);  // value numbering (Phase 8.5)
eliminate_dead_code(&r->ir);        // re-DCE after CSE
mark_loop_invariants(&r->ir);       // sets IRF_INVARIANT
escape_analysis(&r->ir);            // sets IRF_SUNK for non-escaping NEW_*
```

The order matters:
  - DSE before DCE so a NOPped store doesn't keep its value op alive.
  - CSE between DCE passes so deduplicated ops are visible to a
    second-pass DCE.
  - LICM after CSE so invariance is computed on the deduped IR.
  - Escape analysis last so it sees NOPped stores and can mark more
    allocations sinkable.

## IR phases (chronological)

| Phase | What landed |
|---|---|
| 3 | Initial recorder + IR-interpreter |
| 4 | Snapshots / SSTORE rollback / nested-call inlining |
| 4.4 | Array/object/range allocation; `IR_NEW_ARRAY/OBJECT/RANGE_*`, sinking |
| 5 | DSE, DCE, LICM |
| 6 | x86_64 codegen for the v1 op set |
| 8.2 | Inline `INDEX_GET/SET` + `IRT_OBJ` GLOAD codegen (nbody +50%) |
| 8.3 | `OP_FOR_INIT` recorder + `IR_HLEN` + `OP_FOR_NEXT` EXIT-path |
| 8.4 | `SNAP_INDEX` heap rollback for `IR_INDEX_SET` |
| 8.5 | Common subexpression elimination |
| 8.6 | Multi-version trace specialisation (sibling traces per PC) |
| 8.7 | Per-trace global-entry pointer cache |
| 8.8 | `OP_BREAK` recorder support |

## Adding a new IR op

1. Add the enum entry in `source/jit/ir.h` (in the right band).
2. Add the name in `source/jit/ir.c` op-name table.
3. If op1/op2 aren't both real IRRefs, update `ir_op_uses_irref`
   in `source/jit/jit.c`.
4. Implement in the IR-interpreter (`cando_trace_run`'s op switch
   in `source/jit/jit.c`).
5. Update `mark_loop_invariants` if the op can be invariant.
6. Update `escape_analysis` if the op consumes container IRRefs.
7. Update `eliminate_dead_code` (already handled if classifier is
   correct).
8. Update `common_subexpression_elimination` if the op is pure
   (add to `ir_op_is_pure_for_cse`).
9. Add codegen (`source/jit/codegen.c`).
10. If the op needs a runtime helper, declare and define it in
    `jit.c` with the `cando_jit_*_for_mcode` naming.

## Adding a new snap kind

1. Add to `CandoSnapKind` in `source/jit/jit.h`.
2. Update `rec_pending_snap_add` (or add a kind-specific variant
   like `rec_pending_snap_add_index` for SNAP_INDEX).
3. Handle the new kind in `trace_replay_snapshot` (`source/jit/jit.c`).
4. Update analysis passes if the new kind has unusual operand
   semantics.

## Common gotchas

  - **`vals[]` lifetime**: `vals[]` is a per-trace scratch table
    reused across `cando_trace_run` calls.  An IR op writes to
    `vals[i]` where `i` is its IRRef.  IRF_INVARIANT ops only
    write on iter 1; subsequent iters reuse `vals[i]`.

  - **Const-pool refs**: `IR_GLOAD/GSTORE.op1` is a kidx (const
    index), NOT an op-index.  The high bit (`IRREF_KFLAG`) is set.
    `IRREF_KIDX(op1)` extracts the kidx; `IRREF_IS_K(op1)` tests.

  - **`stored_slot` / `stored_name`**: arrays in `mark_loop_invariants`
    that gate SLOAD/GLOAD invariance on absence of any store to
    the same slot/name in the trace.  Adding new write ops requires
    updating these gates.

  - **PINNED ops**: kept alive by DCE even when no IR consumer
    references them.  Used for SET_VAL pair-prefixes (consumed by
    the next op via `i-1` convention) and Phase-8.4 pre-value GETs
    (referenced only by SNAP_INDEX entries, which DCE doesn't track).

  - **Mid-iter side-exits + heap mutations**: any op that mutates
    heap state (IR_INDEX_SET, IR_FIELD_SET, IR_ARRAY_APPEND) needs
    a corresponding rollback entry in the snapshot mechanism.
    SNAP_INDEX (Phase 8.4) handles INDEX_SET; the others would
    need analogous SNAP_FIELD / SNAP_APPEND if they ever land in
    a code path with mid-iter side-exits.

  - **Multi-version siblings**: `find_traces` returns up to 3
    matching traces at a `start_pc`, sorted by `last_used` DESC.
    vm.c iterates them; the first to LOOP_DONE wins.  When all
    fail consistently, `total_dispatch_misses` accumulates on the
    oldest sibling and at thresholds (64/128/256/512) the PC is
    un-blacklisted to allow a new sibling to be recorded.

## Testing

  - `make test` -- 2149 unit + 56 integration tests (no JIT).
  - `CANDO_JIT=1 make test` -- same suites with JIT enabled.
  - `tests/scripts/jit_recorder.cdo` -- end-to-end coverage of
    every recorder feature (sinking, snap rollback, multi-iter
    behaviour).  Add a print-line per new feature so the integration
    test's expected output catches regressions.
  - `valgrind --leak-check=full ./cando script.cdo` -- the JIT
    must be valgrind-clean (no uninitialised reads via
    materialise-on-side-exit).
