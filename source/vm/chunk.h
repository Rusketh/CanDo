/*
 * chunk.h -- Bytecode chunk: the compiled form of a Cando function or
 *            top-level script.
 *
 * A CandoChunk holds:
 *   - the raw bytecode array
 *   - a constant pool (CandoValue array)
 *   - a line-number table (one u32 per bytecode byte)
 *   - metadata: function name, arity, local-slot count
 *
 * Chunks are heap-allocated via cando_chunk_new and freed with
 * cando_chunk_free.  Closures hold a pointer to their chunk; the chunk
 * must outlive all closures that reference it.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_CHUNK_H
#define CANDO_CHUNK_H

#include "../core/common.h"
#include "../core/value.h"
#include "opcodes.h"

/* -------------------------------------------------------------------------
 * CandoChunk
 * ---------------------------------------------------------------------- */
typedef struct CandoChunk {
    /* Bytecode --------------------------------------------------------- */
    u8  *code;          /* raw instruction bytes                          */
    u32  code_len;      /* number of bytes written                        */
    u32  code_cap;      /* allocated capacity of code[]                   */

    /* Constant pool ---------------------------------------------------- */
    CandoValue *constants;  /* indexed by the A operand of OP_CONST etc.  */
    u32         const_count;
    u32         const_cap;

    /* Line-number table (parallel to code[]) --------------------------- */
    u32 *lines;         /* lines[i] = source line for code[i]             */

    /* Metadata --------------------------------------------------------- */
    const char *name;   /* function / chunk name (static or heap string)  */
    u32  arity;         /* number of named parameters                     */
    u32  local_count;   /* total local variable slots (incl. parameters)  */
    u32  upval_count;   /* number of captured upvalues                    */
    bool has_vararg;    /* true if the function accepts ...               */
} CandoChunk;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/* cando_chunk_new -- allocate and zero-initialise a fresh chunk.
 * `name` is stored by pointer (caller must ensure lifetime). */
CandoChunk *cando_chunk_new(const char *name, u32 arity, bool has_vararg);

/* cando_chunk_free -- release all internal storage and the chunk itself. */
void cando_chunk_free(CandoChunk *chunk);

/* -------------------------------------------------------------------------
 * Emitting instructions
 * ---------------------------------------------------------------------- */

/* cando_chunk_emit_byte -- append a single byte at `line`. */
void cando_chunk_emit_byte(CandoChunk *chunk, u8 byte, u32 line);

/* cando_chunk_emit_op -- emit an opcode (no operand). */
void cando_chunk_emit_op(CandoChunk *chunk, CandoOpcode op, u32 line);

/* cando_chunk_emit_op_a -- emit opcode + 2-byte little-endian operand A. */
void cando_chunk_emit_op_a(CandoChunk *chunk, CandoOpcode op,
                            u16 a, u32 line);

/* cando_chunk_emit_op_ab -- emit opcode + operands A and B (4 extra bytes). */
void cando_chunk_emit_op_ab(CandoChunk *chunk, CandoOpcode op,
                             u16 a, u16 b, u32 line);

/* -------------------------------------------------------------------------
 * Jump patching
 *
 * Pattern:
 *   u32 patch = cando_chunk_emit_jump(chunk, OP_JUMP_IF_FALSE, line);
 *   ... emit body ...
 *   cando_chunk_patch_jump(chunk, patch);          // fills in the offset
 * ---------------------------------------------------------------------- */

/* cando_chunk_emit_jump -- emit a jump instruction with a placeholder
 * offset.  Returns the byte offset of the placeholder (for patching).   */
u32 cando_chunk_emit_jump(CandoChunk *chunk, CandoOpcode op, u32 line);

/* cando_chunk_patch_jump -- rewrite the placeholder at `patch_offset`
 * so the jump lands at the current end of the chunk.                    */
void cando_chunk_patch_jump(CandoChunk *chunk, u32 patch_offset);

/* cando_chunk_patch_jump_to -- rewrite placeholder to jump to `target`. */
void cando_chunk_patch_jump_to(CandoChunk *chunk, u32 patch_offset,
                                u32 target);

/* cando_chunk_emit_loop -- emit OP_LOOP that jumps back to `loop_start`. */
void cando_chunk_emit_loop(CandoChunk *chunk, u32 loop_start, u32 line);

/* -------------------------------------------------------------------------
 * Constant pool
 * ---------------------------------------------------------------------- */

/* cando_chunk_add_const -- intern a constant; returns its pool index.
 * Duplicate numbers and strings may share an index.                     */
u16 cando_chunk_add_const(CandoChunk *chunk, CandoValue val);

/* cando_chunk_add_string_const -- convenience: intern a C string.       */
u16 cando_chunk_add_string_const(CandoChunk *chunk, const char *str,
                                  u32 len);

#endif /* CANDO_CHUNK_H */
