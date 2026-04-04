# Parser and Compiler

## Overview

The parser (`source/parser/parser.c`) is a **recursive-descent + Pratt
expression parser** that emits bytecode directly into a `CandoChunk`.  There
is no intermediate AST — each grammar rule emits its opcodes as it parses.

---

## Lexer  (`source/parser/lexer.h`)

```c
typedef struct CandoLexer { ... } CandoLexer;

void cando_lexer_init(CandoLexer *lex, const char *source, usize length);
CandoToken cando_lexer_next(CandoLexer *lex);
```

`cando_lexer_next()` returns the next `CandoToken`.  Tokens have a `code`
(`CandoTokenCode` enum, e.g. `TOK_NUMBER`, `TOK_IDENT`, `TOK_PLUS`), a
`start` pointer into the source buffer, and a `length`.

The lexer handles:
- Integer and floating-point numeric literals
- Double-quoted `"..."` strings (with `\n`, `\t`, `\\`, `\"` escapes)
- Single-quoted `'...'` multi-line strings
- Backtick `` `...${expr}...` `` interpolated strings
- Line (`//`) and block (`/* */`) comments (skipped)
- All keywords: `VAR`, `CONST`, `IF`, `ELSE`, `WHILE`, `FOR`, `FUNCTION`,
  `RETURN`, `TRUE`, `FALSE`, `NULL`, `CLASS`, `GLOBAL`, `TRY`, `CATCH`,
  `FINALLY`, `THROW`, `BREAK`, `CONTINUE`, `IN`, `OF`, `ASYNC`, `AWAIT`, etc.

---

## CandoChunk  (`source/vm/chunk.h`)

```c
typedef struct CandoChunk {
    char       *name;
    u8         *code;         // bytecode byte array
    u32         code_len;
    u32         code_cap;
    CandoValue *constants;    // constant pool
    u32         const_len;
    u32         const_cap;
    // ... local slot info, upvalue descriptors, source-line table ...
    bool        is_top_level;
    bool        is_vararg;
    u32         param_count;
    u32         local_count;
} CandoChunk;
```

Key functions:

```c
CandoChunk *cando_chunk_new(const char *name, u32 param_count, bool is_vararg);
void        cando_chunk_free(CandoChunk *chunk);

// Add a constant to the pool; returns its index
u32  cando_chunk_add_constant(CandoChunk *chunk, CandoValue val);

// Emit one byte or a full instruction
void cando_chunk_emit_byte(CandoChunk *chunk, u8 byte);
void cando_chunk_emit_op(CandoChunk *chunk, CandoOpcode op);
void cando_chunk_emit_op_a(CandoChunk *chunk, CandoOpcode op, u16 a);
```

---

## CandoParser  (`source/parser/parser.h`)

```c
typedef struct CandoParser {
    CandoLexer  lexer;
    CandoToken  current;
    CandoToken  previous;
    CandoChunk *chunk_stack[64];   // stack of active chunks (nested functions)
    u32         chunk_depth;
    bool        had_error;
    char        error_buf[256];
    bool        eval_mode;          // true = compile for eval(), not top-level
    bool        last_stmt_was_expr; // used by cando_parse() for eval return
    // ... scope / local variable tables ...
} CandoParser;
```

```c
void cando_parser_init(CandoParser *p, const char *source, usize length,
                        CandoChunk *top_chunk);

bool cando_parse(CandoParser *p);              // parse entire source
const char *cando_parser_error(CandoParser *p);
```

### eval_mode flag

When `p->eval_mode = true` (set by `native_eval` in `source/lib/eval.c`):

- `cando_parse` emits `OP_RETURN 1` (or `OP_RETURN 0`) instead of `OP_HALT`.
- If the last statement was an expression, the trailing `OP_POP` is removed
  so its value remains on the stack for `OP_RETURN 1`.
- This enables `eval("1 + 2")` to return `3` to the caller.

---

## Scope system

The parser tracks local variables in a scope stack.  Each `{ ... }` block
introduces a new scope level.  Variables declared with `VAR` or `CONST` are
assigned a local slot index within the current chunk.

- `global` prefix or bare assignment to an undeclared name → `OP_DEF_GLOBAL`.
- `var`/`const` inside a function → `OP_DEF_LOCAL`.
- `const` values get `OP_DEF_CONST_LOCAL` or `OP_DEF_CONST_GLOBAL`; the VM
  enforces write protection.

Upvalues are resolved at parse time: when a nested function references a
variable from an enclosing scope, the parser records an upvalue descriptor in
the inner chunk, and `OP_CLOSURE` at runtime wires the `CandoUpvalue` pointers.

---

## Pratt expression parser

Expressions are parsed with a **Pratt parser** (precedence climbing):

```c
static void parse_expr(CandoParser *p, Precedence min_prec);
```

Each token type has a parse rule entry with:
- `prefix_fn` — called when the token starts an expression (e.g. number
  literal, unary `-`, `(` grouping).
- `infix_fn` — called when the token appears between two expressions (e.g.
  `+`, `-`, `*`, `.` field access, `(` call).
- `precedence` — the binding power of the infix operator.

This table-driven approach makes adding new operators straightforward: add
the token, fill in the rule row, and the precedence rules do the rest.

---

## Emitting instructions from parser rules

Parser rules emit directly to the **current chunk** (`cur(p)`):

```c
static void emit_op(CandoParser *p, CandoOpcode op);
static void emit_op_a(CandoParser *p, CandoOpcode op, u16 a);
static u32  emit_const(CandoParser *p, CandoValue v); // adds constant, emits OP_CONST A
```

### Adding a string constant

```c
CandoString *s = cando_string_new("hello", 5);
u32 ci = cando_chunk_add_constant(cur(p), cando_string_value(s));
emit_op_a(p, OP_CONST, (u16)ci);
```

### Emitting a forward jump (patching)

```c
// Emit a placeholder jump; returns the offset of the operand bytes
u32 hole = emit_jump(p, OP_JUMP_IF_FALSE);

// ... emit the then-body ...

// Patch the placeholder with the actual offset
patch_jump(p, hole);
```

---

## Nested function compilation

When the parser encounters a `FUNCTION` declaration:

1. A new `CandoChunk` is created via `cando_chunk_new()`.
2. It is pushed onto `p->chunk_stack`.
3. Parameters are registered as local slots.
4. The function body is compiled into the new chunk.
5. The chunk is popped, added as a constant to the **parent** chunk, and
   `OP_CLOSURE A` is emitted so the VM builds a `CandoClosure` at runtime.

---

## Anonymous FUNCTION expressions

When `TOK_FUNCTION` is followed by `TOK_LPAREN` (not an identifier), the
parser routes to `parse_function_expr` (produces a value) rather than
`parse_function` (declares a name).  This supports:

```cando
var square = FUNCTION(x) { RETURN x * x; };
eval("FUNCTION(a) { RETURN a * 2; }");
```
