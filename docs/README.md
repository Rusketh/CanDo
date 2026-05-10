# CanDo Documentation

Welcome to the CanDo documentation.  This index is the entry point — every
other doc in the tree is reachable from one of the sections below.

CanDo is a small embeddable scripting language.  Source files use the
`.cdo` extension and are compiled to bytecode for a stack-based virtual
machine.  Scripts are UTF-8 plain text; the runtime is C11.  See the
project [Readme.md](../Readme.md) for the high-level pitch and design
goals.

---

## Start here

| Document | What's in it |
|---|---|
| [getting-started.md](getting-started.md) | Build the interpreter, run your first script, set up your editor. |
| [cli.md](cli.md) | The `cando` executable: flags, arguments, exit codes, environment variables. |
| [AI-GUIDE.md](AI-GUIDE.md) | Orientation for AI assistants editing this codebase efficiently. |

## Language reference

The language reference lives in [`language/`](language/README.md) and is
split into focused files:

| Document | What's in it |
|---|---|
| [language/syntax.md](language/syntax.md) | Lexical structure, keywords, operators, literals. |
| [language/types.md](language/types.md) | The five value types: `null`, `bool`, `number`, `string`, `object`. |
| [language/expressions.md](language/expressions.md) | Operators, precedence, evaluation order, special forms. |
| [language/statements.md](language/statements.md) | `IF`, `WHILE`, `FOR`, `BREAK`, `CONTINUE`, blocks, scoping. |
| [language/functions.md](language/functions.md) | Definition forms, multi-return, varargs, masks, closures. |
| [language/classes.md](language/classes.md) | `CLASS`, `EXTENDS`, metamethods, prototype chains. |
| [language/error-handling.md](language/error-handling.md) | `TRY` / `CATCH` / `FINALY` / `THROW`, runtime error messages. |
| [language/threading.md](language/threading.md) | `thread { … }`, `await`, the `thread` library. |
| [language/pipes.md](language/pipes.md) | `~>` map, `~!>` filter+map, `~&>` predicate filter. |
| [language/modules.md](language/modules.md) | `include()` semantics, module caching, file extensions. |

## Library reference

Per-library documentation, function-by-function, with examples, lives in
[`libraries/`](libraries/README.md).  Every standard-library namespace
has a dedicated file.

## Embedding and the C API

For embedders and extension authors:

| Document | What's in it |
|---|---|
| [api/README.md](api/README.md) | Index of embedder docs. |
| [api/embedding.md](api/embedding.md) | Lifecycle, running scripts, exchanging values, error handling. |
| [api/reference.md](api/reference.md) | Reference for every public symbol in `cando.h`. |
| [api/extensions.md](api/extensions.md) | Native functions and binary modules. |

## Internals (contributors)

| Document | What's in it |
|---|---|
| [internals/README.md](internals/README.md) | Index of internals docs. |
| [internals/architecture.md](internals/architecture.md) | End-to-end pipeline. |
| [internals/value-system.md](internals/value-system.md) | `CandoValue`, `CdoValue`, handles, the bridge layer. |
| [internals/parser.md](internals/parser.md) | Lexer, Pratt parser, scope system, bytecode emission. |
| [internals/vm.md](internals/vm.md) | Dispatch loop, call frames, upvalues, opcode reference. |
| [internals/jit.md](internals/jit.md) | Tracing JIT: hot-path detection, IR, native codegen. |

## Binary modules

The native extension modules live in [`../modules/`](../modules/README.md)
with their own documentation alongside their source.  Per-module
descriptions are in [modules/README.md](../modules/README.md).

---

## Cross-cutting topics

A few topics span more than one of the categories above.  Quick links:

- **Concurrency** — [language/threading.md](language/threading.md) (the
  language form), [libraries/thread.md](libraries/thread.md) (the
  library), [libraries/stream.md](libraries/stream.md) (channels and
  pipes).
- **Networking** — [libraries/socket.md](libraries/socket.md),
  [libraries/secure_socket.md](libraries/secure_socket.md),
  [libraries/http.md](libraries/http.md),
  [libraries/https.md](libraries/https.md).
- **Data formats** — [libraries/json.md](libraries/json.md),
  [libraries/yaml.md](libraries/yaml.md),
  [libraries/csv.md](libraries/csv.md).

## Version

These docs track CanDo `1.0.0` — the version constant defined in
[`include/cando.h`](../include/cando.h).
