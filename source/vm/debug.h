/*
 * debug.h -- Bytecode disassembler for Cando chunks.
 *
 * These functions are for developer tooling only; they are not needed
 * by the production VM execution path.  Link them in debug builds or
 * whenever human-readable bytecode dumps are needed.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_DEBUG_H
#define CANDO_DEBUG_H

#include "../core/common.h"
#include "chunk.h"
#include "vm.h"

/* -------------------------------------------------------------------------
 * cando_chunk_disasm -- print a human-readable disassembly of the entire
 * chunk to `out` (use stdout or stderr as appropriate).
 *
 * Format example:
 *   == <name> ==
 *   0000 line  1  OP_CONST       0    '42'
 *   0003 line  1  OP_RETURN      1
 * ---------------------------------------------------------------------- */
CANDO_API void cando_chunk_disasm(const CandoChunk *chunk, FILE *out);

/* -------------------------------------------------------------------------
 * cando_instr_disasm -- disassemble the single instruction at byte offset
 * `offset` within `chunk`.  Writes to `out`.
 *
 * Returns the byte offset of the NEXT instruction (caller can iterate).
 * ---------------------------------------------------------------------- */
CANDO_API u32 cando_instr_disasm(const CandoChunk *chunk, u32 offset, FILE *out);

/* -------------------------------------------------------------------------
 * cando_vm_dump_stack -- print the current VM value stack to `out`.
 * Useful for step-by-step debugging.
 * ---------------------------------------------------------------------- */
CANDO_API void cando_vm_dump_stack(const CandoVM *vm, FILE *out);

/* -------------------------------------------------------------------------
 * cando_vm_dump_globals -- print all global variable names and values.
 * ---------------------------------------------------------------------- */
CANDO_API void cando_vm_dump_globals(const CandoVM *vm, FILE *out);

#endif /* CANDO_DEBUG_H */
