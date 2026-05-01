# The `cando` Command-Line Interpreter

The `cando` executable is a thin wrapper around the embedding API.  It
opens a VM, loads every standard library, runs your script, and exits.

```
cando <file.cdo> [--disasm] [args...]
```

| Element       | Meaning                                                                                          |
|---            |---                                                                                               |
| `<file.cdo>`  | Path to the script to execute.  Required.  May be relative or absolute.                          |
| `--disasm`    | Disassemble the compiled bytecode to **stderr** before running it.  See *Bytecode disassembly* below. |
| `[args...]`   | Everything else after the script path is forwarded to the script as a global `args` array of strings. |

If invoked with no arguments, `cando` prints the version and a usage
line to stderr and exits with code `1`.

## Exit codes

| Code | Meaning                                                                |
|---   |---                                                                     |
| `0`  | Script ran to completion without a runtime error.                      |
| `1`  | Failure: bad CLI args, file not found, parse error, runtime error, OOM, or VM-creation failure. |

A script may exit early with `os.exit(code)`; the code provided becomes
the process exit status.  An uncaught `THROW` inside the script also
exits with code `1` after the runtime error message is printed to stderr.

## The global `args` array

Every script can read `args` directly — it is set up before the script
runs.  `args[0]` is the **first argument after the script path**, *not*
the script path itself.

```cando
// echo.cdo
FOR (VAR i = 0; i < #args; i = i + 1) {
    print(args[i]);
}
```

```bash
$ cando echo.cdo hello world
hello
world

$ cando echo.cdo
$            # args is empty, loop body does not run
```

If the only thing after the script path is `--disasm`, that switch is
consumed by the interpreter and is **not** passed to the script.  `args`
will therefore be empty in `cando script.cdo --disasm`.

`args` is always an array of strings, even when the underlying CLI
argument looks numeric.  Convert with the `+` numeric coercion or
`number(args[0])`:

```cando
VAR n = +args[0];          // unary plus coerces "42" -> 42
```

## Standard streams

| Stream | Used for                                                                |
|---     |---                                                                      |
| stdout | `print(...)`, anything written by `file.write("/dev/stdout", ...)`     |
| stderr | Runtime errors prefixed with `cando: `, the `--disasm` listing         |
| stdin  | `file.readLine("/dev/stdin")` and similar (Linux/macOS); see `os.stdin` for portable handles |

A runtime error message has the form

```
cando: <message> [<file.cdo> line <N>]
```

and is printed to stderr just before `cando` exits with code `1`.

## Bytecode disassembly

`--disasm` prints the compiled chunk for the entire script (including
the contents of inline `FUNCTION` and `CLASS` literals) to **stderr**
before execution begins.  The script then runs normally — disassembly
is purely diagnostic.

```bash
$ cando hello.cdo --disasm 2> hello.dis
hello, world!
$ head hello.dis
== chunk: hello.cdo ==
0000 OP_CONST              0 ; "hello, world!"
0003 OP_LOAD_GLOBAL        1 ; "print"
0006 OP_CALL               1
...
```

This is useful for:

- Verifying that an optimisation hypothesis matches what the parser
  emits.
- Investigating performance: counting opcodes, looking for redundant
  loads, spotting unintended re-evaluations.
- Learning the VM by example.

The mnemonic catalogue lives in [vm-internals.md](vm-internals.md).

## Environment variables

`cando` itself reads no environment variables directly; the VM and
standard libraries do.

| Variable           | Read by                                                              |
|---                 |---                                                                   |
| `PATH`             | `process.exec` to find executables when no explicit path is given.   |
| `HOME`, `TMPDIR`   | `os.tmpDir()`, `os.homeDir()`.                                       |
| `CANDO_PATH`       | `include(...)` search path for relative imports (colon-separated).   |
| `SSL_CERT_FILE`, `SSL_CERT_DIR` | Read by the OpenSSL backend used by `https` and `secure_socket`. |

Scripts can read any environment variable with `os.getEnv("NAME")` and
set them in the **child process only** with `process.exec({env:...})`.

## Embedding equivalent

Everything `cando` does is exposed in the embedding API:

```c
CandoVM *vm = cando_open();
cando_openlibs(vm);
cando_set_args(vm, argc - 2, (const char *const *)(argv + 2));
int rc = cando_dofile(vm, "main.cdo");
if (rc != CANDO_OK)
    fprintf(stderr, "cando: %s\n", cando_errmsg(vm));
cando_close(vm);
return rc == CANDO_OK ? 0 : 1;
```

See [embedding.md](embedding.md) for a complete walk-through.
