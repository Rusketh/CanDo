# Virtual Machine

The VM is a stack-based bytecode interpreter.  Dispatch uses GCC
computed gotos when available, with a `switch`-based fallback.  This
page describes the dispatch loop, stack frames, and the opcode table.

## Files

```
source/vm/
  vm.c, vm.h           dispatch loop (vm_run)
  bridge.c, bridge.h    CandoValue ↔ CdoObject translation
  chunk.c, chunk.h      CandoChunk: bytecode + constant pool + line info
  opcodes.h, opcodes.c  opcode enum, operand format, effect class table
  debug.c               disassembler (driven by --disasm)
```

## State

A VM holds:

- A **value stack** (default 2048 slots, `CANDO_STACK_MAX`).
- A **call stack** (default 256 frames, `CANDO_FRAMES_MAX`).
- A **handle table** (the indirection layer for objects).
- A global table.
- An open-upvalue list (intrusive linked list, head = top of stack).
- A global `_meta` registry.
- The current error state (used by `THROW` / `RERAISE`).

A "thread" handle owns its own stack and call stack, but shares the
global table and handle table with the VM that created it.

## Call frames

```
struct CandoCallFrame {
    CandoChunk *chunk;
    uint8_t    *ip;      /* instruction pointer */
    CandoValue *bp;      /* base pointer into the value stack */
    CdoClosure *closure; /* the executing closure (NULL for top-level chunk) */
    /* TRY frame stack, etc. */
};
```

`OP_CALL` pushes a frame; `OP_RETURN` pops one.  Locals live above
`bp`; the caller's value stack is below.  Function arguments are
already on the stack at call time, so they become the first locals of
the callee.

## Dispatch loop

`vm_run()` is the main hot loop.  Skeleton:

```c
for (;;) {
    CandoOpcode op = *ip++;
    DISPATCH(op);

  case_OP_ADD: {
      CandoValue b = pop();
      CandoValue a = pop();
      push(arith_add(vm, a, b));
      NEXT();
  }

  case_OP_LOAD_LOCAL: {
      uint16_t slot = read_u16(ip); ip += 2;
      push(bp[slot]);
      NEXT();
  }
  /* ... */
}
```

When GCC computed gotos are available (`__GNUC__`), `DISPATCH` and
`NEXT` are direct jumps through a label table built from `opcodes.h`.
This collapses the dispatch overhead to one indirect branch per
instruction with strong branch-prediction locality.

## Operand formats

```c
typedef enum {
    OPFMT_NONE,    /* 1 byte  — opcode only */
    OPFMT_A,       /* 3 bytes — opcode + u16 */
    OPFMT_A_B,     /* 5 bytes — opcode + u16 + u16 */
} CandoOpFmt;
```

Decoders are inlined in the dispatch loop.

## Effect classes

Each opcode is tagged with an effect class for the JIT:

| Class            | Meaning |
|------------------|---------|
| `EFFECT_PURE`    | No side effect; safe to fold or reorder. |
| `EFFECT_LOAD`    | Reads state but does not mutate. |
| `EFFECT_STORE`   | Writes state. |
| `EFFECT_CALL`    | Transfers control elsewhere. |
| `EFFECT_THROW`   | Always raises an exception. |
| `EFFECT_CONTROL` | Modifies flow state (jumps, returns, frames). |

The trace recorder uses these to decide when it can speculatively
inline an opcode and when it must abort.  See [jit.md](jit.md).

## Opcode table

The full list is in `source/vm/opcodes.h`, grouped into bands.  Bands
aren't load-bearing; they just keep the file readable.

### Band 0 — literal constants
`OP_CONST`, `OP_NULL`, `OP_TRUE`, `OP_FALSE`

### Band 1 — stack manipulation
`OP_POP`, `OP_POP_N`, `OP_DUP`

### Band 2 — local variables
`OP_LOAD_LOCAL`, `OP_STORE_LOCAL`, `OP_DEF_LOCAL`, `OP_DEF_CONST_LOCAL`

### Band 3 — global variables
`OP_LOAD_GLOBAL`, `OP_STORE_GLOBAL`, `OP_DEF_GLOBAL`,
`OP_DEF_CONST_GLOBAL`

### Band 4 — upvalues / closures
`OP_LOAD_UPVAL`, `OP_STORE_UPVAL`, `OP_CLOSE_UPVAL`

### Band 5 — arithmetic
`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`, `OP_POW`,
`OP_NEG`, `OP_POS`, `OP_INCR`, `OP_DECR`

### Band 6 — comparison
`OP_EQ`, `OP_NEQ`, `OP_LT`, `OP_GT`, `OP_LEQ`, `OP_GEQ`,
plus `_STACK` variants and `OP_RANGE_CHECK`.

### Band 7 — bitwise
`OP_BIT_AND`, `OP_BIT_OR`, `OP_BIT_XOR`, `OP_BIT_NOT`,
`OP_LSHIFT`, `OP_RSHIFT`

### Band 8 — logical (short-circuit jumps)
`OP_NOT`, `OP_AND_JUMP`, `OP_OR_JUMP`

### Band 9 — objects and arrays
`OP_NEW_OBJECT`, `OP_NEW_ARRAY`, `OP_GET_FIELD`, `OP_SET_FIELD`,
`OP_GET_INDEX`, `OP_SET_INDEX`, `OP_LEN`, `OP_KEYS_OF`, `OP_VALS_OF`

### Band 10 — control flow
`OP_JUMP`, `OP_JUMP_IF_FALSE`, `OP_JUMP_IF_TRUE`, `OP_JUMP_IF_NULL`,
`OP_LOOP`, `OP_BREAK`, `OP_CONTINUE`, `OP_LOOP_MARK`, `OP_LOOP_END`

The high two bits of a `OP_LOOP_MARK` operand encode the loop type:
`CANDO_LOOP_WHILE` (0), `CANDO_LOOP_FOR` (1), `CANDO_LOOP_FOR_OVER`
(2).

### Band 11 — functions and calls
`OP_CLOSURE`, `OP_CALL`, `OP_METHOD_CALL`, `OP_FLUENT_CALL`,
`OP_RETURN`, `OP_TAIL_CALL` (reserved)

### Band 12 — varargs
`OP_LOAD_VARARG`, `OP_VARARG_LEN`, `OP_UNPACK`

### Band 13 — iteration
`OP_RANGE_ASC`, `OP_RANGE_DESC`, `OP_FOR_INIT`, `OP_FOR_NEXT`,
`OP_FOR_OVER_INIT`, `OP_FOR_OVER_NEXT`,
`OP_PIPE_INIT`, `OP_PIPE_NEXT`, `OP_FILTER_NEXT`, `OP_PIPE_END`,
`OP_PIPE_COLLECT`, `OP_FILTER_COLLECT`, `OP_COND_FILTER_COLLECT`,
`OP_FOR_RANGE_INIT`, `OP_FOR_RANGE_NEXT`

`OP_FOR_RANGE_INIT` and `OP_FOR_RANGE_NEXT` are physically placed
after Band 19 in `opcodes.h` (added later in development) but belong
to Band 13 logically — they specialise `FOR i IN N -> M` so the full
range never has to be materialised as an array.

### Band 14 — error handling
`OP_TRY_BEGIN`, `OP_TRY_END`, `OP_CATCH_BEGIN`, `OP_FINALLY_BEGIN`,
`OP_THROW`, `OP_RERAISE`

### Band 15 — async / threads
`OP_ASYNC` (reserved), `OP_AWAIT`, `OP_YIELD` (reserved), `OP_THREAD`

### Band 16 — class sugar
`OP_NEW_CLASS`, `OP_BIND_METHOD`, `OP_INHERIT`,
`OP_BIND_DEFAULT_CALL`

### Band 17 — masks
`OP_MASK_PASS`, `OP_MASK_SKIP`, `OP_MASK_APPLY`

### Band 18 — multi-return spreading
`OP_SPREAD_RET`, `OP_ARRAY_SPREAD`

### Band 19 — call-result comparison
`OP_TRUNCATE_RET`, `OP_EQ_SPREAD`, `OP_NEQ_SPREAD`, `OP_LT_SPREAD`,
`OP_GT_SPREAD`, `OP_LEQ_SPREAD`, `OP_GEQ_SPREAD`

### Sentinels
`OP_NOP`, `OP_HALT`

## TRY / CATCH / FINALY

Each `TRY` block emits `OP_TRY_BEGIN` with operands pointing at the
catch and finally targets.  When `THROW` fires, the runtime walks the
try-frame stack:

1. Run the most recent `FINALY` (if any) before yielding control to a
   matching `CATCH`.
2. Once a `CATCH` is selected, install the thrown values as locals
   bound to the catch parameters.
3. After the `CATCH` body completes, run any pending `FINALY`.
4. Resume normal execution after the `TRY`'s `OP_TRY_END`.

If no `CATCH` matches, the error keeps propagating up the call stack
until the embedder sees `CANDO_ERR_RUNTIME`.

## Adding an opcode

1. Append the new opcode to `source/vm/opcodes.h` in the appropriate
   band.
2. Add a row to the metadata table in `source/vm/opcodes.c`: name,
   `OPFMT_*`, effect class.
3. Implement the dispatch case in `source/vm/vm.c`.  Use a neighbour
   in the same band as a template — they all follow the same
   prologue/epilogue conventions.
4. Emit the opcode from the parser via the helpers in
   `source/parser/emit.h`.
5. If the opcode is JITable, add a translator in `source/jit/`.
   Otherwise mark it as an abort point so traces stop cleanly when
   they encounter it.

## Disassembly

Run any script with `--disasm` to see its compiled bytecode:

```bash
./cando script.cdo --disasm
```

The disassembler emits one line per instruction with operand decoding,
preceded by a function header and constant-pool dump.  Use this when
investigating "why is this slower than I expected" or "why is the
parser emitting *that*".
