# `process`

Spawning and controlling child processes.  Built on POSIX `fork` +
`execvp` and Windows `CreateProcess`.

## Reference

### `process.pid() → number`

Current process ID.

### `process.ppid() → number`

Parent process ID.

### `process.spawn(argv, opts*) → proc`

Spawn a child process.

- `argv` is an array.  The first element is the executable; the rest
  are arguments.  No shell expansion is performed.
- `opts` is an optional object.  Recognised fields:

| Field      | Type   | Default     | Description |
|------------|--------|-------------|-------------|
| `stdin`    | string | `"inherit"` | `"inherit"`, `"pipe"`, or `"null"`. |
| `stdout`   | string | `"inherit"` | Same options. |
| `stderr`   | string | `"inherit"` | Same options. |
| `cwd`      | string | unchanged   | Working directory for the child. |

When a stream is set to `"pipe"`, the corresponding `proc:stdin()` /
`stdout()` / `stderr()` returns a [`stream`](stream.md) you can read or
write.

```cdo
VAR proc = process.spawn(["ls", "-la"], { stdout: "pipe" });
VAR output = proc:stdout():readAll();
proc:wait();
print(output);
```

## Methods on `proc`

### `proc:pid() → number`

Child's process ID.

### `proc:stdin() → stream | null`

Writable stream over the child's stdin (when opened with `pipe`).

### `proc:stdout() → stream | null`, `proc:stderr() → stream | null`

Readable streams.

### `proc:wait() → number`

Block until the child exits.  Returns the exit code, or `-signal` on
POSIX if the child was terminated by a signal.

### `proc:kill(sig*) → proc`

Send a signal (default `SIGTERM` on POSIX, `TerminateProcess` on
Windows).  Returns the receiver for chaining.

## Examples

### Capturing output

```cdo
VAR proc = process.spawn(["git", "rev-parse", "HEAD"], {
    stdout: "pipe",
});
VAR sha = proc:stdout():readAll():trim();
proc:wait();
print(sha);
```

### Piping data into a child

```cdo
VAR proc = process.spawn(["sort"], { stdin: "pipe", stdout: "pipe" });
proc:stdin():writeAll("banana\napple\ncherry\n");
proc:stdin():end();
print(proc:stdout():readAll());
proc:wait();
```

### Background worker with timeout

```cdo
VAR proc = process.spawn(["./long_job"]);
VAR t = thread {
    thread.sleep(5000);
    proc:kill();
};
print("exit:", proc:wait());
thread.cancel(t);
await t;
```
