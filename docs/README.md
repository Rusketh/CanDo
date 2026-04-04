# Cando Developer Documentation

Cando is a C-style scripting language written in C11. It compiles source code to
bytecode and executes it on a stack-based VM. Scripts use the `.cdo` extension.

## Document Index

| Document | What it covers |
|---|---|
| [architecture.md](architecture.md) | End-to-end data flow, the two-layer value system, and how all modules fit together |
| [value-types.md](value-types.md) | `CandoValue`, `CdoValue`, `TypeTag`, `CandoString`, and the bridge layer |
| [vm-internals.md](vm-internals.md) | VM struct, call frames, upvalues, the dispatch loop, and all opcodes |
| [object-system.md](object-system.md) | `CdoObject`, prototype chains, field flags, meta-keys, string prototype |
| [parser-compiler.md](parser-compiler.md) | Lexer, Pratt parser, scope system, chunk / constant pool, eval mode |
| [native-api.md](native-api.md) | How to write a native (`CandoNativeFn`) function — signature, stack, errors |
| [writing-a-module.md](writing-a-module.md) | Step-by-step walkthrough for creating a complete library module |
| [threading.md](threading.md) | Threading syntax (`thread`/`await`), the `thread` library, locking model, and patterns |

## Quick-start build

```
make          # build all test binaries + cando executable
make cando    # build only the cando executable
make test     # build and run all tests
./cando script.cdo
```

## Repository layout

```
source/
  core/        low-level primitives (value, string, lock, handle, memory)
  object/      object-layer types   (CdoObject, array, function, class)
  parser/      lexer + Pratt parser → bytecode
  vm/          stack-based VM       (opcodes, chunk, vm, bridge)
  lib/         built-in libraries   (math, file, eval, string)
  natives.c/h  core global-function registrations (print, type, …)
  main.c       cando executable entry point
tests/         C unit-test drivers for each layer
docs/          this documentation
```
