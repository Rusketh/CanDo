/*
 * opcodes.h -- Cando bytecode instruction set.
 *
 * Instructions are grouped into logical bands for readability.  Every
 * instruction is a single byte (CandoOpcode / u8).  Instructions that
 * require an operand are followed immediately by two bytes encoding a
 * little-endian u16.  Signed operands (jump offsets) are interpreted as
 * i16 at the call site.
 *
 * Helper macros:
 *   CANDO_OP_HAS_ARG(op)   -- true if the opcode carries a 2-byte operand
 *   CANDO_OP_NAME(op)      -- static ASCII name string (defined in opcodes.c)
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_OPCODES_H
#define CANDO_OPCODES_H

#include "../core/common.h"

/* -------------------------------------------------------------------------
 * Loop type identifiers -- stored in CandoLoopFrame.loop_type and encoded
 * in bits[15:14] of OP_LOOP_MARK's B operand.
 * ---------------------------------------------------------------------- */
#define CANDO_LOOP_WHILE    0   /* WHILE loop                             */
#define CANDO_LOOP_FOR      1   /* FOR IN / FOR OF loop                   */
#define CANDO_LOOP_FOR_OVER 2   /* FOR OVER (Lua-style triplet) loop      */

/* -------------------------------------------------------------------------
 * CandoOpcode -- the full Cando instruction set.
 *
 * Ordering within bands is stable; do not reorder without updating
 * opcodes.c and any existing serialised bytecode.
 * ---------------------------------------------------------------------- */
typedef enum {

    /* ===== Band 0: Literal constants ==================================== */
    OP_CONST,           /* push constants[A]                              */
    OP_NULL,            /* push null                                      */
    OP_TRUE,            /* push true                                      */
    OP_FALSE,           /* push false                                     */

    /* ===== Band 1: Stack manipulation =================================== */
    OP_POP,             /* discard top of stack                           */
    OP_POP_N,           /* discard top A values (A = count)               */
    OP_DUP,             /* duplicate top                                  */

    /* ===== Band 2: Local variables ====================================== */
    OP_LOAD_LOCAL,      /* push locals[A]                                 */
    OP_STORE_LOCAL,     /* locals[A] = peek (no pop)                      */
    OP_DEF_LOCAL,       /* pop and assign to local slot A                 */
    OP_DEF_CONST_LOCAL, /* pop and assign to const local slot A           */

    /* ===== Band 3: Global variables ===================================== */
    OP_LOAD_GLOBAL,     /* push global named by constants[A]              */
    OP_STORE_GLOBAL,    /* global[constants[A]] = peek (no pop)           */
    OP_DEF_GLOBAL,      /* pop and assign to global named by constants[A] */
    OP_DEF_CONST_GLOBAL,/* pop and assign to const global constants[A]    */

    /* ===== Band 4: Upvalues (closures) ================================== */
    OP_LOAD_UPVAL,      /* push upvalues[A]                               */
    OP_STORE_UPVAL,     /* upvalues[A] = peek (no pop)                    */
    OP_CLOSE_UPVAL,     /* close upvalue at stack slot A to the heap      */

    /* ===== Band 5: Arithmetic =========================================== */
    OP_ADD,             /* b=pop, a=pop, push a+b                         */
    OP_SUB,             /* b=pop, a=pop, push a-b                         */
    OP_MUL,             /* b=pop, a=pop, push a*b                         */
    OP_DIV,             /* b=pop, a=pop, push a/b                         */
    OP_MOD,             /* b=pop, a=pop, push a%b                         */
    OP_POW,             /* b=pop, a=pop, push a^b                         */
    OP_NEG,             /* a=pop, push -a                                 */
    OP_POS,             /* a=pop, push +a (numeric coerce)                */
    OP_INCR,            /* top += 1 (in-place, no pop)                    */
    OP_DECR,            /* top -= 1 (in-place, no pop)                    */

    /* ===== Band 6: Comparison =========================================== */
    OP_EQ,              /* b=pop, a=pop, push a==b                        */
    OP_NEQ,             /* b=pop, a=pop, push a!=b                        */
    OP_LT,              /* b=pop, a=pop, push a<b                         */
    OP_GT,              /* b=pop, a=pop, push a>b                         */
    OP_LEQ,             /* b=pop, a=pop, push a<=b                        */
    OP_GEQ,             /* b=pop, a=pop, push a>=b                        */
    /* Multi-value comparison: A=count; pops A values (the right-hand
     * stack), then pops one more (the left operand), and pushes bool.
     * EQ_STACK  → true if left matches ANY  right value
     * NEQ_STACK → true if left matches NONE of the right values
     * LT/GT/LEQ/GEQ_STACK → true if relation holds against ALL rights  */
    OP_EQ_STACK,
    OP_NEQ_STACK,
    OP_LT_STACK,
    OP_GT_STACK,
    OP_LEQ_STACK,
    OP_GEQ_STACK,
    /* Range check: max=pop, val=pop, min=pop; A encodes inclusive flags  */
    OP_RANGE_CHECK,

    /* ===== Band 7: Bitwise ============================================== */
    OP_BIT_AND,         /* b=pop, a=pop, push a&b                         */
    OP_BIT_OR,          /* b=pop, a=pop, push a|b                         */
    OP_BIT_XOR,         /* b=pop, a=pop, push a|&b                        */
    OP_BIT_NOT,         /* a=pop, push ~a                                 */
    OP_LSHIFT,          /* b=pop, a=pop, push a<<b                        */
    OP_RSHIFT,          /* b=pop, a=pop, push a>>b                        */

    /* ===== Band 8: Logical ============================================== */
    OP_NOT,             /* a=pop, push !a                                 */
    OP_AND_JUMP,        /* if peek is falsy  jump forward A bytes (short circuit) */
    OP_OR_JUMP,         /* if peek is truthy jump forward A bytes (short circuit) */

    /* ===== Band 9: Objects and arrays =================================== */
    OP_NEW_OBJECT,      /* create empty object, push handle               */
    OP_NEW_ARRAY,       /* pop A values, create array, push handle        */
    OP_GET_FIELD,       /* obj=pop, push obj[constants[A]]                */
    OP_SET_FIELD,       /* val=pop, obj=peek, obj[constants[A]] = val     */
    OP_GET_INDEX,       /* idx=pop, obj=pop, push obj[idx]                */
    OP_SET_INDEX,       /* val=pop, idx=pop, obj=peek, obj[idx]=val       */
    OP_LEN,             /* a=pop, push #a                                 */
    OP_KEYS_OF,         /* a=pop, push stack of keys   (IN  operator)     */
    OP_VALS_OF,         /* a=pop, push stack of values (OF  operator)     */

    /* ===== Band 10: Control flow ======================================== */
    OP_JUMP,            /* unconditional; A = signed i16 byte offset      */
    OP_JUMP_IF_FALSE,   /* pop, jump if falsy; A = signed i16 offset      */
    OP_JUMP_IF_TRUE,    /* pop, jump if truthy; A = signed i16 offset     */
    OP_LOOP,            /* unconditional backward; A = unsigned back bytes */
    /* Break/continue: A encodes loop depth (0 = innermost).
     * The VM walks the loop-frame stack A levels up and jumps.           */
    OP_BREAK,
    OP_CONTINUE,
    /* Loop markers: OP_LOOP_MARK (OPFMT_A_B) records break/continue targets.
     * A = forward offset to break target; B = packed(cont_back[13:0], type[15:14]).
     * loop_type values (bits 15-14 of B):                                */
    OP_LOOP_MARK,
    OP_LOOP_END,        /* pop one loop frame                             */

    /* ===== Band 11: Functions and calls ================================= */
    OP_CLOSURE,         /* build closure from chunk prototype constants[A] */
    OP_CALL,            /* call TOS with A args (pushed before fn)        */
    OP_METHOD_CALL,     /* obj:constants[A](...); arg count in B (next u16) */
    OP_FLUENT_CALL,     /* obj::constants[A](...); returns obj            */
    OP_RETURN,          /* return top A values from current frame         */
    OP_TAIL_CALL,       /* tail-recursive call with A args                */

    /* ===== Band 12: Variable arguments ================================== */
    OP_LOAD_VARARG,     /* push vararg slot A (UINT16_MAX = push all)     */
    OP_VARARG_LEN,      /* push number of varargs                         */
    OP_UNPACK,          /* pop array/object, push all values onto stack   */

    /* ===== Band 13: Iteration =========================================== */
    OP_RANGE_ASC,       /* b=pop, a=pop, push ascending range a->b        */
    OP_RANGE_DESC,      /* b=pop, a=pop, push descending range b<-a       */
    OP_FOR_INIT,        /* init for-each; A = number of loop variables    */
    OP_FOR_NEXT,        /* advance for-each; A = signed jump offset if done */
    OP_FOR_OVER_INIT,   /* init over loop (function-based iterator)       */
    OP_FOR_OVER_NEXT,   /* advance over loop; A = signed jump if done     */
    OP_PIPE_INIT,       /* init pipe (~>) loop; A = stack element count   */
    OP_PIPE_NEXT,       /* advance pipe; A = signed jump if exhausted     */
    OP_FILTER_NEXT,     /* advance filter (~!>); A = signed jump          */
    OP_PIPE_END,        /* clean up pipe state; result_arr stays on stack */
    OP_PIPE_COLLECT,    /* pop body result and append to result array     */
    OP_FILTER_COLLECT,  /* like PIPE_COLLECT but skips null results       */

    /* ===== Band 14: Error handling ====================================== */
    OP_TRY_BEGIN,       /* push try frame; A = signed offset to catch     */
    OP_TRY_END,         /* normal exit from try block                     */
    OP_CATCH_BEGIN,     /* begin catch; A = local count for bound vars    */
    OP_FINALLY_BEGIN,   /* begin finally; A = signed offset past finally  */
    OP_THROW,           /* pop A values and throw; A = count              */
    OP_RERAISE,         /* re-throw current exception                     */

    /* ===== Band 15: Async / threads ===================================== */
    OP_ASYNC,           /* (reserved — was fiber async; unused)           */
    OP_AWAIT,           /* pop CdoThread handle, block until done, push
                           return values; sets last_ret_count             */
    OP_YIELD,           /* (reserved — was fiber yield; unused)           */
    OP_THREAD,          /* pop OBJ_FUNCTION closure, spawn OS thread,
                           push CdoThread handle                          */

    /* ===== Band 16: Class sugar ========================================= */
    OP_NEW_CLASS,       /* create class obj; A = name constant index      */
    OP_BIND_METHOD,     /* bind method at constants[A] to class on TOS    */
    OP_INHERIT,         /* set __index on child class to parent class     */

    /* ===== Band 17: Mask / selector operators =========================== */
    /* These implement the ~ (consume/pass) and . (skip) selector prefixes.
     * ~ keeps the top-of-stack value; . pops and releases it.            */
    OP_MASK_PASS,       /* ~ : keep top-of-stack value (no-op)            */
    OP_MASK_SKIP,       /* . : pop and discard top-of-stack value         */
    OP_MASK_APPLY,      /* apply bitmask to top A stack values; B=bitmask */

    /* ===== Band 18: Multi-return spreading ============================== */
    OP_SPREAD_RET,      /* adjust spread_extra by last_ret_count - 1      */
    OP_ARRAY_SPREAD,    /* adjust array_extra by last_ret_count - 1       */

    /* ===== Band 19: Call-result comparison ============================== */
    /* OP_TRUNCATE_RET: pop all but the first return value from the last
     * call, leaving exactly one value on the stack.  Uses last_ret_count. */
    OP_TRUNCATE_RET,
    /* Spread comparisons: compare stack[top - last_ret_count - 1] against
     * the top last_ret_count values (all return values from last call).
     * EQ_SPREAD / NEQ_SPREAD semantics match EQ_STACK / NEQ_STACK.
     * LT/GT/LEQ/GEQ_SPREAD require the relation to hold against ALL.      */
    OP_EQ_SPREAD,
    OP_NEQ_SPREAD,
    OP_LT_SPREAD,
    OP_GT_SPREAD,
    OP_LEQ_SPREAD,
    OP_GEQ_SPREAD,

    /* ===== Sentinels ==================================================== */
    OP_NOP,             /* no operation                                   */
    OP_HALT,            /* stop the VM (used for top-level script end)    */

    OP_COUNT            /* total number of opcodes — keep last            */
} CandoOpcode;

/* -------------------------------------------------------------------------
 * Operand format for each opcode.
 * ---------------------------------------------------------------------- */
typedef enum {
    OPFMT_NONE,         /* no operand                                     */
    OPFMT_A,            /* one u16 operand A                              */
    OPFMT_A_B,          /* two u16 operands A, B (4 bytes total)          */
} CandoOpFmt;

/* -------------------------------------------------------------------------
 * cando_opcode_name -- return a static ASCII name, e.g. "OP_ADD".
 *                      Returns "OP_UNKNOWN" for out-of-range values.
 * ---------------------------------------------------------------------- */
const char *cando_opcode_name(CandoOpcode op);

/* -------------------------------------------------------------------------
 * cando_opcode_fmt -- return the operand format for `op`.
 * ---------------------------------------------------------------------- */
CandoOpFmt cando_opcode_fmt(CandoOpcode op);

/* -------------------------------------------------------------------------
 * Convenience: byte-size of the full instruction (opcode + operands).
 * ---------------------------------------------------------------------- */
CANDO_INLINE u32 cando_opcode_size(CandoOpcode op) {
    switch (cando_opcode_fmt(op)) {
        case OPFMT_NONE: return 1;
        case OPFMT_A:    return 3;
        case OPFMT_A_B:  return 5;
        default:         return 1;
    }
}

/* -------------------------------------------------------------------------
 * Operand read/write helpers — little-endian u16 stored in bytecode.
 * ---------------------------------------------------------------------- */
CANDO_INLINE u16 cando_read_u16(const u8 *ip) {
    return (u16)(ip[0] | (ip[1] << 8));
}

CANDO_INLINE i16 cando_read_i16(const u8 *ip) {
    return (i16)cando_read_u16(ip);
}

CANDO_INLINE void cando_write_u16(u8 *ip, u16 val) {
    ip[0] = (u8)(val & 0xFF);
    ip[1] = (u8)(val >> 8);
}

#endif /* CANDO_OPCODES_H */
