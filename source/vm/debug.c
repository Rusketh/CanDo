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

u32 cando_instr_disasm(const CandoChunk *chunk, u32 offset, FILE *out) {
    /* Print byte offset and source line. */
    fprintf(out, "%04u ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        fprintf(out, "   | ");
    } else {
        fprintf(out, "%4u ", chunk->lines[offset]);
    }

    u8 raw_op = chunk->code[offset];
    CandoOpcode op = (CandoOpcode)raw_op;

    /* Dispatch to appropriate format printer. */
    switch (op) {
        /* ── No operand ─────────────────────────────────────────────── */
        case OP_NULL:         return disasm_simple("OP_NULL",         offset, out);
        case OP_TRUE:         return disasm_simple("OP_TRUE",         offset, out);
        case OP_FALSE:        return disasm_simple("OP_FALSE",        offset, out);
        case OP_POP:          return disasm_simple("OP_POP",          offset, out);
        case OP_DUP:          return disasm_simple("OP_DUP",          offset, out);
        case OP_ADD:          return disasm_simple("OP_ADD",          offset, out);
        case OP_SUB:          return disasm_simple("OP_SUB",          offset, out);
        case OP_MUL:          return disasm_simple("OP_MUL",          offset, out);
        case OP_DIV:          return disasm_simple("OP_DIV",          offset, out);
        case OP_MOD:          return disasm_simple("OP_MOD",          offset, out);
        case OP_POW:          return disasm_simple("OP_POW",          offset, out);
        case OP_NEG:          return disasm_simple("OP_NEG",          offset, out);
        case OP_POS:          return disasm_simple("OP_POS",          offset, out);
        case OP_INCR:         return disasm_simple("OP_INCR",         offset, out);
        case OP_DECR:         return disasm_simple("OP_DECR",         offset, out);
        case OP_EQ:           return disasm_simple("OP_EQ",           offset, out);
        case OP_NEQ:          return disasm_simple("OP_NEQ",          offset, out);
        case OP_LT:           return disasm_simple("OP_LT",           offset, out);
        case OP_GT:           return disasm_simple("OP_GT",           offset, out);
        case OP_LEQ:          return disasm_simple("OP_LEQ",          offset, out);
        case OP_GEQ:          return disasm_simple("OP_GEQ",          offset, out);
        case OP_BIT_AND:      return disasm_simple("OP_BIT_AND",      offset, out);
        case OP_BIT_OR:       return disasm_simple("OP_BIT_OR",       offset, out);
        case OP_BIT_XOR:      return disasm_simple("OP_BIT_XOR",      offset, out);
        case OP_BIT_NOT:      return disasm_simple("OP_BIT_NOT",      offset, out);
        case OP_LSHIFT:       return disasm_simple("OP_LSHIFT",       offset, out);
        case OP_RSHIFT:       return disasm_simple("OP_RSHIFT",       offset, out);
        case OP_NOT:          return disasm_simple("OP_NOT",          offset, out);
        case OP_NEW_OBJECT:   return disasm_simple("OP_NEW_OBJECT",   offset, out);
        case OP_GET_INDEX:    return disasm_simple("OP_GET_INDEX",    offset, out);
        case OP_SET_INDEX:    return disasm_simple("OP_SET_INDEX",    offset, out);
        case OP_LEN:          return disasm_simple("OP_LEN",          offset, out);
        case OP_KEYS_OF:      return disasm_simple("OP_KEYS_OF",      offset, out);
        case OP_VALS_OF:      return disasm_simple("OP_VALS_OF",      offset, out);
        case OP_LOOP_END:     return disasm_simple("OP_LOOP_END",     offset, out);
        case OP_VARARG_LEN:   return disasm_simple("OP_VARARG_LEN",   offset, out);
        case OP_UNPACK:       return disasm_simple("OP_UNPACK",       offset, out);
        case OP_RANGE_ASC:    return disasm_simple("OP_RANGE_ASC",    offset, out);
        case OP_RANGE_DESC:   return disasm_simple("OP_RANGE_DESC",   offset, out);
        case OP_TRY_END:      return disasm_simple("OP_TRY_END",      offset, out);
        case OP_THROW:        return disasm_simple("OP_THROW",        offset, out);
        case OP_RERAISE:      return disasm_simple("OP_RERAISE",      offset, out);
        case OP_ASYNC:        return disasm_simple("OP_ASYNC",        offset, out);
        case OP_AWAIT:        return disasm_simple("OP_AWAIT",        offset, out);
        case OP_YIELD:        return disasm_simple("OP_YIELD",        offset, out);
        case OP_INHERIT:      return disasm_simple("OP_INHERIT",      offset, out);
        case OP_MASK_PASS:    return disasm_simple("OP_MASK_PASS",    offset, out);
        case OP_MASK_SKIP:    return disasm_simple("OP_MASK_SKIP",    offset, out);
        case OP_MASK_APPLY:   return disasm_u16_u16("OP_MASK_APPLY",  offset, chunk, out);
        case OP_PIPE_END:         return disasm_simple("OP_PIPE_END",         offset, out);
        case OP_PIPE_COLLECT:     return disasm_simple("OP_PIPE_COLLECT",     offset, out);
        case OP_FILTER_COLLECT:   return disasm_simple("OP_FILTER_COLLECT",   offset, out);
        case OP_SPREAD_RET:       return disasm_simple("OP_SPREAD_RET",       offset, out);
        case OP_NOP:              return disasm_simple("OP_NOP",               offset, out);
        case OP_HALT:         return disasm_simple("OP_HALT",         offset, out);

        /* ── Constant pool reference ─────────────────────────────────── */
        case OP_CONST:            return disasm_const(chunk, "OP_CONST",            offset, out);
        case OP_LOAD_GLOBAL:      return disasm_const(chunk, "OP_LOAD_GLOBAL",      offset, out);
        case OP_STORE_GLOBAL:     return disasm_const(chunk, "OP_STORE_GLOBAL",     offset, out);
        case OP_DEF_GLOBAL:       return disasm_const(chunk, "OP_DEF_GLOBAL",       offset, out);
        case OP_DEF_CONST_GLOBAL: return disasm_const(chunk, "OP_DEF_CONST_GLOBAL", offset, out);
        case OP_GET_FIELD:        return disasm_const(chunk, "OP_GET_FIELD",        offset, out);
        case OP_SET_FIELD:        return disasm_const(chunk, "OP_SET_FIELD",        offset, out);
        case OP_CLOSURE:          return disasm_const(chunk, "OP_CLOSURE",          offset, out);
        case OP_NEW_CLASS:        return disasm_const(chunk, "OP_NEW_CLASS",        offset, out);
        case OP_BIND_METHOD:      return disasm_const(chunk, "OP_BIND_METHOD",      offset, out);

        /* ── Slot / count operand ────────────────────────────────────── */
        case OP_LOAD_LOCAL:      return disasm_u16("OP_LOAD_LOCAL",      offset, chunk, out);
        case OP_STORE_LOCAL:     return disasm_u16("OP_STORE_LOCAL",     offset, chunk, out);
        case OP_DEF_LOCAL:       return disasm_u16("OP_DEF_LOCAL",       offset, chunk, out);
        case OP_DEF_CONST_LOCAL: return disasm_u16("OP_DEF_CONST_LOCAL", offset, chunk, out);
        case OP_LOAD_UPVAL:      return disasm_u16("OP_LOAD_UPVAL",      offset, chunk, out);
        case OP_STORE_UPVAL:     return disasm_u16("OP_STORE_UPVAL",     offset, chunk, out);
        case OP_CLOSE_UPVAL:     return disasm_u16("OP_CLOSE_UPVAL",     offset, chunk, out);
        case OP_POP_N:           return disasm_u16("OP_POP_N",           offset, chunk, out);
        case OP_NEW_ARRAY:       return disasm_u16("OP_NEW_ARRAY",       offset, chunk, out);
        case OP_RETURN:          return disasm_u16("OP_RETURN",          offset, chunk, out);
        case OP_CALL:            return disasm_u16("OP_CALL",            offset, chunk, out);
        case OP_TAIL_CALL:       return disasm_u16("OP_TAIL_CALL",       offset, chunk, out);
        case OP_LOAD_VARARG:     return disasm_u16("OP_LOAD_VARARG",     offset, chunk, out);
        case OP_FOR_INIT:        return disasm_u16("OP_FOR_INIT",        offset, chunk, out);
        case OP_FOR_OVER_INIT:   return disasm_u16("OP_FOR_OVER_INIT",   offset, chunk, out);
        case OP_PIPE_INIT:       return disasm_u16("OP_PIPE_INIT",       offset, chunk, out);
        case OP_CATCH_BEGIN:     return disasm_u16("OP_CATCH_BEGIN",     offset, chunk, out);
        case OP_EQ_STACK:        return disasm_u16("OP_EQ_STACK",        offset, chunk, out);
        case OP_NEQ_STACK:       return disasm_u16("OP_NEQ_STACK",       offset, chunk, out);
        case OP_LT_STACK:        return disasm_u16("OP_LT_STACK",        offset, chunk, out);
        case OP_GT_STACK:        return disasm_u16("OP_GT_STACK",        offset, chunk, out);
        case OP_LEQ_STACK:       return disasm_u16("OP_LEQ_STACK",       offset, chunk, out);
        case OP_GEQ_STACK:       return disasm_u16("OP_GEQ_STACK",       offset, chunk, out);
        case OP_RANGE_CHECK:     return disasm_u16("OP_RANGE_CHECK",     offset, chunk, out);
        case OP_LOOP_MARK:       return disasm_u16("OP_LOOP_MARK",       offset, chunk, out);
        case OP_BREAK:           return disasm_u16("OP_BREAK",           offset, chunk, out);
        case OP_CONTINUE:        return disasm_u16("OP_CONTINUE",        offset, chunk, out);

        /* ── Jump instructions (signed offset) ──────────────────────── */
        case OP_JUMP:
            return disasm_jump("OP_JUMP",         offset, 1, chunk, out);
        case OP_JUMP_IF_FALSE:
            return disasm_jump("OP_JUMP_IF_FALSE", offset, 1, chunk, out);
        case OP_JUMP_IF_TRUE:
            return disasm_jump("OP_JUMP_IF_TRUE",  offset, 1, chunk, out);
        case OP_LOOP:
            return disasm_jump("OP_LOOP",          offset, -1, chunk, out);
        case OP_AND_JUMP:
            return disasm_jump("OP_AND_JUMP",      offset, 1, chunk, out);
        case OP_OR_JUMP:
            return disasm_jump("OP_OR_JUMP",       offset, 1, chunk, out);
        case OP_TRY_BEGIN:
            return disasm_jump("OP_TRY_BEGIN",     offset, 1, chunk, out);
        case OP_FINALLY_BEGIN:
            return disasm_jump("OP_FINALLY_BEGIN", offset, 1, chunk, out);
        case OP_FOR_NEXT:
            return disasm_jump("OP_FOR_NEXT",      offset, 1, chunk, out);
        case OP_FOR_OVER_NEXT:
            return disasm_jump("OP_FOR_OVER_NEXT", offset, 1, chunk, out);
        case OP_PIPE_NEXT:
            return disasm_jump("OP_PIPE_NEXT",     offset, 1, chunk, out);
        case OP_FILTER_NEXT:
            return disasm_jump("OP_FILTER_NEXT",   offset, 1, chunk, out);

        /* ── Two-operand instructions ────────────────────────────────── */
        case OP_METHOD_CALL:
            return disasm_u16_u16("OP_METHOD_CALL", offset, chunk, out);
        case OP_FLUENT_CALL:
            return disasm_u16_u16("OP_FLUENT_CALL", offset, chunk, out);

        /* ── Unknown ────────────────────────────────────────────────── */
        default:
            fprintf(out, "OP_UNKNOWN(%02x)\n", raw_op);
            return offset + 1;
    }
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
        fprintf(out, "%s:%s", cando_value_type_name((TypeTag)v->tag), s);
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
