# Threading

CanDo threads are real OS threads.  The implementation wraps
`pthread_create` on POSIX and `CreateThread` on Windows.  Every
`thread { … }` expression spawns a thread that shares the parent VM's
global environment, handle table, and string intern table, but has its
own value stack, call frames, and upvalue list.

There are **no fibers** and no cooperative scheduler.  `thread.sleep`
yields the OS thread like `sleep(3)` does.

## Spawning

```cando
VAR t = thread {
    return 42;
};
```

The expression `thread EXPR` evaluates `EXPR` in a new thread.  `EXPR`
can be:

- A block: `thread { … }`.  Runs the body; the value of the last
  `RETURN` statement (or `NULL`) becomes the thread's result.
- A plain call: `thread f(a, b)`.  Captures the arguments, calls `f`,
  and returns its result.

The expression `thread …` itself evaluates to a **thread handle** — an
object of kind `OBJ_THREAD`.  You pass it to `await`,
`thread.join(t)`, etc.

## Joining — `await` and `thread.join`

```cando
VAR t = thread { return 1, 2, 3; };
VAR a, b, c = await t;                  // 1 2 3
```

`await t` blocks the current thread until `t` finishes and yields its
return values (multi-return is preserved).  `thread.join(t)` is a
callable synonym useful when you need to await dynamically.

If `t` threw an unhandled exception, `await t` re-throws the thrown
value in the current thread — catchable with `TRY … CATCH`.

## Callbacks — `then` and `catch`

```cando
VAR t = thread { return compute(); };

thread.then(t, FUNCTION(result) {
    print("ok:", result);
});

thread.catch(t, FUNCTION(err) {
    print("boom:", err);
});
```

- `thread.then` fires only on successful completion.
- `thread.catch` fires only on error.
- Both fire synchronously on the caller's thread if `t` has already
  finished by the time you register them.

Callbacks are stored inside the thread handle; a `t` can have multiple
`then` and `catch` callbacks and they fire in registration order.

## Introspection

| Call | Returns |
|---|---|
| `thread.state(t)` | `"pending"`, `"running"`, `"done"`, `"error"`, or `"cancelled"` |
| `thread.done(t)` | `TRUE` once state is `"done"` / `"error"` / `"cancelled"` |
| `thread.error(t)` | The value passed to `THROW`, or `NULL` |
| `thread.id()` | Current OS thread ID, as a number |
| `thread.current()` | The current thread's handle, or `NULL` on the main thread |

## Cancellation

`thread.cancel(t)` sets a cancel flag on `t`.  The thread body is not
interrupted mid-instruction; cancellation is co-operative.  A cancelled
thread:

- wakes from `thread.sleep` early
- reports state `"cancelled"`
- causes `await t` to raise an error

## Shared state and automatic locking

All heap objects carry a read/write lock (`CandoLockHeader`).  The VM
acquires these locks around field reads, field writes, array element
reads, array element writes, global loads, global stores, and upvalue
access.  This means plain `VAR x = shared.foo` and `shared.foo = 1`
from multiple threads are free of data races at the per-object level —
*but not* at a logical level.

Example of a race that auto-locking **does not** solve:

```cando
// two threads both run:
counter.n = counter.n + 1;
```

Here the read, the addition, and the write are three separate
instructions; another thread can interleave between them.  Use the
user-level mutex for these patterns:

```cando
object.lock(counter);
counter.n = counter.n + 1;
object.unlock(counter);
```

`object.lock` is re-entrant on the owning thread, so nested
`lock/unlock` pairs are safe.  `object.locked(obj)` reports whether the
lock is currently held by another thread (it's a hint, not a
synchronisation primitive).

### What is shared, and what isn't

Shared between parent and spawned threads:

- Global variables (guarded by a reader-writer lock)
- The handle table and every heap object reachable through it
- The string intern table
- Captured upvalues (guarded by per-upvalue reader-writer locks)

Not shared:

- Stack values
- Call frames
- Local variables
- Open-upvalue list

## Waiting at process exit

The root VM tracks every thread it spawns (directly or transitively) in
a registry.  When you call `cando_close(vm)`, the VM waits for all
still-running threads to finish before destroying state — this is what
`cando_vm_wait_all_threads()` does internally.  So your host program
does not need to join threads manually unless you want to finish early.

## Patterns

### Fan-out, fan-in

```cando
FUNCTION process(item) { … }

VAR items   = [ … ];
VAR workers = items ~> thread process(pipe);

// Gather results in the same order.
VAR results = workers ~> await pipe;
```

### Producer / consumer with a lock

```cando
VAR queue = { items: [], closed: FALSE };

VAR producer = thread {
    FOR i IN 1 -> 100 {
        object.lock(queue);
        queue.items:push(i);
        object.unlock(queue);
    }
    object.lock(queue);
    queue.closed = TRUE;
    object.unlock(queue);
};

VAR consumer = thread {
    WHILE TRUE {
        object.lock(queue);
        IF #queue.items == 0 {
            IF queue.closed { object.unlock(queue); BREAK; }
            object.unlock(queue);
            thread.sleep(1);
            CONTINUE;
        }
        VAR item = queue.items:pop();
        object.unlock(queue);
        handle(item);
    }
};

await producer;
await consumer;
```

### Firing an event without blocking

```cando
thread emit(event);      // fire and forget — no await
```

The thread registry keeps a reference, so the host program will still
wait for it at `cando_close()` time.

## Implementation notes

- Thread handles are `CdoObject`s with `kind = OBJ_THREAD`.  They carry
  the thread's `state`, `error`, `results` array, `then` / `catch`
  callback lists, and the underlying OS thread handle.
- A spawned thread runs in its own `CandoVM` initialised via
  `cando_vm_init_child`.  The child's `globals` and `handles` fields
  point to the parent's tables — they are **not** copied.
- `cando_current_thread()` uses thread-local storage to give native
  functions running inside a spawn access to the current handle.
- The thread trampoline (`cando_vm_exec_closure`) captures the thread
  body's return values into the thread handle before exiting; `await`
  reads them back out.
