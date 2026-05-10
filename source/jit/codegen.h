/*
 * jit/codegen.h -- IR -> x86_64 native code emitter (Phase 6).
 *
 * Tries to compile a fully-recorded trace (post DSE/DCE/LICM) into
 * native machine code that honours the same calling convention as
 * the IR-interpreter.  v1 covers a narrow subset of IR ops; on any
 * unsupported op the compile bails and the trace stays on the
 * IR-interpreter path (mcode_fn = NULL).
 *
 * Linux x86_64 only in v1.  The plan calls out Windows/macOS in
 * Phase 8 and AArch64 in Phase 9; that work goes behind ifdefs.
 *
 * Op coverage in v1:
 *   IR_NOP, IR_LOOP                   -- prologue / epilogue / no-op
 *   IR_KNUM, IR_KBOOL                 -- load constant into vals[i]
 *   IR_SLOAD                          -- frame slot load + numeric type-check
 *   IR_SSTORE                         -- frame slot store
 *   IR_ADD, IR_SUB, IR_MUL            -- XMM arithmetic
 *   IR_LT, IR_LE, IR_GT, IR_GE,
 *     IR_EQ, IR_NEQ                    -- XMM comparison -> 1.0/0.0
 *   IR_GUARD_NUM                      -- runtime no-op (SLOAD already
 *                                        type-checked)
 *   IR_GUARD_TRUE, IR_GUARD_FALSE     -- bail on mismatch with snapshot
 *
 * Anything else (IR_GLOAD/GSTORE, IR_HLOAD_SLOT, IR_AREF, IR_CALL_F1,
 * IR_NEG, IR_DIV, IR_MOD, IR_KNULL/KSTR/KOBJ, IR_GUARD_OBJ/STR) is
 * unsupported in v1 and aborts codegen.  Subsequent commits expand
 * the set incrementally.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_CODEGEN_H
#define CANDO_JIT_CODEGEN_H

#include "jit.h"

/* Compile `t` into native code.  On success, t->mcode contains the
 * finalized executable buffer and t->mcode_fn is the entry point.
 * On failure (unsupported op, out of buffer space, mprotect refusal)
 * t->mcode stays zeroed and the trace runs on the IR-interpreter.
 *
 * `vm` is the VM the trace was recorded in, threaded through so the
 * codegen can resolve fast-native function pointers from
 * vm->fast_natives_f1[] for IR_CALL_F1 emit.
 *
 * Returns true on success, false on bail.  Either way the trace is
 * still runnable; this is purely an optimisation. */
CANDO_API bool cando_jit_codegen_trace(struct CandoVM *vm, CandoTrace *t);

#endif /* CANDO_JIT_CODEGEN_H */
