/*
 * debug.c -- Bytecode disassembler implementation.
 *
 * Must compile with gcc -std=c11.
 */

#include "debug.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Internal helpers
 * ===================================================================== */

/* Print a constant pool value in a concise human-readable form. */
static void print_const(const CandoValue *v, FILE *out) {
    char *s = cando_value_tostring(*v);
    fprintf(out, "'%s'", s);
    cando_free(s);
}

/* Print a simple (no-operand) instruction and return the next offset. */
static u32 disasm_simple(const char *name, u32 offset, FILE *out) {
    fprintf(out, "%s\n", name);
    return offset + 1;
}

/* Print an instruction with one u16 operand. */
static u32 disasm_const(const CandoChunk *chunk, const char *name,
                         u32 offset, FILE *out) {
    u16 idx = cando_read_u16(&chunk->code[offset + 1]);
    fprintf(out, "%-22s %5u    ", name, idx);
    if (idx < chunk->const_count) {
        print_const(&chunk->constants[idx], out);
    }
    fprintf(out, "\n");
    return offset + 3;
}

/* Print an instruction with one u16 signed jump offset. */
static u32 disasm_jump(const char *name, u32 offset, int sign,
                        const CandoChunk *chunk, FILE *out) {
    i16 jump = cando_read_i16(&chunk->code[offset + 1]);
    u32 target = (u32)((i32)(offset + 3) + sign * jump);
    fprintf(out, "%-22s %5d -> %04u\n", name, jump, target);
    CANDO_UNUSED(chunk);
    return offset + 3;
}

/* Print an instruction with one u16 operand (slot/count). */
static u32 disasm_u16(const char *name, u32 offset,
                       const CandoChunk *chunk, FILE *out) {
    u16 a = cando_read_u16(&chunk->code[offset + 1]);
    fprintf(out, "%-22s %5u\n", name, a);
    CANDO_UNUSED(chunk);
    return offset + 3;
}

/* Print an instruction with two u16 operands A and B. */
static u32 disasm_u16_u16(const char *name, u32 offset,
                            const CandoChunk *chunk, FILE *out) {
    u16 a = cando_read_u16(&chunk->code[offset + 1]);
    u16 b = cando_read_u16(&chunk->code[offset + 3]);
    fprintf(out, "%-22s %5u %5u\n", name, a, b);
    CANDO_UNUSED(chunk);
    return offset + 5;
}

/* =========================================================================
 * Public API
 * ===================================================================== */

/* -------------------------------------------------------------------------
 * Operand-kind table
 *
 * Each opcode falls into exactly one disassembly shape.  The shapes
 * differ by what the operand bytes mean:
 *   - DK_NONE        : no operand (1 byte total)
 *   - DK_U16         : raw u16 (slot index, count, etc.)
 *   - DK_CONST       : u16 constant-pool index (also prints the constant)
 *   - DK_JUMP_FWD    : signed u16 forward jump offset
 *   - DK_JUMP_BACK   : signed u16 backward jump offset (OP_LOOP)
 *   - DK_U16_U16     : two u16 operands
 *   - DK_CLOSURE     : variable-length: const idx + capture_count + slots
 *
 * Default-initialised entries become DK_NONE (= 0) which is a safe
 * fallback for any opcode not yet listed.  Adding a new opcode that
 * uses a non-trivial operand shape requires one new table entry.
 * ---------------------------------------------------------------------- */
typedef enum {
    DK_NONE = 0,
    DK_U16,
    DK_CONST,
    DK_JUMP_FWD,
    DK_JUMP_BACK,
    DK_U16_U16,
    DK_CLOSURE,
} DisasmKind;

static const DisasmKind DISASM_KIND[OP_COUNT] = {
    /* Constants / globals / fields use the constant pool. */
    [OP_CONST]            = DK_CONST,
    [OP_LOAD_GLOBAL]      = DK_CONST,
    [OP_STORE_GLOBAL]     = DK_CONST,
    [OP_DEF_GLOBAL]       = DK_CONST,
    [OP_DEF_CONST_GLOBAL] = DK_CONST,
    [OP_GET_FIELD]        = DK_CONST,
    [OP_SET_FIELD]        = DK_CONST,
    [OP_NEW_CLASS]        = DK_CONST,
    [OP_BIND_METHOD]      = DK_CONST,

    /* Variable-length closure literal. */
    [OP_CLOSURE]          = DK_CLOSURE,

    /* Plain u16 operand: slot index, count, mode flag, etc. */
    [OP_LOAD_LOCAL]       = DK_U16,
    [OP_STORE_LOCAL]      = DK_U16,
    [OP_DEF_LOCAL]        = DK_U16,
    [OP_DEF_CONST_LOCAL]  = DK_U16,
    [OP_LOAD_UPVAL]       = DK_U16,
    [OP_STORE_UPVAL]      = DK_U16,
    [OP_CLOSE_UPVAL]      = DK_U16,
    [OP_POP_N]            = DK_U16,
    [OP_NEW_ARRAY]        = DK_U16,
    [OP_RETURN]           = DK_U16,
    [OP_CALL]             = DK_U16,
    [OP_TAIL_CALL]        = DK_U16,
    [OP_LOAD_VARARG]      = DK_U16,
    [OP_FOR_INIT]         = DK_U16,
    [OP_FOR_OVER_INIT]    = DK_U16,
    [OP_PIPE_INIT]        = DK_U16,
    [OP_CATCH_BEGIN]      = DK_U16,
    [OP_EQ_STACK]         = DK_U16,
    [OP_NEQ_STACK]        = DK_U16,
    [OP_LT_STACK]         = DK_U16,
    [OP_GT_STACK]         = DK_U16,
    [OP_LEQ_STACK]        = DK_U16,
    [OP_GEQ_STACK]        = DK_U16,
    [OP_RANGE_CHECK]      = DK_U16,
    [OP_BREAK]            = DK_U16,
    [OP_CONTINUE]         = DK_U16,

    /* Two-operand instructions. */
    [OP_LOOP_MARK]        = DK_U16_U16,
    [OP_MASK_APPLY]       = DK_U16_U16,
    [OP_METHOD_CALL]      = DK_U16_U16,
    [OP_FLUENT_CALL]      = DK_U16_U16,

    /* Forward jumps (signed offset added to next instruction). */
    [OP_JUMP]             = DK_JUMP_FWD,
    [OP_JUMP_IF_FALSE]    = DK_JUMP_FWD,
    [OP_JUMP_IF_TRUE]     = DK_JUMP_FWD,
    [OP_JUMP_IF_NULL]     = DK_JUMP_FWD,
    [OP_AND_JUMP]         = DK_JUMP_FWD,
    [OP_OR_JUMP]          = DK_JUMP_FWD,
    [OP_TRY_BEGIN]        = DK_JUMP_FWD,
    [OP_FINALLY_BEGIN]    = DK_JUMP_FWD,
    [OP_FOR_NEXT]         = DK_JUMP_FWD,
    [OP_FOR_OVER_NEXT]    = DK_JUMP_FWD,
    [OP_PIPE_NEXT]        = DK_JUMP_FWD,
    [OP_FILTER_NEXT]      = DK_JUMP_FWD,

    /* Backward jumps. */
    [OP_LOOP]             = DK_JUMP_BACK,

    /* Everything else (no operand) implicitly = DK_NONE. */
};

/* Special-case disassembler for OP_CLOSURE: const-index + capture list. */
static u32 disasm_closure(const CandoChunk *chunk, u32 offset, FILE *out)
{
    u16 idx = cando_read_u16(&chunk->code[offset + 1]);
    u16 cap = cando_read_u16(&chunk->code[offset + 3]);
    fprintf(out, "%-22s %5u    ", "OP_CLOSURE", idx);
    if (idx < chunk->const_count)
        print_const(&chunk->constants[idx], out);
    fprintf(out, " (captures=%u", cap);
    for (u16 ci = 0; ci < cap; ci++) {
        u16 slot = cando_read_u16(&chunk->code[offset + 5 + ci * 2]);
        fprintf(out, "%s%u", ci == 0 ? ": " : ",", slot);
    }
    fprintf(out, ")\n");
    return offset + 5 + (u32)cap * 2;
}

u32 cando_instr_disasm(const CandoChunk *chunk, u32 offset, FILE *out) {
    /* Print byte offset and source line. */
    fprintf(out, "%04u ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        fprintf(out, "   | ");
    } else {
        fprintf(out, "%4u ", chunk->lines[offset]);
    }

    u8           raw_op = chunk->code[offset];
    CandoOpcode  op     = (CandoOpcode)raw_op;

    if ((u32)op >= OP_COUNT) {
        fprintf(out, "OP_UNKNOWN(%02x)\n", raw_op);
        return offset + 1;
    }

    const char *name = cando_opcode_name(op);
    switch (DISASM_KIND[op]) {
        case DK_NONE:      return disasm_simple(name,    offset, out);
        case DK_U16:       return disasm_u16   (name,    offset, chunk, out);
        case DK_CONST:     return disasm_const (chunk, name, offset, out);
        case DK_JUMP_FWD:  return disasm_jump  (name, offset,  1, chunk, out);
        case DK_JUMP_BACK: return disasm_jump  (name, offset, -1, chunk, out);
        case DK_U16_U16:   return disasm_u16_u16(name, offset, chunk, out);
        case DK_CLOSURE:   return disasm_closure(chunk, offset, out);
    }
    /* Unreachable; satisfy the compiler. */
    fprintf(out, "OP_UNKNOWN(%02x)\n", raw_op);
    return offset + 1;
}

void cando_chunk_disasm(const CandoChunk *chunk, FILE *out) {
    fprintf(out, "== %s ==\n", chunk->name);
    fprintf(out, "  arity=%u  locals=%u  upvals=%u  vararg=%s\n",
            chunk->arity, chunk->local_count, chunk->upval_count,
            chunk->has_vararg ? "yes" : "no");

    for (u32 offset = 0; offset < chunk->code_len; ) {
        offset = cando_instr_disasm(chunk, offset, out);
    }
}

void cando_vm_dump_stack(const CandoVM *vm, FILE *out) {
    fprintf(out, "  stack [");
    for (const CandoValue *v = vm->stack; v < vm->stack_top; v++) {
        if (v != vm->stack) fprintf(out, ", ");
        char *s = cando_value_tostring(*v);
        fprintf(out, "%s:%s", cando_value_type_name(cando_value_tag(*v)), s);
        cando_free(s);
    }
    fprintf(out, "]\n");
}

void cando_vm_dump_globals(const CandoVM *vm, FILE *out) {
    fprintf(out, "  globals {\n");
    for (u32 i = 0; i < vm->globals->capacity; i++) {
        const CandoGlobalEntry *e = &vm->globals->entries[i];
        if (!e->key) continue;
        char *s = cando_value_tostring(e->value);
        fprintf(out, "    %s%s = %s\n",
                e->is_const ? "const " : "",
                e->key->data, s);
        cando_free(s);
    }
    fprintf(out, "  }\n");
}
