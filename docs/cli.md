# Command-Line Interface

The `cando` executable runs CanDo scripts.  This page documents every
flag, exit code, and environment variable it understands.

## Synopsis

```
cando <file.cdo> [interpreter-flags] [script-args...]
```

Positional arguments after the script path are forwarded to the script
verbatim, exposed as the global array `args`.  Interpreter flags are
parsed by `cando` itself and are **not** forwarded.

## Interpreter flags

| Flag | Description |
|---|---|
| `--disasm` | Disassemble the compiled bytecode to stderr before execution.  Useful for debugging the parser or seeing what the script actually compiles to. |
| `--jit` | Enable the JIT.  This installs profiling counters, the trace recorder, and the IR-to-native code generator. |
| `--no-jit` | Force-disable the JIT.  Wins over both `--jit` and the `CANDO_JIT` environment variable. |
| `--jit-stats` | Print a one-line summary of JIT activity to stderr at exit (back-edges, function entries, traces compiled, traces aborted, last abort reason). |
| `--jit-dump` | Pretty-print the IR of every compiled trace.  Implies `--jit-stats`. |
| `--no-console` | Detach the inherited console at startup and disable the `console` standard library for the script's lifetime.  Useful for launching CanDo scripts from a GUI shortcut or as a service.  See [libraries/console.md](libraries/console.md#cli-flag----no-console) for the precise behaviour. |

Flag order is free: interpreter flags can appear **before or after** the
script path.  The first non-flag argument is treated as the script;
everything after it that isn't a recognised flag becomes part of
`args[]`.

```bash
cando script.cdo --jit              # flag after script
cando --jit script.cdo              # flag before script -- equivalent
cando --no-console --jit script.cdo arg1 arg2
```

## Script arguments

Everything after the interpreter flags is passed to the script unchanged
through the global `args` array:

```bash
./cando myscript.cdo --port 8080 input.txt
```

Inside `myscript.cdo`:

```cdo
print(args);          // ["--port", "8080", "input.txt"]
print(#args);         // 3
```

If you need true argument parsing (long flags, `--key=value` forms,
sub-commands), do it inside the script — `cando` itself only does the
minimum splitting required to separate its own flags from the script's.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Successful completion (or `os.exit(0)` / `app.quit()`). |
| `1` | Runtime error or uncaught `THROW`.  The error message is written to stderr. |
| `64` | Bad invocation — wrong number of arguments, unknown flag, missing file. |
| any | The exit code passed to `os.exit(n)` or `app.quit(n)`, capped at the host's exit-code range (0–255 on POSIX). |

## Environment variables

| Variable | Description |
|---|---|
| `CANDO_JIT=1` | Equivalent to passing `--jit`.  Convenient for `make`-based workflows that don't want to thread a flag through.  Overridden by `--no-jit`. |

The interpreter does not read any other environment variables; standard
shell env vars (`PATH`, `HOME`, `TMPDIR`, etc.) are accessible to scripts
through `os.getenv()`.

## A typical run with diagnostics

```bash
./cando ./bench.cdo --jit --jit-stats
```

Sample output to stderr at exit:

```
[jit] backedges=12345 func_entries=678 iter_next=910
       trace_starts=42 traces_compiled=39 trace_aborts=3
       trace_iters=1.2M trace_exits=87 hot_pcs=44
       blacklisted_pcs=0 traces_evicted=0
       last_abort=guard_failed
```

Counter meanings are documented in
[internals/jit.md](internals/jit.md).

## Disassembling without running

There is no dedicated "compile only" flag.  To inspect bytecode without
executing, write a guard at the top of the script:

```cdo
IF FALSE { /* compile, but never run */ }
```

…and pass `--disasm`.  The disassembler emits one line per instruction,
with operand decoding.  The opcode table is in
[internals/vm.md](internals/vm.md).
