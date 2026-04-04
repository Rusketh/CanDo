# VM Internals

## CandoVM struct  (`source/vm/vm.h`)

```c
struct CandoVM {
    // Value stack
    CandoValue  stack[2048];
    CandoValue *stack_top;       // one past the last pushed value

    // Call frames
    CandoCallFrame frames[256];
    u32            frame_count;

    // Try/catch stack
    CandoTryFrame  try_stack[64];
    u32            try_depth;

    // Loop stack
    CandoLoopFrame loop_stack[64];
    u32            loop_depth;

    // Open upvalue linked list
    CandoUpvalue  *open_upvalues;

    // Global variables
    CandoGlobalEnv globals;          // open-addressed hash table

    // Native function table
    CandoNativeFn  native_fns[64];
    u32            native_count;

    // Memory controller (may be NULL)
    CandoMemCtrl  *mem;

    // Handle table (CdoObject* lookup by index)
    CandoHandleTable handles;

    // Error state
    CandoValue     error_vals[8];   // thrown values (error_vals[0] = first arg)
    u32            error_val_count; // number of values in the current throw
    bool           has_error;
    char           error_msg[512];  // formatted string of error_vals[0]

    // Multi-return state
    int            last_ret_count;
    int            spread_extra;

    // Eval re-entrancy
    u32            eval_stop_frame;
    CandoValue     eval_result;

    // Built-in type prototypes
    CandoValue     string_proto;     // string method table (or null)
};
```

---

## CandoCallFrame

```c
typedef struct CandoCallFrame {
    CandoClosure *closure;  // function being executed
    u8           *ip;       // instruction pointer into closure->chunk
    CandoValue   *slots;    // base of this frame's window in vm->stack
    u32           ret_count; // expected return-value count (0 = any)
} CandoCallFrame;
```

`slots` points directly into `vm->stack`.  Local variable 0 is `slots[0]`,
local 1 is `slots[1]`, and so on.  The frame's operands live above its locals.

---

## CandoClosure

```c
typedef struct CandoClosure {
    CandoChunk    *chunk;
    CandoUpvalue **upvalues;
    u32            upvalue_count;
} CandoClosure;
```

A closure wraps a `CandoChunk*` (shared; chunks are NOT owned by closures)
and an optional array of captured upvalue pointers.

---

## Upvalues

```c
typedef struct CandoUpvalue {
    CandoValue        *location;  // points into the stack while frame is live
    CandoValue         closed;    // heap copy after OP_CLOSE_UPVAL
    struct CandoUpvalue *next;    // intrusive linked list of open upvalues
} CandoUpvalue;
```

- **Open**: `location` points into `vm->stack`.
- **Closed**: `OP_CLOSE_UPVAL` copies the value to `closed` and redirects
  `location` to `&upvalue->closed`.

---

## Execution lifecycle

```c
// Initialise (mem may be NULL for tests)
void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem);

// Run a top-level chunk; returns VM_OK / VM_HALT / VM_RUNTIME_ERR
CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk);

// Run an eval-mode chunk inside a running VM (re-entrant)
CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue *result_out);

// Release all VM-owned resources
void cando_vm_destroy(CandoVM *vm);
```

`cando_vm_exec` wraps the chunk in a `CandoClosure`, pushes `frames[0]`, and
calls the internal `vm_run()` dispatch loop.

---

## The dispatch loop

The VM uses **GCC computed gotos** (`&&label`, `goto *dispatch_table[op]`) for
maximum branch-prediction friendliness.  The loop structure is:

```c
static CandoVMResult vm_run(CandoVM *vm) {
    // Build a dispatch table indexed by CandoOpcode
    static void *dispatch_table[OP_COUNT] = { &&do_OP_CONST, ... };

    #define DISPATCH() goto *dispatch_table[(CandoOpcode)(*ip++)]

    DISPATCH();  // first instruction

    do_OP_CONST: { ... DISPATCH(); }
    do_OP_ADD:   { ... DISPATCH(); }
    ...
}
```

This compiles to an indirect jump per instruction — equivalent to a
`switch` but without the bounds check or jump table limitations.

---

## Instruction encoding

Every instruction is one byte (`CandoOpcode`).  Instructions with operands
are followed by 2-byte little-endian `u16` values:

| Format | Bytes | Description |
|---|---|---|
| `OPFMT_NONE` | 1 | opcode only |
| `OPFMT_A` | 3 | opcode + u16 A |
| `OPFMT_A_B` | 5 | opcode + u16 A + u16 B |

Helpers in `opcodes.h`:

```c
CandoOpFmt cando_opcode_fmt(CandoOpcode op);
u32        cando_opcode_size(CandoOpcode op);  // 1, 3, or 5
u16        cando_read_u16(const u8 *ip);
i16        cando_read_i16(const u8 *ip);       // signed jump offsets
void       cando_write_u16(u8 *ip, u16 val);
```

---

## Opcode reference

### Literals

| Opcode | Operand | Effect |
|---|---|---|
| `OP_CONST` | A | push `constants[A]` |
| `OP_NULL` | — | push null |
| `OP_TRUE` | — | push true |
| `OP_FALSE` | — | push false |

### Stack

| Opcode | Operand | Effect |
|---|---|---|
| `OP_POP` | — | discard top |
| `OP_POP_N` | A | discard top A values |
| `OP_DUP` | — | duplicate top |

### Variables

| Opcode | Operand | Effect |
|---|---|---|
| `OP_LOAD_LOCAL` | A | push `slots[A]` |
| `OP_STORE_LOCAL` | A | `slots[A] = peek` (no pop) |
| `OP_DEF_LOCAL` | A | pop → `slots[A]`; resets `spread_extra` |
| `OP_DEF_CONST_LOCAL` | A | same; marks slot constant |
| `OP_LOAD_GLOBAL` | A | push global named `constants[A]` |
| `OP_STORE_GLOBAL` | A | write global `constants[A]` = peek |
| `OP_DEF_GLOBAL` | A | pop → new global `constants[A]`; resets `spread_extra` |
| `OP_DEF_CONST_GLOBAL` | A | same; write-protected |

### Upvalues

| Opcode | Operand | Effect |
|---|---|---|
| `OP_LOAD_UPVAL` | A | push `upvalues[A]` |
| `OP_STORE_UPVAL` | A | `upvalues[A] = peek` |
| `OP_CLOSE_UPVAL` | A | close upvalue at stack slot A to heap |

### Arithmetic / comparison / bitwise / logical

Standard one-or-two-operand opcodes: `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`,
`OP_MOD`, `OP_POW`, `OP_NEG`, `OP_POS`, `OP_INCR`, `OP_DECR`,
`OP_EQ`, `OP_NEQ`, `OP_LT`, `OP_GT`, `OP_LEQ`, `OP_GEQ`,
`OP_EQ_STACK`, `OP_NEQ_STACK`, `OP_LT_STACK`, …, `OP_RANGE_CHECK`,
`OP_BIT_AND`, `OP_BIT_OR`, `OP_BIT_XOR`, `OP_BIT_NOT`, `OP_LSHIFT`, `OP_RSHIFT`,
`OP_NOT`, `OP_AND_JUMP` (A = forward byte offset if falsy, peek not popped),
`OP_OR_JUMP` (A = forward byte offset if truthy).

### Objects

| Opcode | Operand | Effect |
|---|---|---|
| `OP_NEW_OBJECT` | — | push new empty object |
| `OP_NEW_ARRAY` | A | pop A values, push array |
| `OP_GET_FIELD` | A | obj=pop; push `obj[constants[A]]` |
| `OP_SET_FIELD` | A | val=pop, obj=peek; `obj[constants[A]] = val` |
| `OP_GET_INDEX` | — | idx=pop, obj=pop; push `obj[idx]` |
| `OP_SET_INDEX` | — | val=pop, idx=pop, obj=peek; `obj[idx] = val` |
| `OP_LEN` | — | pop; push length |

### Functions and calls

| Opcode | Operand | Effect |
|---|---|---|
| `OP_CLOSURE` | A | build `CandoClosure` from `constants[A]` chunk prototype |
| `OP_CALL` | A | call TOS with A args (pushed before the function) |
| `OP_METHOD_CALL` | A, B | `obj:constants[A](...)`; B = arg count |
| `OP_FLUENT_CALL` | A, B | same but returns `obj` instead of method result |
| `OP_RETURN` | A | return top A values; pop frame |
| `OP_TAIL_CALL` | A | tail-recursive call optimisation |

### Call convention

For `OP_CALL A`:
- Stack before: `[... callee, arg0, arg1, ..., argA-1]`
- `callee` is at `stack_top - A - 1`.
- A new `CandoCallFrame` is pushed with `slots = &callee`.
- On `OP_RETURN N`, the top N values replace the callee slot range.

### Multi-return and spreading

`OP_RETURN A` leaves A values above `frame->slots`.  The caller sees them via
`vm->last_ret_count`.

`OP_SPREAD_RET` adjusts `vm->spread_extra` by `last_ret_count - 1` to account
for functions that return more values than the single "slot" the call
expression normally occupies.  All four variable-defining opcodes reset
`spread_extra` to 0 after consuming it.

### Control flow

`OP_JUMP`, `OP_JUMP_IF_FALSE`, `OP_JUMP_IF_TRUE`: A is a signed i16 byte
offset relative to the byte *after* the operand bytes.

`OP_LOOP`: unconditional backward jump; A = unsigned backward byte count.

`OP_BREAK` / `OP_CONTINUE`: A = loop depth (0 = innermost).  The VM walks
the `loop_stack` A levels up and jumps to `break_ip` or `cont_ip`.

### Try / catch / finally

```
OP_TRY_BEGIN   A   push CandoTryFrame; A = signed offset to catch block
  <try body>
OP_TRY_END         normal exit; pop try frame
OP_JUMP        A   skip catch block on normal path
  <catch block>
OP_CATCH_BEGIN A   push A values from vm->error_vals[] (padding null for missing);
               A   top of stack = error_vals[0] (first thrown arg)
  <catch body>    (each param bound by successive OP_DEF_LOCAL instructions)
OP_FINALLY_BEGIN A begin finally; A = signed offset past finally block
  <finally body>
OP_THROW       A   pop A values in order, store in error_vals[0..A-1], throw
OP_RERAISE         re-throw current exception
```

**Multi-argument throw/catch:**

| Thrown count | Catch params | Result |
|---|---|---|
| N == P | P params | all params bound |
| N < P | P params | first N bound; remaining params = `null` |
| N > P | P params | first P bound; extra thrown values dropped |

`error_vals[0]` is always the first argument and is also stringified into
`error_msg` for display purposes.

On error, the VM restores `stack_top`, `frame_count`, and `loop_depth` from
the saved `CandoTryFrame`, then jumps to `catch_ip`.  `OP_CATCH_BEGIN` then
pushes the bound values, clearing `error_vals`.

**Error state fields:**

| Field | Set by | Description |
|---|---|---|
| `error_vals[0..7]` | `OP_THROW`, `cando_vm_error()`, `vm_runtime_error()` | thrown values |
| `error_val_count` | same | how many values are in the current throw |
| `has_error` | same | true while an error is propagating |
| `error_msg` | same | string form of `error_vals[0]`, used for display |

### Eval stop (`VM_EVAL_DONE`)

When `cando_vm_exec_eval` runs an eval chunk, it sets `vm->eval_stop_frame` =
current `frame_count`.  When `OP_RETURN` decrements `frame_count` to equal
`eval_stop_frame`, the VM stores the return value in `vm->eval_result` and
returns `VM_EVAL_DONE` instead of continuing.  The outer `vm_run` call then
sees `VM_EVAL_DONE` and resumes normally.

---

## Stack helpers

```c
void       cando_vm_push(CandoVM *vm, CandoValue val);
CandoValue cando_vm_pop(CandoVM *vm);
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist); // 0 = top
u32        cando_vm_stack_depth(const CandoVM *vm);
```
