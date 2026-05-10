/*
 * jit/ir.c -- IR construction and dumper.
 *
 * Phase 3.1 of docs/jit-plan.md.  This file is the data plumbing only;
 * the recorder that drives the construction lives in jit_record.c
 * (Phase 3.2+) and the optimiser/backend in later phases.
 */

#include "ir.h"

#include <stdio.h>
#include <inttypes.h>

/* Initial capacities; grown geometrically by the emit/intern helpers. */
#define IR_INITIAL_CAP   32u
#define KPOOL_INITIAL_CAP 8u

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */
void cando_trace_ir_init(CandoTraceIR *t) {
    if (!t) return;
    t->ir          = NULL;
    t->ir_count    = 0;
    t->ir_cap      = 0;
    t->constants   = NULL;
    t->const_count = 0;
    t->const_cap   = 0;

    /* Reserve ir[0] as the IR_NOP sentinel so a zeroed IRRef stays
     * harmless.  Allocate eagerly so the recorder never sees an
     * empty .ir array. */
    t->ir     = cando_alloc(sizeof(IRIns) * IR_INITIAL_CAP);
    t->ir_cap = IR_INITIAL_CAP;
    t->ir[0]  = (IRIns){ .op = IR_NOP, .type = IRT_VOID,
                         .flags = 0, .op1 = 0, .op2 = 0 };
    t->ir_count = 1;
}

void cando_trace_ir_destroy(CandoTraceIR *t) {
    if (!t) return;
    /* Release any retained CandoStrings in the constant pool.  Numbers
     * and object handles need no cleanup. */
    for (u32 i = 0; i < t->const_count; i++)
        cando_value_release(t->constants[i]);
    cando_free(t->ir);
    cando_free(t->constants);
    t->ir          = NULL;
    t->ir_count    = 0;
    t->ir_cap      = 0;
    t->constants   = NULL;
    t->const_count = 0;
    t->const_cap   = 0;
}

void cando_trace_ir_reset(CandoTraceIR *t) {
    if (!t) return;
    for (u32 i = 0; i < t->const_count; i++)
        cando_value_release(t->constants[i]);
    t->const_count = 0;
    /* Keep ir[0] (the NOP sentinel) and reuse the buffer. */
    t->ir_count = 1;
}

/* -----------------------------------------------------------------------
 * Internal: grow buffers
 * --------------------------------------------------------------------- */
static void ir_ensure_capacity(CandoTraceIR *t, u32 want) {
    if (want <= t->ir_cap) return;
    u32 nc = t->ir_cap ? t->ir_cap * 2 : IR_INITIAL_CAP;
    while (nc < want) nc *= 2;
    t->ir = cando_realloc(t->ir, sizeof(IRIns) * nc);
    t->ir_cap = nc;
}

static void const_ensure_capacity(CandoTraceIR *t, u32 want) {
    if (want <= t->const_cap) return;
    u32 nc = t->const_cap ? t->const_cap * 2 : KPOOL_INITIAL_CAP;
    while (nc < want) nc *= 2;
    t->constants = cando_realloc(t->constants, sizeof(CandoValue) * nc);
    t->const_cap = nc;
}

/* -----------------------------------------------------------------------
 * Construction
 * --------------------------------------------------------------------- */
IRRef cando_ir_emit(CandoTraceIR *t, IROp op, IRType type,
                    u8 flags, IRRef op1, IRRef op2) {
    CANDO_ASSERT(t != NULL);
    ir_ensure_capacity(t, t->ir_count + 1);
    IRRef ref = t->ir_count;
    t->ir[ref] = (IRIns){
        .op    = (u16)op,
        .type  = (u8)type,
        .flags = flags,
        .op1   = op1,
        .op2   = op2,
    };
    t->ir_count++;
    return ref;
}

IRRef cando_ir_const(CandoTraceIR *t, CandoValue v) {
    CANDO_ASSERT(t != NULL);

    /* Dedup numbers and strings so the pool stays compact -- the recorder
     * tends to see the same constant many times in a tight loop. */
    if (cando_is_number(v)) {
        f64 vn = cando_as_number(v);
        for (u32 i = 0; i < t->const_count; i++) {
            if (cando_is_number(t->constants[i]) &&
                cando_as_number(t->constants[i]) == vn) {
                cando_value_release(v);
                return IRREF_K(i);
            }
        }
    } else if (cando_is_string(v)) {
        CandoString *vs = cando_as_string(v);
        for (u32 i = 0; i < t->const_count; i++) {
            if (cando_is_string(t->constants[i])) {
                CandoString *cs = cando_as_string(t->constants[i]);
                if (cs == vs ||
                    (cs->length == vs->length &&
                     memcmp(cs->data, vs->data, cs->length) == 0)) {
                    cando_value_release(v);
                    return IRREF_K(i);
                }
            }
        }
    }

    const_ensure_capacity(t, t->const_count + 1);
    u32 idx = t->const_count++;
    t->constants[idx] = v;
    return IRREF_K(idx);
}

IRRef cando_ir_emit_knum(CandoTraceIR *t, f64 n) {
    IRRef k = cando_ir_const(t, cando_number(n));
    return cando_ir_emit(t, IR_KNUM, IRT_NUM, 0, k, 0);
}

const IRIns *cando_ir_get_ins(const CandoTraceIR *t, IRRef ref) {
    if (!t) return NULL;
    if (IRREF_IS_K(ref)) return NULL;
    if (ref >= t->ir_count) return NULL;
    return &t->ir[ref];
}

CandoValue cando_ir_get_const(const CandoTraceIR *t, IRRef ref) {
    if (!t || !IRREF_IS_K(ref)) return cando_null();
    u32 idx = IRREF_KIDX(ref);
    if (idx >= t->const_count) return cando_null();
    return t->constants[idx];
}

/* -----------------------------------------------------------------------
 * Name tables
 * --------------------------------------------------------------------- */
static const char *const s_op_names[IR__COUNT] = {
    [IR_NOP]         = "IR_NOP",
    [IR_KNUM]        = "IR_KNUM",
    [IR_KSTR]        = "IR_KSTR",
    [IR_KOBJ]        = "IR_KOBJ",
    [IR_KNULL]       = "IR_KNULL",
    [IR_KBOOL]       = "IR_KBOOL",
    [IR_SLOAD]       = "IR_SLOAD",
    [IR_SSTORE]      = "IR_SSTORE",
    [IR_ADD]         = "IR_ADD",
    [IR_SUB]         = "IR_SUB",
    [IR_MUL]         = "IR_MUL",
    [IR_DIV]         = "IR_DIV",
    [IR_MOD]         = "IR_MOD",
    [IR_NEG]         = "IR_NEG",
    [IR_EQ]          = "IR_EQ",
    [IR_NEQ]         = "IR_NEQ",
    [IR_LT]          = "IR_LT",
    [IR_LE]          = "IR_LE",
    [IR_GT]          = "IR_GT",
    [IR_GE]          = "IR_GE",
    [IR_GUARD_NUM]   = "IR_GUARD_NUM",
    [IR_GUARD_OBJ]   = "IR_GUARD_OBJ",
    [IR_GUARD_STR]   = "IR_GUARD_STR",
    [IR_GUARD_TRUE]  = "IR_GUARD_TRUE",
    [IR_GUARD_FALSE] = "IR_GUARD_FALSE",
    [IR_HLOAD]       = "IR_HLOAD",
    [IR_HLOAD_SLOT]  = "IR_HLOAD_SLOT",
    [IR_HREF]        = "IR_HREF",
    [IR_AREF]        = "IR_AREF",
    [IR_GLOAD]       = "IR_GLOAD",
    [IR_GSTORE]      = "IR_GSTORE",
    [IR_CALL_F1]     = "IR_CALL_F1",
    [IR_NEW_ARRAY]   = "IR_NEW_ARRAY",
    [IR_ARRAY_APPEND]= "IR_ARRAY_APPEND",
    [IR_INDEX_GET]   = "IR_INDEX_GET",
    [IR_LOOP]        = "IR_LOOP",
};

static const char *const s_type_names[IRT__COUNT] = {
    [IRT_NIL]  = "nil",
    [IRT_BOOL] = "bool",
    [IRT_NUM]  = "num",
    [IRT_STR]  = "str",
    [IRT_OBJ]  = "obj",
    [IRT_PTR]  = "ptr",
    [IRT_VOID] = "void",
};

const char *cando_ir_op_name(IROp op) {
    if ((u32)op >= IR__COUNT) return "IR_???";
    const char *n = s_op_names[(u32)op];
    return n ? n : "IR_???";
}

const char *cando_ir_type_name(IRType t) {
    if ((u32)t >= IRT__COUNT) return "?";
    const char *n = s_type_names[(u32)t];
    return n ? n : "?";
}

/* -----------------------------------------------------------------------
 * Dumper
 * --------------------------------------------------------------------- */

/* Format an IR operand as either #N (instruction ref) or kN (constant
 * pool ref).  Buffer must be at least 16 bytes. */
static void format_ref(IRRef ref, char *buf, size_t len) {
    if (ref == IRREF_NIL) {
        snprintf(buf, len, "-");
    } else if (IRREF_IS_K(ref)) {
        snprintf(buf, len, "k%u", IRREF_KIDX(ref));
    } else {
        snprintf(buf, len, "#%u", (u32)ref);
    }
}

/* Format a stack-slot operand (used by SLOAD / SSTORE for op1).
 * Slot operands are stored as raw u32 values, not IRRefs, so they
 * never have the IRREF_KFLAG bit set. */
static void format_slot(IRRef raw, char *buf, size_t len) {
    snprintf(buf, len, "s%u", (u32)raw);
}

void cando_ir_dump(const CandoTraceIR *t, FILE *out) {
    if (!t || !out) return;

    fprintf(out, "==== trace IR (%u instructions, %u constants) ====\n",
            t->ir_count, t->const_count);

    /* Constant pool first, so subsequent kN references are interpretable. */
    for (u32 i = 0; i < t->const_count; i++) {
        char *s = cando_value_tostring(t->constants[i]);
        fprintf(out, "  k%-3u  %s\n", i, s ? s : "?");
        cando_free(s);
    }

    for (u32 i = 1; i < t->ir_count; i++) {
        const IRIns *in = &t->ir[i];
        char b1[16], b2[16];
        /* SLOAD / SSTORE / HLOAD_SLOT encode op1 as a raw slot number
         * (not an IRRef); render as "sN" to avoid confusion with
         * IRRefs.  IR_AREF's op1 was a slot in earlier phases but is
         * now an IRRef (Phase 5b: HLOAD_SLOT does the resolution and
         * AREF takes the resolved pointer ref). */
        if (in->op == IR_SLOAD || in->op == IR_SSTORE ||
            in->op == IR_HLOAD_SLOT)
            format_slot(in->op1, b1, sizeof(b1));
        else if (in->op == IR_CALL_F1)
            /* op1 is the fast-native registry index (a raw u32),
             * not an IRRef.  Render as nN to set it apart. */
            snprintf(b1, sizeof(b1), "n%u", (u32)in->op1);
        else if (in->op == IR_NEW_ARRAY)
            /* op1 is the literal capacity hint, not an IRRef. */
            snprintf(b1, sizeof(b1), "=%u", (u32)in->op1);
        else
            format_ref(in->op1, b1, sizeof(b1));
        format_ref(in->op2, b2, sizeof(b2));
        fprintf(out, "  %04u  %-4s  %-16s %-6s %-6s%s%s\n",
                i,
                cando_ir_type_name((IRType)in->type),
                cando_ir_op_name((IROp)in->op),
                b1, b2,
                (in->flags & IRF_GUARD)     ? " [GUARD]" : "",
                (in->flags & IRF_INVARIANT) ? " [INV]"   : "");
    }
}
