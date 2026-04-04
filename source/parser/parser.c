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
static const ParseRule *get_rule(CdoTokenType t);

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

static bool check(CandoParser *p, CdoTokenType t)
{
    return p->current.type == t;
}

static bool match(CandoParser *p, CdoTokenType t)
{
    if (!check(p, t)) return false;
    advance(p);
    return true;
}

static void consume(CandoParser *p, CdoTokenType t, const char *msg)
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

/* ---- constant helpers -------------------------------------------------- */

/* Intern a string constant; returns pool index. */
static u16 str_const(CandoParser *p, const char *s, u32 len)
{
    CandoString *cs  = cando_string_new(s, len);
    CandoValue   val = cando_string_value(cs);
    return cando_chunk_add_const(cur(p), val);
}

/* Intern the previous token's lexeme as a string constant. */
static u16 prev_name_const(CandoParser *p)
{
    return str_const(p, p->previous.start, p->previous.length);
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
    if (p->local_count >= CANDO_LOCAL_MAX) {
        error(p, "too many local variables in scope");
        return 0;
    }
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

static void parse_string_literal(CandoParser *p, bool can_assign)
{
    (void)can_assign;
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

    parse_expression(p);
    consume(p, TOK_RPAREN, "expected ')' after expression");
}

static void parse_unary(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    CdoTokenType op = p->previous.type;
    parse_precedence(p, PREC_UNARY);
    switch (op) {
    case TOK_MINUS: emit_op(p, OP_NEG); break;
    case TOK_BANG:  emit_op(p, OP_NOT); break;
    case TOK_HASH:  emit_op(p, OP_LEN); break;
    default: break;
    }
    p->last_expr_was_call = false;
}

/* Emit load for a name, using local slot if available, else global. */
static void emit_load(CandoParser *p, const char *name, u32 len)
{
    int slot = resolve_local(p, name, len);
    if (slot >= 0) {
        emit_op_a(p, OP_LOAD_LOCAL, (u16)slot);
    } else {
        u16 idx = str_const(p, name, len);
        emit_op_a(p, OP_LOAD_GLOBAL, idx);
    }
}

/* Emit store for a name. */
static void emit_store(CandoParser *p, const char *name, u32 len)
{
    int slot = resolve_local(p, name, len);
    if (slot >= 0) {
        emit_op_a(p, OP_STORE_LOCAL, (u16)slot);
    } else {
        u16 idx = str_const(p, name, len);
        emit_op_a(p, OP_STORE_GLOBAL, idx);
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
            parse_expression(p);
            if (count == 0xFFFF) { error(p, "too many array elements"); break; }
            count++;
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
    p->last_expr_was_unpack = p->last_expr_was_call;
}

/* =========================================================================
 * Infix parse functions
 * ======================================================================= */

static void parse_binary(CandoParser *p, bool can_assign)
{
    (void)can_assign;
    CdoTokenType op = p->previous.type;
    const ParseRule *rule = get_rule(op);
    Precedence next = (op == TOK_CARET)
                      ? rule->precedence
                      : (Precedence)(rule->precedence + 1);
    /* Reset before parsing RHS so we get a clean read of what was parsed. */
    p->last_multi_push      = 1;
    p->last_expr_was_unpack = false;
    parse_precedence(p, next);

    /* Helper: emit the right comparison opcode based on the RHS.
     *
     * Rules:
     *   ...myfunc()        → spread_op  (compare against all return values)
     *   (~.~) myfunc()     → stack_op n (compare against n mask-selected values)
     *   myfunc()           → OP_TRUNCATE_RET + plain_op (first return only)
     *   a, b, c            → stack_op n (user-defined list; truncates any calls)
     *   plain value        → plain_op
     */
#define MULTI_CMP(plain_op, stack_op, spread_op)                          \
    do {                                                                  \
        bool _was_call   = p->last_expr_was_call;                         \
        bool _was_unpack = p->last_expr_was_unpack;                       \
        u32  _mpush      = p->last_multi_push;                            \
        p->last_expr_was_unpack = false;                                  \
        if (_was_unpack) {                                                \
            /* ...myfunc() — compare against all return values */         \
            emit_op(p, spread_op);                                        \
        } else if (_mpush > 1 && !(p->call_depth == 0                    \
                                   && check(p, TOK_COMMA))) {             \
            /* (~.~) myfunc() with no trailing comma — mask multi-push */ \
            emit_op_a(p, stack_op, (u16)_mpush);                          \
        } else if (p->call_depth == 0 && check(p, TOK_COMMA)) {          \
            /* user-defined comma list */                                  \
            u16 _n = (_mpush > 1) ? (u16)_mpush : 1;                     \
            if (_n == 1 && _was_call) emit_op(p, OP_TRUNCATE_RET);       \
            while (match(p, TOK_COMMA)) {                                 \
                p->last_multi_push      = 1;                              \
                p->last_expr_was_call   = false;                          \
                p->last_expr_was_unpack = false;                          \
                parse_precedence(p, next);                                \
                if (p->last_expr_was_call && !p->last_expr_was_unpack)    \
                    emit_op(p, OP_TRUNCATE_RET);                          \
                _n = (u16)(_n + p->last_multi_push);                      \
            }                                                             \
            emit_op_a(p, stack_op, _n);                                   \
        } else {                                                          \
            if (_was_call) emit_op(p, OP_TRUNCATE_RET);                   \
            emit_op(p, plain_op);                                         \
        }                                                                 \
    } while (0)

    switch (op) {
    case TOK_PLUS:       emit_op(p, OP_ADD);       break;
    case TOK_MINUS:      emit_op(p, OP_SUB);       break;
    case TOK_STAR:       emit_op(p, OP_MUL);       break;
    case TOK_SLASH:      emit_op(p, OP_DIV);       break;
    case TOK_PERCENT:    emit_op(p, OP_MOD);       break;
    case TOK_CARET:      emit_op(p, OP_POW);       break;
    case TOK_AMP:        emit_op(p, OP_BIT_AND);   break;
    case TOK_BITOR:      emit_op(p, OP_BIT_OR);    break;
    case TOK_BITXOR:     emit_op(p, OP_BIT_XOR);   break;
    case TOK_LSHIFT:     emit_op(p, OP_LSHIFT);    break;
    case TOK_RSHIFT:     emit_op(p, OP_RSHIFT);    break;
    case TOK_EQ:         MULTI_CMP(OP_EQ,  OP_EQ_STACK,  OP_EQ_SPREAD);  break;
    case TOK_NEQ:        MULTI_CMP(OP_NEQ, OP_NEQ_STACK, OP_NEQ_SPREAD); break;
    case TOK_LT:         MULTI_CMP(OP_LT,  OP_LT_STACK,  OP_LT_SPREAD);  break;
    case TOK_LEQ:        MULTI_CMP(OP_LEQ, OP_LEQ_STACK, OP_LEQ_SPREAD); break;
    case TOK_GT:
    case TOK_GEQ: {
        /* Convergent range check: min > val < max  (or >=, <=).
         * Stack before: [..., min, val]  →  parse max  →  OP_RANGE_CHECK.
         * Multi-comparison (comma) takes priority over chained operators. */
        bool _was_call   = p->last_expr_was_call;
        bool _was_unpack = p->last_expr_was_unpack;
        u32  _mpush      = p->last_multi_push;
        p->last_expr_was_unpack = false;
        CandoOpcode plain_op = (op == TOK_GT) ? OP_GT : OP_GEQ;
        CandoOpcode stack_op = (op == TOK_GT) ? OP_GT_STACK : OP_GEQ_STACK;
        CandoOpcode spread_op = (op == TOK_GT) ? OP_GT_SPREAD : OP_GEQ_SPREAD;
        CdoTokenType right_tok = p->current.type;
        if (right_tok == TOK_LT || right_tok == TOK_LEQ) {
            /* Range check — truncate RHS call to first value if needed. */
            if (_was_call && !_was_unpack) emit_op(p, OP_TRUNCATE_RET);
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
        } else if (_was_unpack) {
            emit_op(p, spread_op);
        } else if (_mpush > 1 && !(p->call_depth == 0
                                   && right_tok == TOK_COMMA)) {
            emit_op_a(p, stack_op, (u16)_mpush);
        } else if (p->call_depth == 0 && right_tok == TOK_COMMA) {
            u16 _n = (_mpush > 1) ? (u16)_mpush : 1;
            if (_n == 1 && _was_call) emit_op(p, OP_TRUNCATE_RET);
            while (match(p, TOK_COMMA)) {
                p->last_multi_push      = 1;
                p->last_expr_was_call   = false;
                p->last_expr_was_unpack = false;
                parse_precedence(p, next);
                if (p->last_expr_was_call && !p->last_expr_was_unpack)
                    emit_op(p, OP_TRUNCATE_RET);
                _n = (u16)(_n + p->last_multi_push);
            }
            emit_op_a(p, stack_op, _n);
        } else {
            if (_was_call) emit_op(p, OP_TRUNCATE_RET);
            emit_op(p, plain_op);
            if (right_tok == TOK_LT || right_tok == TOK_GT ||
                right_tok == TOK_LEQ || right_tok == TOK_GEQ) {
                error_current(p, "cannot chain comparison operators; use && to combine");
            }
        }
        break;
    }
    case TOK_RANGE_ASC:  emit_op(p, OP_RANGE_ASC); break;
    case TOK_RANGE_DESC: emit_op(p, OP_RANGE_DESC);break;
    default: break;
    }
#undef MULTI_CMP
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
        emit_op_a(p, OP_GET_FIELD, idx);
    }
}

/* Subscript: expr[expr] */
static void parse_subscript(CandoParser *p, bool can_assign)
{
    parse_expression(p);
    consume(p, TOK_RBRACKET, "expected ']' after index");
    if (can_assign && match(p, TOK_ASSIGN)) {
        parse_expression(p);
        emit_op(p, OP_SET_INDEX);
    } else {
        emit_op(p, OP_GET_INDEX);
    }
}

/* Function call: expr(arg, arg, ...) */
static void parse_call(CandoParser *p, bool can_assign)
{
    (void)can_assign;
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
    p->call_depth++;

    /* Parse the first expression and detect whether it is a sole function
     * call that spreads multiple return values onto the stack.  In that
     * case there will be no comma following the call, and we emit a single
     * OP_MASK_APPLY rather than per-value PASS/SKIP ops.                 */
    p->last_expr_was_call = false;
    parse_precedence(p, PREC_ASSIGN);
    bool single_call = p->last_expr_was_call && !check(p, TOK_COMMA);

    if (single_call) {
        /* The function already pushed `n` return values onto the stack.
         * Build a bitmask (bit i=1 → keep, 0 → skip) and emit one
         * OP_MASK_APPLY instruction to filter them at runtime.           */
        u16 bitmask = 0;
        for (u32 i = 0; i < n; i++) {
            if (bits[i]) {
                bitmask |= (u16)(1u << i);
                pass_count++;
            }
        }
        emit_op_ab(p, OP_MASK_APPLY, (u16)n, bitmask);
        p->last_expr_was_call   = false; /* mask consumed the spread */
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
    }

    p->call_depth--;
    p->last_multi_push = pass_count;
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

    /* Collection is already on the stack (left operand of ~>). */
    emit_op_a(p, OP_PIPE_INIT, 1);
    /* Stack: [result_arr, elem0..elemN-1, count, src_idx=0] */

    /* Declare the 'pipe' local variable so body expressions can use it. */
    scope_begin(p);
    emit_op(p, OP_NULL);
    u32 pipe_slot = declare_local(p, "pipe", 4, false);
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    /* Loop header: OP_PIPE_NEXT pushes the next element or jumps to exit. */
    u32 loop_start = cur(p)->code_len;
    u32 exit_jump  = emit_jump(p, OP_PIPE_NEXT);
    /* Element is now on expression stack; pop it into the pipe local. */
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    /* Parse body (expression or block). */
    if (match(p, TOK_LBRACE)) {
        /* Block body. 'return expr;' inside emits expr + OP_JUMP (patched
         * below) instead of OP_RETURN, so the enclosing function is not
         * exited.                                                         */
        bool saved_in_pipe     = p->in_pipe_body;
        u32  saved_exit_count  = p->pipe_exit_count;
        p->in_pipe_body    = true;
        p->pipe_exit_count = 0;

        parse_block(p);

        /* Fallthrough (no return statement executed): push null as result. */
        emit_op(p, OP_NULL);
        /* Patch all early-return exits to jump here (past the null). */
        for (u32 i = 0; i < p->pipe_exit_count; i++)
            patch_jump(p, p->pipe_exits[i]);

        p->in_pipe_body   = saved_in_pipe;
        p->pipe_exit_count = saved_exit_count;
    } else {
        /* Inline expression body. */
        parse_expression(p);
    }

    /* Append body result to the result array, then loop. */
    emit_op(p, OP_PIPE_COLLECT);
    emit_loop(p, loop_start);

    patch_jump(p, exit_jump);
    scope_end(p);
    emit_op(p, OP_PIPE_END);
}

/* Filter operator: collection ~!> predicate_expr
 *   Like pipe (~>) but null body results are dropped from the output.   */
static void parse_filter_op(CandoParser *p, bool can_assign)
{
    (void)can_assign;

    emit_op_a(p, OP_PIPE_INIT, 1);

    scope_begin(p);
    emit_op(p, OP_NULL);
    u32 pipe_slot = declare_local(p, "pipe", 4, false);
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    u32 loop_start = cur(p)->code_len;
    u32 exit_jump  = emit_jump(p, OP_FILTER_NEXT);
    emit_op_a(p, OP_DEF_LOCAL, (u16)pipe_slot);

    if (match(p, TOK_LBRACE)) {
        bool saved_in_pipe    = p->in_pipe_body;
        u32  saved_exit_count = p->pipe_exit_count;
        p->in_pipe_body    = true;
        p->pipe_exit_count = 0;

        parse_block(p);

        emit_op(p, OP_NULL);
        for (u32 i = 0; i < p->pipe_exit_count; i++)
            patch_jump(p, p->pipe_exits[i]);

        p->in_pipe_body    = saved_in_pipe;
        p->pipe_exit_count = saved_exit_count;
    } else {
        parse_expression(p);
    }

    emit_op(p, OP_FILTER_COLLECT);
    emit_loop(p, loop_start);

    patch_jump(p, exit_jump);
    scope_end(p);
    emit_op(p, OP_PIPE_END);
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

    /* Build the closure object and immediately spawn it as a thread. */
    u16 pc_idx = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_op_a(p, OP_CLOSURE, pc_idx);
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

    /* Pipe / filter */
    [TOK_PIPE_OP]        = { NULL,                 parse_pipe_op,    PREC_PIPE_PREC  },
    [TOK_FILTER_OP]      = { NULL,                 parse_filter_op,  PREC_PIPE_PREC  },
};

static const ParseRule *get_rule(CdoTokenType t)
{
    if ((u32)t >= TOK_COUNT) return &RULES[TOK_EOF];
    return &RULES[t];
}

/* =========================================================================
 * Core Pratt driver
 * ======================================================================= */

static void parse_precedence(CandoParser *p, Precedence min_prec)
{
    advance(p);
    ParseFn prefix = get_rule(p->previous.type)->prefix;
    if (!prefix) {
        error(p, "expected expression");
        return;
    }

    bool can_assign = (min_prec <= PREC_ASSIGN);
    prefix(p, can_assign);

    while (!check(p, TOK_EOF)) {
        Precedence cur_prec = get_rule(p->current.type)->precedence;
        if (cur_prec < min_prec) break;
        advance(p);
        ParseFn infix = get_rule(p->previous.type)->infix;
        if (!infix) break;
        infix(p, can_assign);
    }

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

    scope_begin(p);
    parse_block(p);
    scope_end(p);

    emit_loop(p, loop_start);
    patch_jump(p, exit_jump);
    emit_op(p, OP_POP);
}

/* --- FOR loop -----------------------------------------------------------
 * Syntax:  FOR ident IN expr { block }   -- iterate keys / indices
 *          FOR ident OF expr { block }   -- iterate values / elements
 *
 * Uses OP_FOR_INIT / OP_FOR_NEXT for proper iterator protocol.
 * The loop variable is bound as a local inside the loop scope.
 */
static void parse_for(CandoParser *p)
{
    consume(p, TOK_IDENT, "expected loop variable name");
    const char *var_name = p->previous.start;
    u32         var_len  = p->previous.length;

    bool keys_mode = false;
    if (match(p, TOK_IN)) {
        keys_mode = true;
    } else if (!match(p, TOK_OF) && !match(p, TOK_OVER)) {
        error_current(p, "expected IN, OF, or OVER after loop variable");
        return;
    }

    parse_expression(p);   /* iterable — now on stack */
    consume(p, TOK_LBRACE, "expected '{' after FOR iterable");

    /* OP_FOR_INIT mode: 1 = keys (IN), 0 = values (OF/OVER) */
    emit_op_a(p, OP_FOR_INIT, keys_mode ? 1 : 0);

    u32 loop_start = cur(p)->code_len;
    u32 exit_jump  = emit_jump(p, OP_FOR_NEXT);   /* jumps when exhausted */

    scope_begin(p);
    /* OP_FOR_NEXT pushed the next element; bind it as a local.          */
    u32 slot = declare_local(p, var_name, var_len, false);
    emit_op_a(p, OP_DEF_LOCAL, (u16)slot);

    parse_block(p);
    scope_end(p);

    emit_loop(p, loop_start);
    patch_jump(p, exit_jump);
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

#define MAX_PARAMS 64
    const char *param_names[MAX_PARAMS];
    u32         param_lens[MAX_PARAMS];
    u16 arity = 0;

    consume(p, TOK_LPAREN, "expected '(' after 'function'");
    if (!check(p, TOK_RPAREN)) {
        do {
            if (check(p, TOK_RPAREN)) break;
            if (match(p, TOK_VARARG)) break;
            consume(p, TOK_IDENT, "expected parameter name");
            if (arity >= MAX_PARAMS) { error(p, "too many parameters"); break; }
            param_names[arity] = p->previous.start;
            param_lens[arity]  = p->previous.length;
            arity++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected ')' after parameters");
    consume(p, TOK_LBRACE, "expected '{' before function body");

    u32 skip_body = emit_jump(p, OP_JUMP);
    u32 fn_start  = cur(p)->code_len;

    scope_begin(p);
    /* Slot 0 is the function value in the call frame; reserve it with a
     * sentinel local so scope_end cleans up the slot numbering correctly. */
    declare_local(p, "", 0, false);
    for (u16 i = 0; i < arity; i++)
        declare_local(p, param_names[i], param_lens[i], false);
    parse_block(p);
    scope_end(p);

    emit_op(p, OP_NULL);
    emit_op_a(p, OP_RETURN, 1);

    patch_jump(p, skip_body);

    u16 pc_idx = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_op_a(p, OP_CLOSURE, pc_idx);
#undef MAX_PARAMS
}

/* --- FUNCTION declaration -----------------------------------------------
 * Syntax:  FUNCTION name(params) { block }
 *
 * Emits a jump over the body, then the body ending with OP_RETURN.
 * The function's start PC is stored as a number constant and defined
 * as a global variable.  Parameters are tracked but not yet bound to
 * local slots in this flat-chunk model.
 */
static void parse_function(CandoParser *p)
{
    consume(p, TOK_IDENT, "expected function name");
    const char *fn_name = p->previous.start;
    u32         fn_len  = p->previous.length;

    consume(p, TOK_LPAREN, "expected '(' after function name");

#define MAX_PARAMS 64
    const char *param_names[MAX_PARAMS];
    u32         param_lens[MAX_PARAMS];
    u16 arity = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (check(p, TOK_RPAREN)) break;
            if (match(p, TOK_VARARG)) break;
            consume(p, TOK_IDENT, "expected parameter name");
            if (arity >= MAX_PARAMS) { error(p, "too many parameters"); break; }
            param_names[arity] = p->previous.start;
            param_lens[arity]  = p->previous.length;
            arity++;
        } while (match(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected ')' after parameters");
    consume(p, TOK_LBRACE, "expected '{' before function body");

    /* Jump over the body at definition time. */
    u32 skip_body = emit_jump(p, OP_JUMP);

    u32 fn_start = cur(p)->code_len;

    scope_begin(p);
    /* Slot 0 is the function value in the call frame; reserve it with a
     * sentinel local so scope_end cleans up the slot numbering correctly. */
    declare_local(p, "", 0, false);
    for (u16 i = 0; i < arity; i++)
        declare_local(p, param_names[i], param_lens[i], false);
    parse_block(p);
    scope_end(p);

    emit_op(p, OP_NULL);          /* implicit return null */
    emit_op_a(p, OP_RETURN, 1);

    patch_jump(p, skip_body);

    /* Build a closure object from the function's start PC and define it. */
    u16 pc_idx   = cando_chunk_add_const(cur(p), cando_number((f64)fn_start));
    emit_op_a(p, OP_CLOSURE, pc_idx);
    u16 name_idx = str_const(p, fn_name, fn_len);
    emit_op_a(p, OP_DEF_GLOBAL, name_idx);
#undef MAX_PARAMS
}

/* --- CLASS declaration --------------------------------------------------
 * Syntax:  CLASS Name { [FUNCTION method(params) { block }]* }
 */
static void parse_class(CandoParser *p)
{
    consume(p, TOK_IDENT, "expected class name");
    u16 name_idx = prev_name_const(p);

    /* Optional superclass / constructor parameters */
    if (match(p, TOK_LPAREN)) {
        u32 depth = 1;
        while (!check(p, TOK_EOF) && depth > 0) {
            if (match(p, TOK_LPAREN))      depth++;
            else if (match(p, TOK_RPAREN)) depth--;
            else                            advance(p);
        }
    }

    consume(p, TOK_LBRACE, "expected '{' before class body");
    emit_op_a(p, OP_NEW_CLASS, name_idx);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        match(p, TOK_STATIC);
        match(p, TOK_PRIVATE);

        if (match(p, TOK_FUNCTION)) {
            consume(p, TOK_IDENT, "expected method name");
            u16 meth_idx = prev_name_const(p);

            consume(p, TOK_LPAREN, "expected '(' after method name");
            if (!check(p, TOK_RPAREN)) {
                do {
                    if (check(p, TOK_RPAREN)) break;
                    if (match(p, TOK_VARARG)) break;
                    consume(p, TOK_IDENT, "expected parameter name");
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "expected ')' after method parameters");
            consume(p, TOK_LBRACE, "expected '{' before method body");

            u32 skip = emit_jump(p, OP_JUMP);
            u32 meth_start = cur(p)->code_len;

            scope_begin(p);
            parse_block(p);
            scope_end(p);

            emit_op(p, OP_NULL);
            emit_op_a(p, OP_RETURN, 1);
            patch_jump(p, skip);

            /* Push method PC as number, then bind to the class on TOS.  */
            u16 pc_idx = cando_chunk_add_const(cur(p),
                                               cando_number((f64)meth_start));
            emit_op_a(p, OP_CONST, pc_idx);
            emit_op_a(p, OP_BIND_METHOD, meth_idx);
        } else {
            advance(p);  /* skip unknown token */
        }
    }

    consume(p, TOK_RBRACE, "expected '}' after class body");
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
    p->local_count        = 0;
    p->scope_depth        = 0;
    p->last_expr_was_call   = false;
    p->last_expr_was_unpack = false;
    p->call_depth           = 0;
    p->eval_mode          = false;
    p->last_stmt_was_expr = false;
    p->in_pipe_body       = false;
    p->pipe_exit_count    = 0;
    p->last_multi_push    = 1;
    p->error_msg[0] = '\0';

    p->current.type = TOK_EOF;
    advance(p);
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
