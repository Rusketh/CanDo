# `os`

Operating-system services: time, environment variables, sub-shell
invocation, process exit.

## Reference

### `os.time() → number`

Unix timestamp in seconds (integer-valued double).

```cdo
print(os.time());                  // 1714400000
```

### `os.clock() → number`

Monotonic clock in seconds (process CPU time on POSIX).  Useful for
measuring elapsed time across a region of code.

```cdo
VAR t0 = os.clock();
expensive_work();
print(`took ${os.clock() - t0}s`);
```

### `os.getenv(name) → string | null`

Read an environment variable.  Returns `NULL` if unset.

```cdo
VAR home = os.getenv("HOME") || "/tmp";
```

### `os.setenv(name, value) → bool`

Set or overwrite an environment variable for the current process and
its descendants.

```cdo
os.setenv("LANG", "C.UTF-8");
```

### `os.execute(cmd) → number`

Run `cmd` via the host shell (`/bin/sh -c` on POSIX, `cmd.exe /c` on
Windows) and return the shell's **exit status** as a number.  Returns
`-1` if the shell could not be launched.  Stdout and stderr go to the
parent process's standard streams; they are *not* captured.

```cdo
VAR status = os.execute("ls /tmp > /dev/null");
print(status);                     // 0
```

To capture output, set working directory, redirect streams, or pass
arguments without shell quoting, use [`process.spawn`](process.md)
instead.

### `os.exit(code*)`

Terminate the process.  `code` defaults to `0`.

```cdo
IF !startup_ok() {
    os.exit(1);
}
```

`os.exit` does *not* run `FINALY` blocks of any active `TRY`.  For a
graceful shutdown that lets cleanup run, throw an error or call
`app.quit()` (see [app.md](app.md)).

## Examples

### Loading config based on environment

```cdo
VAR env = os.getenv("ENV") || "dev";
VAR cfg = include(`./config/${env}.yaml`);
```

### Timing a block

```cdo
FUNCTION timed(label, fn) {
    VAR t0 = os.clock();
    VAR result = fn();
    print(`[${label}] ${os.clock() - t0}s`);
    RETURN result;
}

VAR rows = timed("parse", () => csv.parse(file.read("big.csv")));
```
