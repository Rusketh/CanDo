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
#define IRF_INVARIANT   0x04    /* loop-invariant: produces the same value
                                   on every iteration of the recorded
                                   loop.  Set at recording finish-time by
                                   a forward-pass analysis (jit.c) so the
                                   IR-interpreter can skip it on
                                   iterations 2+.  An op is invariant iff
                                   none of its inputs ever change inside
                                   the trace -- SLOADs of slots that have
                                   no SSTORE, GLOADs of names that have
                                   no GSTORE, and pure ops whose operands
                                   are themselves invariant. */
#define IRF_SUNK        0x08    /* Phase 4.4j: allocation IR op
                                   (IR_NEW_ARRAY / IR_NEW_OBJECT /
                                   IR_RANGE_*) whose result IRRef
                                   never escapes the trace iteration --
                                   only used by INDEX/FIELD GET/SET or
                                   ARRAY_APPEND.  Phase 4.4k codegen
                                   replaces sunk allocations with stack
                                   buffers + lowers field/index access
                                   to direct memory ops; on side-exit
                                   it materialises the buffer into a
                                   real heap object. */
#define IRF_NUM_KNOWN   0x10    /* IR_SLOAD whose slot is also SSTORE'd
                                   somewhere in the trace with a value
                                   of known IRT_NUM type.  Iter 1 still
                                   validates the slot via the standard
                                   NaN-box guard; on iter 2+ the SSTORE
                                   from the previous iter guarantees the
                                   slot holds a numeric, so codegen can
                                   skip the per-iter type guard (gated
                                   on the cached skip_invariant byte).
                                   Set by mark_known_num_sloads in
                                   jit.c, consumed by emit_sload in
                                   codegen.c. */

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
    IR_HLOAD_SLOT,         /* op1: source slot (frame-relative, raw u32);
                              reads vm->frames[top].slots[op1] as a
                              boxed object and resolves its handle to a
                              raw CdoObject*.  Returns IRT_PTR (stored
                              in TraceVal.p).  Side-exits with
                              TRACE_BAD_TYPE if the slot doesn't hold
                              an object.  This op is loop-invariant
                              when the source slot has no SSTORE in
                              the trace, so LICM hoists the expensive
                              cando_bridge_resolve out of the loop. */
    IR_HREF,               /* op1: obj ptr, op2: key IRRef; CdoValue load   */
    IR_AREF,               /* op1: resolved CdoObject* IRRef (IRT_PTR
                              from IR_HLOAD_SLOT), op2: index IRRef
                              (IRT_NUM).  Reads array[index] as IRT_NUM.
                              Side-exits with TRACE_BAD_TYPE if the
                              source isn't an array, the index is out
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

    /* ===== Band 6: Native calls ========================================= */
    IR_CALL_F1,            /* op1: u32 fast-native index (NOT an IRRef --
                              direct lookup into vm->jit->fast_natives[]).
                              op2: numeric IRRef (the single argument).
                              Returns IRT_NUM.  At trace_run time invokes
                              the registered f64 (*)(f64) function pointer
                              directly -- no VM-stack-passing overhead.
                              Loop-invariant when its argument is.       */

    /* ===== Band 6b: Allocation (Phase 4.4a+) ============================ */
    IR_NEW_ARRAY,          /* op1: u32 expected element count (literal,
                              for capacity reservation).  No IRRef
                              operands.  Returns IRT_OBJ -- vals[i].u
                              holds the resulting CandoValue's u64 bits
                              (a NaN-boxed object handle).  IR-interp
                              calls cando_bridge_new_array.  Codegen
                              v0 bails on this op so traces fall back
                              to the IR-interpreter; codegen lands in
                              Phase 4.4g.                                 */
    IR_ARRAY_APPEND,       /* op1: array IRRef (IRT_OBJ).  op2: value
                              IRRef (IRT_NUM in v1).  Returns IRT_VOID.
                              IR-interp resolves the handle and calls
                              cdo_array_push.  Always emitted in
                              groups immediately after IR_NEW_ARRAY by
                              the recorder.                                */
    IR_INDEX_GET,          /* op1: array IRRef (IRT_OBJ).  op2: index
                              IRRef (IRT_NUM).  Returns IRT_NUM.
                              IR-interp resolves the handle, calls
                              cdo_array_rawget_idx, requires the
                              element be numeric.  Side-exits on bad
                              array, out-of-range, or non-numeric
                              element via cur_snap.  v1 only handles
                              numeric arrays; string-keyed indexing
                              and non-array containers abort the
                              recorder upstream.                         */
    IR_INDEX_SET_VAL,      /* op1: value IRRef.  No-op at trace_run
                              time -- carries the value into the
                              immediately-following IR_INDEX_SET via
                              the PINNED-pair convention.  PINNED
                              flag prevents DCE from killing it. */
    IR_INDEX_SET,          /* op1: array IRRef.  op2: index IRRef.
                              Reads its value operand from the
                              preceding IR_INDEX_SET_VAL (via i-1).
                              IR-interp resolves the array, calls
                              cdo_array_rawset_idx with the value as
                              a CdoValue number.  Side-exits on
                              non-array via cur_snap. */
    IR_NEW_OBJECT,         /* No operands.  Returns IRT_OBJ; vals[i].u
                              is the freshly-allocated empty object's
                              CandoValue u64 bits.  IR-interp calls
                              cando_bridge_new_object. */
    IR_FIELD_SET_VAL,      /* Same shape as IR_INDEX_SET_VAL: PINNED
                              no-op carrying the value into the
                              following IR_FIELD_SET. */
    IR_FIELD_SET,          /* op1: object IRRef (IRT_OBJ).  op2:
                              name const-pool ref (the IRREF_KFLAG
                              bit is set; KIDX into trace constants
                              gives a CandoString*).  Reads value
                              from the preceding IR_FIELD_SET_VAL.
                              IR-interp resolves obj, interns key,
                              calls cdo_object_rawset.  v1 ignores
                              __newindex -- objects with metamethods
                              get wrong behaviour but the recorder
                              restricts the entry shape via the
                              fresh-object check upstream. */
    IR_FIELD_GET,          /* op1: object IRRef (IRT_OBJ).  op2:
                              name const-pool ref.  Returns IRT_NUM.
                              IR-interp resolves obj + interns key +
                              cdo_object_rawget, requires numeric.
                              Side-exits on missing/non-numeric. */
    IR_RANGE_ASC,          /* op1: start IRRef (IRT_NUM, integer
                              value).  op2: end IRRef (IRT_NUM,
                              integer value, inclusive).  Returns
                              IRT_OBJ -- a freshly-allocated array
                              of cdo_number(v) for v in start..end.
                              Mirrors OP_RANGE_ASC in vm.c.  v1
                              always allocates per call; the
                              actual sinking lands in 4.4j+k. */
    IR_RANGE_DESC,         /* Same as IR_RANGE_ASC but counts down
                              from start to end (inclusive). */

    /* ===== Band 6.5: Object introspection =============================== */
    IR_HLEN,               /* op1: CdoObject* IRRef (IRT_PTR), op2 unused.
                              Returns IRT_NUM = arr->items_len.  Mirrors
                              cdo_array_len without the read-lock (the
                              JIT assumes single-mutator semantics like
                              everywhere else in the codegen).  Used by
                              OP_FOR_INIT recording to set up the FOR-
                              state's len slot. */

    /* ===== Band 7: Trace control ======================================== */
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
