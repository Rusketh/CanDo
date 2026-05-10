# `jit`

Runtime control of the tracing JIT.  By default the JIT is **off**;
enable it with the `--jit` CLI flag, the `CANDO_JIT=1` environment
variable, or `jit.on()` from a script.

The implementation overview is in
[../internals/jit.md](../internals/jit.md); this page is the script-side
control surface.

## Reference

### `jit.isAvailable() → bool`

`TRUE` when the build includes the JIT.  `FALSE` for builds that
disabled it at compile time.

### `jit.status() → string`

`"on"`, `"off"`, or `"unavailable"`.

### `jit.on()` / `jit.off()` / `jit.toggle()`

Enable, disable, or flip the JIT at runtime.  Already-recorded traces
are not discarded — they remain in the trace cache and continue to be
chosen when their entry conditions match.

```cdo
IF jit.isAvailable() AND benchmark_mode {
    jit.on();
}
```

### `jit.reset()`

Drop every recorded trace and clear hot-pc counters.  Subsequent
execution starts fresh.  Useful in tests to factor JIT warm-up out of a
measurement.

### `jit.stats() → object`

A snapshot of the JIT's profiling counters:

| Field                | Description |
|----------------------|-------------|
| `backedges`          | Number of loop back-edges executed. |
| `func_entries`       | Number of function-entry events. |
| `iter_next`          | Iterator advancement events. |
| `trace_starts`       | Trace recording attempts. |
| `traces_compiled`    | Successfully-compiled traces. |
| `trace_aborts`       | Recording aborts. |
| `trace_iters`        | Iterations executed inside compiled traces. |
| `trace_exits`        | Side-exits from compiled traces. |
| `hot_pcs`            | Distinct PC sites that hit the hot threshold. |
| `blacklisted_pcs`    | Sites that aborted enough to be blacklisted. |
| `traces_evicted`     | Traces dropped from the cache. |
| `last_abort`         | Reason string for the most recent abort. |

Same numbers as the `--jit-stats` summary line; calling `jit.stats()`
gives you the snapshot mid-run.

```cdo
VAR before = jit.stats();
hot_loop();
VAR after = jit.stats();
print(`compiled traces: ${after.traces_compiled - before.traces_compiled}`);
```

## Examples

### Conditionally enable JIT

```cdo
IF os.getenv("BENCH") == "1" {
    jit.on();
}
```

### Compare with and without JIT

```cdo
FUNCTION run(label) {
    VAR t0 = os.clock();
    workload();
    print(`${label}: ${os.clock() - t0}s`);
}

jit.off();  jit.reset();  run("interp");
jit.on();                  run("warmup");      // includes recording cost
                           run("hot");          // traces already cached
```

### Diagnose abort causes

```cdo
hot_loop();
VAR s = jit.stats();
print(`aborts=${s.trace_aborts} last=${s.last_abort}`);
```

`last_abort` is the human-readable reason string for the most recent
trace abort — typically `"guard_failed"`, `"unsupported_op"`, or
`"length_exceeded"`.  See [../internals/jit.md](../internals/jit.md).
