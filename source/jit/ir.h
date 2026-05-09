/*
 * jit/ir.h -- linear SSA IR for the CanDo trace recorder.
 *
 * One trace == one CandoTraceIR holding:
 *   - a flat array of IRIns instructions, indexed 1..ir_count (index 0
 *     is reserved as IRREF_NIL so a zeroed IRRef stays harmless),
 *   - a constant pool of CandoValue literals referenced by IR_K* ops,
 *   - a snapshot list (built up later in Phase 3.2 by the recorder).
 *
 * This file lays out the data and the construction primitives.  The
 * recorder, optimiser, and codegen consume them in Phase 3+/4+/6+.
 *
 * Layout choices (see docs/jit-plan.md §6.1):
 *
 *   IRIns is 12 bytes.  Two u32 operand slots cover (a) IRRef references
 *   to other IR instructions and (b) constant-pool indices.  A 16-bit op
 *   gives plenty of headroom; an 8-bit type and 8-bit flags fill out
 *   the header.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_IR_H
#define CANDO_JIT_IR_H

#include "../core/common.h"
#include "../core/value.h"

/* -----------------------------------------------------------------------
 * IRRef -- index into CandoTraceIR.ir[].
 *
 * Index 0 is reserved (IRREF_NIL) so a zero-initialised operand reads
 * as "no value" rather than aliasing a real instruction.  The recorder
 * must never emit at index 0.
 *
 * The high bit (IRREF_KFLAG) is set when the reference points into the
 * constant pool instead of the IR array.  Optimisation passes (Phase 5)
 * use this to fold-or-substitute without a separate bool.
 * --------------------------------------------------------------------- */
typedef u32 IRRef;

#define IRREF_NIL       ((IRRef)0)
#define IRREF_KFLAG     ((IRRef)0x80000000u)
#define IRREF_KMASK     ((IRRef)0x7FFFFFFFu)
#define IRREF_IS_K(r)   (((r) & IRREF_KFLAG) != 0)
#define IRREF_K(idx)    ((IRRef)((idx) | IRREF_KFLAG))
#define IRREF_KIDX(r)   ((u32)((r) & IRREF_KMASK))

/* -----------------------------------------------------------------------
 * IRType -- the value type produced by an IR instruction.
 *
 * Stays narrower than CandoValue's TypeTag because numbers are unboxed
 * inside a trace and the recorder tags them with IRT_NUM at record
 * time.  IRT_BOOL is distinct from IRT_NUM so guard logic can branch
 * on a bit-precise type rather than going through the boxed accessors.
 * --------------------------------------------------------------------- */
typedef enum {
    IRT_NIL    = 0,
    IRT_BOOL   = 1,
    IRT_NUM    = 2,
    IRT_STR    = 3,
    IRT_OBJ    = 4,
    IRT_PTR    = 5,    /* raw pointer (CdoObject*, CdoString*) */
    IRT_VOID   = 6,    /* op produces no value (stores, guards) */
    IRT__COUNT
} IRType;

/* -----------------------------------------------------------------------
 * IRFlag -- bit flags on each IR instruction.
 * --------------------------------------------------------------------- */
#define IRF_GUARD       0x01    /* this op is a guard / side-exit anchor   */
#define IRF_PINNED      0x02    /* optimiser may not reorder past this op  */

/* -----------------------------------------------------------------------
 * IROp -- IR opcode set.
 *
 * Stable ordering within bands; do not renumber without updating ir.c
 * and any persisted IR dumps.  Phases 3.2+ extend this list as new
 * recording sites are added.
 * --------------------------------------------------------------------- */
typedef enum {
    IR_NOP = 0,            /* no-op placeholder; ir[0] is permanently NOP   */

    /* ===== Band 0: Constants ============================================ */
    IR_KNUM,               /* op1: const-pool idx (CandoValue, IRT_NUM)     */
    IR_KSTR,               /* op1: const-pool idx (CandoValue, IRT_STR)     */
    IR_KOBJ,               /* op1: HandleIndex literal in the low 32 bits   */
    IR_KNULL,              /* (no operands)                                 */
    IR_KBOOL,              /* op1: 0 or 1                                   */

    /* ===== Band 1: Stack / locals ======================================= */
    IR_SLOAD,              /* op1: stack-slot offset; loads a CandoValue    */
    IR_SSTORE,             /* op1: slot, op2: value IRRef; IRT_VOID         */

    /* ===== Band 2: Arithmetic =========================================== */
    IR_ADD,                /* op1, op2: number IRRefs; IRT_NUM              */
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_MOD,
    IR_NEG,                /* op1: number IRRef; IRT_NUM                    */

    /* ===== Band 3: Comparison =========================================== */
    IR_EQ,                 /* op1, op2: any-typed IRRefs; IRT_BOOL          */
    IR_NEQ,
    IR_LT,                 /* numeric ordering; IRT_BOOL                    */
    IR_LE,
    IR_GT,
    IR_GE,

    /* ===== Band 4: Guards =============================================== */
    /* All guards: op1 = value IRRef, op2 = snapshot ref (Phase 3.2).
     * Type tag of the guard IR matches the type it is asserting. */
    IR_GUARD_NUM,          /* op1 must be IRT_NUM                           */
    IR_GUARD_OBJ,          /* op1 must be IRT_OBJ                           */
    IR_GUARD_STR,
    IR_GUARD_TRUE,         /* op1 must be a truthy bool/non-null            */
    IR_GUARD_FALSE,

    /* ===== Band 5: Object / array / global access ====================== */
    IR_HLOAD,              /* op1: handle IRRef -> raw CdoObject* (IRT_PTR) */
    IR_HREF,               /* op1: obj ptr, op2: key IRRef; CdoValue load   */
    IR_AREF,               /* op1: source slot (frame-relative, raw u32),
                              op2: index IRRef (IRT_NUM); reads
                              vm->frames[top].slots[op1] as an array
                              and returns array[op2] as IRT_NUM.
                              Side-exits with TRACE_BAD_TYPE if the slot
                              doesn't hold an array, the index is out
                              of range, or the element is non-numeric. */
    IR_GLOAD,              /* op1: constant-pool ref of the global name
                              (a CandoString*); reads the named global
                              from vm->globals and returns it as IRT_NUM.
                              Side-exits with TRACE_BAD_TYPE if the
                              global is missing or non-numeric. */
    IR_GSTORE,             /* op1: constant-pool ref of name; op2: value
                              IRRef (must be IRT_NUM in v1).  Writes the
                              value to vm->globals.  Side-exits if the
                              global is const-protected. */

    /* ===== Band 6: Trace control ======================================== */
    IR_LOOP,               /* head-of-loop marker; the trace closes here    */

    IR__COUNT
} IROp;

/* -----------------------------------------------------------------------
 * IRIns -- one SSA instruction.  12 bytes.
 * --------------------------------------------------------------------- */
typedef struct IRIns {
    u16 op;        /* IROp                                                  */
    u8  type;      /* IRType                                                */
    u8  flags;     /* IRF_* bitmask                                         */
    IRRef op1;
    IRRef op2;
} IRIns;

CANDO_STATIC_ASSERT(sizeof(IRIns) == 12, "IRIns must be 12 bytes");

/* -----------------------------------------------------------------------
 * CandoTraceIR -- the IR container.  Owned by the trace; freed on trace
 * destruction.  Phase 3.2+ wraps this in a fuller CandoTrace that adds
 * snapshots, codegen output, exit stubs, etc.
 * --------------------------------------------------------------------- */
typedef struct CandoTraceIR {
    IRIns       *ir;        /* index 0 = IR_NOP sentinel; real ops at >= 1 */
    u32          ir_count;  /* number of in-use slots, including ir[0]      */
    u32          ir_cap;    /* allocated capacity                           */

    CandoValue  *constants; /* KNUM / KSTR pool                             */
    u32          const_count;
    u32          const_cap;
} CandoTraceIR;

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

/* cando_trace_ir_init -- prepare an empty IR container with the ir[0]
 * NOP sentinel pre-installed. */
CANDO_API void cando_trace_ir_init(CandoTraceIR *t);

/* cando_trace_ir_destroy -- release all internal storage. */
CANDO_API void cando_trace_ir_destroy(CandoTraceIR *t);

/* cando_trace_ir_reset -- drop all instructions and constants but keep
 * the underlying buffers, so the recorder can reuse one IR container
 * across many trace attempts. */
CANDO_API void cando_trace_ir_reset(CandoTraceIR *t);

/* -----------------------------------------------------------------------
 * Construction
 * --------------------------------------------------------------------- */

/* cando_ir_emit -- append one instruction; returns its IRRef.
 * The IR_NOP sentinel at index 0 is preserved -- the first emit lands
 * at index 1. */
CANDO_API IRRef cando_ir_emit(CandoTraceIR *t, IROp op, IRType type,
                              u8 flags, IRRef op1, IRRef op2);

/* cando_ir_const -- intern a CandoValue in the constant pool; returns
 * an IRRef with the IRREF_KFLAG bit set so consumers can distinguish
 * pool entries from instruction references at zero cost. */
CANDO_API IRRef cando_ir_const(CandoTraceIR *t, CandoValue v);

/* cando_ir_emit_knum -- shortcut: intern a number then emit IR_KNUM.
 * Returns the IR_KNUM instruction's IRRef (NOT the constant-pool ref). */
CANDO_API IRRef cando_ir_emit_knum(CandoTraceIR *t, f64 n);

/* cando_ir_get_ins -- safe accessor for an instruction.  Returns NULL
 * on out-of-range references and on constant-pool refs. */
CANDO_API const IRIns *cando_ir_get_ins(const CandoTraceIR *t, IRRef ref);

/* cando_ir_get_const -- accessor for a constant-pool entry.  Caller is
 * responsible for checking IRREF_IS_K(ref) first; out-of-range returns
 * cando_null(). */
CANDO_API CandoValue cando_ir_get_const(const CandoTraceIR *t, IRRef ref);

/* -----------------------------------------------------------------------
 * Pretty-printing
 * --------------------------------------------------------------------- */

/* cando_ir_op_name  -- "IR_ADD", "IR_KNUM", etc.  Unknown -> "IR_???". */
CANDO_API const char *cando_ir_op_name(IROp op);

/* cando_ir_type_name -- "num", "obj", "void", etc. */
CANDO_API const char *cando_ir_type_name(IRType t);

/* cando_ir_dump -- print a human-readable listing of every instruction.
 * Format: "%04d  TYPE  IR_OP  op1, op2".  Intended for debug builds and
 * the future --Xjit-dump=ir CLI flag. */
CANDO_API void cando_ir_dump(const CandoTraceIR *t, FILE *out);

#endif /* CANDO_JIT_IR_H */
