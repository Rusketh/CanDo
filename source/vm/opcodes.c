/*
 * opcodes.c -- Opcode name table and operand-format table for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "opcodes.h"

/* -------------------------------------------------------------------------
 * Name table -- parallel to CandoOpcode enum.
 * ---------------------------------------------------------------------- */
static const char *const s_opcode_names[OP_COUNT] = {
    /* Band 0: constants */
    [OP_CONST]            = "OP_CONST",
    [OP_NULL]             = "OP_NULL",
    [OP_TRUE]             = "OP_TRUE",
    [OP_FALSE]            = "OP_FALSE",
    /* Band 1: stack */
    [OP_POP]              = "OP_POP",
    [OP_POP_N]            = "OP_POP_N",
    [OP_DUP]              = "OP_DUP",
    /* Band 2: locals */
    [OP_LOAD_LOCAL]       = "OP_LOAD_LOCAL",
    [OP_STORE_LOCAL]      = "OP_STORE_LOCAL",
    [OP_DEF_LOCAL]        = "OP_DEF_LOCAL",
    [OP_DEF_CONST_LOCAL]  = "OP_DEF_CONST_LOCAL",
    /* Band 3: globals */
    [OP_LOAD_GLOBAL]      = "OP_LOAD_GLOBAL",
    [OP_STORE_GLOBAL]     = "OP_STORE_GLOBAL",
    [OP_DEF_GLOBAL]       = "OP_DEF_GLOBAL",
    [OP_DEF_CONST_GLOBAL] = "OP_DEF_CONST_GLOBAL",
    /* Band 4: upvalues */
    [OP_LOAD_UPVAL]       = "OP_LOAD_UPVAL",
    [OP_STORE_UPVAL]      = "OP_STORE_UPVAL",
    [OP_CLOSE_UPVAL]      = "OP_CLOSE_UPVAL",
    /* Band 5: arithmetic */
    [OP_ADD]              = "OP_ADD",
    [OP_SUB]              = "OP_SUB",
    [OP_MUL]              = "OP_MUL",
    [OP_DIV]              = "OP_DIV",
    [OP_MOD]              = "OP_MOD",
    [OP_POW]              = "OP_POW",
    [OP_NEG]              = "OP_NEG",
    [OP_POS]              = "OP_POS",
    [OP_INCR]             = "OP_INCR",
    [OP_DECR]             = "OP_DECR",
    /* Band 6: comparison */
    [OP_EQ]               = "OP_EQ",
    [OP_NEQ]              = "OP_NEQ",
    [OP_LT]               = "OP_LT",
    [OP_GT]               = "OP_GT",
    [OP_LEQ]              = "OP_LEQ",
    [OP_GEQ]              = "OP_GEQ",
    [OP_EQ_STACK]         = "OP_EQ_STACK",
    [OP_NEQ_STACK]        = "OP_NEQ_STACK",
    [OP_LT_STACK]         = "OP_LT_STACK",
    [OP_GT_STACK]         = "OP_GT_STACK",
    [OP_LEQ_STACK]        = "OP_LEQ_STACK",
    [OP_GEQ_STACK]        = "OP_GEQ_STACK",
    [OP_RANGE_CHECK]      = "OP_RANGE_CHECK",
    /* Band 7: bitwise */
    [OP_BIT_AND]          = "OP_BIT_AND",
    [OP_BIT_OR]           = "OP_BIT_OR",
    [OP_BIT_XOR]          = "OP_BIT_XOR",
    [OP_BIT_NOT]          = "OP_BIT_NOT",
    [OP_LSHIFT]           = "OP_LSHIFT",
    [OP_RSHIFT]           = "OP_RSHIFT",
    /* Band 8: logical */
    [OP_NOT]              = "OP_NOT",
    [OP_AND_JUMP]         = "OP_AND_JUMP",
    [OP_OR_JUMP]          = "OP_OR_JUMP",
    /* Band 9: objects */
    [OP_NEW_OBJECT]       = "OP_NEW_OBJECT",
    [OP_NEW_ARRAY]        = "OP_NEW_ARRAY",
    [OP_GET_FIELD]        = "OP_GET_FIELD",
    [OP_SET_FIELD]        = "OP_SET_FIELD",
    [OP_GET_INDEX]        = "OP_GET_INDEX",
    [OP_SET_INDEX]        = "OP_SET_INDEX",
    [OP_LEN]              = "OP_LEN",
    [OP_KEYS_OF]          = "OP_KEYS_OF",
    [OP_VALS_OF]          = "OP_VALS_OF",
    /* Band 10: control */
    [OP_JUMP]             = "OP_JUMP",
    [OP_JUMP_IF_FALSE]    = "OP_JUMP_IF_FALSE",
    [OP_JUMP_IF_TRUE]     = "OP_JUMP_IF_TRUE",
    [OP_JUMP_IF_NULL]     = "OP_JUMP_IF_NULL",
    [OP_LOOP]             = "OP_LOOP",
    [OP_BREAK]            = "OP_BREAK",
    [OP_CONTINUE]         = "OP_CONTINUE",
    [OP_LOOP_MARK]        = "OP_LOOP_MARK",
    [OP_LOOP_END]         = "OP_LOOP_END",
    [OP_IF_MARK]          = "OP_IF_MARK",
    [OP_IF_END]           = "OP_IF_END",
    [OP_SETTLE]           = "OP_SETTLE",
    [OP_IF_TEST_MATCHED]  = "OP_IF_TEST_MATCHED",
    [OP_IF_TEST_PREV]     = "OP_IF_TEST_PREV",
    [OP_IF_SET_RAN]       = "OP_IF_SET_RAN",
    [OP_IF_CLEAR_PREV]    = "OP_IF_CLEAR_PREV",
    /* Band 11: functions */
    [OP_CLOSURE]          = "OP_CLOSURE",
    [OP_CALL]             = "OP_CALL",
    [OP_METHOD_CALL]      = "OP_METHOD_CALL",
    [OP_FLUENT_CALL]      = "OP_FLUENT_CALL",
    [OP_RETURN]           = "OP_RETURN",
    [OP_TAIL_CALL]        = "OP_TAIL_CALL",
    /* Band 12: varargs */
    [OP_LOAD_VARARG]      = "OP_LOAD_VARARG",
    [OP_VARARG_LEN]       = "OP_VARARG_LEN",
    [OP_UNPACK]           = "OP_UNPACK",
    /* Band 13: iteration */
    [OP_RANGE_ASC]        = "OP_RANGE_ASC",
    [OP_RANGE_DESC]       = "OP_RANGE_DESC",
    [OP_FOR_INIT]         = "OP_FOR_INIT",
    [OP_FOR_NEXT]         = "OP_FOR_NEXT",
    [OP_FOR_OVER_INIT]    = "OP_FOR_OVER_INIT",
    [OP_FOR_OVER_NEXT]    = "OP_FOR_OVER_NEXT",
    [OP_PIPE_INIT]        = "OP_PIPE_INIT",
    [OP_PIPE_NEXT]        = "OP_PIPE_NEXT",
    [OP_FILTER_NEXT]      = "OP_FILTER_NEXT",
    [OP_PIPE_END]         = "OP_PIPE_END",
    [OP_PIPE_COLLECT]     = "OP_PIPE_COLLECT",
    [OP_FILTER_COLLECT]   = "OP_FILTER_COLLECT",
    [OP_COND_FILTER_COLLECT] = "OP_COND_FILTER_COLLECT",
    /* Band 14: error handling */
    [OP_TRY_BEGIN]        = "OP_TRY_BEGIN",
    [OP_TRY_END]          = "OP_TRY_END",
    [OP_CATCH_BEGIN]      = "OP_CATCH_BEGIN",
    [OP_FINALLY_BEGIN]    = "OP_FINALLY_BEGIN",
    [OP_THROW]            = "OP_THROW",
    [OP_RERAISE]          = "OP_RERAISE",
    /* Band 15: threads */
    [OP_ASYNC]            = "OP_ASYNC",
    [OP_AWAIT]            = "OP_AWAIT",
    [OP_YIELD]            = "OP_YIELD",
    [OP_THREAD]           = "OP_THREAD",
    /* Band 16: classes */
    [OP_NEW_CLASS]         = "OP_NEW_CLASS",
    [OP_BIND_METHOD]       = "OP_BIND_METHOD",
    [OP_INHERIT]           = "OP_INHERIT",
    [OP_BIND_DEFAULT_CALL] = "OP_BIND_DEFAULT_CALL",
    /* Band 17: mask/selector */
    [OP_MASK_PASS]        = "OP_MASK_PASS",
    [OP_MASK_SKIP]        = "OP_MASK_SKIP",
    [OP_MASK_APPLY]       = "OP_MASK_APPLY",
    /* Band 18: multi-return spreading */
    [OP_SPREAD_RET]       = "OP_SPREAD_RET",
    [OP_ARRAY_SPREAD]     = "OP_ARRAY_SPREAD",
    /* Band 19: call-result comparison */
    [OP_TRUNCATE_RET]     = "OP_TRUNCATE_RET",
    [OP_EQ_SPREAD]        = "OP_EQ_SPREAD",
    [OP_NEQ_SPREAD]       = "OP_NEQ_SPREAD",
    [OP_LT_SPREAD]        = "OP_LT_SPREAD",
    [OP_GT_SPREAD]        = "OP_GT_SPREAD",
    [OP_LEQ_SPREAD]       = "OP_LEQ_SPREAD",
    [OP_GEQ_SPREAD]       = "OP_GEQ_SPREAD",
    /* Sentinels */
    [OP_NOP]              = "OP_NOP",
    [OP_HALT]             = "OP_HALT",
};

/* -------------------------------------------------------------------------
 * Format table -- parallel to CandoOpcode enum.
 * ---------------------------------------------------------------------- */
static const CandoOpFmt s_opcode_fmts[OP_COUNT] = {
    /* Band 0: constants */
    [OP_CONST]            = OPFMT_A,
    [OP_NULL]             = OPFMT_NONE,
    [OP_TRUE]             = OPFMT_NONE,
    [OP_FALSE]            = OPFMT_NONE,
    /* Band 1: stack */
    [OP_POP]              = OPFMT_NONE,
    [OP_POP_N]            = OPFMT_A,
    [OP_DUP]              = OPFMT_NONE,
    /* Band 2: locals */
    [OP_LOAD_LOCAL]       = OPFMT_A,
    [OP_STORE_LOCAL]      = OPFMT_A,
    [OP_DEF_LOCAL]        = OPFMT_A,
    [OP_DEF_CONST_LOCAL]  = OPFMT_A,
    /* Band 3: globals */
    [OP_LOAD_GLOBAL]      = OPFMT_A,
    [OP_STORE_GLOBAL]     = OPFMT_A,
    [OP_DEF_GLOBAL]       = OPFMT_A,
    [OP_DEF_CONST_GLOBAL] = OPFMT_A,
    /* Band 4: upvalues */
    [OP_LOAD_UPVAL]       = OPFMT_A,
    [OP_STORE_UPVAL]      = OPFMT_A,
    [OP_CLOSE_UPVAL]      = OPFMT_A,
    /* Band 5: arithmetic */
    [OP_ADD]              = OPFMT_NONE,
    [OP_SUB]              = OPFMT_NONE,
    [OP_MUL]              = OPFMT_NONE,
    [OP_DIV]              = OPFMT_NONE,
    [OP_MOD]              = OPFMT_NONE,
    [OP_POW]              = OPFMT_NONE,
    [OP_NEG]              = OPFMT_NONE,
    [OP_POS]              = OPFMT_NONE,
    [OP_INCR]             = OPFMT_NONE,
    [OP_DECR]             = OPFMT_NONE,
    /* Band 6: comparison */
    [OP_EQ]               = OPFMT_NONE,
    [OP_NEQ]              = OPFMT_NONE,
    [OP_LT]               = OPFMT_NONE,
    [OP_GT]               = OPFMT_NONE,
    [OP_LEQ]              = OPFMT_NONE,
    [OP_GEQ]              = OPFMT_NONE,
    [OP_EQ_STACK]         = OPFMT_A,
    [OP_NEQ_STACK]        = OPFMT_A,
    [OP_LT_STACK]         = OPFMT_A,
    [OP_GT_STACK]         = OPFMT_A,
    [OP_LEQ_STACK]        = OPFMT_A,
    [OP_GEQ_STACK]        = OPFMT_A,
    [OP_RANGE_CHECK]      = OPFMT_A,
    /* Band 7: bitwise */
    [OP_BIT_AND]          = OPFMT_NONE,
    [OP_BIT_OR]           = OPFMT_NONE,
    [OP_BIT_XOR]          = OPFMT_NONE,
    [OP_BIT_NOT]          = OPFMT_NONE,
    [OP_LSHIFT]           = OPFMT_NONE,
    [OP_RSHIFT]           = OPFMT_NONE,
    /* Band 8: logical */
    [OP_NOT]              = OPFMT_NONE,
    [OP_AND_JUMP]         = OPFMT_A,
    [OP_OR_JUMP]          = OPFMT_A,
    /* Band 9: objects */
    [OP_NEW_OBJECT]       = OPFMT_NONE,
    [OP_NEW_ARRAY]        = OPFMT_A,
    [OP_GET_FIELD]        = OPFMT_A,
    [OP_SET_FIELD]        = OPFMT_A,
    [OP_GET_INDEX]        = OPFMT_NONE,
    [OP_SET_INDEX]        = OPFMT_NONE,
    [OP_LEN]              = OPFMT_NONE,
    [OP_KEYS_OF]          = OPFMT_NONE,
    [OP_VALS_OF]          = OPFMT_NONE,
    /* Band 10: control */
    [OP_JUMP]             = OPFMT_A,
    [OP_JUMP_IF_FALSE]    = OPFMT_A,
    [OP_JUMP_IF_TRUE]     = OPFMT_A,
    [OP_JUMP_IF_NULL]     = OPFMT_A,
    [OP_LOOP]             = OPFMT_A,
    [OP_BREAK]            = OPFMT_A,
    [OP_CONTINUE]         = OPFMT_A,
    [OP_LOOP_MARK]        = OPFMT_A_B,
    [OP_LOOP_END]         = OPFMT_NONE,
    [OP_IF_MARK]          = OPFMT_A,
    [OP_IF_END]           = OPFMT_NONE,
    [OP_SETTLE]           = OPFMT_A,
    [OP_IF_TEST_MATCHED]  = OPFMT_NONE,
    [OP_IF_TEST_PREV]     = OPFMT_NONE,
    [OP_IF_SET_RAN]       = OPFMT_NONE,
    [OP_IF_CLEAR_PREV]    = OPFMT_NONE,
    /* Band 11: functions */
    [OP_CLOSURE]          = OPFMT_A,
    [OP_CALL]             = OPFMT_A,
    [OP_METHOD_CALL]      = OPFMT_A_B,
    [OP_FLUENT_CALL]      = OPFMT_A_B,
    [OP_RETURN]           = OPFMT_A,
    [OP_TAIL_CALL]        = OPFMT_A,
    /* Band 12: varargs */
    [OP_LOAD_VARARG]      = OPFMT_A,
    [OP_VARARG_LEN]       = OPFMT_NONE,
    [OP_UNPACK]           = OPFMT_NONE,
    /* Band 13: iteration */
    [OP_RANGE_ASC]        = OPFMT_NONE,
    [OP_RANGE_DESC]       = OPFMT_NONE,
    [OP_FOR_INIT]         = OPFMT_A,
    [OP_FOR_NEXT]         = OPFMT_A,
    [OP_FOR_OVER_INIT]    = OPFMT_A_B,
    [OP_FOR_OVER_NEXT]    = OPFMT_A,
    [OP_PIPE_INIT]        = OPFMT_A,
    [OP_PIPE_NEXT]        = OPFMT_A,
    [OP_FILTER_NEXT]      = OPFMT_A,
    [OP_PIPE_END]            = OPFMT_NONE,
    [OP_PIPE_COLLECT]        = OPFMT_NONE,
    [OP_FILTER_COLLECT]      = OPFMT_NONE,
    [OP_COND_FILTER_COLLECT] = OPFMT_NONE,
    /* Band 14: error handling */
    [OP_TRY_BEGIN]        = OPFMT_A,
    [OP_TRY_END]          = OPFMT_NONE,
    [OP_CATCH_BEGIN]      = OPFMT_A,
    [OP_FINALLY_BEGIN]    = OPFMT_A,
    [OP_THROW]            = OPFMT_A,
    [OP_RERAISE]          = OPFMT_NONE,
    /* Band 15: threads */
    [OP_ASYNC]            = OPFMT_NONE,
    [OP_AWAIT]            = OPFMT_NONE,
    [OP_YIELD]            = OPFMT_NONE,
    [OP_THREAD]           = OPFMT_NONE,
    /* Band 16: classes */
    [OP_NEW_CLASS]         = OPFMT_A,
    [OP_BIND_METHOD]       = OPFMT_A,
    [OP_INHERIT]           = OPFMT_NONE,
    [OP_BIND_DEFAULT_CALL] = OPFMT_NONE,
    /* Band 17: mask/selector */
    [OP_MASK_PASS]        = OPFMT_NONE,
    [OP_MASK_SKIP]        = OPFMT_NONE,
    [OP_MASK_APPLY]       = OPFMT_A_B,
    /* Band 18: multi-return spreading */
    [OP_SPREAD_RET]       = OPFMT_NONE,
    [OP_ARRAY_SPREAD]     = OPFMT_NONE,
    /* Band 19: call-result comparison */
    [OP_TRUNCATE_RET]     = OPFMT_NONE,
    [OP_EQ_SPREAD]        = OPFMT_NONE,
    [OP_NEQ_SPREAD]       = OPFMT_NONE,
    [OP_LT_SPREAD]        = OPFMT_NONE,
    [OP_GT_SPREAD]        = OPFMT_NONE,
    [OP_LEQ_SPREAD]       = OPFMT_NONE,
    [OP_GEQ_SPREAD]       = OPFMT_NONE,
    /* Sentinels */
    [OP_NOP]              = OPFMT_NONE,
    [OP_HALT]             = OPFMT_NONE,
};

/* -------------------------------------------------------------------------
 * Per-opcode descriptor table -- parallel to CandoOpcode enum.
 *
 * Variable-arity opcodes (OP_CALL, OP_NEW_ARRAY, OP_THROW, OP_POP_N,
 * OP_RETURN, OP_TAIL_CALL, OP_METHOD_CALL, OP_FLUENT_CALL, OP_UNPACK,
 * OP_LOAD_VARARG-with-spread) report (0, 0) and the consumer reads the
 * actual arity from the operand or from runtime state.  See
 * docs/vm-internals.md for the per-opcode call convention.
 *
 * Effect classification follows docs/jit-plan.md §4.1.  When in doubt
 * between LOAD and PURE, prefer LOAD: the recorder must conservatively
 * assume any state read may not be reordered with calls.
 * ---------------------------------------------------------------------- */
#define _OP(arity_in, arity_out, eff, throw_, recurse_) \
    { (u8)(arity_in), (u8)(arity_out), (u8)(eff), (u8)(throw_), (u8)(recurse_) }

static const CandoOpInfo s_opcode_info[OP_COUNT] = {
    /* Band 0: literals -- pure pushes */
    [OP_CONST]               = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_NULL]                = _OP(0, 1, EFFECT_PURE,    0, 0),
    [OP_TRUE]                = _OP(0, 1, EFFECT_PURE,    0, 0),
    [OP_FALSE]               = _OP(0, 1, EFFECT_PURE,    0, 0),

    /* Band 1: stack -- POP_N is variable */
    [OP_POP]                 = _OP(1, 0, EFFECT_PURE,    0, 0),
    [OP_POP_N]               = _OP(0, 0, EFFECT_PURE,    0, 0),
    [OP_DUP]                 = _OP(1, 2, EFFECT_PURE,    0, 0),

    /* Band 2: locals -- read or write the slot table */
    [OP_LOAD_LOCAL]          = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_STORE_LOCAL]         = _OP(1, 1, EFFECT_STORE,   0, 0), /* peek+store */
    [OP_DEF_LOCAL]           = _OP(1, 0, EFFECT_STORE,   0, 0),
    [OP_DEF_CONST_LOCAL]     = _OP(1, 0, EFFECT_STORE,   0, 0),

    /* Band 3: globals -- LOAD may throw on undefined name */
    [OP_LOAD_GLOBAL]         = _OP(0, 1, EFFECT_LOAD,    1, 0),
    [OP_STORE_GLOBAL]        = _OP(1, 1, EFFECT_STORE,   1, 0), /* peek+store */
    [OP_DEF_GLOBAL]          = _OP(1, 0, EFFECT_STORE,   0, 0),
    [OP_DEF_CONST_GLOBAL]    = _OP(1, 0, EFFECT_STORE,   0, 0),

    /* Band 4: upvalues */
    [OP_LOAD_UPVAL]          = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_STORE_UPVAL]         = _OP(1, 1, EFFECT_STORE,   0, 0),
    [OP_CLOSE_UPVAL]         = _OP(0, 0, EFFECT_STORE,   0, 0),

    /* Band 5: arithmetic -- may throw on type mismatch, and dispatches
     * __add / __sub / __mul / __div / __mod / __pow / __unm metamethods
     * via cando_vm_call_meta which re-enters the VM (may_recurse=1). */
    [OP_ADD]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_SUB]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_MUL]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_DIV]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_MOD]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_POW]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_NEG]                 = _OP(1, 1, EFFECT_PURE,    1, 1),
    [OP_POS]                 = _OP(1, 1, EFFECT_PURE,    1, 0),
    [OP_INCR]                = _OP(1, 1, EFFECT_PURE,    1, 1), /* in-place */
    [OP_DECR]                = _OP(1, 1, EFFECT_PURE,    1, 1),

    /* Band 6: comparison -- dispatches __equal / __greater metamethods
     * via the same call_meta path, hence may_recurse=1. */
    [OP_EQ]                  = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_NEQ]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_LT]                  = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_GT]                  = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_LEQ]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    [OP_GEQ]                 = _OP(2, 1, EFFECT_PURE,    1, 1),
    /* _STACK variants: variable left-side + A right-sides; arity=0 */
    [OP_EQ_STACK]            = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_NEQ_STACK]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_LT_STACK]            = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_GT_STACK]            = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_LEQ_STACK]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_GEQ_STACK]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_RANGE_CHECK]         = _OP(3, 1, EFFECT_PURE,    1, 1),

    /* Band 7: bitwise -- may throw on non-numeric */
    [OP_BIT_AND]             = _OP(2, 1, EFFECT_PURE,    1, 0),
    [OP_BIT_OR]              = _OP(2, 1, EFFECT_PURE,    1, 0),
    [OP_BIT_XOR]             = _OP(2, 1, EFFECT_PURE,    1, 0),
    [OP_BIT_NOT]             = _OP(1, 1, EFFECT_PURE,    1, 0),
    [OP_LSHIFT]              = _OP(2, 1, EFFECT_PURE,    1, 0),
    [OP_RSHIFT]              = _OP(2, 1, EFFECT_PURE,    1, 0),

    /* Band 8: logical -- AND_JUMP/OR_JUMP peek without popping */
    [OP_NOT]                 = _OP(1, 1, EFFECT_PURE,    0, 0),
    [OP_AND_JUMP]            = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_OR_JUMP]             = _OP(0, 0, EFFECT_CONTROL, 0, 0),

    /* Band 9: objects -- everything here can heap-allocate or trigger
     * a metamethod call, hence may_throw + may_recurse on field/index
     * ops that route through __index / __newindex. */
    [OP_NEW_OBJECT]          = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_NEW_ARRAY]           = _OP(0, 1, EFFECT_LOAD,    0, 0), /* var-arity */
    [OP_GET_FIELD]           = _OP(1, 1, EFFECT_LOAD,    1, 1),
    [OP_SET_FIELD]           = _OP(1, 1, EFFECT_STORE,   1, 1), /* peek obj */
    [OP_GET_INDEX]           = _OP(2, 1, EFFECT_LOAD,    1, 1),
    [OP_SET_INDEX]           = _OP(2, 1, EFFECT_STORE,   1, 1), /* peek obj */
    [OP_LEN]                 = _OP(1, 1, EFFECT_LOAD,    1, 1),
    [OP_KEYS_OF]             = _OP(1, 0, EFFECT_LOAD,    1, 0), /* var-arity push */
    [OP_VALS_OF]             = _OP(1, 0, EFFECT_LOAD,    1, 0),

    /* Band 10: control flow */
    [OP_JUMP]                = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_JUMP_IF_FALSE]       = _OP(1, 0, EFFECT_CONTROL, 0, 0),
    [OP_JUMP_IF_TRUE]        = _OP(1, 0, EFFECT_CONTROL, 0, 0),
    [OP_JUMP_IF_NULL]        = _OP(0, 0, EFFECT_CONTROL, 0, 0), /* peek */
    [OP_LOOP]                = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_BREAK]               = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_CONTINUE]            = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_LOOP_MARK]           = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_LOOP_END]            = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_IF_MARK]             = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_IF_END]              = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_SETTLE]              = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_IF_TEST_MATCHED]     = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_IF_TEST_PREV]        = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_IF_SET_RAN]          = _OP(0, 0, EFFECT_STORE,   0, 0),
    [OP_IF_CLEAR_PREV]       = _OP(0, 0, EFFECT_STORE,   0, 0),

    /* Band 11: functions and calls -- all may_recurse */
    [OP_CLOSURE]             = _OP(0, 1, EFFECT_LOAD,    0, 0), /* var tail */
    [OP_CALL]                = _OP(0, 0, EFFECT_CALL,    1, 1), /* var-arity */
    [OP_METHOD_CALL]         = _OP(0, 0, EFFECT_CALL,    1, 1),
    [OP_FLUENT_CALL]         = _OP(0, 0, EFFECT_CALL,    1, 1),
    [OP_RETURN]              = _OP(0, 0, EFFECT_CONTROL, 0, 0), /* var-arity */
    [OP_TAIL_CALL]           = _OP(0, 0, EFFECT_CALL,    1, 1),

    /* Band 12: varargs */
    [OP_LOAD_VARARG]         = _OP(0, 1, EFFECT_LOAD,    0, 0), /* var-arity if A=UINT16_MAX */
    [OP_VARARG_LEN]          = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_UNPACK]              = _OP(1, 0, EFFECT_LOAD,    1, 0), /* var-arity */

    /* Band 13: iteration */
    [OP_RANGE_ASC]           = _OP(2, 1, EFFECT_LOAD,    1, 0),
    [OP_RANGE_DESC]          = _OP(2, 1, EFFECT_LOAD,    1, 0),
    [OP_FOR_INIT]            = _OP(0, 0, EFFECT_CONTROL, 1, 0),
    [OP_FOR_NEXT]            = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_FOR_OVER_INIT]       = _OP(0, 0, EFFECT_CONTROL, 1, 0),
    [OP_FOR_OVER_NEXT]       = _OP(0, 0, EFFECT_CONTROL, 1, 1), /* calls iter fn */
    [OP_PIPE_INIT]           = _OP(0, 0, EFFECT_CONTROL, 1, 0),
    [OP_PIPE_NEXT]           = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_FILTER_NEXT]         = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_PIPE_END]            = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_PIPE_COLLECT]        = _OP(1, 0, EFFECT_STORE,   0, 0),
    [OP_FILTER_COLLECT]      = _OP(1, 0, EFFECT_STORE,   0, 0),
    [OP_COND_FILTER_COLLECT] = _OP(1, 0, EFFECT_STORE,   0, 0),

    /* Band 14: error handling */
    [OP_TRY_BEGIN]           = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_TRY_END]             = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_CATCH_BEGIN]         = _OP(0, 0, EFFECT_CONTROL, 0, 0), /* var pushes */
    [OP_FINALLY_BEGIN]       = _OP(0, 0, EFFECT_CONTROL, 0, 0),
    [OP_THROW]               = _OP(0, 0, EFFECT_THROW,   1, 0), /* var-arity */
    [OP_RERAISE]             = _OP(0, 0, EFFECT_THROW,   1, 0),

    /* Band 15: threads -- all may_recurse since they re-enter the VM */
    [OP_ASYNC]               = _OP(0, 0, EFFECT_CALL,    1, 1),
    [OP_AWAIT]               = _OP(1, 0, EFFECT_CALL,    1, 1), /* var ret */
    [OP_YIELD]               = _OP(0, 0, EFFECT_CALL,    1, 1),
    [OP_THREAD]              = _OP(1, 1, EFFECT_CALL,    1, 1),

    /* Band 16: classes */
    [OP_NEW_CLASS]           = _OP(0, 1, EFFECT_LOAD,    0, 0),
    [OP_BIND_METHOD]         = _OP(1, 0, EFFECT_STORE,   0, 0), /* class stays on TOS */
    [OP_INHERIT]             = _OP(2, 1, EFFECT_STORE,   1, 0),
    [OP_BIND_DEFAULT_CALL]   = _OP(0, 0, EFFECT_STORE,   0, 0),

    /* Band 17: mask/selector -- pure stack shuffles */
    [OP_MASK_PASS]           = _OP(0, 0, EFFECT_PURE,    0, 0),
    [OP_MASK_SKIP]           = _OP(1, 0, EFFECT_PURE,    0, 0),
    [OP_MASK_APPLY]          = _OP(0, 0, EFFECT_PURE,    0, 0), /* var-arity */

    /* Band 18: multi-return spreading -- adjust counters, no stack change */
    [OP_SPREAD_RET]          = _OP(0, 0, EFFECT_PURE,    0, 0),
    [OP_ARRAY_SPREAD]        = _OP(0, 0, EFFECT_PURE,    0, 0),

    /* Band 19: call-result comparison -- consume last_ret_count + 1.
     * Same metamethod-dispatch caveat as band 6 -- may_recurse=1.   */
    [OP_TRUNCATE_RET]        = _OP(0, 0, EFFECT_PURE,    0, 0),
    [OP_EQ_SPREAD]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_NEQ_SPREAD]          = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_LT_SPREAD]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_GT_SPREAD]           = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_LEQ_SPREAD]          = _OP(0, 1, EFFECT_PURE,    1, 1),
    [OP_GEQ_SPREAD]          = _OP(0, 1, EFFECT_PURE,    1, 1),

    /* Sentinels */
    [OP_NOP]                 = _OP(0, 0, EFFECT_PURE,    0, 0),
    [OP_HALT]                = _OP(0, 0, EFFECT_CONTROL, 0, 0),
};

#undef _OP

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

const char *cando_opcode_name(CandoOpcode op) {
    if ((u32)op >= OP_COUNT) return "OP_UNKNOWN";
    const char *name = s_opcode_names[(u32)op];
    return name ? name : "OP_UNKNOWN";
}

CandoOpFmt cando_opcode_fmt(CandoOpcode op) {
    if ((u32)op >= OP_COUNT) return OPFMT_NONE;
    return s_opcode_fmts[(u32)op];
}

CandoOpInfo cando_opcode_info(CandoOpcode op) {
    if ((u32)op >= OP_COUNT) {
        return (CandoOpInfo){ 0, 0, EFFECT_PURE, 0, 0 };
    }
    return s_opcode_info[(u32)op];
}

u32 cando_instr_size_at(const u8 *code, u32 offset) {
    CandoOpcode op = (CandoOpcode)code[offset];
    if (op == OP_CLOSURE) {
        /* OP_CLOSURE = 1B opcode + 2B const idx + 2B capture_count + 2B*N */
        u16 cap = (u16)code[offset + 3] | ((u16)code[offset + 4] << 8);
        return 5u + (u32)cap * 2u;
    }
    return cando_opcode_size(op);
}
