# Parser and Compiler

## Overview

The parser (`source/parser/parser.c`) is a recursive-descent + Pratt
expression parser that emits bytecode directly into a `CandoChunk`. No
intermediate AST -- each grammar rule emits its opcodes as it parses.

## Lexer (`source/parser/lexer.h`)

```c
typedef struct CandoLexer { ... } CandoLexer;

void cando_lexer_init(CandoLexer *lex, const char *source, usize length);
CandoToken cando_lexer_next(CandoLexer *lex);
```

`CandoToken` has a `code` (`CandoTokenCode` enum), a `start` pointer into
the source buffer, and a `length`.

The lexer handles:

- Integer and floating-point numeric literals
- Double-quoted `"..."` strings (with `\n`, `\t`, `\\`, `\"` escapes)
- Single-quoted `'...'` raw multi-line strings (no escapes, no interpolation)
- Backtick `` `...${expr}...` `` interpolated strings (with escapes)
- Line (`//`) and block (`/* */`) comments (skipped)
- All keywords (uppercase except `pipe`): `VAR`, `CONST`, `IF`, `ELSE`,
  `WHILE`, `FOR`, `FUNCTION`, `RETURN`, `TRUE`, `FALSE`, `NULL`, `CLASS`,
  `GLOBAL`, `STATIC`, `PRIVATE`, `TRY`, `CATCH`, `FINALY` (one L!), `THROW`,
  `BREAK`, `CONTINUE`, `IN`, `OF`, `OVER`, `ASYNC`, `AWAIT`, `THREAD`, `pipe`
- Special tokens: `~>` (pipe operator), `~!>` (filter operator),
  `->` (ascending range), `<-` (descending range), `::` (fluent call),
  `|&` (bitwise xor), `=>` (reserved)

## CandoChunk (`source/vm/chunk.h`)

```c
typedef struct CandoChunk {
    char       *name;
    u8         *code;
    u32         code_len;
    u32         code_cap;
    CandoValue *constants;
    u32         const_len;
    u32         const_cap;
    bool        is_top_level;
    bool        is_vararg;
    u32         param_count;
    u32         local_count;
} CandoChunk;
```

Key functions:

- `cando_chunk_new(name, param_count, is_vararg)`
- `cando_chunk_free(chunk)`
- `cando_chunk_add_const(chunk, val)` -- returns `u16`
- `cando_chunk_add_string_const(chunk, str, len)` -- returns `u16`
- `cando_chunk_emit_byte(chunk, byte, line)`
- `cando_chunk_emit_op(chunk, op, line)`
- `cando_chunk_emit_op_a(chunk, op, a, line)`
- `cando_chunk_emit_op_ab(chunk, op, a, b, line)`
- `cando_chunk_emit_jump(chunk, op, line)` -- returns `u32`
- `cando_chunk_patch_jump(chunk, patch_offset)`
- `cando_chunk_patch_jump_to(chunk, patch_offset, target)`
- `cando_chunk_emit_loop(chunk, loop_start, line)`

## CandoParser (`source/parser/parser.h`)

```c
typedef struct CandoParser {
    CandoLexer  lexer;
    CandoToken  current;
    CandoToken  previous;
    CandoChunk *chunk_stack[64];
    u32         chunk_depth;
    bool        had_error;
    char        error_buf[256];
    bool        eval_mode;
    bool        last_stmt_was_expr;
    // scope / local variable tables
} CandoParser;
```

- `cando_parser_init(p, source, length, top_chunk)`
- `cando_parse(p)` -- returns `bool`
- `cando_parser_error(p)` -- returns `const char*`

## eval_mode

When `p->eval_mode = true` (set by `native_eval` in `lib/eval.c`):

- `cando_parse` emits `OP_RETURN 1` instead of `OP_HALT`.
- If the last statement was an expression, the trailing `OP_POP` is removed
  so its value remains on the stack for `OP_RETURN 1`.
- This enables `eval("1 + 2")` returning `3`.

## Scope system

The parser tracks locals in a scope stack. Each `{ }` block introduces a new
scope level. Variables declared with `VAR`/`CONST` get local slot indices.

- `global` prefix or bare assignment to undeclared name -- `OP_DEF_GLOBAL`.
- `var`/`const` inside function -- `OP_DEF_LOCAL`.
- `const` -- `OP_DEF_CONST_LOCAL` / `OP_DEF_CONST_GLOBAL` (VM enforces write
  protection).
- Upvalues resolved at parse time: nested function referencing enclosing scope
  variable records an upvalue descriptor. `OP_CLOSURE` wires `CandoUpvalue`
  pointers at runtime.

## Pratt expression parser

```c
static void parse_expr(CandoParser *p, Precedence min_prec);
```

Each token type has a parse rule: `prefix_fn` (unary `-`, number literal,
`(` grouping), `infix_fn` (`+`, `-`, `*`, `.`, `(` call), and `precedence`
(binding power). Table-driven -- adding operators means adding a token and
filling in the rule row.

## Emitting instructions

Parser rules emit directly to current chunk (`cur(p)`):

- `emit_op(p, op)`
- `emit_op_a(p, op, a)`
- `emit_const(p, val)` -- adds constant, emits `OP_CONST A`

Forward jumps use the `emit_jump` / `patch_jump` pattern.

## Nested function compilation

When the parser encounters `FUNCTION`:

1. New `CandoChunk` created via `cando_chunk_new`.
2. Pushed onto `p->chunk_stack`.
3. Parameters registered as local slots.
4. Function body compiled into new chunk.
5. Chunk popped, added as constant to parent chunk, `OP_CLOSURE A` emitted.

## Anonymous functions

`FUNCTION` followed by `(` (not identifier) routes to `parse_function_expr`
(value) not `parse_function` (declaration). Supports:

```cando
VAR sq = FUNCTION(x) { RETURN x*x; };
```

Cross-reference: `vm-internals.md` for opcode semantics, `value-types.md` for
`CandoValue`/`CandoString`.
