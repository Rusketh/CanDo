/*
 * parser.h -- Cando Pratt recursive-descent compiler to bytecode.
 *
 * Parses Cando source text and emits bytecode into a VM CandoChunk using
 * the unified CandoOpcode instruction set.  No AST is built; opcodes are
 * emitted directly during the parse.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_PARSER_H
#define CANDO_PARSER_H

#include "../core/common.h"
#include "../core/value.h"
#include "../vm/opcodes.h"
#include "../vm/chunk.h"
#include "lexer.h"

/* -------------------------------------------------------------------------
 * Local variable tracking for scope resolution.
 *
 * At scope_depth == 0 (global scope) variable operations use the global
 * band (OP_LOAD_GLOBAL / OP_STORE_GLOBAL / OP_DEF_GLOBAL).
 * At scope_depth  > 0 (inside a block or function) they use the local band
 * (OP_LOAD_LOCAL / OP_STORE_LOCAL / OP_DEF_LOCAL) with a slot index.
 *
 * The locals[] / upvalue_specs[] tables grow dynamically: starting at
 * CANDO_LOCAL_INITIAL_CAP and doubling on demand up to CANDO_LOCAL_MAX,
 * which is set to the bytecode encoding ceiling (slot indices are u16).
 * ------------------------------------------------------------------------ */
#define CANDO_LOCAL_MAX         UINT16_MAX  /* hard cap = bytecode u16 limit */
#define CANDO_LOCAL_INITIAL_CAP 16          /* starting capacity per scope    */

/* Maximum number of named parameters in a function or class declaration.
 * Used to size the parser's stack-local param-name buffers; raising this
 * just costs a bit of compile-time stack space per function compiled.   */
#define CANDO_MAX_PARAMS        64

typedef struct {
    const char *name;     /* pointer into the source text (not owning)    */
    u32         len;      /* byte length of the name                      */
    int         depth;    /* scope depth when declared                    */
    bool        is_const; /* declared with CONST                          */
} CandoLocal;

/* -------------------------------------------------------------------------
 * CandoParser -- state for the single-pass recursive-descent compiler.
 * ------------------------------------------------------------------------ */
typedef struct {
    CandoLexer   lexer;
    CandoToken   current;
    CandoToken   previous;
    bool         had_error;
    bool         panic_mode;
    CandoChunk  *chunk;          /* chunk currently being compiled          */
    char         error_msg[512];

    /* Scope tracking ---------------------------------------------------- */
    CandoLocal  *locals;          /* heap buffer, grown via cando_realloc   */
    u32          local_count;
    u32          local_capacity;  /* allocated slots in locals[]            */
    int          scope_depth;    /* 0 = global scope                        */

    /* Multi-return spreading -------------------------------------------- */
    bool         last_expr_was_call;   /* true if last expr was a call      */
    bool         last_expr_was_unpack; /* true if last expr was ...call()   */

    /* Comma-list depth -------------------------------------------------- */
    /* Incremented when inside function args, array elements, object fields,
     * or any other comma-separated list.  When > 0, comparison operators
     * do NOT consume trailing commas for multi-comparison.                 */
    u32          call_depth;

    /* Eval mode --------------------------------------------------------- */
    bool         eval_mode;          /* emit OP_RETURN instead of OP_HALT  */
    bool         last_stmt_was_expr; /* last top-level stmt was expr stmt  */

    /* Pipe / filter body tracking --------------------------------------- */
    /* When in_pipe_body is true a `return` statement inside a ~>/~!> body
     * emits an expression + OP_JUMP (patched later) instead of OP_RETURN,
     * so it does not exit the enclosing function.                         */
    bool         in_pipe_body;
    u32          pipe_exits[16];     /* patch-list of OP_JUMP offsets      */
    u32          pipe_exit_count;

    /* Mask multi-push tracking ----------------------------------------- */
    /* Set by parse_mask to indicate how many values were actually pushed
     * onto the stack (pass_count).  All other expressions leave this at 1.
     * parse_var_decl uses this to count values correctly.                 */
    u32          last_multi_push;

    /* Ternary nesting depth -------------------------------------------- */
    /* Incremented while parsing the THEN branch of a `cond ? then : else`
     * expression so that ':' is treated as the ternary delimiter rather
     * than as the method-call infix operator.                            */
    u32          ternary_then_depth;

    /* Closure capture tracking ----------------------------------------- */
    /* When parsing the body of a nested function, `outer_locals` /
     * `outer_count` point at the enclosing function's local table.  An
     * identifier that doesn't resolve inside the current body but does
     * resolve in the outer table is captured: its outer-frame slot is
     * appended to upvalue_specs[] and referenced via OP_LOAD_UPVAL.  At
     * the matching OP_CLOSURE the recorded slots are written out as
     * capture metadata so the VM can snapshot them at runtime.         */
    CandoLocal  *outer_locals;
    u32          outer_count;
    u16         *upvalue_specs;   /* heap buffer, grown via cando_realloc   */
    u16          upvalue_count;
    u32          upvalue_capacity;

    /* Safe-access chain tracking (?., ?[) ------------------------------ */
    /* When true, every member-access infix in the current expression also
     * emits an OP_JUMP_IF_NULL guard so a null result mid-chain skips the
     * remainder of the chain.  Patches are recorded in safe_chain_jumps[]
     * and rewritten to point past the chain when parse_precedence finishes. */
    bool         in_safe_chain;
    u32          safe_chain_jumps[32];
    u32          safe_chain_count;
} CandoParser;

/* Initialise parser; `chunk` receives all emitted bytecode.
 * The chunk must already be allocated via cando_chunk_new().             */
CANDO_API void cando_parser_init(CandoParser *p, const char *source, usize len,
                       CandoChunk *chunk);

/* Compile full source.  Returns true on success. */
CANDO_API bool cando_parse(CandoParser *p);

/* Release any heap buffers owned by the parser (locals / upvalue_specs).
 * Safe to call once after cando_parser_init regardless of cando_parse outcome. */
CANDO_API void cando_parser_free(CandoParser *p);

/* Last error string, or NULL if no error. */
CANDO_API const char *cando_parser_error(const CandoParser *p);

#endif /* CANDO_PARSER_H */
