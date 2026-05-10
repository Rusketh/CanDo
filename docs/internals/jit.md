# JIT

CanDo's JIT is a **tracing JIT**.  It records linear sequences of
bytecode operations as the interpreter runs them, compiles those
traces to native machine code, and dispatches into the compiled
version on subsequent executions.  Side-exits return control to the
interpreter.

The JIT is **off by default**.  Enable it with `--jit`, the
`CANDO_JIT=1` environment variable, or `jit.on()` from a script.  Use
`--jit-stats` to see profiling counters, and `--jit-dump` to print the
IR of every compiled trace.

## Files

```
source/jit/
  recorder.c        trace recording
  ir.c              IR construction and optimization
  codegen.c         IR → native (x86-64 today)
  cache.c           trace cache and eviction
  jit.c, jit.h       public interface; the runtime knobs
```

## Lifecycle of a trace

```
   interpreter executing bytecode
              │
              │ on a back-edge / func entry / iter advance:
              ▼
       hot-pc counter++
              │
              ▼
     hot-pc threshold crossed
              │
              ▼
       start recording trace
              │
              │ each interpreter step is recorded as IR
              ▼
        emit guards on every speculation
              │
              ▼
   reach back-edge / outer loop / hot tail call
              │
              ▼
        compile to native code
              │
              ▼
     install native entry at the loop header
              │
              │ subsequent executions enter the native trace
              ▼
       run until a guard fails (side-exit)
              │
              ▼
       return to the interpreter
```

## Hot-path detection

The recorder watches three event classes:

| Event           | Source                          |
|-----------------|----------------------------------|
| Back-edges      | `OP_LOOP` returning to an earlier `OP_LOOP_MARK` |
| Function entry  | `OP_CALL` / `OP_METHOD_CALL` |
| Iterator next   | `OP_FOR_NEXT`, `OP_FOR_OVER_NEXT`, `OP_PIPE_NEXT` |

A per-PC counter tracks how often each site fires.  When the counter
crosses the hot threshold, the next execution at that PC starts trace
recording.

## Recording

While recording, every interpreter operation is mirrored into IR
nodes:

- Numeric ops become typed IR nodes (`add`, `sub`, `mul`, …).
- Field reads become `load_field` with a guard on the receiver's hidden
  shape.
- Method calls become `call` IR with guards on the target.
- Stack and local activity becomes IR-level value tracking.

Every speculation (assumed type, assumed shape, assumed branch
direction) becomes an IR `guard` node.  If the guard would fail at
run-time, the native trace bails to a side-exit that re-enters the
interpreter at the right PC with the right state.

Recording aborts when:

- An unsupported opcode is encountered.
- The trace exceeds a length limit.
- A guard would have to be very expensive (e.g. a varargs spread of
  unknown length).
- The recorder detects a non-fall-through jump that would split the
  trace.

The most recent abort reason is reported as `last_abort` in
`jit.stats()`.

## Compilation

Once the trace closes (typically by reaching a back-edge or returning
to the outer loop), the IR is:

1. Type-specialized — most numeric ops are concrete `f64` after the
   recorder's guards have proven the operand types.
2. Optimized — common subexpression elimination, dead code, guard
   hoisting.
3. Lowered to native — register allocation and instruction emission.

The native code lives in an executable region inside the trace cache
and is keyed by the entry PC.

## Side-exits

Each guard in the trace points at a small bail-out stub that:

- Spills the IR-tracked values back into VM-visible state.
- Sets `ip` to the right interpreter PC.
- Returns to the dispatch loop.

The interpreter resumes as if the trace had never run.  After
many guard failures at the same exit, the trace is **blacklisted**:
the recorder won't try again at that PC for the rest of the run.

## Trace cache

Compiled traces share a fixed-size native code region.  When it fills,
the **least-recently-used** trace is evicted.  Evicted traces leave
their PC counters intact, so a subsequent re-recording is possible
once the trace cache has room again.

## Counters

`jit.stats()` and `--jit-stats` expose the same numbers:

| Field               | Description |
|---------------------|-------------|
| `backedges`         | Loop back-edges executed (interpreted + traced). |
| `func_entries`      | Function entries recorded by the recorder. |
| `iter_next`         | Iterator advancement events. |
| `trace_starts`      | Trace recording attempts. |
| `traces_compiled`   | Successfully compiled traces. |
| `trace_aborts`      | Recording aborts. |
| `trace_iters`       | Iterations executed inside compiled traces. |
| `trace_exits`       | Side-exits from compiled traces. |
| `hot_pcs`           | Distinct PCs that hit the hot threshold. |
| `blacklisted_pcs`   | PCs blacklisted after repeated aborts. |
| `traces_evicted`    | Traces dropped from the cache. |
| `last_abort`        | Last abort's reason string. |

## Adding a new opcode to the JIT

1. Decide if the opcode is JITable.  Pure arithmetic, pure stack
   moves, and most field/index reads are easy.  Calls into native
   functions, throws, GC-triggering allocations, and anything that
   manipulates thread state are typically not.
2. If JITable, add a recorder case in `source/jit/recorder.c` that
   builds the corresponding IR.
3. Add a lowering case in `source/jit/codegen.c`.
4. If not JITable, mark the opcode as an abort point so traces stop
   cleanly.

## Tuning thresholds

The hot threshold and trace length cap are compile-time constants in
`source/jit/jit.h`.  For benchmarking, lowering the threshold makes
recording start sooner; raising the trace cap can let bigger loops
trace as a single unit at the cost of more compile time.

## Reading the dump

`--jit-dump` prints each compiled trace's IR.  The output is grouped
by trace ID with an entry-PC line, then one IR node per line in
emission order, with type tags and guard targets.  Use this when a
trace seems to be doing the right thing on paper but performance
suggests otherwise.

## Status

The JIT covers the common arithmetic / stack / field hot paths and
typical numeric loops.  Coverage of class-method dispatch, regex, and
the streaming layer is intentionally limited — the trace recorder
aborts cleanly on these so correctness is unaffected.
