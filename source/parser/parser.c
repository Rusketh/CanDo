/*
 * parser.c -- Cando Pratt recursive-descent compiler to bytecode.
 *
 * Design:
 *   - Single-pass: no AST; opcodes are emitted as tokens are consumed.
 *   - Pratt (top-down operator precedence) for expressions.
 *   - Recursive-descent for statements.
 *   - Constants pool: strings (variable/property names, string literals)
 *     and numbers are stored as CandoValue in chunk->constants[].
 *   - Jump operands are 16-bit little-endian signed offsets; patching is
 *     handled by the VM chunk API (cando_chunk_patch_jump).
 *   - Scope tracking: depth 0 = global (OP_LOAD/STORE/DEF_GLOBAL),
 *     depth > 0 = local slots (OP_LOAD/STORE/DEF_LOCAL).
 *   - Error recovery: panic-mode synchronisation at statement boundaries.
 */

#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal Pratt infrastructure
 * ======================================================================= */

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,      /* =  +=  -=  *=  /=  %=  ^=          right-assoc */
    PREC_TERNARY,     /* ?:                                  right-assoc */
    PREC_OR,          /* ||                                               */
    PREC_AND,         /* &&                                               */
    PREC_BITOR,       /* |                                                */
    PREC_BITXOR,      /* |&                                               */
    PREC_BITAND,      /* &                                                */
    PREC_EQUALITY,    /* ==  !=                                           */
    PREC_COMPARISON,  /* <  >  <=  >=                                     */
    PREC_RANGE,       /* ->  <-                                           */
    PREC_SHIFT,       /* <<  >>                                           */
    PREC_TERM,        /* +  -                                             */
    PREC_FACTOR,      /* *  /  %                                          */
    PREC_POWER,       /* ^                                right-assoc     */
    PREC_PIPE_PREC,   /* ~>  ~!>                                          */
    PREC_UNARY,       /* unary - ! # ~                                    */
    PREC_CALL_PREC,   /* ()  []  .  ::                                    */
    PREC_PRIMARY,
} Precedence;

typedef void (*ParseFn)(CandoParser *p, bool can_assign);

typedef struct {
    ParseFn    prefix;
    ParseFn    infix;
    Precedence precedence;
} ParseRule;

/* Forward declarations */
static void        parse_expression(CandoParser *p);
static void        parse_precedence(CandoParser *p, Precedence min_prec);
static void        parse_statement(CandoParser *p);
static void        parse_block(CandoParser *p);
static void        parse_function_expr(CandoParser *p, bool can_assign);
static void        parse_class_expr(CandoParser *p, bool can_assign);
static const ParseRule *get_rule(CandoTokenType t);

/* ---- chunk shortcut --------------------------------------------------- */
static CandoChunk *cur(CandoParser *p) { return p->chunk; }

/* ---- error ------------------------------------------------------------- */
static void error_at(CandoParser *p, const CandoToken *tok, const char *msg)
{
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error  = true;
    snprintf(p->error_msg, sizeof(p->error_msg),
             "[line %u] Error at '%.*s': %s",
             tok->line, (int)tok->length, tok->start, msg);
}

static void error(CandoParser *p, const char *msg)
{
    error_at(p, &p->previous, msg);
}

static void error_current(CandoParser *p, const char *msg)
{
    error_at(p, &p->current, msg);
}

/* ---- token advancement ------------------------------------------------ */
static void advance(CandoParser *p)
{
    p->previous = p->current;
    for (;;) {
        p->current = cando_lexer_next(&p->lexer);
        if (p->current.type != TOK_ERROR) break;
        error_current(p, cando_lexer_error_msg(&p->lexer));
    }
}

static bool check(CandoParser *p, CandoTokenType t)
{
    return p->current.type == t;
}

static bool match(CandoParser *p, CandoTokenType t)
{
    if (!check(p, t)) return false;
    advance(p);
    return true;
}

static void consume(CandoParser *p, CandoTokenType t, const char *msg)
{
    if (check(p, t)) { advance(p); return; }
    error_current(p, msg);
}

/* ---- emit helpers ------------------------------------------------------ */

static u32 emit_line(CandoParser *p) { return p->previous.line; }

static void emit_op(CandoParser *p, CandoOpcode op)
{
    cando_chunk_emit_op(cur(p), op, emit_line(p));
}

static void emit_op_a(CandoParser *p, CandoOpcode op, u16 a)
{
    cando_chunk_emit_op_a(cur(p), op, a, emit_line(p));
}

static void emit_op_ab(CandoParser *p, CandoOpcode op, u16 a, u16 b)
{
    cando_chunk_emit_op_ab(cur(p), op, a, b, emit_line(p));
}

/* Emit a jump with placeholder operand; returns the patch position. */
static u32 emit_jump(CandoParser *p, CandoOpcode op)
{
    return cando_chunk_emit_jump(cur(p), op, emit_line(p));
}

/* Patch a forward jump at the position returned by emit_jump. */
static void patch_jump(CandoParser *p, u32 pos)
{
    cando_chunk_patch_jump(cur(p), pos);
}

/* Emit a backward loop instruction targeting loop_start. */
static void emit_loop(CandoParser *p, u32 loop_start)
{
    cando_chunk_emit_loop(cur(p), loop_start, emit_line(p));
}

/* ---- loop-mark helpers -------------------------------------------------
 * OP_LOOP_MARK carries two operands (OPFMT_A_B, 5 bytes):
 *   A = break_fwd  -- forward offset from (ip after instruction) to break target
 *   B = packed     -- bits[13:0]=cont_back (backward offset to continue target)
 *                     bits[15:14]=loop_type (CANDO_LOOP_*)
 *
 * emit_loop_mark: emits the instruction with a placeholder break_fwd (to be
 *   patched later) and the known cont_back computed from loop_start.
 *   Returns the patch position of the break_fwd operand.
 *
 * patch_loop_mark_break: patches break_fwd so ip+break_fwd lands at the
 *   current code position (the instruction after the loop).
 * ----------------------------------------------------------------------- */
static u32 emit_loop_mark(CandoParser *p, u32 loop_start, u8 loop_type)
{
    /* OP_LOOP_MARK is 5 bytes: [op][A_lo][A_hi][B_lo][B_hi]
     * After executing the instruction, ip = code_len + 5.
     * cont_back = ip_after - loop_start.                         */
    u32 mark_pos  = cur(p)->code_len;
    u32 ip_after  = mark_pos + 5;
    u16 cont_back = (u16)(ip_after - loop_start);
    u16 b_packed  = (cont_back & 0x3FFF) | ((u16)loop_type << 14);
    /* Emit with placeholder 0 for break_fwd; we'll patch it later. */
    emit_op_ab(p, OP_LOOP_MARK, 0, b_packed);
    /* The A operand (break_fwd) sits at mark_pos + 1. */
    return mark_pos + 1;
}

static void patch_loop_mark_break(CandoParser *p, u32 patch_at)
{
    /* break_fwd is relative to ip *after* the full 5-byte instruction.
     * The A operand is at patch_at; the instruction ends at patch_at + 4
     * (2 bytes for A + 2 bytes for B already emitted).
     * ip_after_instr = patch_at - 1 + 5 = patch_at + 4.
     * break_fwd = code_len - (patch_at + 4).                    */
    u32 ip_after = patch_at + 4; /* patch_at is 1 past the opcode byte */
    i16 off      = (i16)((i32)cur(p)->code_len - (i32)ip_after);
    cando_write_u16(&cur(p)->code[patch_at], (u16)off);
}

/* ---- constant helpers -------------------------------------------------- */

/* Intern a string constant; returns pool index.  Routes through
 * cando_chunk_intern_string so we only heap-allocate on a pool miss --
 * every identifier reference in the script flows through here.          */
static u16 str_const(CandoParser *p, const char *s, u32 len)
{
    return cando_chunk_intern_string(cur(p), s, len);
}

/* Intern the previous token's lexeme as a string constant. */
static u16 prev_name_const(CandoParser *p)
{
    return str_const(p, p->previous.start, p->previous.length);
}

/* ---- dynamic local / upvalue table growth ------------------------------ */

/* Grow locals[] to hold at least `needed` entries.  Returns false (and
 * raises a parser error) if the absolute cap CANDO_LOCAL_MAX is reached. */
static bool ensure_locals_capacity(CandoParser *p, u32 needed)
{
    if (needed <= p->local_capacity) return true;
    if (needed > CANDO_LOCAL_MAX) {
        error(p, "too many local variables in scope");
        return false;
    }
    u32 new_cap = p->local_capacity ? p->local_capacity : CANDO_LOCAL_INITIAL_CAP;
    while (new_cap < needed) {
        if (new_cap >= CANDO_LOCAL_MAX / 2) { new_cap = CANDO_LOCAL_MAX; break; }
        new_cap *= 2;
    }
    p->locals = (CandoLocal *)cando_realloc(p->locals,
                                            (usize)new_cap * sizeof(CandoLocal));
    p->local_capacity = new_cap;
    return true;
}

/* Same growth strategy for the upvalue capture-spec table. */
static bool ensure_upvalues_capacity(CandoParser *p, u32 needed)
{
    if (needed <= p->upvalue_capacity) return true;
    if (needed > CANDO_LOCAL_MAX) {
        error(p, "too many captured upvalues");
        return false;
    }
    u32 new_cap = p->upvalue_capacity ? p->upvalue_capacity
                                      : CANDO_LOCAL_INITIAL_CAP;
    while (new_cap < needed) {
        if (new_cap >= CANDO_LOCAL_MAX / 2) { new_cap = CANDO_LOCAL_MAX; break; }
        new_cap *= 2;
    }
    p->upvalue_specs = (u16 *)cando_realloc(p->upvalue_specs,
                                            (usize)new_cap * sizeof(u16));
    p->upvalue_capacity = new_cap;
    return true;
}

/* Saved enclosing-scope state captured when entering a nested function /
 * class body.  See enter_function_scope / leave_function_scope below.    */
typedef struct {
    CandoLocal *locals;
    u32         local_count;
    u32         local_capacity;
    int         scope_depth;
    CandoLocal *outer_locals;
    u32         outer_count;
    u16        *upvalue_specs;
    u16         upvalue_count;
    u32         upvalue_capacity;
} FnScopeSave;

/* Snapshot the current locals/upvalues tables and replace them with fresh
 * empty buffers so the nested function body parses against its own scope.
 * The saved outer locals[] becomes p->outer_locals so resolve_upvalue can
 * see captures into the immediate enclosing function.                    */
static void enter_function_scope(CandoParser *p, FnScopeSave *s)
{
    s->locals           = p->locals;
    s->local_count      = p->local_count;
    s->local_capacity   = p->local_capacity;
    s->scope_depth      = p->scope_depth;
    s->outer_locals     = p->outer_locals;
    s->outer_count      = p->outer_count;
    s->upvalue_specs    = p->upvalue_specs;
    s->upvalue_count    = p->upvalue_count;
    s->upvalue_capacity = p->upvalue_capacity;

    p->locals           = (CandoLocal *)cando_alloc(
                              sizeof(CandoLocal) * CANDO_LOCAL_INITIAL_CAP);
    p->local_count      = 0;
    p->local_capacity   = CANDO_LOCAL_INITIAL_CAP;
    p->scope_depth      = 1;
    p->outer_locals     = s->locals;
    p->outer_count      = s->local_count;
    p->upvalue_specs    = (u16 *)cando_alloc(
                              sizeof(u16) * CANDO_LOCAL_INITIAL_CAP);
    p->upvalue_count    = 0;
    p->upvalue_capacity = CANDO_LOCAL_INITIAL_CAP;
}

/* Restore enclosing scope.  The body's upvalue capture buffer is handed
 * back to the caller (ownership transferred via *out_specs / *out_count)
 * so it can be passed to emit_closure before being freed.                */
static void leave_function_scope(CandoParser *p, FnScopeSave *s,
                                 u16 **out_specs, u16 *out_count)
{
    *out_specs = p->upvalue_specs;
    *out_count = p->upvalue_count;

    cando_free(p->locals);

    p->locals           = s->locals;
    p->local_count      = s->local_count;
    p->local_capacity   = s->local_capacity;
    p->scope_depth      = s->scope_depth;
    p->outer_locals     = s->outer_locals;
    p->outer_count      = s->outer_count;
    p->upvalue_specs    = s->upvalue_specs;
    p->upvalue_count    = s->upvalue_count;
    p->upvalue_capacity = s->upvalue_capacity;
}

/* ---- scope helpers ----------------------------------------------------- */

static void scope_begin(CandoParser *p)
{
    p->scope_depth++;
}

static void scope_end(CandoParser *p)
{
    p->scope_depth--;
    /* Remove locals that belonged to the exited scope.  They were assigned
     * to stack slots via OP_DEF_LOCAL, so nothing to pop from the expr
     * stack; the slot is simply reclaimed by the compiler.               */
    while (p->local_count > 0 &&
           p->locals[p->local_count - 1].depth > p->scope_depth) {
        p->local_count--;
    }
}

/* Resolve a name to a local slot index; returns -1 if not found. */
static int resolve_local(CandoParser *p, const char *name, u32 len)
{
    for (int i = (int)p->local_count - 1; i >= 0; i--) {
        CandoLocal *loc = &p->locals[i];
        if (loc->len == len && memcmp(loc->name, name, len) == 0)
            return i;
    }
    return -1;
}

/* Declare a new local variable at current scope depth. */
static u32 declare_local(CandoParser *p, const char *name, u32 len,
                          bool is_const)
{
    if (!ensure_locals_capacity(p, p->local_count + 1)) return 0;
    u32 slot = p->local_count;
    CandoLocal *loc = &p->locals[p->local_count++];
    loc->name     = name;
    loc->len      = len;
    loc->depth    = p->scope_depth;
    loc->is_const = is_const;
    /* Update chunk metadata so the VM knows how many local slots to
     * allocate when entering the frame.                               */
    if (p->local_count > cur(p)->local_count)
        cur(p)->local_count = p->local_count;
    return slot;
}

/* ---- function/class body compilation helpers --------------------------- */

/* Parse a parenthesised parameter list of comma-separated identifiers.
 *
 * Caller has already consumed the opening '('.  The closing ')' is
 * consumed using `close_msg` as the "expected ')'" diagnostic.  Names
 * and lengths are stored into the caller's arrays up to `max`; past
 * the cap an error is raised and excess parameters are skipped.
 *
 * A trailing '...' (TOK_VARARG) terminates the list.  This is currently
 * accepted for forwards compatibility but not propagated to
 * chunk->has_vararg, so behaves like an arity-fixing terminator.
 *
 * Returns the parsed arity.                                             */
static u16 parse_param_list(CandoParser *p,
                            const char **names, u32 *lens,
                            u32 max,
                            const char *close_msg)
{
    u16 arity = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (check(p, TOK_RPAREN)) break;
            if (match(p, TOK_VARARG)) break;
            consume(p, TOK_IDENT, "expected parameter name");
            if (arity >= max) { error(p, "too many parameters"); break; }
            names[arity] = p->previous.start;
            lens[arity]  = p->previous.length;
            arity++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, close_msg);
    return arity;
}

/* Compile the body of a function-style construct (function expression,
 * function declaration, class constructor): consume the '{' opener
 * (using `open_brace_msg` as the diagnostic), emit the jump-skip over
 * the body, switch to a fresh inner scope, declare the call-frame
 * sentinel slot 0 and the named parameters, parse the block, emit the
 * implicit `return null`, and restore the outer scope.
 *
 * On return:
 *   *fn_start_out   -- code offset of the function body's first byte
 *                       (used as the constant-pool entry for OP_CLOSURE)
 *   *skip_jump_out  -- patch position of the OP_JUMP that skips the body
 *                       (caller must call patch_jump after the body)
 *   *uv_specs_out   -- ownership-transferred buffer of upvalue capture
 *                       slot indices; caller must cando_free after use.
 *   *uv_count_out   -- number of entries in *uv_specs_out.            */
static void compile_function_body(CandoParser *p,
                                  const char *const *param_names,
                                  const u32 *param_lens,
                                  u16 arity,
                                  const char *open_brace_msg,
                                  u32 *fn_start_out,
                                  u32 *skip_jump_out,
                                  u16 **uv_specs_out,
                                  u16 *uv_count_out)
{
    consume(p, TOK_LBRACE, open_brace_msg);

    *skip_jump_out = emit_jump(p, OP_JUMP);
    *fn_start_out  = cur(p)->code_len;

    FnScopeSave saved;
    enter_function_scope(p, &saved);

    /* Slot 0 is the function value in the call frame. */
    declare_local(p, "", 0, false);
    for (u16 i = 0; i < arity; i++)
        declare_local(p, param_names[i], param_lens[i], false);

    parse_block(p);

    emit_op(p, OP_NULL);
    emit_op_a(p, OP_RETURN, 1);

    leave_function_scope(p, &saved, uv_specs_out, uv_count_out);
}

/* =========================================================================
 * Prefix parse functions
 * ======================================================================= */

static void parse_number(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    char buf[64];
    u32 len = p->previous.length;
    if (len >= (u32)sizeof(buf)) len = (u32)sizeof(buf) - 1;
    memcpy(buf, p->previous.start, len);
    buf[len] = '\0';
    f64 val = strtod(buf, NULL);
    u16 idx = cando_chunk_add_const(cur(p), cando_number(val));
    emit_op_a(p, OP_CONST, idx);
}

/* Compile a backtick template string `lit${expr}lit...` as a chain of
 * concatenations. Each ${expr} is wrapped in toString(...) so non-string
 * values (numbers, bools, ...) interpolate correctly via the OP_ADD string-
 * concat path. The lexer has already validated brace balance for the whole
 * token; we re-walk the body here to split segments and re-lex each
 * expression substring with a sub-lexer. */
static void parse_template_string(CandoParser *p)
{
    const char *body  = p->previous.start + 1;
    u32         total = p->previous.length >= 2 ? p->previous.length - 2 : 0;
    u32         start_line = p->previous.line;

    bool emitted = false;  /* true once we've pushed something on the stack */
    u32  i = 0;

    while (i < total) {
        /* Scan literal segment up to the next "${" or end. */
        u32 lit_start = i;
        while (i < total) {
            char c = body[i];
            if (c == '\\' && i + 1 < total) { i += 2; continue; }
            if (c == '$' && i + 1 < total && body[i + 1] == '{') break;
            i++;
        }
        u32 lit_len = i - lit_start;
        if (lit_len > 0) {
            u16 idx = str_const(p, body + lit_start, lit_len);
            emit_op_a(p, OP_CONST, idx);
            if (emitted) emit_op(p, OP_ADD);
            emitted = true;
        }
        if (i >= total) break;

        /* Found "${": find the matching '}'. */
        i += 2;
        u32 expr_start = i;
        int depth = 1;
        while (i < total) {
            char c = body[i];
            if (c == '\\' && i + 1 < total) { i += 2; continue; }
            if      (c == '{') depth++;
            else if (c == '}') { depth--; if (depth == 0) break; }
            i++;
        }
        u32 expr_len = i - expr_start;
        if (i < total && body[i] == '}') i++;  /* consume closing '}' */

        /* Emit toString(<expr>) — load the global, parse the expression,
         * then OP_CALL with one argument. */
        u16 fn_idx = str_const(p, "toString", 8);
        emit_op_a(p, OP_LOAD_GLOBAL, fn_idx);

        /* Trim leading whitespace so an empty `${ }` is detected cleanly. */
        u32 trim_start = expr_start, trim_end = expr_start + expr_len;
        while (trim_start < trim_end &&
               (body[trim_start] == ' '  || body[trim_start] == '\t' ||
                body[trim_start] == '\n' || body[trim_start] == '\r'))
            trim_start++;

        if (trim_start >= trim_end) {
            /* Empty interpolation: pass an empty string so toString(...) is
             * still well-formed. */
            u16 empty_idx = str_const(p, "", 0);
            emit_op_a(p, OP_CONST, empty_idx);
        } else {
            /* Save outer parser state, swap in a sub-lexer over the
             * expression substring, parse, then restore.                  */
            CandoLexer saved_lexer    = p->lexer;
            CandoToken saved_current  = p->current;
            CandoToken saved_previous = p->previous;
            bool       saved_panic    = p->panic_mode;
            u32        saved_call_d   = p->call_depth;
            bool       saved_was_call = p->last_expr_was_call;
            bool       saved_was_unp  = p->last_expr_was_unpack;
            u32        saved_mpush    = p->last_multi_push;

            cando_lexer_init(&p->lexer, body + expr_start, expr_len);
            p->lexer.line = start_line;
            p->panic_mode = false;
            p->call_depth = 0;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            p->last_multi_push      = 1;

            advance(p);  /* prime current with first token of the sub-expr */
            parse_expression(p);
            /* If the inner expression was itself a call, spread its return
             * values into our toString argument list so the static argc=1
             * gets adjusted at runtime to the actual count.               */
            if (p->last_expr_was_call) emit_op(p, OP_SPREAD_RET);

            p->lexer    = saved_lexer;
            p->current  = saved_current;
            p->previous = saved_previous;
            p->panic_mode           = saved_panic;
            p->call_depth           = saved_call_d;
            p->last_expr_was_call   = saved_was_call;
            p->last_expr_was_unpack = saved_was_unp;
            p->last_multi_push      = saved_mpush;
        }

        emit_op_a(p, OP_CALL, 1);
        if (emitted) emit_op(p, OP_ADD);
        emitted = true;
    }

    if (!emitted) {
        u16 idx = str_const(p, "", 0);
        emit_op_a(p, OP_CONST, idx);
    }

    /* The result is a single string value. */
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->last_multi_push      = 1;
}

static void parse_string_literal(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    if (p->previous.type == TOK_STRING_BT) {
        parse_template_string(p);
        return;
    }
    const char *s   = p->previous.start + 1;
    u32         len = p->previous.length >= 2 ? p->previous.length - 2 : 0;
    u16 idx = str_const(p, s, len);
    emit_op_a(p, OP_CONST, idx);
}

static void parse_literal(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    switch (p->previous.type) {
    case TOK_NULL_KW:  emit_op(p, OP_NULL);  break;
    case TOK_TRUE_KW:  emit_op(p, OP_TRUE);  break;
    case TOK_FALSE_KW: emit_op(p, OP_FALSE); break;
    default: CANDO_UNREACHABLE();
    }
}

static void parse_mask_emit(CandoParser *p, bool *bits, u32 n);

static void parse_grouping(CandoParser *p, bool can_assign)
{
    (void)can_assign;

    /* Mask syntax: (~.~) expr  or  (.) expr  or  (...) expr  etc.
     * A leading ~ or . (or ... expanding to three skips) inside the
     * parens signals a mask pattern rather than a grouping expression. */
    if (check(p, TOK_TILDE) || check(p, TOK_DOT) || check(p, TOK_VARARG)) {
        bool bits[32];
        u32  n = 0;
        while (n < 32 && (check(p, TOK_TILDE) || check(p, TOK_DOT) || check(p, TOK_VARARG))) {
            advance(p);
            if (p->previous.type == TOK_VARARG) {
                /* ... expands to three skip (.) bits */
                for (int k = 0; k < 3 && n < 32; k++)
                    bits[n++] = false;
            } else {
                bits[n++] = (p->previous.type == TOK_TILDE);
            }
        }
        consume(p, TOK_RPAREN, "expected ')' after mask pattern");
        parse_mask_emit(p, bits, n);
        return;
    }

    /* Explicit parens allow multi-comparison (e.g. (i == 1, 3, 5)) even
     * inside comma-separated contexts like function arguments.  Zero
     * call_depth so MULTI_CMP can consume the comma list, then restore. */
    u32 saved_depth = p->call_depth;
    p->call_depth = 0;
    parse_expression(p);
    p->call_depth = saved_depth;
    consume(p, TOK_RPAREN, "expected ')' after expression");
}

static void parse_unary(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    CandoTokenType op = p->previous.type;
    parse_precedence(p, PREC_UNARY);
    switch (op) {
    case TOK_MINUS: emit_op(p, OP_NEG); break;
    case TOK_BANG:  emit_op(p, OP_NOT); break;
    case TOK_HASH:  emit_op(p, OP_LEN); break;
    default: break;
    }
    p->last_expr_was_call = false;
}

/* Look up `name` in the immediate enclosing function's local table; if
 * found, register an upvalue capture (de-duplicating by slot) and return
 * its index in upvalue_specs[].  Returns -1 if no outer local matches.
 *
 * Only the *immediate* enclosing function is consulted -- closures over
 * locals more than one function deep are not supported yet, but
 * single-level capture covers script-body locals that the user is most
 * likely to close over (e.g. loop variables in the script's main scope).
 */
static int resolve_upvalue(CandoParser *p, const char *name, u32 len)
{
    if (!p->outer_locals || p->outer_count == 0) return -1;
    for (int i = (int)p->outer_count - 1; i >= 0; i--) {
        CandoLocal *loc = &p->outer_locals[i];
        if (loc->len == len && memcmp(loc->name, name, len) == 0) {
            /* Already captured? */
            for (u16 j = 0; j < p->upvalue_count; j++) {
                if (p->upvalue_specs[j] == (u16)i) return (int)j;
            }
            if (!ensure_upvalues_capacity(p, (u32)p->upvalue_count + 1))
                return -1;
            p->upvalue_specs[p->upvalue_count] = (u16)i;
            return (int)p->upvalue_count++;
        }
    }
    return -1;
}

/* Emit load for a name, using local slot if available, then closure
 * upvalue, else global. */
static void emit_load(CandoParser *p, const char *name, u32 len)
{
    int slot = resolve_local(p, name, len);
    if (slot >= 0) {
        emit_op_a(p, OP_LOAD_LOCAL, (u16)slot);
        return;
    }
    int uv = resolve_upvalue(p, name, len);
    if (uv >= 0) {
        emit_op_a(p, OP_LOAD_UPVAL, (u16)uv);
        return;
    }
    u16 idx = str_const(p, name, len);
    emit_op_a(p, OP_LOAD_GLOBAL, idx);
}

/* Emit store for a name. */
static void emit_store(CandoParser *p, const char *name, u32 len)
{
    int slot = resolve_local(p, name, len);
    if (slot >= 0) {
        emit_op_a(p, OP_STORE_LOCAL, (u16)slot);
        return;
    }
    int uv = resolve_upvalue(p, name, len);
    if (uv >= 0) {
        emit_op_a(p, OP_STORE_UPVAL, (u16)uv);
        return;
    }
    u16 idx = str_const(p, name, len);
    emit_op_a(p, OP_STORE_GLOBAL, idx);
}

/* Emit OP_CLOSURE plus the variable-length capture metadata that the VM
 * uses to snapshot upvalue slots at runtime.  Format on the wire:
 *
 *     OP_CLOSURE   (1 byte opcode + 2-byte u16 const index)
 *     u16          capture_count
 *     u16 * count  outer-frame slot indices (one per upvalue)
 */
static void emit_closure(CandoParser *p, u16 pc_idx,
                         const u16 *captures, u16 count)
{
    emit_op_a(p, OP_CLOSURE, pc_idx);
    u32 line = p->previous.line;
    cando_chunk_emit_byte(cur(p), (u8)(count & 0xFF), line);
    cando_chunk_emit_byte(cur(p), (u8)(count >> 8),   line);
    for (u16 i = 0; i < count; i++) {
        u16 s = captures[i];
        cando_chunk_emit_byte(cur(p), (u8)(s & 0xFF), line);
        cando_chunk_emit_byte(cur(p), (u8)(s >> 8),   line);
    }
}

/* Identifier in expression position: variable load or assignment target. */
static void parse_variable_expr(CandoParser *p, bool can_assign)
{
    const char *name = p->previous.start;
    u32         len  = p->previous.length;

    if (can_assign) {
        /* Compound assignment operators */
        CandoOpcode compound_op = OP_NOP;
        bool is_compound = false;

        if      (check(p, TOK_PLUS_ASSIGN))    { compound_op = OP_ADD; is_compound = true; }
        else if (check(p, TOK_MINUS_ASSIGN))   { compound_op = OP_SUB; is_compound = true; }
        else if (check(p, TOK_STAR_ASSIGN))    { compound_op = OP_MUL; is_compound = true; }
        else if (check(p, TOK_SLASH_ASSIGN))   { compound_op = OP_DIV; is_compound = true; }
        else if (check(p, TOK_PERCENT_ASSIGN)) { compound_op = OP_MOD; is_compound = true; }
        else if (check(p, TOK_CARET_ASSIGN))   { compound_op = OP_POW; is_compound = true; }

        if (is_compound) {
            advance(p);
            emit_load(p, name, len);
            parse_expression(p);
            emit_op(p, compound_op);
            emit_store(p, name, len);
            return;
        }

        if (match(p, TOK_ASSIGN)) {
            parse_expression(p);
            emit_store(p, name, len);
            return;
        }

        /* Postfix ++ */
        if (match(p, TOK_INCR)) {
            emit_load(p, name, len);
            emit_op(p, OP_DUP);
            u16 one = cando_chunk_add_const(cur(p), cando_number(1.0));
            emit_op_a(p, OP_CONST, one);
            emit_op(p, OP_ADD);
            emit_store(p, name, len);
            emit_op(p, OP_POP);
            return;
        }
        /* Postfix -- */
        if (match(p, TOK_DECR)) {
            emit_load(p, name, len);
            emit_op(p, OP_DUP);
            u16 one = cando_chunk_add_const(cur(p), cando_number(1.0));
            emit_op_a(p, OP_CONST, one);
            emit_op(p, OP_SUB);
            emit_store(p, name, len);
            emit_op(p, OP_POP);
            return;
        }
    }

    emit_load(p, name, len);
}

/* Array literal: [ expr, expr, ... ] */
static void parse_array_literal(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    u16 count = 0;
    if (!check(p, TOK_RBRACKET)) {
        p->call_depth++;
        do {
            if (check(p, TOK_RBRACKET)) break;
            p->last_multi_push      = 1;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            parse_expression(p);
            if (p->last_expr_was_unpack) {
                /* ...arr spread: OP_UNPACK expands the array onto the stack
                 * and sets last_ret_count; OP_ARRAY_SPREAD accumulates the
                 * extra elements in array_extra (separate from spread_extra
                 * so subsequent function calls do not consume it).       */
                emit_op(p, OP_UNPACK);
                emit_op(p, OP_ARRAY_SPREAD);
                if (count == 0xFFFF) { error(p, "too many array elements"); break; }
                count++;
            } else {
                /* Literal, masked list, function call, or other expression.
                 * For masks: last_multi_push = compile-time pass count.
                 * For calls: last_multi_push = 1 (single slot; multi-return
                 * function calls in arrays keep existing behaviour).     */
                u16 add = (u16)p->last_multi_push;
                if ((u32)count + add > 0xFFFF) { error(p, "too many array elements"); break; }
                count += add;
            }
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    }
    consume(p, TOK_RBRACKET, "expected ']' after array elements");
    emit_op_a(p, OP_NEW_ARRAY, count);
}

/* Object literal: { key: expr, key: expr, ... }
 *
 * The VM's OP_NEW_OBJECT creates an empty object.  Each key-value pair
 * is set with OP_SET_FIELD which pops the value but leaves the object
 * on the stack (peek semantics), so the object persists for chaining.   */
static void parse_object_literal(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    emit_op(p, OP_NEW_OBJECT);   /* push empty object */

    if (!check(p, TOK_RBRACE)) {
        p->call_depth++;
        do {
            if (check(p, TOK_RBRACE)) break;
            /* Key: identifier or string */
            u16 key_idx;
            if (check(p, TOK_IDENT)) {
                advance(p);
                key_idx = prev_name_const(p);
            } else if (check(p, TOK_STRING_DQ) || check(p, TOK_STRING_SQ)) {
                advance(p);
                const char *s = p->previous.start + 1;
                u32 len = p->previous.length >= 2 ? p->previous.length - 2 : 0;
                key_idx = str_const(p, s, len);
            } else {
                error_current(p, "expected property key");
                break;
            }
            consume(p, TOK_COLON, "expected ':' after object key");
            parse_expression(p);
            /* OP_SET_FIELD: val=pop, obj=peek, obj[constants[A]] = val  */
            emit_op_a(p, OP_SET_FIELD, key_idx);
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    }
    consume(p, TOK_RBRACE, "expected '}' after object literal");
    /* object is still on the stack */
}

/* Unpack prefix: ...expr
 * Parses the following expression and marks it as an unpack.  Only valid
 * before a function/method call in a comparison context; the comparison
 * handler then emits a _SPREAD opcode to compare against all return values.
 * In other contexts the ... is consumed and the expression is evaluated
 * normally (callers are responsible for checking last_expr_was_unpack).    */
static void parse_unpack(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    parse_precedence(p, PREC_UNARY);
    /* Mark as unpack regardless of whether the operand was a call so that
     * ...variable spread works in array literals as well as ...func().  */
    p->last_expr_was_unpack = true;
}

/* =========================================================================
 * Infix parse functions
 * ======================================================================= */

/* Emit a comparison opcode for parse_binary that handles multi-value
 * RHS forms.  Caller has already parsed the RHS and snapshotted the
 * resulting per-expression flags (so the >/>= range-check branch can
 * inspect the same values before dispatching here).
 *
 * Rules:
 *   ...myfunc()        → spread_op  (compare against all return values)
 *   (~.~) myfunc()     → stack_op n (compare against n mask-selected values)
 *   plain call         → OP_TRUNCATE_RET + plain_op (first return only)
 *   a, b, c            → stack_op n (user-defined list; truncates any calls)
 *   plain value        → plain_op
 *
 * `next_prec` is the precedence to re-enter parse_precedence with for
 * each comma-separated value in the user-defined list form.
 *
 * Returns true iff the plain-value (single-comparison) branch fired,
 * so callers can apply the >/>= chained-comparison diagnostic.        */
static bool emit_multi_cmp(CandoParser *p,
                           bool was_call, bool was_unpack, u32 mpush,
                           CandoOpcode plain_op,
                           CandoOpcode stack_op,
                           CandoOpcode spread_op,
                           Precedence next_prec)
{
    if (was_unpack) {
        emit_op(p, spread_op);
        return false;
    }
    if (mpush > 1 && !(p->call_depth == 0 && check(p, TOK_COMMA))) {
        emit_op_a(p, stack_op, (u16)mpush);
        return false;
    }
    if (p->call_depth == 0 && check(p, TOK_COMMA)) {
        u16 n = (mpush > 1) ? (u16)mpush : 1;
        if (n == 1 && was_call) emit_op(p, OP_TRUNCATE_RET);
        while (match(p, TOK_COMMA)) {
            p->last_multi_push      = 1;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            parse_precedence(p, next_prec);
            if (p->last_expr_was_call && !p->last_expr_was_unpack)
                emit_op(p, OP_TRUNCATE_RET);
            n = (u16)(n + p->last_multi_push);
        }
        emit_op_a(p, stack_op, n);
        return false;
    }
    if (was_call) emit_op(p, OP_TRUNCATE_RET);
    emit_op(p, plain_op);
    return true;
}

/* Snapshot the per-expression flags left over from the just-parsed RHS
 * and clear last_expr_was_unpack (the comparison consumed it).         */
static inline void cmp_snapshot(CandoParser *p,
                                bool *was_call, bool *was_unpack, u32 *mpush)
{
    *was_call   = p->last_expr_was_call;
    *was_unpack = p->last_expr_was_unpack;
    *mpush      = p->last_multi_push;
    p->last_expr_was_unpack = false;
}

/* Simple binary-infix tokens that map 1:1 to a single emit_op call.
 * Entries default to OP_CONST (= 0), which is never a valid binary op,
 * so a non-zero value at SIMPLE_BINOP[op] flags a simple dispatch. */
static const CandoOpcode SIMPLE_BINOP[TOK_COUNT] = {
    [TOK_PLUS]       = OP_ADD,
    [TOK_MINUS]      = OP_SUB,
    [TOK_STAR]       = OP_MUL,
    [TOK_SLASH]      = OP_DIV,
    [TOK_PERCENT]    = OP_MOD,
    [TOK_CARET]      = OP_POW,
    [TOK_AMP]        = OP_BIT_AND,
    [TOK_BITOR]      = OP_BIT_OR,
    [TOK_BITXOR]     = OP_BIT_XOR,
    [TOK_LSHIFT]     = OP_LSHIFT,
    [TOK_RSHIFT]     = OP_RSHIFT,
    [TOK_RANGE_ASC]  = OP_RANGE_ASC,
    [TOK_RANGE_DESC] = OP_RANGE_DESC,
};

static void parse_binary(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    CandoTokenType op = p->previous.type;
    const ParseRule *rule = get_rule(op);
    Precedence next = (op == TOK_CARET)
                      ? rule->precedence
                      : (Precedence)(rule->precedence + 1);
    /* Reset before parsing RHS so we get a clean read of what was parsed. */
    p->last_multi_push      = 1;
    p->last_expr_was_unpack = false;
    parse_precedence(p, next);

    /* Fast path: arithmetic / bitwise / range tokens map straight to a
     * single opcode via SIMPLE_BINOP.  Everything else (comparisons)
     * has multi-value RHS handling and falls into the switch below.   */
    CandoOpcode simple = SIMPLE_BINOP[op];
    if (simple) {
        emit_op(p, simple);
        p->last_expr_was_call   = false;
        p->last_expr_was_unpack = false;
        return;
    }

    switch (op) {
    case TOK_EQ:
    case TOK_NEQ:
    case TOK_LT:
    case TOK_LEQ: {
        bool was_call, was_unpack; u32 mpush;
        cmp_snapshot(p, &was_call, &was_unpack, &mpush);
        CandoOpcode plain_op  = (op == TOK_EQ)  ? OP_EQ
                              : (op == TOK_NEQ) ? OP_NEQ
                              : (op == TOK_LT)  ? OP_LT
                                                : OP_LEQ;
        CandoOpcode stack_op  = (op == TOK_EQ)  ? OP_EQ_STACK
                              : (op == TOK_NEQ) ? OP_NEQ_STACK
                              : (op == TOK_LT)  ? OP_LT_STACK
                                                : OP_LEQ_STACK;
        CandoOpcode spread_op = (op == TOK_EQ)  ? OP_EQ_SPREAD
                              : (op == TOK_NEQ) ? OP_NEQ_SPREAD
                              : (op == TOK_LT)  ? OP_LT_SPREAD
                                                : OP_LEQ_SPREAD;
        (void)emit_multi_cmp(p, was_call, was_unpack, mpush,
                             plain_op, stack_op, spread_op, next);
        break;
    }
    case TOK_GT:
    case TOK_GEQ: {
        /* Convergent range check: min > val < max  (or >=, <=).
         * Stack before: [..., min, val]  →  parse max  →  OP_RANGE_CHECK.
         * Multi-comparison (comma) takes priority over chained operators. */
        bool was_call, was_unpack; u32 mpush;
        cmp_snapshot(p, &was_call, &was_unpack, &mpush);
        CandoOpcode plain_op  = (op == TOK_GT) ? OP_GT        : OP_GEQ;
        CandoOpcode stack_op  = (op == TOK_GT) ? OP_GT_STACK  : OP_GEQ_STACK;
        CandoOpcode spread_op = (op == TOK_GT) ? OP_GT_SPREAD : OP_GEQ_SPREAD;
        CandoTokenType right_tok = p->current.type;

        if (right_tok == TOK_LT || right_tok == TOK_LEQ) {
            /* Range check — truncate RHS call to first value if needed. */
            if (was_call && !was_unpack) emit_op(p, OP_TRUNCATE_RET);
            bool left_inc  = (op == TOK_GEQ);
            bool right_inc = (right_tok == TOK_LEQ);
            advance(p);
            p->last_multi_push      = 1;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            parse_precedence(p, PREC_COMPARISON + 1);
            if (p->last_expr_was_call && !p->last_expr_was_unpack)
                emit_op(p, OP_TRUNCATE_RET);
            emit_op_a(p, OP_RANGE_CHECK,
                      (u16)((left_inc ? 1 : 0) | (right_inc ? 2 : 0)));
            break;
        }

        bool plain_branch = emit_multi_cmp(p, was_call, was_unpack, mpush,
                                           plain_op, stack_op, spread_op, next);
        /* Chained-comparison diagnostic: only meaningful when the plain
         * single-comparison opcode was emitted (LT/LEQ already took the
         * range path above, so check only GT/GEQ here).                 */
        if (plain_branch && (right_tok == TOK_GT || right_tok == TOK_GEQ)) {
            error_current(p, "cannot chain comparison operators; use && to combine");
        }
        break;
    }
    default: break;
    }
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
}

/* Logical AND: short-circuit using OP_AND_JUMP (peeks top, jumps if falsy).
 *   [left]
 *   OP_AND_JUMP → end    ; if falsy, left stays as result; jump past right
 *   OP_POP               ; discard truthy left
 *   [right]              ; right becomes result
 * end:
 */
static void parse_and(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    u32 end_jump = emit_jump(p, OP_AND_JUMP);
    emit_op(p, OP_POP);
    parse_precedence(p, PREC_AND);
    patch_jump(p, end_jump);
}

/* Logical OR: short-circuit using OP_OR_JUMP (peeks top, jumps if truthy).
 *   [left]
 *   OP_OR_JUMP → end     ; if truthy, left stays as result; jump past right
 *   OP_POP               ; discard falsy left
 *   [right]              ; right becomes result
 * end:
 */
static void parse_or(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    u32 end_jump = emit_jump(p, OP_OR_JUMP);
    emit_op(p, OP_POP);
    parse_precedence(p, PREC_OR);
    patch_jump(p, end_jump);
}

/* Record a safe-chain null-guard jump.  Called from access infix handlers
 * when in_safe_chain is true (or from the ?. / ?[ handlers themselves).
 * The patch position is stored so parse_precedence can rewrite it to point
 * past the entire chain once parsing finishes.                            */
static void emit_safe_chain_guard(CandoParser *p)
{
    u32 patch = emit_jump(p, OP_JUMP_IF_NULL);
    if (p->safe_chain_count <
        (u32)(sizeof(p->safe_chain_jumps) / sizeof(p->safe_chain_jumps[0]))) {
        p->safe_chain_jumps[p->safe_chain_count++] = patch;
    } else {
        error(p, "safe-access chain too long");
    }
}

/* Property access: expr.name */
static void parse_dot(CandoParser *p, bool can_assign)
{
    /* Allow keywords as property names so that e.g. obj.catch, obj.then,
     * obj.class are all valid (mirrors JavaScript behaviour). */
    if (p->current.type != TOK_IDENT &&
            (p->current.type < TOK_IF || p->current.type > TOK_PIPE_KW)) {
        error_current(p, "expected property name after '.'");
        return;
    }
    advance(p);
    u16 idx = prev_name_const(p);

    if (can_assign && match(p, TOK_ASSIGN)) {
        parse_expression(p);
        emit_op_a(p, OP_SET_FIELD, idx);
    } else {
        if (p->in_safe_chain) emit_safe_chain_guard(p);
        emit_op_a(p, OP_GET_FIELD, idx);
    }
}

/* Safe property access: expr ?. name
 * If receiver is null, leaves null on the stack and short-circuits the rest
 * of the chain.  Otherwise behaves like '.'.                              */
static void parse_safe_dot(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    if (p->current.type != TOK_IDENT &&
            (p->current.type < TOK_IF || p->current.type > TOK_PIPE_KW)) {
        error_current(p, "expected property name after '?.'");
        return;
    }
    advance(p);
    u16 idx = prev_name_const(p);

    /* Activate safe-chain mode for the remainder of this expression. */
    p->in_safe_chain = true;
    emit_safe_chain_guard(p);
    emit_op_a(p, OP_GET_FIELD, idx);
}

/* Subscript: expr[expr] */
static void parse_subscript(CandoParser *p, bool can_assign)
{
    /* If we're in a safe-chain, guard the receiver before evaluating the
     * index so a null receiver cleanly skips index evaluation.  We emit
     * the guard here while the receiver is still on TOS.                  */
    if (p->in_safe_chain) emit_safe_chain_guard(p);
    parse_expression(p);
    consume(p, TOK_RBRACKET, "expected ']' after index");
    if (can_assign && match(p, TOK_ASSIGN)) {
        parse_expression(p);
        emit_op(p, OP_SET_INDEX);
    } else {
        emit_op(p, OP_GET_INDEX);
    }
}

/* Safe subscript: expr ?[ expr ]
 * If receiver is null, leaves null on the stack and short-circuits the rest
 * of the chain.  Otherwise behaves like '['.                              */
static void parse_safe_subscript(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    p->in_safe_chain = true;
    emit_safe_chain_guard(p);   /* guards the receiver before index evaluation */
    parse_expression(p);
    consume(p, TOK_RBRACKET, "expected ']' after index");
    emit_op(p, OP_GET_INDEX);
}

/* Function call: expr(arg, arg, ...) */
static void parse_call(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    /* In a safe-access chain, calling a null callable should short-circuit
     * to null rather than raise.  Emit the guard before evaluating args.   */
    if (p->in_safe_chain) emit_safe_chain_guard(p);
    u16 argc = 0;
    if (!check(p, TOK_RPAREN)) {
        p->call_depth++;
        do {
            if (check(p, TOK_RPAREN)) break;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            p->last_multi_push      = 1;
            parse_expression(p);
            if (p->last_expr_was_call) {
                /* Function call (including single-call masked calls): spread
                 * all return values; OP_SPREAD_RET + runtime spread_extra
                 * communicates the actual count to OP_CALL.              */
                emit_op(p, OP_SPREAD_RET);
                if (argc == 0xFFFF) { error(p, "too many arguments"); break; }
                argc++;
            } else {
                /* Non-call expression (literal, masked list, etc.): use the
                 * compile-time push count set by parse_mask_emit.        */
                u16 add = (u16)p->last_multi_push;
                if ((u32)argc + add > 0xFFFF) { error(p, "too many arguments"); break; }
                argc += add;
            }
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    }
    consume(p, TOK_RPAREN, "expected ')' after arguments");
    emit_op_a(p, OP_CALL, argc);
    p->last_expr_was_call = true;
}

/* Fluent call: expr::method(args) — returns receiver
 * OP_FLUENT_CALL is OPFMT_A_B: A = name const index, B = argc           */
static void parse_fluent(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    consume(p, TOK_IDENT, "expected method name after '::'");
    u16 name_idx = prev_name_const(p);
    consume(p, TOK_LPAREN, "expected '(' after fluent method name");
    /* Guard the receiver in a safe-access chain. */
    /* Note: the receiver was already on TOS when '::' was parsed; the
     * method name is encoded as constant operand of OP_FLUENT_CALL.       */
    /* We can't insert the guard before name parsing (already past it);
     * but the receiver hasn't been popped — it's still on TOS — so emit
     * the guard now, before argument evaluation.                          */
    if (p->in_safe_chain) emit_safe_chain_guard(p);
    u16 argc = 0;
    if (!check(p, TOK_RPAREN)) {
        p->call_depth++;
        do {
            if (check(p, TOK_RPAREN)) break;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            parse_expression(p);
            if (p->last_expr_was_call) emit_op(p, OP_SPREAD_RET);
            if (argc == 0xFFFF) { error(p, "too many arguments"); break; }
            argc++;
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    }
    consume(p, TOK_RPAREN, "expected ')' after fluent arguments");
    emit_op_ab(p, OP_FLUENT_CALL, name_idx, argc);
    p->last_expr_was_call = true;
}

/* Method call: expr:method(args) — using single colon (Cando style)
 * OP_METHOD_CALL is OPFMT_A_B: A = name const index, B = argc           */
static void parse_method_call(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    consume(p, TOK_IDENT, "expected method name after ':'");
    u16 name_idx = prev_name_const(p);
    consume(p, TOK_LPAREN, "expected '(' after method name");
    /* Guard the receiver in a safe-access chain. */
    if (p->in_safe_chain) emit_safe_chain_guard(p);
    u16 argc = 0;
    if (!check(p, TOK_RPAREN)) {
        p->call_depth++;
        do {
            if (check(p, TOK_RPAREN)) break;
            p->last_expr_was_call   = false;
            p->last_expr_was_unpack = false;
            parse_expression(p);
            if (p->last_expr_was_call) emit_op(p, OP_SPREAD_RET);
            if (argc == 0xFFFF) { error(p, "too many arguments"); break; }
            argc++;
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    }
    consume(p, TOK_RPAREN, "expected ')' after method arguments");
    emit_op_ab(p, OP_METHOD_CALL, name_idx, argc);
    p->last_expr_was_call = true;
}

/* Mask selector: (~.~) or (.) or (...) before a comma-separated expression list.
 *
 *   ~  = consume / pass the value (keep it on the stack)
 *   .  = skip the value (drop it from the stack)
 *   ...inside parens expands to three skip (.) bits
 *
 * Examples:
 *   (~.~) 1, 3, 5  →  pushes 1 and 5  (3 is discarded)
 *   (~) 1, 2       →  pushes 1  (pure-~: extras outside mask are skipped)
 *   (.) 1, 2       →  pushes 2  (pure-.: extras outside mask are passed as ~)
 *
 * Pure-mask rules (all one type):
 *   Pure ~ : the ~ bits consume their values; values outside the mask are skipped.
 *   Pure . : the . bits skip their values; values outside the mask are passed (~).
 * Mixed   : each value is covered explicitly; no implicit extras are consumed.
 *
 * Sets p->last_multi_push to the number of values actually left on the stack. */
static void parse_mask_emit(CandoParser *p, bool *bits, u32 n)
{
    /* Determine if the mask is pure (all same type) or mixed. */
    bool first_bit = bits[0];
    bool is_pure   = true;
    for (u32 i = 1; i < n; i++) {
        if (bits[i] != first_bit) { is_pure = false; break; }
    }

    u32 pass_count = 0;
    /* Save the outer list depth before we enter our own sub-expression
     * context.  If outer_depth > 0 (inside an array literal, a function
     * call's argument list, etc.) any comma that follows the first
     * expression belongs to the outer list, not to the mask.  In that
     * situation the mask must always be treated as a single-expression
     * mask rather than a comma-separated value list.                     */
    u32 outer_depth = p->call_depth;
    p->call_depth++;

    /* Parse the first expression and detect whether it is a sole function
     * call that spreads multiple return values onto the stack.  In that
     * case there will be no comma following the call (or we are inside an
     * outer list where the comma belongs to the outer list), and we emit
     * a single OP_MASK_APPLY rather than per-value PASS/SKIP ops.        */
    p->last_expr_was_call = false;
    parse_precedence(p, PREC_ASSIGN);
    /* In an outer list context a trailing comma is the list separator —
     * not a mask value separator — so treat the mask as single-expression. */
    bool no_list_comma = (outer_depth > 0) || !check(p, TOK_COMMA);
    bool single_call   = p->last_expr_was_call && no_list_comma;

    if (single_call) {
        /* The function already pushed its return values onto the stack.
         * Build a bitmask (bit i=1 → keep, 0 → skip) and emit one
         * OP_MASK_APPLY instruction to filter them at runtime.
         * n is the number of mask bits; the VM will use last_ret_count
         * as the actual value count so mismatched arities don't crash.  */
        u16 bitmask = 0;
        for (u32 i = 0; i < n; i++) {
            if (bits[i]) {
                bitmask |= (u16)(1u << i);
                pass_count++;
            }
        }
        emit_op_ab(p, OP_MASK_APPLY, (u16)n, bitmask);
        /* Keep last_expr_was_call = true so callers (parse_call) emit
         * OP_SPREAD_RET, letting the VM propagate the actual kept count
         * through spread_extra at runtime.                               */
        p->last_expr_was_unpack = false;
    } else if (!check(p, TOK_COMMA)) {
        /* Single non-call expression with no comma: apply only bit[0].
         * This handles cases like  (...) 1->10  where the expression is
         * a single value rather than a comma-separated list.             */
        if (bits[0]) {
            emit_op(p, OP_MASK_PASS);
            pass_count++;
        } else {
            emit_op(p, OP_MASK_SKIP);
        }
        p->last_expr_was_call   = false;
        p->last_expr_was_unpack = false;
    } else {
        /* Original comma-separated path: one expression per mask bit.    */
        if (bits[0]) {
            emit_op(p, OP_MASK_PASS);
            pass_count++;
        } else {
            emit_op(p, OP_MASK_SKIP);
        }

        for (u32 i = 1; i < n; i++) {
            if (!match(p, TOK_COMMA)) {
                error(p, "expected ',' between mask values");
                break;
            }
            parse_precedence(p, PREC_ASSIGN);
            if (bits[i]) {
                emit_op(p, OP_MASK_PASS);
                pass_count++;
            } else {
                emit_op(p, OP_MASK_SKIP);
            }
        }

        /* For pure masks, consume all remaining comma-separated values.
         *   Pure ~ (first_bit=true) : extras are skipped
         *   Pure . (first_bit=false): extras are passed (~)             */
        if (is_pure) {
            bool extra_pass = !first_bit;
            while (match(p, TOK_COMMA)) {
                parse_precedence(p, PREC_ASSIGN);
                if (extra_pass) {
                    emit_op(p, OP_MASK_PASS);
                    pass_count++;
                } else {
                    emit_op(p, OP_MASK_SKIP);
                }
            }
        }
        p->last_expr_was_call   = false;
        p->last_expr_was_unpack = false;
    }

    p->call_depth--;
    p->last_multi_push = pass_count;
}

/* Compile a `~>` / `~!>` / `~&>` body.  Stack contract:
 *   in : [..., collection]                        (already pushed by LHS)
 *   out: [..., result_array]                       after OP_PIPE_END
 *
 * `next_op` advances the iterator one element (or jumps to exit when
 * exhausted) and `collect_op` decides what each iteration contributes
 * to the output array (append, filter-by-truthiness, etc).
 *
 * The body may be either an inline expression or a `{ ... }` block; in
 * a block, `return expr;` emits a patchable forward jump rather than
 * OP_RETURN so the enclosing function is not exited.  All such jumps
 * land at the OP_NULL fallthrough that follows the block, so an early
 * return contributes its expression to the collect op.                  */
static void compile_pipe_body(CandoParser *p,
                              CandoOpcode next_op,
                              CandoOpcode collect_op)
{
    /* Collection is already on the stack (left operand). */
    emit_op_a(p, OP_PIPE_INIT, 1);
    /* Stack: [result_arr, elem0..elemN-1, count, src_idx=0] */

    /* Declare the 'pipe' local variable so body expressions can use it. */
    scope_begin(p);
    emit_op(p, OP_NULL);
    u32 pipe_slot = declare_local(p, "pipe", 4, false);
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    /* Loop header: next_op pushes the next element or jumps to exit. */
    u32 loop_start = cur(p)->code_len;
    u32 exit_jump  = emit_jump(p, next_op);
    /* Element is now on expression stack; pop it into the pipe local. */
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    /* Parse body (expression or block). */
    if (match(p, TOK_LBRACE)) {
        bool saved_in_pipe    = p->in_pipe_body;
        u32  saved_exit_count = p->pipe_exit_count;
        p->in_pipe_body    = true;
        p->pipe_exit_count = 0;

        parse_block(p);

        /* Fallthrough (no return statement executed): push null as result. */
        emit_op(p, OP_NULL);
        /* Patch all early-return exits to jump here (past the null). */
        for (u32 i = 0; i < p->pipe_exit_count; i++)
            patch_jump(p, p->pipe_exits[i]);

        p->in_pipe_body    = saved_in_pipe;
        p->pipe_exit_count = saved_exit_count;
    } else {
        parse_expression(p);
    }

    /* Append body result to the result array (or filter), then loop. */
    emit_op(p, collect_op);
    emit_loop(p, loop_start);

    patch_jump(p, exit_jump);
    scope_end(p);
    emit_op(p, OP_PIPE_END);
}

/* Pipe operator: collection ~> body_expr
 *   collection ~> expr               -- inline expression body
 *   collection ~> { stmts; }         -- block body; 'return expr;' yields value
 *
 * Stack layout during execution (separate from locals frame):
 *   [result_arr | elem0..elemN-1 | count | src_idx]
 * The 'pipe' local variable holds the current element each iteration.   */
static void parse_pipe_op(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    compile_pipe_body(p, OP_PIPE_NEXT, OP_PIPE_COLLECT);
}

/* Ternary conditional: cond ? then_expr : else_expr
 * Evaluates cond once, returns then_expr if cond is truthy, else else_expr.
 * Right-associative so `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`. */
static void parse_ternary(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    /* Condition is already on the stack (left operand). */
    u32 else_jump = emit_jump(p, OP_JUMP_IF_FALSE);
    emit_op(p, OP_POP);            /* discard truthy condition */
    p->ternary_then_depth++;
    parse_precedence(p, PREC_TERNARY);
    p->ternary_then_depth--;
    u32 end_jump = emit_jump(p, OP_JUMP);

    consume(p, TOK_COLON, "expected ':' in ternary expression");

    patch_jump(p, else_jump);
    emit_op(p, OP_POP);            /* discard falsy condition */
    /* Right-associative: parse the else branch at the same precedence so
     * a chained ternary becomes the entire false branch.                  */
    parse_precedence(p, PREC_TERNARY);
    patch_jump(p, end_jump);

    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->last_multi_push      = 1;
}

/* Filter operator: collection ~!> predicate_expr
 *   Like pipe (~>) but null body results are dropped from the output.   */
static void parse_filter_op(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    compile_pipe_body(p, OP_FILTER_NEXT, OP_FILTER_COLLECT);
}

/* Conditional filter operator: collection ~&> predicate_expr
 *   Like ~!>, but the body is interpreted as a boolean predicate: when the
 *   body returns a truthy value the *original* element (not the body
 *   result) is kept in the output array; otherwise the element is dropped.
 *   Identical structure to ~!> except the collect opcode.               */
static void parse_cond_filter_op(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    compile_pipe_body(p, OP_FILTER_NEXT, OP_COND_FILTER_COLLECT);
}

/* =========================================================================
 * Thread expression: thread <expr>  or  thread { block }
 *
 * The operand is compiled as an implicit zero-parameter anonymous function
 * body (same inline-chunk pattern as parse_function_expr), followed by
 * OP_CLOSURE to build a function handle and OP_THREAD to spawn it.
 * The result on the stack is a CdoThread handle.
 * ======================================================================= */
static void parse_thread_expr(CandoParser *p, bool can_assign)
{
    (void)can_assign;

    /* If the token after 'thread' is '.', '[', or '(' the user is accessing
     * the 'thread' library object (e.g. thread.sleep(5)), not spawning a
     * new thread.  Treat 'thread' as a plain global variable in that case. */
    if (check(p, TOK_DOT) || check(p, TOK_LBRACKET) || check(p, TOK_LPAREN)) {
        u16 idx = str_const(p, "thread", 6);
        emit_op_a(p, OP_LOAD_GLOBAL, idx);
        return;
    }

    /* Jump over the implicit function body at definition time. */
    u32 skip_body = emit_jump(p, OP_JUMP);
    u32 fn_start  = cur(p)->code_len;

    scope_begin(p);
    /* Slot 0: implicit function sentinel (same as named functions). */
    declare_local(p, "", 0, false);

    bool is_block = match(p, TOK_LBRACE);
    if (is_block) {
        /* Block form: thread { stmts; return val; } */
        parse_block(p);
    } else {
        /* Expression form: thread <expr> — the expression value IS the result. */
        parse_expression(p);
    }

    scope_end(p);

    /* Implicit return:
     *  - Block form: push null as default return value (explicit `return` inside
     *    the block exits early; this covers the fall-through case).
     *  - Expression form: the expression result is already on the stack; return
     *    it directly without pushing an extra null. */
    if (is_block) emit_op(p, OP_NULL);
    emit_op_a(p, OP_RETURN, 1);

    patch_jump(p, skip_body);

    /* Build the closure object and immediately spawn it as a thread.
     * Threads don't reset/restore the parser's local table the way
     * function bodies do, so they never accumulate upvalue captures --
     * emit zero captures to keep the OP_CLOSURE wire format uniform. */
    u16 pc_idx = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_closure(p, pc_idx, NULL, 0);
    emit_op(p, OP_THREAD);

    p->last_expr_was_call = true;  /* await + multi-return spreading works */
}

/* =========================================================================
 * Await expression: await <expr>
 *
 * Prefix operator that pops a CdoThread handle, blocks until done, and
 * pushes all return values.  Sets last_expr_was_call so multi-return
 * variable declarations (var a, b = await t) work correctly.
 * ======================================================================= */
static void parse_await_expr(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    /* Parse the thread expression (the operand). */
    parse_precedence(p, PREC_UNARY);
    emit_op(p, OP_AWAIT);
    p->last_expr_was_call = true;  /* enable multi-return spreading */
}

/* =========================================================================
 * Parse rule table
 * ======================================================================= */
static const ParseRule RULES[TOK_COUNT] = {
    /* Literals */
    [TOK_NUMBER]         = { parse_number,        NULL,              PREC_NONE       },
    [TOK_STRING_DQ]      = { parse_string_literal, NULL,             PREC_NONE       },
    [TOK_STRING_SQ]      = { parse_string_literal, NULL,             PREC_NONE       },
    [TOK_STRING_BT]      = { parse_string_literal, NULL,             PREC_NONE       },
    [TOK_IDENT]          = { parse_variable_expr,  NULL,             PREC_NONE       },

    /* Keywords that are literals / expressions */
    [TOK_NULL_KW]        = { parse_literal,        NULL,             PREC_NONE       },
    [TOK_TRUE_KW]        = { parse_literal,        NULL,             PREC_NONE       },
    [TOK_FALSE_KW]       = { parse_literal,        NULL,             PREC_NONE       },
    [TOK_PIPE_KW]        = { parse_variable_expr,  NULL,             PREC_NONE       },

    /* Grouping */
    [TOK_LPAREN]         = { parse_grouping,       parse_call,       PREC_CALL_PREC  },
    [TOK_LBRACKET]       = { parse_array_literal,  parse_subscript,  PREC_CALL_PREC  },
    [TOK_LBRACE]         = { parse_object_literal, NULL,             PREC_NONE       },
    [TOK_FUNCTION]       = { parse_function_expr,  NULL,             PREC_NONE       },
    [TOK_CLASS]          = { parse_class_expr,     NULL,             PREC_NONE       },
    [TOK_THREAD]         = { parse_thread_expr,    NULL,             PREC_NONE       },
    [TOK_AWAIT]          = { parse_await_expr,     NULL,             PREC_NONE       },

    /* Unary prefix */
    [TOK_MINUS]          = { parse_unary,          parse_binary,     PREC_TERM       },
    [TOK_BANG]           = { parse_unary,          NULL,             PREC_NONE       },
    [TOK_HASH]           = { parse_unary,          NULL,             PREC_NONE       },
    [TOK_TILDE]          = { NULL,                  NULL,             PREC_NONE       },
    [TOK_VARARG]         = { parse_unpack,          NULL,             PREC_NONE       },

    /* Arithmetic */
    [TOK_PLUS]           = { NULL,                 parse_binary,     PREC_TERM       },
    [TOK_STAR]           = { NULL,                 parse_binary,     PREC_FACTOR     },
    [TOK_SLASH]          = { NULL,                 parse_binary,     PREC_FACTOR     },
    [TOK_PERCENT]        = { NULL,                 parse_binary,     PREC_FACTOR     },
    [TOK_CARET]          = { NULL,                 parse_binary,     PREC_POWER      },

    /* Bitwise */
    [TOK_AMP]            = { NULL,                 parse_binary,     PREC_BITAND     },
    [TOK_BITOR]          = { NULL,                 parse_binary,     PREC_BITOR      },
    [TOK_BITXOR]         = { NULL,                 parse_binary,     PREC_BITXOR     },
    [TOK_LSHIFT]         = { NULL,                 parse_binary,     PREC_SHIFT      },
    [TOK_RSHIFT]         = { NULL,                 parse_binary,     PREC_SHIFT      },

    /* Comparison */
    [TOK_EQ]             = { NULL,                 parse_binary,     PREC_EQUALITY   },
    [TOK_NEQ]            = { NULL,                 parse_binary,     PREC_EQUALITY   },
    [TOK_LT]             = { NULL,                 parse_binary,     PREC_COMPARISON },
    [TOK_GT]             = { NULL,                 parse_binary,     PREC_COMPARISON },
    [TOK_LEQ]            = { NULL,                 parse_binary,     PREC_COMPARISON },
    [TOK_GEQ]            = { NULL,                 parse_binary,     PREC_COMPARISON },

    /* Range */
    [TOK_RANGE_ASC]      = { NULL,                 parse_binary,     PREC_RANGE      },
    [TOK_RANGE_DESC]     = { NULL,                 parse_binary,     PREC_RANGE      },

    /* Logical */
    [TOK_AND]            = { NULL,                 parse_and,        PREC_AND        },
    [TOK_OR]             = { NULL,                 parse_or,         PREC_OR         },

    /* Member access / mask pass-selector */
    [TOK_DOT]            = { NULL,                  parse_dot,        PREC_CALL_PREC  },
    [TOK_FLUENT]         = { NULL,                 parse_fluent,     PREC_CALL_PREC  },
    [TOK_COLON]          = { NULL,                 parse_method_call,PREC_CALL_PREC  },
    [TOK_QDOT]           = { NULL,                 parse_safe_dot,   PREC_CALL_PREC  },
    [TOK_QLBRACKET]      = { NULL,                 parse_safe_subscript, PREC_CALL_PREC },

    /* Ternary conditional */
    [TOK_QUESTION]       = { NULL,                 parse_ternary,    PREC_TERNARY    },

    /* Pipe / filter */
    [TOK_PIPE_OP]        = { NULL,                 parse_pipe_op,    PREC_PIPE_PREC  },
    [TOK_FILTER_OP]      = { NULL,                 parse_filter_op,  PREC_PIPE_PREC  },
    [TOK_COND_FILTER_OP] = { NULL,                 parse_cond_filter_op, PREC_PIPE_PREC },
};

static const ParseRule *get_rule(CandoTokenType t)
{
    if ((u32)t >= TOK_COUNT) return &RULES[TOK_EOF];
    return &RULES[t];
}

/* =========================================================================
 * Core Pratt driver
 * ======================================================================= */

static void parse_precedence(CandoParser *p, Precedence min_prec)
{
    /* Each expression parse starts with a clean call-flag so that flags from
     * a prior expression (e.g. the FOR-OVER iterable) do not leak into the
     * body and cause spurious OP_TRUNCATE_RET inside comparisons.           */
    p->last_expr_was_call = false;

    /* Save and reset safe-chain state for this expression.  A '?.' / '?['
     * encountered during this call will set in_safe_chain=true, and any
     * subsequent member-access infix in the same chain will append a guard
     * to safe_chain_jumps[].  After the infix loop finishes we patch all
     * recorded jumps to land here (past the chain).                        */
    bool saved_safe_active = p->in_safe_chain;
    u32  saved_safe_count  = p->safe_chain_count;
    p->in_safe_chain  = false;
    p->safe_chain_count = saved_safe_count;  /* preserve outer entries */

    advance(p);
    ParseFn prefix = get_rule(p->previous.type)->prefix;
    if (!prefix) {
        error(p, "expected expression");
        return;
    }

    bool can_assign = (min_prec <= PREC_ASSIGN);
    prefix(p, can_assign);

    while (!check(p, TOK_EOF)) {
        /* Inside the THEN branch of a ternary, ':' is the ternary
         * delimiter, not the method-call infix.                          */
        if (p->ternary_then_depth > 0 && check(p, TOK_COLON)) break;
        Precedence cur_prec = get_rule(p->current.type)->precedence;
        if (cur_prec < min_prec) break;
        advance(p);
        ParseFn infix = get_rule(p->previous.type)->infix;
        if (!infix) break;
        infix(p, can_assign);
    }

    /* If a safe chain was opened during this expression, patch all of its
     * null-guard jumps to land here, leaving the (possibly null) chain
     * value on the stack.                                                 */
    if (p->in_safe_chain) {
        for (u32 i = saved_safe_count; i < p->safe_chain_count; i++)
            patch_jump(p, p->safe_chain_jumps[i]);
    }
    p->in_safe_chain    = saved_safe_active;
    p->safe_chain_count = saved_safe_count;

    if (can_assign && match(p, TOK_ASSIGN)) {
        error(p, "invalid assignment target");
    }
}

static void parse_expression(CandoParser *p)
{
    parse_precedence(p, PREC_ASSIGN);
}

/* =========================================================================
 * Statement parsing
 * ======================================================================= */

static void synchronise(CandoParser *p)
{
    p->panic_mode = false;
    while (!check(p, TOK_EOF)) {
        if (p->previous.type == TOK_SEMI) return;
        switch (p->current.type) {
        case TOK_IF:
        case TOK_WHILE:
        case TOK_FOR:
        case TOK_FUNCTION:
        case TOK_CLASS:
        case TOK_VAR:
        case TOK_CONST:
        case TOK_RETURN:
        case TOK_THROW:
        case TOK_TRY:
        case TOK_THREAD:
            return;
        default:
            advance(p);
        }
    }
}

/* --- Block: { stmt* } -------------------------------------------------- */
static void parse_block(CandoParser *p)
{
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF))
        parse_statement(p);
    consume(p, TOK_RBRACE, "expected '}' to close block");
}

/* --- VAR / CONST declaration ------------------------------------------- */
static void parse_var_decl(CandoParser *p, bool is_const)
{
#define MAX_MULTI_VARS 16
    /* Collect comma-separated variable names */
    struct { const char *name; u32 len; } vars[MAX_MULTI_VARS];
    int var_count = 0;

    do {
        consume(p, TOK_IDENT, "expected variable name");
        if (var_count >= MAX_MULTI_VARS) {
            error(p, "too many variables in declaration");
            break;
        }
        vars[var_count].name = p->previous.start;
        vars[var_count].len  = p->previous.length;
        var_count++;
    } while (match(p, TOK_COMMA));

    if (match(p, TOK_ASSIGN)) {
        /* Parse comma-separated initialiser expressions */
        int expr_count = 0;
        p->call_depth++;
        do {
            p->last_expr_was_call = false;
            p->last_multi_push    = 1;
            parse_expression(p);
            if (p->last_expr_was_call) emit_op(p, OP_SPREAD_RET);
            expr_count += (int)p->last_multi_push;
        } while (match(p, TOK_COMMA));
        p->call_depth--;

        /* If fewer expressions than variables and last expr was not a call,
         * pad remaining variables with NULL. */
        if (expr_count < var_count && !p->last_expr_was_call) {
            for (int i = expr_count; i < var_count; i++)
                emit_op(p, OP_NULL);
        }
    } else {
        /* No initialiser: push NULL for each variable */
        for (int i = 0; i < var_count; i++)
            emit_op(p, OP_NULL);
    }
    consume(p, TOK_SEMI, "expected ';' after variable declaration");

    /* Emit definitions in reverse order (stack is LIFO: last pushed = top) */
    for (int i = var_count - 1; i >= 0; i--) {
        const char *name = vars[i].name;
        u32         len  = vars[i].len;
        if (p->scope_depth > 0) {
            u32 slot = declare_local(p, name, len, is_const);
            emit_op_a(p, is_const ? OP_DEF_CONST_LOCAL : OP_DEF_LOCAL, (u16)slot);
        } else {
            u16 name_idx = str_const(p, name, len);
            emit_op_a(p, is_const ? OP_DEF_CONST_GLOBAL : OP_DEF_GLOBAL, name_idx);
        }
    }
#undef MAX_MULTI_VARS
}

/* --- IF statement -------------------------------------------------------
 * Syntax:  IF expr { block } [ ELSE { block } ]
 */
static void parse_if(CandoParser *p)
{
    parse_expression(p);
    consume(p, TOK_LBRACE, "expected '{' after IF condition");

    u32 then_jump = emit_jump(p, OP_JUMP_IF_FALSE);
    emit_op(p, OP_POP);

    scope_begin(p);
    parse_block(p);
    scope_end(p);

    u32 else_jump = emit_jump(p, OP_JUMP);
    patch_jump(p, then_jump);
    emit_op(p, OP_POP);

    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) {
            parse_if(p);
        } else {
            consume(p, TOK_LBRACE, "expected '{' after ELSE");
            scope_begin(p);
            parse_block(p);
            scope_end(p);
        }
    }
    patch_jump(p, else_jump);
}

/* --- WHILE loop --------------------------------------------------------- */
static void parse_while(CandoParser *p)
{
    u32 loop_start = cur(p)->code_len;
    parse_expression(p);
    consume(p, TOK_LBRACE, "expected '{' after WHILE condition");

    u32 exit_jump = emit_jump(p, OP_JUMP_IF_FALSE);
    emit_op(p, OP_POP);

    /* OP_LOOP_MARK: records break/continue targets and stack depth.
     * cont_ip  = loop_start (re-evaluate the condition on CONTINUE).
     * break_ip = patched below to point past the final OP_POP.        */
    u32 mark_patch = emit_loop_mark(p, loop_start, CANDO_LOOP_WHILE);

    scope_begin(p);
    parse_block(p);
    scope_end(p);

    emit_op(p, OP_LOOP_END);
    emit_loop(p, loop_start);
    patch_jump(p, exit_jump);
    emit_op(p, OP_POP);
    /* break_ip lands here — after the OP_POP that clears the condition. */
    patch_loop_mark_break(p, mark_patch);
}

/* --- FOR loop -----------------------------------------------------------
 * Syntax:  FOR ident [, ident]* IN expr { block }   -- iterate keys / indices
 *          FOR ident [, ident]* OF expr { block }   -- iterate values / elements
 *          FOR ident [, ident]* OVER expr [, expr]* { block } -- Lua-style iterator
 *
 * Uses OP_FOR_INIT / OP_FOR_NEXT (for IN/OF) or OP_FOR_OVER_INIT / OP_FOR_OVER_NEXT
 * (for OVER) for proper iterator protocol.
 * The loop variables are bound as locals inside the loop scope.
 */
static void parse_for(CandoParser *p)
{
#define MAX_FOR_VARS 16
    struct { const char *name; u32 len; } vars[MAX_FOR_VARS];
    int var_count = 0;

    do {
        consume(p, TOK_IDENT, "expected loop variable name");
        if (var_count < MAX_FOR_VARS) {
            vars[var_count].name = p->previous.start;
            vars[var_count].len  = p->previous.length;
            var_count++;
        } else {
            error(p, "too many loop variables");
        }
    } while (match(p, TOK_COMMA));

    bool is_over = false;
    bool keys_mode = false;
    if (match(p, TOK_IN)) {
        keys_mode = true;
    } else if (match(p, TOK_OF)) {
        keys_mode = false;
    } else if (match(p, TOK_OVER)) {
        is_over = true;
    } else {
        error_current(p, "expected IN, OF, or OVER after loop variable(s)");
        return;
    }

    if (!is_over && var_count > 1) {
        error(p, "only 'over' loops support multiple loop variables");
    }

    /* For 'over' loops, the expression should be able to return multiple values (triplet). */
    bool last_was_call = false;
    u32  expr_count = 0;
    if (is_over) {
        p->call_depth++;
        do {
            p->last_expr_was_call = false;
            p->last_multi_push    = 1;
            parse_expression(p);
            last_was_call = p->last_expr_was_call;
            if (last_was_call) emit_op(p, OP_SPREAD_RET);
            expr_count += p->last_multi_push;
            if (expr_count >= 3) break;
        } while (match(p, TOK_COMMA));
        p->call_depth--;
    } else {
        parse_expression(p);   /* iterable — now on stack */
    }

    consume(p, TOK_LBRACE, "expected '{' after FOR iterable");

    if (is_over) {
        /* OP_FOR_OVER_INIT: A = number of variables to bind,
         * B = count of values pushed (top bit set if last expr was a call). */
        u16 b_val = (u16)(expr_count & 0x7FFF);
        if (last_was_call) b_val |= 0x8000;
        emit_op_ab(p, OP_FOR_OVER_INIT, (u16)var_count, b_val);

        u32 loop_start = cur(p)->code_len;
        u32 exit_jump  = emit_jump(p, OP_FOR_OVER_NEXT);   /* jumps when exhausted */

        scope_begin(p);
        /* OP_FOR_OVER_NEXT pushes variables. Bind them in reverse order
         * (NEXT pushes var1..varN; DEF_LOCAL pops from top = varN first). */
        for (int i = var_count - 1; i >= 0; i--) {
            u32 slot = declare_local(p, vars[i].name, vars[i].len, false);
            emit_op_a(p, OP_DEF_LOCAL, (u16)slot);
        }

        /* LOOP_MARK: cont_ip = loop_start (re-run NEXT), break cleans up
         * the 4-item iterator triplet via CANDO_LOOP_FOR_OVER handler.   */
        u32 mark_patch = emit_loop_mark(p, loop_start, CANDO_LOOP_FOR_OVER);

        parse_block(p);
        scope_end(p);

        emit_op(p, OP_LOOP_END);
        emit_loop(p, loop_start);
        patch_jump(p, exit_jump);
        /* break_ip lands here — after the loop. */
        patch_loop_mark_break(p, mark_patch);
    } else {
        /* If the iterable is a range (->/<-), it produces an array of values.
         * FOR IN over a range should iterate those values, not indices, so
         * override to OF mode (values) regardless of IN/OF keyword. */
        if (keys_mode && cur(p)->code_len > 0) {
            u8 last_op = cur(p)->code[cur(p)->code_len - 1];
            if (last_op == (u8)OP_RANGE_ASC || last_op == (u8)OP_RANGE_DESC)
                keys_mode = false;
        }

        /* OP_FOR_INIT mode: 1 = keys (IN), 0 = values (OF) */
        emit_op_a(p, OP_FOR_INIT, keys_mode ? 1 : 0);

        u32 loop_start = cur(p)->code_len;
        u32 exit_jump  = emit_jump(p, OP_FOR_NEXT);   /* jumps when exhausted */

        scope_begin(p);
        /* OP_FOR_NEXT pushed the next element; bind it as a local.          */
        u32 slot = declare_local(p, vars[0].name, vars[0].len, false);
        emit_op_a(p, OP_DEF_LOCAL, (u16)slot);

        /* LOOP_MARK: cont_ip = loop_start (re-run FOR_NEXT), break unwinds
         * the FOR state ([val0..valN, count, index]) via CANDO_LOOP_FOR.  */
        u32 mark_patch = emit_loop_mark(p, loop_start, CANDO_LOOP_FOR);

        parse_block(p);
        scope_end(p);

        emit_op(p, OP_LOOP_END);
        emit_loop(p, loop_start);
        patch_jump(p, exit_jump);
        /* break_ip lands here — after the loop. */
        patch_loop_mark_break(p, mark_patch);
    }
#undef MAX_FOR_VARS
}

/* --- TRY / CATCH / FINALY -----------------------------------------------
 * Syntax:  TRY { block } CATCH (ident) { block } [ FINALY { block } ]
 *
 * Uses OP_TRY_BEGIN / OP_TRY_END / OP_CATCH_BEGIN.
 */
static void parse_try(CandoParser *p)
{
    consume(p, TOK_LBRACE, "expected '{' after TRY");

    u32 handler_jump = emit_jump(p, OP_TRY_BEGIN);

    scope_begin(p);
    parse_block(p);
    scope_end(p);

    emit_op(p, OP_TRY_END);

    u32 skip_catch = emit_jump(p, OP_JUMP);
    patch_jump(p, handler_jump);

    consume(p, TOK_CATCH, "expected CATCH after TRY block");
    consume(p, TOK_LPAREN, "expected '(' after CATCH");

    /* Collect catch parameter names (comma-separated identifiers).      */
#define MAX_CATCH_PARAMS 8
    const char *catch_names[MAX_CATCH_PARAMS];
    u32         catch_lens[MAX_CATCH_PARAMS];
    u16 catch_count = 0;

    consume(p, TOK_IDENT, "expected catch variable name");
    catch_names[catch_count] = p->previous.start;
    catch_lens[catch_count]  = p->previous.length;
    catch_count++;

    while (match(p, TOK_COMMA)) {
        if (catch_count >= MAX_CATCH_PARAMS) {
            error(p, "too many catch parameters");
            break;
        }
        consume(p, TOK_IDENT, "expected catch variable name");
        catch_names[catch_count] = p->previous.start;
        catch_lens[catch_count]  = p->previous.length;
        catch_count++;
    }

    consume(p, TOK_RPAREN, "expected ')' after catch parameter(s)");
    consume(p, TOK_LBRACE, "expected '{' after CATCH(...)");

    /* Catch block: OP_CATCH_BEGIN pushes N values from error_vals[].   */
    scope_begin(p);
    emit_op_a(p, OP_CATCH_BEGIN, catch_count);
    for (u16 i = 0; i < catch_count; i++) {
        u32 slot = declare_local(p, catch_names[i], catch_lens[i], false);
        emit_op_a(p, OP_DEF_LOCAL, (u16)slot);
    }
    parse_block(p);
    scope_end(p);

    patch_jump(p, skip_catch);

    if (match(p, TOK_FINALY)) {
        consume(p, TOK_LBRACE, "expected '{' after FINALY");
        scope_begin(p);
        parse_block(p);
        scope_end(p);
    }
}

/* --- FUNCTION expression ------------------------------------------------
 * Syntax:  function(params) { block }
 *
 * Anonymous function expression; leaves the start PC on the stack.
 */
static void parse_function_expr(CandoParser *p, bool can_assign)
{
    (void)can_assign;

    const char *param_names[CANDO_MAX_PARAMS];
    u32         param_lens[CANDO_MAX_PARAMS];

    consume(p, TOK_LPAREN, "expected '(' after 'function'");
    u16 arity = parse_param_list(p, param_names, param_lens, CANDO_MAX_PARAMS,
                                 "expected ')' after parameters");

    u32 fn_start, skip_body;
    u16 *body_uv_specs;
    u16  body_uv_count;
    compile_function_body(p, param_names, param_lens, arity,
                          "expected '{' before function body",
                          &fn_start, &skip_body,
                          &body_uv_specs, &body_uv_count);

    patch_jump(p, skip_body);

    u16 pc_idx = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_closure(p, pc_idx, body_uv_specs, body_uv_count);
    cando_free(body_uv_specs);

    /* The function *expression* evaluates to a closure value — it is not a
     * call.  Clear last_expr_was_call / last_expr_was_unpack so callers such
     * as parse_call don't mistakenly emit OP_SPREAD_RET based on whatever
     * the final expression inside the function body happened to be.       */
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->last_multi_push      = 1;
}

/* --- FUNCTION declaration -----------------------------------------------
 * Syntax:  FUNCTION name(params) { block }
 */
static void parse_function(CandoParser *p)
{
    consume(p, TOK_IDENT, "expected function name");
    const char *fn_name = p->previous.start;
    u32         fn_len  = p->previous.length;

    consume(p, TOK_LPAREN, "expected '(' after function name");

    const char *param_names[CANDO_MAX_PARAMS];
    u32         param_lens[CANDO_MAX_PARAMS];
    u16 arity = parse_param_list(p, param_names, param_lens, CANDO_MAX_PARAMS,
                                 "expected ')' after parameters");

    u32 fn_start, skip_body;
    u16 *body_uv_specs;
    u16  body_uv_count;
    compile_function_body(p, param_names, param_lens, arity,
                          "expected '{' before function body",
                          &fn_start, &skip_body,
                          &body_uv_specs, &body_uv_count);

    patch_jump(p, skip_body);

    /* Build a closure object from the function's start PC and define it. */
    u16 pc_idx   = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_closure(p, pc_idx, body_uv_specs, body_uv_count);
    cando_free(body_uv_specs);
    u16 name_idx = str_const(p, fn_name, fn_len);
    emit_op_a(p, OP_DEF_GLOBAL, name_idx);
}

/* --- CLASS expression body ----------------------------------------------
 *
 * Both the statement form (`class Name = (params) { body }`) and the
 * expression forms (`class [Name] (params) { body }`) share the same body
 * compilation: a constructor function whose params and body are written
 * exactly like `function(params) { body }`, plus the bookkeeping that
 * makes the class callable.
 *
 * Preconditions when this helper is invoked:
 *   - The class object is already on top of the stack (created by either
 *     OP_NEW_CLASS — for a named class — or OP_NEW_OBJECT — for the
 *     anonymous expression form).
 *   - If `extends Parent` was parsed, the parent expression has been
 *     compiled BEFORE the class object so the stack reads
 *     [..., parent, class] and a single OP_INHERIT pops the parent and
 *     records `class.__index = parent`.
 *
 * Emits, with the class still on TOS at the end:
 *   1. (optional) OP_INHERIT
 *   2. Constructor body compiled inline + OP_CLOSURE pushing the function
 *      value, then OP_BIND_METHOD __constructor.
 *   3. OP_BIND_DEFAULT_CALL — wires the class's __call to the VM's
 *      default constructor wrapper.
 * ----------------------------------------------------------------------- */
static void emit_class_body(CandoParser *p, bool has_extends)
{
    if (has_extends) {
        /* Stack: [..., parent, class] -> [..., class] */
        emit_op(p, OP_INHERIT);
    }

    /* Parameter list is optional: `class Foo = { }` is shorthand for
     * `class Foo = () { }` (an empty constructor that ignores any args). */
    const char *param_names[CANDO_MAX_PARAMS];
    u32         param_lens[CANDO_MAX_PARAMS];
    u16 arity = 0;

    if (match(p, TOK_LPAREN)) {
        arity = parse_param_list(p, param_names, param_lens, CANDO_MAX_PARAMS,
                                 "expected ')' after class parameters");
    }

    /* Compile the constructor body with the same shape as a
     * `function(params) { ... }` expression so the resulting closure can
     * be called via cando_vm_call_value() from the default __call native. */
    u32 fn_start, skip_body;
    u16 *body_uv_specs;
    u16  body_uv_count;
    compile_function_body(p, param_names, param_lens, arity,
                          "expected '{' before class body",
                          &fn_start, &skip_body,
                          &body_uv_specs, &body_uv_count);

    patch_jump(p, skip_body);

    /* Build the constructor closure (an OBJ_FUNCTION) and bind it to
     * the class as `__constructor`.  Using OP_CLOSURE rather than the
     * inline-PC trick lets cando_vm_call_value() invoke it from C.       */
    u16 ctor_pc_idx   = cando_chunk_add_const(cur(p),
                                              cando_number((f64)fn_start));
    emit_closure(p, ctor_pc_idx, body_uv_specs, body_uv_count);
    cando_free(body_uv_specs);
    static const char kCtorName[] = "__constructor";
    u16 ctor_name_idx = str_const(p, kCtorName,
                                  (u32)(sizeof(kCtorName) - 1));
    emit_op_a(p, OP_BIND_METHOD, ctor_name_idx);

    /* Make the class callable: class.__call = vm->default_class_call. */
    emit_op(p, OP_BIND_DEFAULT_CALL);
}

/* Optional `extends Parent` clause.
 * When present, push the parent expression onto the stack BEFORE the class
 * object so OP_INHERIT (emitted later by emit_class_body) can pop it.
 * Returns true if an EXTENDS clause was consumed.
 * ----------------------------------------------------------------------- */
static bool parse_class_extends(CandoParser *p)
{
    if (!match(p, TOK_EXTENDS)) return false;
    consume(p, TOK_IDENT, "expected parent class name after EXTENDS");
    u16 parent_idx = prev_name_const(p);
    /* Look up the parent at the global scope; classes live in globals. */
    emit_op_a(p, OP_LOAD_GLOBAL, parent_idx);
    return true;
}

/* --- CLASS declaration (statement form) ---------------------------------
 * Syntax:  class Name [extends Parent] = [(params)] { body }
 *
 * Desugars to: var Name = class Name [extends Parent] [(params)] { body }
 * The class is bound as a global; its __type meta-key is set to Name.
 * ----------------------------------------------------------------------- */
static void parse_class(CandoParser *p)
{
    consume(p, TOK_IDENT, "expected class name");
    u16 name_idx = prev_name_const(p);

    bool has_extends = parse_class_extends(p);
    /* Stack now: [..., parent?]  (nothing yet for the class).            */

    /* The `=` is mandatory in the statement form -- it is what
     * distinguishes `class Foo = (...) {...}` from a stray expression.
     * Allow both `class Foo = (params) { body }` (with params) and
     * `class Foo = { body }` (constructor takes no args).                */
    consume(p, TOK_ASSIGN, "expected '=' in class declaration");

    /* Now create the class object on TOS and (if needed) wire __index. */
    emit_op_a(p, OP_NEW_CLASS, name_idx);

    emit_class_body(p, has_extends);

    /* Statement form: bind the class as a global. */
    emit_op_a(p, OP_DEF_GLOBAL, name_idx);
}

/* --- CLASS expression ---------------------------------------------------
 * Pratt prefix handler for `TOK_CLASS`.
 *
 * Forms:
 *   class                (params) { body }   -- anonymous, no __type
 *   class Name           (params) { body }   -- named, __type = "Name"
 *   class [Name] extends Parent (params) { body }
 *
 * Distinguishing the expression form from the statement form is done by
 * the caller: parse_statement matches TOK_CLASS first and dispatches to
 * parse_class (the statement form) before parse_expr_stmt has a chance to
 * route through this Pratt prefix.  Inside expressions (e.g. on the RHS of
 * a `var x = ...`), this handler runs.
 * ----------------------------------------------------------------------- */
static void parse_class_expr(CandoParser *p, bool can_assign)
{
    (void)can_assign;

    bool has_name = check(p, TOK_IDENT);
    u16  name_idx = 0;
    if (has_name) {
        advance(p);
        name_idx = prev_name_const(p);
    }

    bool has_extends = parse_class_extends(p);

    /* Push the class object: named classes get __type via OP_NEW_CLASS;
     * anonymous classes use OP_NEW_OBJECT so __type is left unset.        */
    if (has_name)
        emit_op_a(p, OP_NEW_CLASS, name_idx);
    else
        emit_op(p, OP_NEW_OBJECT);

    emit_class_body(p, has_extends);

    /* The class is now on TOS as a value -- the surrounding expression
     * (e.g. `var X = class ...`) will assign or use it.                   */
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->last_multi_push      = 1;
}

/* --- RETURN ------------------------------------------------------------- */
static void parse_return(CandoParser *p)
{
    if (p->in_pipe_body) {
        /* Inside a ~> / ~!> block body: do not exit the function.
         * Push the return value (or null) and jump to after the block;
         * OP_PIPE_COLLECT / OP_FILTER_COLLECT will consume it.            */
        if (check(p, TOK_SEMI) || check(p, TOK_RBRACE) || check(p, TOK_EOF)) {
            emit_op(p, OP_NULL);
            match(p, TOK_SEMI);
        } else {
            parse_expression(p);
            match(p, TOK_SEMI);
        }
        if (p->pipe_exit_count < 16) {
            p->pipe_exits[p->pipe_exit_count++] = emit_jump(p, OP_JUMP);
        } else {
            error(p, "too many return statements in pipe body (max 16)");
        }
        return;
    }

    if (check(p, TOK_SEMI) || check(p, TOK_RBRACE) || check(p, TOK_EOF)) {
        emit_op(p, OP_NULL);
        match(p, TOK_SEMI);
        emit_op_a(p, OP_RETURN, 1);
        return;
    }
    u16 count = 0;
    p->call_depth++;
    do {
        parse_expression(p);
        count++;
    } while (match(p, TOK_COMMA));
    p->call_depth--;
    match(p, TOK_SEMI);
    emit_op_a(p, OP_RETURN, count);
}

/* --- THROW -------------------------------------------------------------- */
static void parse_throw(CandoParser *p)
{
    u16 count = 0;
    p->call_depth++;
    parse_expression(p);
    count++;
    while (match(p, TOK_COMMA)) {
        parse_expression(p);
        count++;
    }
    p->call_depth--;
    consume(p, TOK_SEMI, "expected ';' after THROW");
    emit_op_a(p, OP_THROW, count);
}

/* --- BREAK / CONTINUE --------------------------------------------------- */
static void parse_break(CandoParser *p)
{
    match(p, TOK_SEMI);
    emit_op_a(p, OP_BREAK, 0);   /* depth 0 = innermost loop */
}

static void parse_continue(CandoParser *p)
{
    match(p, TOK_SEMI);
    emit_op_a(p, OP_CONTINUE, 0);
}

/* --- Expression statement ----------------------------------------------- */
static void parse_expr_stmt(CandoParser *p)
{
    parse_expression(p);
    if (p->eval_mode && check(p, TOK_EOF)) {
        /* trailing semicolon optional on the last expression in eval mode */
    } else {
        consume(p, TOK_SEMI, "expected ';' after expression");
    }
    emit_op(p, OP_POP);
}

/* --- Top-level statement dispatcher ------------------------------------- */
static void parse_statement(CandoParser *p)
{
    p->last_stmt_was_expr = false;
    bool this_was_expr = false;

    if (match(p, TOK_VAR)) {
        parse_var_decl(p, false);
    } else if (match(p, TOK_CONST)) {
        parse_var_decl(p, true);
    } else if (match(p, TOK_IF)) {
        parse_if(p);
    } else if (match(p, TOK_WHILE)) {
        parse_while(p);
    } else if (match(p, TOK_FOR)) {
        parse_for(p);
    } else if (match(p, TOK_TRY)) {
        parse_try(p);
    } else if (match(p, TOK_FUNCTION)) {
        if (check(p, TOK_LPAREN)) {
            /* Anonymous function expression used as a statement value.
             * TOK_FUNCTION is already consumed; parse_function_expr takes
             * it from there, then we treat it like an expression statement. */
            parse_function_expr(p, false);
            if (p->eval_mode && check(p, TOK_EOF)) {
                /* no semicolon needed before EOF in eval mode */
            } else {
                consume(p, TOK_SEMI, "expected ';' after expression");
            }
            emit_op(p, OP_POP);
            this_was_expr = true;
        } else {
            parse_function(p);
        }
    } else if (match(p, TOK_CLASS)) {
        parse_class(p);
    } else if (match(p, TOK_RETURN)) {
        parse_return(p);
    } else if (match(p, TOK_THROW)) {
        parse_throw(p);
    } else if (match(p, TOK_BREAK)) {
        parse_break(p);
    } else if (match(p, TOK_CONTINUE)) {
        parse_continue(p);
    } else if (match(p, TOK_LBRACE)) {
        /* Block statement: open a scope to contain any locals.          */
        scope_begin(p);
        parse_block(p);
        scope_end(p);
    } else if (match(p, TOK_SEMI)) {
        /* empty statement */
    } else {
        parse_expr_stmt(p);
        this_was_expr = true;
    }

    /* Commit: nested parse_statement calls (e.g. inside parse_if) must not
     * corrupt the flag seen by the top-level cando_parse loop.            */
    p->last_stmt_was_expr = this_was_expr;

    if (p->panic_mode) synchronise(p);
}

/* =========================================================================
 * Public API
 * ======================================================================= */

void cando_parser_init(CandoParser *p, const char *source, usize len,
                       CandoChunk *chunk)
{
    CANDO_ASSERT(p != NULL);
    CANDO_ASSERT(chunk != NULL);
    cando_lexer_init(&p->lexer, source, len);
    p->had_error          = false;
    p->panic_mode         = false;
    p->chunk              = chunk;
    p->locals             = (CandoLocal *)cando_alloc(
                                sizeof(CandoLocal) * CANDO_LOCAL_INITIAL_CAP);
    p->local_count        = 0;
    p->local_capacity     = CANDO_LOCAL_INITIAL_CAP;
    p->scope_depth        = 0;
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->call_depth           = 0;
    p->eval_mode          = false;
    p->last_stmt_was_expr = false;
    p->in_pipe_body       = false;
    p->pipe_exit_count    = 0;
    p->last_multi_push    = 1;
    p->in_safe_chain      = false;
    p->safe_chain_count   = 0;
    p->ternary_then_depth = 0;
    p->outer_locals       = NULL;
    p->outer_count        = 0;
    p->upvalue_specs      = (u16 *)cando_alloc(
                                sizeof(u16) * CANDO_LOCAL_INITIAL_CAP);
    p->upvalue_count      = 0;
    p->upvalue_capacity   = CANDO_LOCAL_INITIAL_CAP;
    p->error_msg[0] = '\0';

    p->current.type = TOK_EOF;
    advance(p);
}

void cando_parser_free(CandoParser *p)
{
    if (!p) return;
    cando_free(p->locals);
    p->locals = NULL;
    p->local_count = 0;
    p->local_capacity = 0;
    cando_free(p->upvalue_specs);
    p->upvalue_specs = NULL;
    p->upvalue_count = 0;
    p->upvalue_capacity = 0;
}

bool cando_parse(CandoParser *p)
{
    CANDO_ASSERT(p != NULL);
    while (!check(p, TOK_EOF))
        parse_statement(p);

    if (p->eval_mode) {
        /* If the last top-level statement was an expression statement, strip
         * the trailing OP_POP and return the value; otherwise return null. */
        if (p->last_stmt_was_expr && cur(p)->code_len > 0 &&
            cur(p)->code[cur(p)->code_len - 1] == (u8)OP_POP) {
            cur(p)->code_len--;   /* remove OP_POP */
            emit_op_a(p, OP_RETURN, 1);
        } else {
            emit_op_a(p, OP_RETURN, 0);
        }
    } else {
        emit_op(p, OP_HALT);
    }
    return !p->had_error;
}

const char *cando_parser_error(const CandoParser *p)
{
    if (!p->had_error) return NULL;
    return p->error_msg;
}
