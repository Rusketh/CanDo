# JIT Compiler

Cando ships a tracing JIT that compiles hot loops to native x86_64 code.
The JIT is opt-in for v1 (will become opt-out in a future release).

## Quick start

```sh
# Run with the JIT enabled
cando --jit script.cdo

# Show JIT statistics at exit
cando --jit-stats script.cdo

# Show generated IR + mcode for every compiled trace
cando --jit-dump script.cdo

# Force-disable the JIT (wins over --jit, --jit-stats, --jit-dump,
# and CANDO_JIT environment variable)
cando --no-jit script.cdo
```

`CANDO_JIT=1` in the environment is equivalent to `--jit` when no CLI flag overrides.

## What gets JIT-compiled

The JIT looks for **hot loops** -- backedges (`OP_LOOP`) that have run a
threshold number of times.  When a loop hits the threshold, the recorder
captures one iteration's body as SSA IR.  Several optimization passes
run, then the IR is compiled to native code.  Subsequent iterations
dispatch into the compiled body via `cando_trace_run`.

Supported in traces:
  - Numeric (`f64`) and object-handle (`u64`) values in frame slots
  - Numeric and array-typed globals (`IR_GLOAD` / `IR_GSTORE`)
  - Array indexing (`a[i]`, `a[i] = v`) -- inline read/write, no
    per-access lock
  - Object literals (`{ x: 1, y: 2 }`) -- with allocation sinking
    when the alloc never escapes the loop
  - Range iteration (`FOR i IN 0 -> N`)
  - Nested FOR loops (inner unrolled into the outer trace, with
    SNAP_INDEX heap rollback for safe mid-iter side-exits)
  - Native calls registered via `cando_vm_register_fast_native_f1`
    (e.g. `math.sqrt` -- inlined as a single `sqrtsd` instruction)
  - `BREAK` from a hot WHILE loop (treated as a side-exit anchor)

Not supported in v1 (recorder aborts, falls back to bytecode):
  - Function-entry tracing (so pure-recursion benchmarks like fib
    don't currently JIT -- see "deferred work" below)
  - Side traces (a hot side-exit doesn't trigger a sibling trace
    starting from that point -- side-exits unconditionally fall
    through to bytecode)
  - Most opcodes outside the v1 set (`OP_FOR_OVER_INIT`, complex
    string ops, etc.) -- the recorder enumerates a closed set
    rather than catching everything

## Performance expectations

Best-of-5 wall-clock against the bytecode interpreter on the four
benches in `tests/bench/`:

| Bench | no-JIT | JIT | Speedup | Notes |
|---|---:|---:|---:|---|
| `mandelbrot` | 200ms | 25ms | **8.0x** | The numeric-only inner-loop case; ideal for tracing |
| `loops` | 1326ms | 615ms | **2.15x** | Bottlenecked on `IR_GLOAD/GSTORE` for the `sum` global |
| `nbody` | 128ms | 70ms | **1.83x** | Limited by trace specialisation for varying inner length |
| `fib` | 619ms | 644ms | 0.96x | No backedges -> no traces (function-entry tracing deferred) |

JIT overhead: a few-millisecond startup cost from initial recording
+ codegen.  For sub-100ms scripts the JIT may pessimise; `--no-jit`
is a good default for short-running scripts.

## Stats output

`--jit-stats` prints a one-line summary at exit:

```
jit: backedges=11116 func_entries=1003 iter_next=11116
     trace_starts=4 traces_compiled=3 trace_aborts=1
     trace_iters=8929 trace_exits=7933
     hot_pcs=7 blacklisted=4 traces_evicted=0
     last_abort="loop inside inlined call (v1 limitation)"
```

  - `backedges` -- total `OP_LOOP` runs (= total bytecode loop iters)
  - `trace_starts` / `trace_aborts` / `traces_compiled` -- recording
    attempts and outcomes
  - `trace_iters` -- iterations executed via the JIT (mcode or IR-interp)
  - `trace_exits` -- side-exits back to bytecode
  - `hot_pcs` -- distinct PCs that crossed the hot threshold
  - `blacklisted` -- PCs the recorder gave up on
  - `last_abort` -- the most recent abort reason (debugging aid)

Useful ratios:
  - `trace_iters / backedges` -- fraction of loop iters that ran via
    JIT (closer to 1.0 = better)
  - `trace_iters / trace_exits` -- average iters per "session" before
    a side-exit (higher = less bytecode round-tripping)

## --jit-dump output

For each compiled trace, `--jit-dump` prints:
  - The trace's IR (one line per SSA op, with operand IRRefs and
    flag annotations: `[GUARD] [INV] [PIN] [SUNK]`)
  - The trace's compiled mcode as a hex listing (16 bytes/line,
    indexed from the mcode base -- can be cross-referenced with
    `objdump -D --target binary -m i386:x86-64` after extraction)
  - Any `sink_recs` (slot/stack-offset/capacity for sunk
    allocations whose materialisation deferred to side-exit)

Example for `loops.cdo`:

```
trace 1  start_pc=0x...
==== trace IR (15 instructions, 2 constants) ====
  k0    1
  k1    sum
  0001  num   IR_SLOAD         s3     -
  0002  num   IR_SLOAD         s2     -      [INV]
  0003  bool  IR_LT            #1     #2
  0004  bool  IR_GUARD_TRUE    #3     -      [GUARD]
  ...
==== trace mcode (4610 bytes @ 0x...) ====
  0000 55 48 89 e5 53 41 54 41 55 41 56 41 57 48 83 ec
  ...
```

## Trace lifecycle

1. Bytecode runs.  Each `OP_LOOP` calls `cando_jit_hot_hit`, which
   bumps a per-PC counter.
2. When the counter crosses the threshold (default 50), the PC is
   marked **hot** and **blacklisted** (so a recording-in-flight
   doesn't re-trigger).
3. The next time bytecode hits this PC, the recorder activates and
   observes each subsequent op until it sees the same PC again
   (loop closed) or a non-recordable op (recording aborts).
4. After successful recording: DSE / DCE / CSE / LICM / escape-
   analysis passes run, then x86_64 codegen.  If codegen succeeds,
   the trace is added to `vm->jit->traces[]` (capacity 64; LRU
   eviction).
5. On future hits at this PC, `cando_jit_find_traces` returns the
   matching traces; vm.c iterates them and runs the first one whose
   guards stay satisfied.  When all fail or no trace exists, fall
   back to bytecode.

## Multi-version trace specialisation

Multiple traces may share a `start_pc` -- each can specialise for
different runtime conditions (e.g. nbody's `FOR_INIT` length guard
captures the inner range's length as a constant; non-matching outer
iters side-exit at the guard with no heap mutations to roll back).

When all sibling traces for a PC consistently side-exit, the
unblacklist threshold (4 misses by default, capped at 4 spawn
attempts per PC) re-arms the hot counter so the next backedge
records a new sibling for whatever runtime state is current.

## Deferred work (post-v1)

These are documented in `docs/jit-plan.md` with full design notes:

  - **Function-entry tracing** -- would unlock pure-recursion
    benchmarks (fib) and let traces span function-call boundaries
    without inlining.  Requires a separate hot table for OP_CALL
    sites, a recorder mode that records function bodies, and
    codegen that handles entry/return calling-convention shuffles.
    ~500 LOC.

  - **Side traces + trace linking** -- when a side-exit at a
    specific guard fires repeatedly, record a sibling trace starting
    from that guard's snapshot exit-pc and patch the guard's stub
    to jump directly into the sibling.  Replaces the "side-exit ->
    bytecode -> re-enter" loop with native-to-native transitions.
    Multi-week work; the largest remaining win for branchy traces.

  - **Loop scalar promotion** for globals (separate snapshot
    capture) -- fully avoid the per-iter hash lookup AND function
    call.  ~30-40% on global-heavy loops.  Phase 8.7 ships the
    cache-pointer half of this; promotion proper is deferred
    pending a separate snapshot-source mechanism that doesn't
    conflict with rollback semantics (an earlier attempt regressed
    Phase 4.1's div_rollback test).
