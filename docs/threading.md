# Cando Threading

Cando supports true OS-level concurrency.  Every `thread` expression spawns a
real OS thread (pthreads on Linux/macOS, Win32 on Windows) and returns a
*thread handle* that can be awaited, polled, joined, or cancelled.

---

## Table of Contents

1. [The `thread` expression](#the-thread-expression)
2. [The `await` expression](#the-await-expression)
3. [Thread handles](#thread-handles)
4. [The `thread` standard library](#the-thread-standard-library)
5. [Shared state and auto-locking](#shared-state-and-auto-locking)
6. [Error propagation](#error-propagation)
7. [Cancellation](#cancellation)
8. [Patterns and examples](#patterns-and-examples)
9. [Implementation notes](#implementation-notes)

---

## The `thread` expression

`thread` is a unary prefix operator with the lowest binding strength.  It
accepts any expression or block:

```cando
// Block form
var t = thread {
    var x = heavyCompute();
    return x;
};

// Expression form — any expression works
var t2 = thread file.read("big.csv");
var t3 = thread add(a, b);
var t4 = thread obj.method();
```

Internally the compiler wraps the operand in an implicit anonymous closure
(using the same jump-over pattern as named functions) and emits `OP_CLOSURE`
followed by `OP_THREAD`.  `OP_THREAD` allocates a `CdoThread` object, stores
it in the handle table, spawns an OS thread, and pushes the thread handle onto
the value stack.

The spawned closure captures the same upvalues it would capture if called
directly — all variables in scope at the point of the `thread` expression are
accessible from the thread body.  Global variables are also shared.

---

## The `await` expression

`await` blocks the calling thread until the target thread finishes and then
unpacks its return values:

```cando
var result        = await t;    // single value
var a, b          = await t2;   // multi-return
var x, y, z       = await t3;   // up to 8 values
```

If the thread returns no values, `await` produces a single `null`.

`await` is compatible with multi-return assignment — it sets `last_ret_count`
so the same spread/mask machinery used for regular function calls applies:

```cando
function minmax(a, b) { return a < b ? a, b : b, a; }

var t = thread minmax(7, 3);
var lo, hi = await t;    // lo = 3, hi = 7
```

---

## Thread handles

A thread handle is an opaque value of kind `OBJ_THREAD`.  It can be stored in
variables, passed to functions, placed in arrays, and used as an argument to
`thread.done`, `thread.join`, and `thread.cancel`.

```cando
var handles = [];
for i of 1 -> 10 {
    handles[i - 1] = thread { return i * i; };
}

for i of 0 -> 9 {
    print(await handles[i]);   // prints squares in order
}
```

Thread handles are garbage-collected once no script variable holds a reference
to them and the OS thread has exited.

---

## The `thread` standard library

The global `thread` object exposes ten management functions.

### `thread.sleep(ms)`

Suspends the **calling** thread for `ms` milliseconds.  Non-integer values
are truncated; negative values are treated as zero.

```cando
thread.sleep(500);   // pause 500 ms
```

### `thread.id()`

Returns a non-zero numeric identifier for the **calling** thread.  The value
is stable for the lifetime of the thread but is not guaranteed to be
sequential.

```cando
print("my id: " + thread.id());
```

### `thread.done(t)`

Non-blocking poll.  Returns `true` if thread `t` has finished (state is
`DONE`, `ERROR`, or `CANCELLED`), `false` otherwise.

```cando
var t = thread { thread.sleep(200); return 1; };
while !thread.done(t) {
    thread.sleep(10);
}
var r = await t;
```

### `thread.join(t)`

Identical to `await t` — blocks until `t` finishes and returns its results.
Useful when the handle is already in a variable and you prefer the library
style over the keyword:

```cando
var r = thread.join(t);
```

If the thread ended with an error, `thread.join` returns `null`.

### `thread.cancel(t)`

Attempts a best-effort cancellation.  Sets the thread's state to `CANCELLED`
if it is currently `PENDING` or `RUNNING`, then wakes any thread blocked in
`await` or `thread.join` so it can observe the new state.  Returns `true` if
the state was changed, `false` if the thread had already finished.

**Important:** `cancel` is a cooperative signal.  It does not interrupt
running code; the thread body is not notified and will continue until it
naturally exits.  The cancellation is reflected immediately in `thread.done`
and causes `await`/`thread.join` to unblock and return `null`.

```cando
var t = thread { thread.sleep(60000); return 0; };
if thread.cancel(t) {
    print("cancelled");
}
```

### `thread.state(t)`

Returns the lifecycle state of thread `t` as a string.

| Return value  | Meaning                                       |
|---------------|-----------------------------------------------|
| `"pending"`   | Created, OS thread not yet started            |
| `"running"`   | OS thread is executing                        |
| `"done"`      | Finished normally; results are available      |
| `"error"`     | Finished with an unhandled error              |
| `"cancelled"` | Cancelled before or during execution          |
| `"null"`      | Argument is not a valid thread handle         |

```cando
var t = thread { return 1; };
print(thread.state(t));  // "pending" or "running"
await t;
print(thread.state(t));  // "done"
```

### `thread.error(t)`

Returns the error value stored on thread `t` if its state is `"error"`,
or `null` otherwise.

```cando
var t = thread { throw "something went wrong"; };
await t;
if thread.state(t) == "error" {
    print(thread.error(t));   // something went wrong
}
```

### `thread.current()`

Returns the thread handle for the currently executing Cando thread.
Returns `null` when called from the main (non-spawned) thread.

```cando
var t = thread {
    var me = thread.current();
    print(thread.state(me));  // "running"
    return 1;
};
await t;
print(thread.current());  // null  (main thread)
```

### `thread.then(t, fn)`

Registers `fn` as a success callback.  When thread `t` finishes with
state `DONE`, `fn` is called with `t`'s return values as its arguments.

- If `t` is already `DONE` when `then` is called, `fn` fires immediately
  (synchronously, re-entering the VM).
- If `t` errors or is cancelled, `fn` is never called.
- Each thread supports at most one `then` callback; calling `then` a
  second time replaces the first.

```cando
var t = thread { return 42; };
thread.then(t, function(result) {
    print("got: " + result);   // got: 42
});
await t;
```

Multi-return threads pass all values to the callback:

```cando
var t = thread { return 1, 2, 3; };
thread.then(t, function(a, b, c) {
    print(a + b + c);   // 6
});
await t;
```

### `thread.catch(t, fn)`

Registers `fn` as an error callback.  When thread `t` finishes with
state `ERROR`, `fn` is called with the error value as its argument.

- If `t` has already errored when `catch` is called, `fn` fires immediately.
- If `t` succeeds or is cancelled, `fn` is never called.
- Each thread supports at most one `catch` callback.

```cando
var t = thread { throw "oops"; };
thread.catch(t, function(err) {
    print("caught: " + err);   // caught: oops
});
await t;
```

Together, `then` and `catch` provide a promise-like pattern:

```cando
var t = thread { return heavyCompute(); };
thread.then(t,  function(r)   { print("ok: "  + r); });
thread.catch(t, function(err) { print("err: " + err); });
// ... do other work while t runs ...
await t;
```

---

## Shared state and auto-locking

Every Cando heap object — objects, arrays, strings, upvalues, and globals —
carries a `CandoLockHeader` (16 bytes at offset 0).  The header holds an
atomic exclusive-writer ID and an atomic shared-reader count.

The VM automatically acquires the appropriate lock for each operation:

| Operation                    | Lock type  |
|------------------------------|------------|
| `OP_LOAD_GLOBAL`             | Read lock  |
| `OP_STORE_GLOBAL`            | Write lock |
| `OP_DEF_GLOBAL`              | Write lock |
| `OP_LOAD_UPVAL`              | Read lock  |
| `OP_STORE_UPVAL`             | Write lock |
| `CdoObject` field read       | Read lock  |
| `CdoObject` field write      | Write lock |
| `CdoObject` array element r/w | Read/write |

Locking is re-entrant for writes: the same thread can acquire the write lock
multiple times without deadlocking.

The lock is a spinlock — suitable for brief, low-contention critical sections.
For long-running computations on shared state, design your code to copy data
into the thread body rather than repeatedly accessing a shared object.

---

## Error propagation

If a thread body throws an uncaught error, the thread transitions to the
`ERROR` state and the error value is stored on the thread handle.

`await` on an errored thread does **not** re-throw the error into the calling
thread — it returns `null` instead.  Inspect the thread state with
`thread.done` and `thread.join` before deciding how to handle it.  A future
version may expose the error value directly.

---

## Cancellation

`thread.cancel(t)` sets the thread state to `CANCELLED` and broadcasts on the
internal condition variable so any thread blocked in `await`/`thread.join`
wakes up and receives `null`.

The running thread body is **not** interrupted.  If you need cooperative
cancellation inside the thread, poll a shared flag:

```cando
var stop = false;

var t = thread {
    while !stop {
        doWork();
    }
    return "stopped";
};

thread.sleep(1000);
stop = true;       // thread body will see this on the next loop iteration
var r = await t;
```

---

## Patterns and examples

### Fan-out / fan-in

```cando
function processItem(item) {
    // some expensive work
    return item * item;
}

var items = [1, 2, 3, 4, 5, 6, 7, 8];

// Fan out: spawn one thread per item
var handles = items ~> thread processItem(pipe);

// Fan in: collect results in order
var results = handles ~> await pipe;
```

### Race: first result wins

```cando
function fetchMirror(url) { return file.read(url); }

var mirrors = ["mirror1.txt", "mirror2.txt", "mirror3.txt"];
var handles = mirrors ~> thread fetchMirror(pipe);

var result = null;
while result == null {
    for i in handles {
        if thread.done(handles[i]) {
            result = await handles[i];
            break;
        }
    }
    thread.sleep(5);
}
print(result);
```

### Background task with cancellation timeout

```cando
var t = thread {
    thread.sleep(5000);
    return "slow result";
};

thread.sleep(1000);
if !thread.done(t) {
    thread.cancel(t);
    print("timed out");
} else {
    print(await t);
}
```

### Producer / consumer via shared array

```cando
var queue   = [];
var done_flag = false;

var producer = thread {
    for i of 1 -> 5 {
        queue[#queue] = i;
        thread.sleep(10);
    }
    done_flag = true;
};

var consumer = thread {
    var consumed = 0;
    while !done_flag || #queue > 0 {
        if #queue > 0 {
            var item = queue[0];
            // remove front element
            var newq = [];
            for i of 1 -> #queue - 1 { newq[i - 1] = queue[i]; }
            queue = newq;
            consumed += item;
        } else {
            thread.sleep(1);
        }
    }
    return consumed;
};

await producer;
var total = await consumer;
print(total);  // 15
```

---

## Implementation notes

This section is for contributors and embedders using the library API.

### Embedding and thread safety

**One `CandoVM*` = one calling thread at a time.**  Do not call VM API
functions from multiple OS threads on the same VM.

You may:
- Use `thread`/`await` freely in CanDo scripts — the runtime handles child
  thread VMs internally with appropriate locking.
- Create multiple `CandoVM*` instances (each used by a single thread) —
  see [embedding.md](embedding.md).
- Call `cando_vm_wait_all_threads(vm)` after `cando_vm_exec` returns to
  ensure all script-spawned threads have finished before `cando_close`.

The global string intern table is protected by a mutex and safe for
concurrent calls from separate `CandoVM*` instances.

### Child VM pattern

Each `thread` expression creates a lightweight child `CandoVM`:

- **Stack and call frames** are private to the child.
- **Handle table** (`CandoHandleTable`) is *shared by pointer* from the parent
  — the same thread-safe handle table is used by all threads.
- **Global environment** (`CandoGlobalEnv`) is *shared by pointer* — all
  threads see the same globals, protected by the global lock.
- **Native function table** is copied by value at spawn time.

### Thread lifecycle

```
PENDING  →  RUNNING  →  DONE
                    ↘  ERROR
                    ↘  CANCELLED
```

The state is an `_Atomic(CdoThreadState)` field.  Transitions are made by the
thread trampoline (`vm_thread_trampoline`) via `cdo_thread_set_results` and
`cdo_thread_set_error`, or externally by `thread.cancel` via an atomic CAS.

### `OP_THREAD` and `OP_AWAIT`

`OP_THREAD`:
1. Pops an `OBJ_FUNCTION` closure off the value stack.
2. Allocates a `CdoThread` and a `ThreadArg`.
3. Calls `cando_os_thread_create` with `vm_thread_trampoline` as the entry point.
4. Detaches the OS thread.
5. Pushes the `CdoThread` handle.

`OP_AWAIT`:
1. Pops a value; resolves it as `OBJ_THREAD` via the handle table.
2. Calls `cdo_thread_wait` (blocks on `done_cond`).
3. If `DONE`: pushes all `result_count` values and sets `last_ret_count`.
4. If `ERROR`/`CANCELLED`: pushes `null` and sets `last_ret_count = 1`.

### Thread result capture

`cando_vm_exec_closure` sets `thread_stop_frame` to the frame depth at which
the thread body's `OP_RETURN` should fire.  `OP_RETURN` checks
`thread_stop_frame` before `eval_stop_frame` so thread returns are captured
into `vm->thread_results[]` rather than being handled as a normal return.
