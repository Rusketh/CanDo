# `gc`

Garbage collector control.  CanDo's GC is a generational collector
that runs incrementally on heap pressure; in normal scripts you should
never need to touch it manually.

## Reference

### `gc.collect()`

Run a full GC cycle synchronously.  Returns when the cycle has
finished.

```cdo
gc.collect();
```

Useful in tests that want deterministic finalization, or after a known
spike in allocation when you want to keep memory usage tight.

### `gc.count() → number`

Approximate live-object count.  Unstable — useful as a sanity check or
for debugging memory leaks across iterations of a loop, not as a
precise measurement.

```cdo
print(gc.count());                 // some integer
```

### `gc.threshold(bytes*)`

With no argument, returns the current allocation threshold for an
automatic minor GC.  With one numeric argument, sets the threshold.
Setting it to `0` disables automatic GC (you must then call
`gc.collect()` yourself).

```cdo
print(gc.threshold());             // current value
gc.threshold(64 * 1024 * 1024);    // 64 MiB
```

## When to call this

Almost never.  The GC is tuned to be invisible to script code under
typical workloads.  Reach for `gc.collect()` only when:

- You're in a benchmark and want to factor out GC pauses.
- You've allocated a huge transient working set and want it reclaimed
  before the next phase begins.
- You're debugging a suspected leak and want to confirm whether
  particular objects are reachable.

For tight memory control, raising `gc.threshold()` on a long-running
process can reduce overall pause time at the cost of a higher peak
working set.

## Examples

### Periodic forced GC in a long-running server

```cdo
thread {
    WHILE TRUE {
        thread.sleep(60 * 1000);
        gc.collect();
    }
};
```

### Leak debug

```cdo
VAR before = gc.count();
do_one_request();
gc.collect();
VAR after = gc.count();
print(`delta = ${after - before}`);
```

A non-zero delta after collection suggests something the request
allocated is still reachable via a global, a closure, or a `_meta`
override.
