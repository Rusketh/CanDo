/*
 * chunk.c -- Bytecode chunk implementation.
 *
 * Must compile with gcc -std=c11.
 */

#include "chunk.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Growth factor for dynamic arrays inside a chunk.
 * ---------------------------------------------------------------------- */
#define CHUNK_GROW(cap)  ((cap) < 8 ? 8 : (cap) * 2)

/* =========================================================================
 * Lifecycle
 * ===================================================================== */

CandoChunk *cando_chunk_new(const char *name, u32 arity, bool has_vararg) {
    CandoChunk *c = (CandoChunk *)cando_alloc(sizeof(CandoChunk));
    c->code        = NULL;
    c->code_len    = 0;
    c->code_cap    = 0;
    c->constants   = NULL;
    c->const_count = 0;
    c->const_cap   = 0;
    c->lines       = NULL;
    c->name        = name ? name : "<anonymous>";
    c->arity       = arity;
    c->local_count = arity;   /* parameters occupy the first local slots  */
    c->upval_count = 0;
    c->has_vararg  = has_vararg;
    return c;
}

void cando_chunk_free(CandoChunk *chunk) {
    if (!chunk) return;

    /* Release any string constants in the pool. */
    for (u32 i = 0; i < chunk->const_count; i++) {
        cando_value_release(chunk->constants[i]);
    }

    cando_free(chunk->code);
    cando_free(chunk->constants);
    cando_free(chunk->lines);
    cando_free(chunk);
}

/* =========================================================================
 * Internal helpers
 * ===================================================================== */

static void chunk_ensure_code(CandoChunk *chunk, u32 extra) {
    if (chunk->code_len + extra <= chunk->code_cap) return;
    u32 new_cap = CHUNK_GROW(chunk->code_cap);
    while (new_cap < chunk->code_len + extra) new_cap *= 2;
    chunk->code  = (u8 *)cando_realloc(chunk->code,  new_cap);
    chunk->lines = (u32 *)cando_realloc(chunk->lines, new_cap * sizeof(u32));
    chunk->code_cap = new_cap;
}

static void chunk_ensure_const(CandoChunk *chunk, u32 extra) {
    if (chunk->const_count + extra <= chunk->const_cap) return;
    u32 new_cap = CHUNK_GROW(chunk->const_cap);
    while (new_cap < chunk->const_count + extra) new_cap *= 2;
    chunk->constants  = (CandoValue *)cando_realloc(chunk->constants,
                                            new_cap * sizeof(CandoValue));
    chunk->const_cap  = new_cap;
}

/* =========================================================================
 * Emitting instructions
 * ===================================================================== */

void cando_chunk_emit_byte(CandoChunk *chunk, u8 byte, u32 line) {
    chunk_ensure_code(chunk, 1);
    chunk->code[chunk->code_len]  = byte;
    chunk->lines[chunk->code_len] = line;
    chunk->code_len++;
}

void cando_chunk_emit_op(CandoChunk *chunk, CandoOpcode op, u32 line) {
    CANDO_ASSERT(cando_opcode_fmt(op) == OPFMT_NONE);
    cando_chunk_emit_byte(chunk, (u8)op, line);
}

void cando_chunk_emit_op_a(CandoChunk *chunk, CandoOpcode op,
                            u16 a, u32 line) {
    CANDO_ASSERT(cando_opcode_fmt(op) == OPFMT_A);
    chunk_ensure_code(chunk, 3);
    chunk->code[chunk->code_len]     = (u8)op;
    chunk->lines[chunk->code_len]    = line;
    chunk->code[chunk->code_len + 1] = (u8)(a & 0xFF);
    chunk->lines[chunk->code_len + 1]= line;
    chunk->code[chunk->code_len + 2] = (u8)(a >> 8);
    chunk->lines[chunk->code_len + 2]= line;
    chunk->code_len += 3;
}

void cando_chunk_emit_op_ab(CandoChunk *chunk, CandoOpcode op,
                             u16 a, u16 b, u32 line) {
    CANDO_ASSERT(cando_opcode_fmt(op) == OPFMT_A_B);
    chunk_ensure_code(chunk, 5);
    u32 base = chunk->code_len;
    chunk->code[base]   = (u8)op;
    chunk->code[base+1] = (u8)(a & 0xFF);
    chunk->code[base+2] = (u8)(a >> 8);
    chunk->code[base+3] = (u8)(b & 0xFF);
    chunk->code[base+4] = (u8)(b >> 8);
    for (u32 i = 0; i < 5; i++) chunk->lines[base + i] = line;
    chunk->code_len += 5;
}

/* =========================================================================
 * Jump patching
 * ===================================================================== */

u32 cando_chunk_emit_jump(CandoChunk *chunk, CandoOpcode op, u32 line) {
    CANDO_ASSERT(cando_opcode_fmt(op) == OPFMT_A);
    /* Emit with a placeholder offset (0x7FFF = max positive i16). */
    u32 patch_at = chunk->code_len + 1; /* byte offset of the u16 operand */
    cando_chunk_emit_op_a(chunk, op, 0x7FFF, line);
    return patch_at;
}

void cando_chunk_patch_jump(CandoChunk *chunk, u32 patch_offset) {
    /* The jump instruction is 3 bytes: [op][lo][hi].
     * The jump target is relative to the byte AFTER the instruction.
     * patch_offset points at [lo].  The instruction starts at patch_offset-1.
     * End of instruction = patch_offset + 2.
     * offset = code_len - (patch_offset + 2)                             */
    u32 instr_end = patch_offset + 2;
    CANDO_ASSERT(chunk->code_len >= instr_end);
    i16 offset = (i16)(chunk->code_len - instr_end);
    cando_write_u16(&chunk->code[patch_offset], (u16)offset);
}

void cando_chunk_patch_jump_to(CandoChunk *chunk, u32 patch_offset,
                                u32 target) {
    u32 instr_end = patch_offset + 2;
    i16 offset    = (i16)((i32)target - (i32)instr_end);
    cando_write_u16(&chunk->code[patch_offset], (u16)offset);
}

void cando_chunk_emit_loop(CandoChunk *chunk, u32 loop_start, u32 line) {
    /* OP_LOOP jumps backward.  The offset is the number of bytes to step
     * back from the byte AFTER the full 3-byte instruction.              */
    u32 instr_end = chunk->code_len + 3; /* after emitting */
    CANDO_ASSERT(instr_end > loop_start);
    u16 offset = (u16)(instr_end - loop_start);
    cando_chunk_emit_op_a(chunk, OP_LOOP, offset, line);
}

/* =========================================================================
 * Constant pool
 * ===================================================================== */

u16 cando_chunk_add_const(CandoChunk *chunk, CandoValue val) {
    /* Dedup numbers and strings to keep the pool compact. */
    if (cando_is_number(val)) {
        for (u32 i = 0; i < chunk->const_count; i++) {
            if (cando_is_number(chunk->constants[i]) &&
                chunk->constants[i].as.number == val.as.number) {
                cando_value_release(val);
                return (u16)i;
            }
        }
    } else if (cando_is_string(val)) {
        for (u32 i = 0; i < chunk->const_count; i++) {
            if (cando_is_string(chunk->constants[i])) {
                CandoString *a = chunk->constants[i].as.string;
                CandoString *b = val.as.string;
                if (a->length == b->length &&
                    memcmp(a->data, b->data, a->length) == 0) {
                    cando_value_release(val);
                    return (u16)i;
                }
            }
        }
    }

    CANDO_ASSERT_MSG(chunk->const_count < 65535,
                     "constant pool overflow (max 65535 entries)");
    chunk_ensure_const(chunk, 1);
    u16 idx = (u16)chunk->const_count;
    chunk->constants[chunk->const_count++] = val;
    return idx;
}

u16 cando_chunk_add_string_const(CandoChunk *chunk, const char *str,
                                  u32 len) {
    CandoString *s = cando_string_new(str, len);
    return cando_chunk_add_const(chunk, cando_string_value(s));
}
