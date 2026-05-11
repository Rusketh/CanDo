# Parser and Compiler

The front end is a single-pass **lexer + Pratt parser** that emits
bytecode directly into a `CandoChunk` — there is no intermediate AST.
This page describes how it's structured and where to look when you
want to change it.

## Files

```
source/parser/
  lexer.c, lexer.h        token stream
  token.h                  TokenType enum, keyword/operator table
  parser.c, parser.h       Pratt parser; emits bytecode as it parses.
                           Scope tracking (locals, upvalues, captures)
                           and the emit helpers (emit_op / emit_op_a /
                           emit_op_ab) live here as well — there is no
                           separate scope.c or emit.c.
```

## Lexer

`source/parser/lexer.c` is hand-written and byte-driven.

- The lexer is a streaming consumer; the parser pulls tokens on demand
  via `lexer_next()`.
- Identifiers are checked against a static keyword table.  Keywords
  are case-insensitive but **mixed-case rejection** lives here:
  `Var`, `iF`, etc. are returned as `TOK_IDENT`.
- Numbers, strings (three styles), and multi-character operators are
  all consumed in `lexer_next()`.
- Backtick strings are handled by switching the lexer into a special
  state that emits a stream of segments and `${ … }` chunks; the
  parser stitches these into a `OP_ADD`-chain.

If you need to add a new operator token, the keyword/operator table
in `token.h` is the source of truth.

## Pratt parser

`source/parser/parser.c` is a Pratt parser.  Each token type has up to
two entries in a dispatch table:

| Field      | When it fires                              |
|------------|--------------------------------------------|
| `prefix`   | The token starts an expression.            |
| `infix`    | The token follows an expression.           |
| `bp`       | Binding power (precedence) for `infix`.    |

`parse_expr(min_bp)` reads a prefix and then keeps consuming infix
tokens while their binding power is >= `min_bp`.

Statement-level dispatch (`IF`, `WHILE`, `FOR`, `CLASS`, `TRY`,
`THROW`, `BREAK`, …) lives in `parse_statement()`.  Each statement
parser is responsible for emitting its own opcodes.

### Adding a new statement

1. Add a token type if needed (in `token.h`) and teach the lexer to
   produce it.
2. Add a case in `parse_statement()`.
3. Emit the right opcode sequence using the `emit_op` / `emit_op_a` /
   `emit_op_ab` helpers in `parser.c`.

### Adding a new operator

1. Add the token type in `token.h`.
2. Add an entry in the parser's dispatch table.  Most binary operators
   need an `infix` handler that parses the right side, then emits a
   single opcode.
3. Pick a binding power consistent with the precedence table in
   [`../language/expressions.md`](../language/expressions.md).

## Scope system

Scope tracking lives directly in `parser.c` (in the `CandoParser`
struct and helpers around it — there is no separate `scope.c`).  A
scope owns:

- A list of local names → slot indices.
- A list of captured upvalues for the function it belongs to.
- Whether each local is currently `CONST`.
- Loop and `TRY` frames for `BREAK`/`CONTINUE`/`THROW` to know what
  to unwind.

When a function's body declares a local that closes over an
enclosing-function local, the inner function records an upvalue
descriptor (parent slot, or parent upvalue index, plus a "from local
or upvalue" bit).  The runtime materializes these into the closure's
upvalue array on `OP_CLOSURE`.

When a block ends, the compiler emits `OP_POP_N` for the locals that
go out of scope, plus `OP_CLOSE_UPVAL` for any of those locals that
were captured by closures.

## Bytecode emission

`source/parser/emit.h` exposes:

```c
void emit_op   (Compiler *c, CandoOpcode op);
void emit_op_a (Compiler *c, CandoOpcode op, uint16_t a);
void emit_op_ab(Compiler *c, CandoOpcode op, uint16_t a, uint16_t b);
```

These append the encoded instruction to the current chunk's bytecode
buffer.  The opcode dispatch table in `source/vm/opcodes.h` defines
the `OPFMT_*` for each opcode, which determines how many bytes the
helper writes.

Constant emission goes through `emit_const(c, value) → uint16_t` —
returning the constant-pool index — and the caller emits an
`OP_CONST a=index`.

Forward jumps are emitted with a placeholder operand; `patch_jump(at,
target)` fills in the offset once the target is known.  The pattern is
in every conditional and loop emitter.

## Class sugar

`CLASS Foo = (self, …) { … }` desugars to:

1. Compile the constructor body as an ordinary function.
2. Emit `OP_NEW_CLASS` with the constructor in the constant pool.
3. For each method assigned afterwards, emit `OP_BIND_METHOD`.
4. If `EXTENDS` is present, emit `OP_INHERIT`.
5. `OP_BIND_DEFAULT_CALL` installs the constructor-runner as the
   class's `__call`.

The runtime side of this is in `source/vm/vm.c`.

## Errors

Parse errors throw via `parser_error(p, fmt, ...)`, which sets the
parser's error state and longjmps back to the top of `cando_dofile`
/ `cando_dostring`.  The first error wins; subsequent errors are
suppressed to avoid cascade failures.

## Diagnostics

`--disasm` (CLI flag) prints the disassembly after compilation.  The
disassembler lives in `source/vm/debug.c` and is also reachable from
the embedding API for tools.

When debugging the parser, the easiest workflow is to write a small
`.cdo` file that triggers the issue and run it under `--disasm`.

## What to read first

If you've never touched the parser:

1. Read `parse_expression` and the operator dispatch table in
   `source/parser/parser.c` to understand how Pratt parsing works
   here.
2. Look at how `parse_if`, `parse_while`, and `parse_for` emit jumps
   and patches.
3. Look at how `parse_function` builds an upvalue list and emits
   `OP_CLOSURE`.

After that, almost every other change is an extension of those
patterns.
