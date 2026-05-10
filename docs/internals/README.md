# Internals

Contributor documentation for the CanDo runtime.  These pages describe
how the implementation is organized — the parser, the VM, the value
system, the GC, the JIT — so that someone changing the runtime can do
so without re-deriving the design from the source.

If you're scripting in CanDo or embedding it, you don't need to read
this directory.  The user-facing language reference is in
[`../language/`](../language/README.md), and the embedding API is in
[`../api/`](../api/README.md).

## Pages

| Page                              | Topic |
|-----------------------------------|-------|
| [architecture.md](architecture.md) | End-to-end pipeline.  How a `.cdo` file becomes execution. |
| [value-system.md](value-system.md) | The two-layer value model: `CandoValue`, `CdoObject`, handles, the bridge. |
| [parser.md](parser.md)            | Lexer, Pratt parser, scope system, bytecode emission. |
| [vm.md](vm.md)                    | Dispatch loop, call frames, upvalues, opcode reference. |
| [jit.md](jit.md)                  | Tracing JIT: hot-path detection, IR, native codegen, abort modes. |

## Quick orientation

```
source/
  core/        CandoValue, memory, handle table, atomics, locks
  object/      CdoObject (heap), arrays, strings, functions, threads
  parser/      lexer.c, parser.c (Pratt), scopes, emit
  vm/          vm.c (dispatch loop), bridge.c, chunk.c, debug.c
  jit/         trace recorder + IR + native codegen (gated by `--jit`)
  lib/         standard library — one .c/.h pair per namespace
  natives.c    print, type, toString, inspect
  cando_lib.c  public embedding API surface
  main.c       CLI entry point
  compat/      Windows / POSIX shims
```

The single public header is [`include/cando.h`](../../include/cando.h);
everything else is private and free to refactor.

For changes that should be driven by an AI assistant, also read
[`../AI-GUIDE.md`](../AI-GUIDE.md) — it captures the conventions and
common-mistake list that reviewers would otherwise have to repeat.

## Build system

Two parallel build systems are supported:

- **CMake** (preferred) — `cmake -B build && cmake --build build`.
- **GNU Make** — `make`, `make cando`, `make test`.

Both build the same artefacts: `libcando.{a,so,dylib,dll}`,
`libcando_static.lib`, and the `cando` interpreter.  See
[`../getting-started.md`](../getting-started.md) for prerequisites.

## Tests

- Unit tests live next to the code as `test_<area>.c`.  They are
  picked up by the build system automatically.
- Integration scripts live in `tests/scripts/`.  The runner in
  `tests/integration/` executes every `.cdo` it finds and diffs the
  output against `tests/scripts/expected/<name>.txt` if one exists.
- `make test` runs both layers.

When adding a feature, **always** include a `tests/scripts/<name>.cdo`
that exercises it.  When fixing a bug, add a regression script that
fails before your change and passes after.
