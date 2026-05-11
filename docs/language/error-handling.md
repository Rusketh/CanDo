# Error Handling

CanDo errors are values that propagate up the call stack, unwinding
frames as they go.  They are caught with `TRY` / `CATCH`, raised with
`THROW`, and observed at the C boundary as `CANDO_ERR_RUNTIME`.

## `TRY` / `CATCH` / `FINALY`

```cdo
TRY {
    risky_work();
} CATCH (err) {
    print("failed:", err);
} FINALY {
    cleanup();
}
```

- `TRY { … }` runs the body.  If a `THROW` (or runtime error) escapes
  it, control transfers to the matching `CATCH`.
- `CATCH (err)` binds the thrown value(s) to its parameter list and
  runs its body.  Without `CATCH`, the error keeps unwinding.
- `FINALY { … }` always runs — whether the `TRY` block completed
  normally, threw and was caught, or threw and is still unwinding.

> `FINALY` is spelled with **one L**.  `FINALLY` is *not* accepted.

The three blocks can appear in any order after `TRY`, but only `CATCH`
and `FINALY` are valid follow-ups.  At least one of them must be
present.

## `THROW`

```cdo
THROW "validation failed";
THROW 404, "not found";
```

`THROW` accepts one or more values; they're queued onto the error
record together.  The matching `CATCH` accepts a parameter list; values
unpack positionally:

```cdo
TRY {
    THROW "validation", 422, "missing 'name'";
} CATCH (kind, code, detail) {
    print(kind, code, detail);
}
```

Excess thrown values are dropped; missing ones arrive as `NULL`.

You can throw any value — strings (most common), numbers, arrays,
objects.  When the embedder receives an uncaught error, it sees the
**stringified first value** through `cando_errmsg(vm)`.

## Re-throwing

CanDo has no dedicated re-raise keyword.  To propagate an error from a
`CATCH`, either omit the `CATCH` clause for the cases you don't want to
handle, or `THROW` a fresh value (the new error replaces the old one):

```cdo
TRY {
    risky();
} CATCH (e) {
    log(e);
    THROW e;          // forward the same value
}
```

## Runtime errors

Built-in operations raise errors that look exactly like a `THROW` of a
string — they're catchable the same way:

```cdo
TRY {
    VAR v = 1 / 0;
} CATCH (msg) {
    print("caught:", msg);     // caught: division by zero
}
```

What follows is the catalogue of error messages the runtime produces.
You can match on these strings if you need to react differently to
different failure modes.

### Arithmetic and operator errors

| Message                                                    | Cause |
|---|---|
| `division by zero`                                         | `a / 0` or `a % 0`. |
| `operands must be numbers (got <T> and <T>)`               | `a + b` where neither side has a `__add` metamethod and at least one side is non-numeric / non-string. |
| `operands must be numbers`                                 | A binary operator (`-`, `*`, `/`, etc.) received a non-numeric operand. |
| `comparison requires numbers`                              | `<`, `<=`, `>`, `>=` between non-numeric values without a `__lt` / `__le` metamethod. |
| `range requires numbers`                                   | `a -> b` or `a <- b` outside a `FOR` (the range-list form). |
| `range check requires numbers`                             | A `FOR i IN a -> b { … }` where `a` or `b` isn't a number. |
| `unary '-' requires a number`                              | `-x` on a non-numeric value without `__unm`. |
| `'++' requires a number`, `'--' requires a number`         | Increment/decrement on a non-number. |
| `# operator requires a string or object`                   | `#x` on `null`, `bool`, or `number` without `__len`. |

### Type errors on access

| Message                                          | Cause |
|---|---|
| `field access on non-object (got <T>)`           | `v.name` when `v` is not an object or string. |
| `field assignment on non-object`                 | `v.name = …` when `v` is not an object. |
| `index access on non-object`                     | `v[k]` on a non-object (and non-string). |
| `index assignment on non-object`                 | `v[k] = …` on a non-object. |
| `index must be a number or string`               | `arr[obj]` and similar. |
| `IN operator requires an object`                 | `FOR k IN v` where `v` isn't an iterable object. |
| `OF operator requires an object`                 | `FOR x OF v` where `v` isn't an array. |
| `pipe/filter (~>/~!>) requires an array source`  | `arr ~> body` where `arr` isn't an array. |

### Calls and methods

| Message                                          | Cause |
|---|---|
| `can only call functions (got <T>)`              | Called a non-callable value.  An object with `__call` would have been dispatched instead. |
| `method call on non-object (got <T>)`            | `v:m()` on a non-object value. |
| `method is not callable`                         | `v:m()` where `m` resolved but isn't a function. |
| `meta-method is not callable`                    | A meta-method field (`__add`, `__lt`, …) resolved to a non-callable value. |
| `call stack overflow`                            | Recursion depth exceeds `CANDO_FRAMES_MAX` (256). |
| `stack overflow in method call`                  | Value stack exhausted while building the call frame (default 2048). |
| `undefined variable '<name>'`                    | Read of a global that was never assigned. |

### Control flow and bindings

| Message                                  | Cause |
|---|---|
| `cannot assign to constant '<name>'`     | Reassigning a `CONST` binding. |
| `BREAK outside loop`                     | `BREAK` (or `BREAK n`) used where no loop is active. |
| `CONTINUE outside loop`                  | `CONTINUE` used where no loop is active. |
| `SETTLE outside IF`                      | `SETTLE` (or `SETTLE n`) used outside an `IF` chain. |

### Threads and concurrency

| Message                                  | Cause |
|---|---|
| `thread: expected a function`            | `thread.spawn(v)` with a non-function `v`. |
| `thread: failed to create OS thread`     | `pthread_create` / Windows equivalent failed. |
| `await: expected a thread handle`        | `await v` with no operand, or wrong type. |
| `await: value is not a thread`           | `await v` where `v` is not a thread object. |

### Class machinery

These come from the class compilation primitives and only appear if
the bytecode was hand-crafted or corrupted; ordinary scripts never see
them.

| Message                                       | Notes |
|---|---|
| `INHERIT: expected class objects`             | `OP_INHERIT` saw bad operands. |
| `BIND_METHOD: expected class object`          | Method-binding failed. |
| `BIND_DEFAULT_CALL: expected class object`    | Default `__call` setup failed. |
| `class __call: missing class receiver`        | Default class `__call` invoked with no class. |
| `class __call: invalid class handle`          | Default class `__call` saw an invalid handle. |

### Reserved / not yet implemented

| Message                                          | Notes |
|---|---|
| `tail call not yet implemented`                  | The `OP_TAIL_CALL` opcode is reserved. |
| `ASYNC not implemented (use 'thread' instead)`   | Reserved keyword; use `thread` for concurrency. |
| `YIELD not implemented (use 'thread' instead)`   | Same. |

## Stack unwinding

Throwing an error unwinds the call stack until it reaches a matching
`CATCH`.  As frames are popped:

- Local variables are released.
- Upvalues that were captured by closures are "closed" (moved to the
  heap), preserving their values for any closure that survives.
- `FINALY` blocks run in reverse order of their `TRY` ancestors.

If a `FINALY` block itself throws, that new error replaces the original
one — be careful with cleanup code that might fail.

## Catching at the C boundary

For embedders: an uncaught script error becomes `CANDO_ERR_RUNTIME` from
the entry point (`cando_dofile`, `cando_dostring`, etc.).
`cando_errmsg(vm)` returns the formatted error string until the next
entry-point call clears it.  See [../api/embedding.md](../api/embedding.md).

## Best practice — narrow `TRY` blocks

Wrap the smallest possible region:

```cdo
// Good
VAR data;
TRY {
    data = json.parse(text);
} CATCH (e) {
    THROW "config: " + e;
}
process(data);

// Less good — also catches errors from process(), which may be a real
// bug rather than a parse failure.
TRY {
    VAR data = json.parse(text);
    process(data);
} CATCH (e) {
    THROW "config: " + e;
}
```
