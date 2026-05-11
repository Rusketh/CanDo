# `app`

Application lifecycle hooks.  The `app` namespace is wired up to the
`cando` CLI to provide co-operative shutdown semantics — call
`app.quit()` instead of `os.exit()` when you want `FINALY` blocks to run
and for the host embedder to see a clean return rather than a hard
process exit.

## Reference

### `app.quit(code*)`

Request that the script terminate.  Sets the exit code (default `0`)
and signals the runtime to wind down — the current top-level frame
runs to completion, all `FINALY` blocks fire, and `cando_dofile`
returns control to the embedder.

```cdo
IF !startup_ok() {
    app.quit(1);
}
```

### `app.exit(code*)`

Hard exit.  Equivalent to `os.exit(code)` — the process terminates
immediately without running `FINALY`.  Use only when something has gone
unrecoverably wrong.

### `app.isQuitting() → bool`

`TRUE` once `app.quit()` has been called and shutdown is in progress.
Useful inside long-running loops to break out cleanly:

```cdo
WHILE !app.isQuitting() {
    handle_one_event();
}
```

### `app.holds() → number`

Number of outstanding "hold" tokens (currently used internally by the
windowing module — increments while a window is open and decrements
when it closes).  Embedders can use it to tell whether the script is
still doing useful work or just waiting on a closed event loop.

### `app.exitCode(code*) → number`

With no argument, returns the exit code currently set (default `0`).
With a numeric argument, updates the exit code and returns the new
value.  This is the value the embedder will see when the script ends.

## Examples

### Graceful shutdown handler

```cdo
VAR worker = thread {
    WHILE !app.isQuitting() {
        handle_job();
    }
};

// Trap something that signals shutdown — e.g. a special HTTP request:
http.createServer(FUNCTION(req, res) {
    IF req.path == "/shutdown" {
        res:send("bye");
        app.quit(0);
    } ELSE {
        res:send("hi");
    }
}):listen(8080);

await worker;
```

### Telling the embedder to use a non-zero exit code

```cdo
TRY {
    main();
} CATCH (e) {
    print("fatal:", e);
    app.quit(1);
}
```
