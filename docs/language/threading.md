# Threading

CanDo's runtime is built around **real OS threads**.  There is no global
interpreter lock: every thread executes script code in parallel, with
the GC and the value system designed from the start to make that safe.

The language gives you two built-in forms — the `thread { … }` expression
and the `await` operator — and a `thread` standard-library namespace
covering the rest of the surface (sleep, join, cancel, query state).

This page covers the language-level forms and the threading model.  For
the per-function reference see
[../libraries/thread.md](../libraries/thread.md); for thread-safe
data-passing see [../libraries/stream.md](../libraries/stream.md)
(channels) and [../libraries/object.md](../libraries/object.md)
(`object.lock` / `unlock`).

## `thread { … }` and `thread expr`

Spawning takes either a block:

```cdo
VAR t = thread {
    thread.sleep(50);
    RETURN "done";
};
```

…or a plain call expression:

```cdo
FUNCTION compute(x) { RETURN x * x; }
VAR t = thread compute(7);
```

In both forms the spawn is **non-blocking**: the calling thread keeps
executing immediately, while a fresh OS thread runs the body.  The
expression evaluates to a **thread handle** — an object you can pass
around, store in arrays, await, or query.

```cdo
VAR workers = [];
FOR i IN 1 -> 8 {
    workers:push(thread { RETURN i * i; });
}

FOR t OF workers {
    print(await t);
}
```

A thread shares the spawning thread's **globals** but gets its own
**locals** and call stack.  It does *not* see the parent's `VAR` bindings
unless they were captured into the function value being called.

## `await`

`await t` blocks the current thread until `t` finishes, then returns
whatever `t` returned (potentially a multi-value result):

```cdo
VAR t = thread { RETURN 1, 2, 3; };
VAR a, b, c = await t;       // a=1, b=2, c=3
```

If `t` threw, `await` rethrows the same value at the awaiting site, so
errors propagate the way they would for a synchronous call:

```cdo
TRY {
    await thread { THROW "oh no"; };
} CATCH (msg) {
    print(msg);              // oh no
}
```

`await` accepts any expression that evaluates to a thread handle.  Other
values produce a runtime error.

## Cancellation

Threads can request **co-operative** cancellation through
`thread.cancel(t)`:

```cdo
VAR t = thread {
    WHILE TRUE {
        IF thread.current():state() == "cancelled" { RETURN; }
        do_some_work();
    }
};

thread.sleep(100);
thread.cancel(t);
await t;
```

Cancellation is *advisory* — the script must check for it.  Long-running
operations in the standard library (file I/O, `:recvAll`, etc.) do not
abort automatically when the thread is cancelled.

## State machine

A thread handle is always in one of five states:

| State        | Meaning                                       |
|--------------|-----------------------------------------------|
| `pending`    | Spawned but not yet started.                  |
| `running`    | Actively executing.                           |
| `done`       | Returned cleanly.  `t:join()` returns the value(s). |
| `error`      | Threw an uncaught error.  `t:error()` returns the thrown value. |
| `cancelled`  | Cancellation was acknowledged.                |

Inspect with `t:state()`:

```cdo
print(t:state());            // running | done | error | cancelled
print(t:done());             // bool — has it finished, success or error?
```

## Sharing mutable state safely

The value system is thread-safe (handles, GC, intern tables) but
**user-level mutation is not automatically synchronized**.  Two threads
appending to the same array can race and lose updates.

Two options:

### `object.lock` / `object.unlock`

```cdo
VAR shared = { hits: 0 };

FUNCTION bump() {
    object.lock(shared);
    TRY {
        shared.hits = shared.hits + 1;
    } FINALY {
        object.unlock(shared);
    }
}

VAR ts = [];
FOR i IN 1 -> 10 { ts:push(thread bump()); }
FOR t OF ts { await t; }
print(shared.hits);          // 10
```

The lock is **re-entrant**: the same thread can `lock` a second time
without deadlocking, as long as it `unlock`s the same number of times.

### `stream.channel` (recommended)

Channels are bounded queues with blocking reads and writes.  This
avoids most of the pitfalls of shared-state synchronization.

```cdo
VAR ch = stream.channel(100);

VAR producers = [];
FOR i IN 1 -> 4 {
    producers:push(thread {
        FOR j IN 1 -> 25 { ch:write(`p${i}:${j}`); }
    });
}

VAR consumer = thread {
    FOR n IN 1 -> 100 {
        VAR msg = ch:read(64);
        print(msg);
    }
};

FOR t OF producers { await t; }
await consumer;
```

See [../libraries/stream.md](../libraries/stream.md) for the full
channel API.

## Limits

| Limit | Default | Source |
|---|---|---|
| Maximum threads | platform-dependent | `pthread_create` rejection or Win32 `_beginthreadex` |
| Per-thread call stack | 256 frames | `CANDO_FRAMES_MAX` |
| Per-thread value stack | 2048 slots | `CANDO_STACK_MAX` |

The runtime does not pool worker threads automatically — `thread { … }`
creates a fresh OS thread every time.  For high-throughput scenarios
where you'd otherwise spawn millions of short-lived threads, use a
small pool of long-running workers fed by a `stream.channel`.

## Common patterns

### Fan-out / fan-in

```cdo
VAR results = inputs ~> thread compute(pipe);   // array of thread handles
results = results ~> await pipe;                 // array of resolved values
```

### Worker pool

```cdo
VAR jobs = stream.channel(0);

VAR workers = [];
FOR i IN 1 -> 4 {
    workers:push(thread {
        WHILE TRUE {
            VAR job = jobs:read(4096);
            IF job == "" { RETURN; }
            handle(json.parse(job));
        }
    });
}

FOR job OF queue { jobs:write(json.stringify(job)); }
jobs:end();
FOR w OF workers { await w; }
```

### Periodic background work

```cdo
VAR ticker = thread {
    WHILE thread.current():state() != "cancelled" {
        flush_metrics();
        thread.sleep(1000);
    }
};

// ...later, on shutdown
thread.cancel(ticker);
await ticker;
```
