# `thread`

The `thread` library complements the language-level [`thread { … }`
expression and `await` operator](../language/threading.md).  It exposes
sleep, join, cancel, state queries, and per-thread completion callbacks.

Every per-thread function (`done`, `join`, `cancel`, `state`, `error`,
`then`, `catch`) is also reachable through the `_meta.thread` prototype
as a method on the thread receiver itself, so `t:done()` and
`thread.done(t)` are interchangeable.

## Reference

### `thread.sleep(ms)`

Block the current thread for `ms` milliseconds.

```cdo
thread.sleep(250);
```

### `thread.id() → number`

Numeric ID of the current thread.  Useful for diagnostics and logging.

### `thread.current() → thread | null`

Current thread handle, or `NULL` on the main thread.

### `thread.spawn(fn, ...args) → thread`

Spawn a new thread that calls `fn(...args)`.  Equivalent to
`thread fn(...args)` at the language level; use the function form when
the function is computed.

```cdo
VAR fn = workers[0];
VAR t = thread.spawn(fn, payload);
```

### `thread.done(t) → bool`

Has thread `t` finished, either with success or an error?

### `thread.state(t) → string`

One of `"pending"`, `"running"`, `"done"`, `"error"`, `"cancelled"`.

### `thread.join(t) → ...`

Block until `t` completes; return its return values.  Equivalent to
`await t`, but does **not** rethrow on error — instead returns `NULL`
and you can read `thread.error(t)` separately.

### `thread.cancel(t) → bool`

Request co-operative cancellation.  Returns `TRUE` if the request was
recorded, `FALSE` if `t` had already finished.  The thread itself must
poll `thread.current():state() == "cancelled"` to honour the request.

### `thread.error(t) → any`

The value passed to `THROW` inside `t`, if it errored.  `NULL` if `t`
finished cleanly or hasn't finished yet.

### `thread.then(t, fn)`

Register a success callback.  `fn` is called with `t`'s return values
once `t` finishes successfully.  Multiple callbacks may be registered;
they fire in registration order.  If `t` has already finished
successfully, `fn` is called immediately.

### `thread.catch(t, fn)`

Register an error callback.  `fn` is called with the thrown value when
`t` errors.

## Examples

### Fan-out with awaits

```cdo
VAR workers = inputs ~> thread compute(pipe);
VAR results = workers ~> await pipe;
```

### Background loop with cancellation

```cdo
VAR ticker = thread {
    WHILE thread.current():state() != "cancelled" {
        flush_metrics();
        thread.sleep(1000);
    }
};

// ...later
thread.cancel(ticker);
await ticker;
```

### Promise-style callbacks

```cdo
VAR t = thread fetch_remote_data();
t:then(FUNCTION(data) { print("got:", #data, "rows"); });
t:catch(FUNCTION(err)  { print("oh no:", err); });
```

### Manual error inspection (no rethrow)

```cdo
VAR t = thread might_fail();
thread.join(t);

IF t:state() == "error" {
    print("failed:", thread.error(t));
}
```

## See also

- [../language/threading.md](../language/threading.md) — the `thread {
  … }` expression, `await`, and the threading model.
- [stream.md](stream.md) — `stream.channel` for thread-safe queues.
- [object.md](object.md) — `object.lock` / `unlock` for mutexes.
