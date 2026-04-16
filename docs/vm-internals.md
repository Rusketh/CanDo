# VM Internals

## CandoVM (`source/vm/vm.h`)

```c
struct CandoVM {
    CandoValue      stack[CANDO_STACK_MAX];      // 2048
    CandoValue     *stack_top;

    CandoCallFrame  frames[CANDO_FRAMES_MAX];    // 256
    u32             frame_count;

    CandoTryFrame   try_stack[CANDO_TRY_MAX];   // 64
    u32             try_depth;

    CandoLoopFrame  loop_stack[CANDO_LOOP_MAX];  // 64
    u32             loop_depth;

    CandoUpvalue   *open_upvalues;

    CandoGlobalEnv *globals;          // shared pointer; children use parent's
    CandoGlobalEnv *globals_owned;    // heap-allocated; NULL for child VMs

    CandoNativeFn   native_fns[CANDO_NATIVE_MAX]; // 128
    u32             native_count;

    CandoMemCtrl   *mem;

    CandoHandleTable *handles;        // shared pointer; children use parent's
    CandoHandleTable *handles_owned;

    CandoValue      error_vals[CANDO_MAX_THROW_ARGS]; // 32
    u32             error_val_count;
    bool            has_error;
    char            error_msg[512];

    int             last_ret_count;
    int             spread_extra;
    int             array_extra;

    u32             eval_stop_frame;
    CandoValue     *eval_results;
    u32             eval_result_count;
    u32             eval_result_cap;

    u32             thread_stop_frame;
    CandoValue      thread_results[CANDO_MAX_THROW_ARGS];
    u32             thread_result_count;

    CandoValue      string_proto;
    CandoValue      array_proto;

    CandoModuleEntry *module_cache;
    u32               module_cache_count;
    u32               module_cache_cap;

    CandoThreadRegistry *thread_registry;
    CandoThreadRegistry *thread_registry_owned;
};
```

## Call frames

```c
typedef struct CandoCallFrame {
    CandoClosure *closure;
    u8           *ip;
    CandoValue   *slots;      // base of frame's window in vm->stack
    u32           ret_count;
    bool          is_fluent;   // return receiver instead of result
} CandoCallFrame;
```

`slots` points into `vm->stack`. Local 0 is `slots[0]`, local 1 is
`slots[1]`, and so on. Operands live above the locals.

## Closures

```c
typedef struct CandoClosure {
    CandoChunk    *chunk;
    CandoUpvalue **upvalues;
    u32            upvalue_count;
} CandoClosure;
```

Wraps a shared `CandoChunk*` (not owned by the closure) and an array of
captured upvalue pointers.

## Upvalues

```c
typedef struct CandoUpvalue {
    CandoLockHeader    lock;
    CandoValue        *location;
    CandoValue         closed;
    struct CandoUpvalue *next;
} CandoUpvalue;
```

- **Open**: `location` points into `vm->stack`.
- **Closed**: `OP_CLOSE_UPVAL` copies the value to `closed` and redirects
  `location` to `&closed`.
- `lock` provides thread-safe access when the upvalue is shared between
  parent and child threads.

## Try frames

```c
typedef struct CandoTryFrame {
    u8  *catch_ip;
    u8  *finally_ip;
    u32  stack_save;
    u32  frame_save;
    u32  loop_save;
} CandoTryFrame;
```

On error the VM restores `stack_top`, `frame_count`, and `loop_depth`
from the saved values, then jumps to `catch_ip`.

## Loop frames

```c
typedef struct CandoLoopFrame {
    u8  *break_ip;
    u8  *cont_ip;
    u32  stack_save;
    u8   loop_type;   // CANDO_LOOP_WHILE=0, CANDO_LOOP_FOR=1, CANDO_LOOP_FOR_OVER=2
} CandoLoopFrame;
```

`OP_BREAK`/`OP_CONTINUE` walk the loop stack A levels up and jump to
`break_ip` or `cont_ip`.

## Execution lifecycle

```c
void          cando_vm_init(CandoVM *vm, CandoMemCtrl *mem);
CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk);
CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue **results_out, u32 *count_out);
void          cando_vm_destroy(CandoVM *vm);
```

`cando_vm_exec` wraps the chunk in a `CandoClosure`, pushes `frames[0]`,
and enters the dispatch loop. Returns `VM_OK`, `VM_HALT`, or
`VM_RUNTIME_ERR`.

```c
typedef enum {
    VM_OK,
    VM_RUNTIME_ERR,
    VM_HALT,
    VM_EVAL_DONE,
} CandoVMResult;
```

## Dispatch loop

The VM uses GCC computed gotos (`&&label`, `goto *dispatch_table[op]`):

```c
static CandoVMResult vm_run(CandoVM *vm) {
    static void *dispatch_table[OP_COUNT] = { &&do_OP_CONST, ... };
    #define DISPATCH() goto *dispatch_table[(CandoOpcode)(*ip++)]

    DISPATCH();

    do_OP_CONST: { ... DISPATCH(); }
    do_OP_ADD:   { ... DISPATCH(); }
    ...
}
```

Each instruction is one indirect jump with no bounds check.

## Instruction encoding

Every instruction is one byte (`CandoOpcode`). Operands follow as
little-endian `u16` values:

| Format | Bytes | Description |
|---|---|---|
| `OPFMT_NONE` | 1 | opcode only |
| `OPFMT_A` | 3 | opcode + u16 A |
| `OPFMT_A_B` | 5 | opcode + u16 A + u16 B |

```c
CandoOpFmt cando_opcode_fmt(CandoOpcode op);
u32        cando_opcode_size(CandoOpcode op);   // 1, 3, or 5
u16        cando_read_u16(const u8 *ip);
i16        cando_read_i16(const u8 *ip);
void       cando_write_u16(u8 *ip, u16 val);
```

## Opcode reference

### Band 0 — Literals

| Opcode | Operand | Effect |
|---|---|---|
| `OP_CONST` | A | push `constants[A]` |
| `OP_NULL` | — | push null |
| `OP_TRUE` | — | push true |
| `OP_FALSE` | — | push false |

### Band 1 — Stack

| Opcode | Operand | Effect |
|---|---|---|
| `OP_POP` | — | discard top |
| `OP_POP_N` | A | discard top A values |
| `OP_DUP` | — | duplicate top |

### Band 2 — Locals

| Opcode | Operand | Effect |
|---|---|---|
| `OP_LOAD_LOCAL` | A | push `slots[A]` |
| `OP_STORE_LOCAL` | A | `slots[A] = peek` (no pop) |
| `OP_DEF_LOCAL` | A | pop → `slots[A]` |
| `OP_DEF_CONST_LOCAL` | A | pop → const `slots[A]` |

### Band 3 — Globals

| Opcode | Operand | Effect |
|---|---|---|
| `OP_LOAD_GLOBAL` | A | push global named by `constants[A]` |
| `OP_STORE_GLOBAL` | A | `global[constants[A]] = peek` |
| `OP_DEF_GLOBAL` | A | pop → new global `constants[A]` |
| `OP_DEF_CONST_GLOBAL` | A | pop → write-protected global |

### Band 4 — Upvalues

| Opcode | Operand | Effect |
|---|---|---|
| `OP_LOAD_UPVAL` | A | push `upvalues[A]` |
| `OP_STORE_UPVAL` | A | `upvalues[A] = peek` |
| `OP_CLOSE_UPVAL` | A | close upvalue at stack slot A to heap |

### Band 5 — Arithmetic

`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`, `OP_POW` — pop two,
push result. `OP_NEG`, `OP_POS` — pop one, push result. `OP_INCR`,
`OP_DECR` — modify top in place.

### Band 6 — Comparison

`OP_EQ`, `OP_NEQ`, `OP_LT`, `OP_GT`, `OP_LEQ`, `OP_GEQ` — pop two,
push bool.

Multi-value comparison (`_STACK` variants, operand A = count): pop A
right-hand values, pop one left operand, push bool. `EQ_STACK`/`NEQ_STACK`
use any/none semantics; ordering variants require the relation against all.

`OP_RANGE_CHECK` — pop max, val, min; A encodes inclusive flags.

### Band 7 — Bitwise

`OP_BIT_AND`, `OP_BIT_OR`, `OP_BIT_XOR`, `OP_LSHIFT`, `OP_RSHIFT` — pop
two, push result. `OP_BIT_NOT` — pop one, push result.

### Band 8 — Logical

`OP_NOT` — pop, push `!v`. `OP_AND_JUMP` / `OP_OR_JUMP` — short-circuit:
peek top; if falsy/truthy jump forward A bytes without popping.

### Band 9 — Objects

| Opcode | Operand | Effect |
|---|---|---|
| `OP_NEW_OBJECT` | — | push new empty object |
| `OP_NEW_ARRAY` | A | pop A values, push array |
| `OP_GET_FIELD` | A | obj=pop; push `obj[constants[A]]` |
| `OP_SET_FIELD` | A | val=pop, obj=peek; `obj[constants[A]] = val` |
| `OP_GET_INDEX` | — | idx=pop, obj=pop; push `obj[idx]` |
| `OP_SET_INDEX` | — | val=pop, idx=pop, obj=peek; `obj[idx]=val` |
| `OP_LEN` | — | pop; push length |
| `OP_KEYS_OF` | — | pop; push stack of keys (FOR IN) |
| `OP_VALS_OF` | — | pop; push stack of values (FOR OF) |

### Band 10 — Control flow

| Opcode | Operand | Effect |
|---|---|---|
| `OP_JUMP` | A | unconditional; A = signed i16 offset |
| `OP_JUMP_IF_FALSE` | A | pop; jump if falsy |
| `OP_JUMP_IF_TRUE` | A | pop; jump if truthy |
| `OP_LOOP` | A | unconditional backward; A = unsigned back bytes |
| `OP_BREAK` | A | break from loop depth A (0 = innermost) |
| `OP_CONTINUE` | A | continue at loop depth A |
| `OP_LOOP_MARK` | A, B | record break/continue targets; B packs cont offset + loop type |
| `OP_LOOP_END` | — | pop one loop frame |

### Band 11 — Functions

| Opcode | Operand | Effect |
|---|---|---|
| `OP_CLOSURE` | A | build closure from chunk `constants[A]` |
| `OP_CALL` | A | call TOS with A args |
| `OP_METHOD_CALL` | A, B | `obj:constants[A](...)` with B args |
| `OP_FLUENT_CALL` | A, B | same but returns `obj` |
| `OP_RETURN` | A | return top A values |
| `OP_TAIL_CALL` | A | tail-recursive call with A args |

### Band 12 — Varargs

`OP_LOAD_VARARG` (A) — push vararg slot A (`UINT16_MAX` = push all).
`OP_VARARG_LEN` — push count. `OP_UNPACK` — pop array/object, push all
values.

### Band 13 — Iteration

`OP_RANGE_ASC` / `OP_RANGE_DESC` — pop two, push ascending/descending
range. `OP_FOR_INIT` (A) / `OP_FOR_NEXT` (A) — for-each init and advance.
`OP_FOR_OVER_INIT` / `OP_FOR_OVER_NEXT` (A) — generic iterator (Lua-style
triplet). `OP_PIPE_INIT` (A) / `OP_PIPE_NEXT` (A) / `OP_FILTER_NEXT` (A)
/ `OP_PIPE_END` / `OP_PIPE_COLLECT` / `OP_FILTER_COLLECT` — pipe (`~>`)
and filter (`~!>`) operators.

### Band 14 — Error handling

| Opcode | Operand | Effect |
|---|---|---|
| `OP_TRY_BEGIN` | A | push try frame; A = offset to catch |
| `OP_TRY_END` | — | normal exit from try |
| `OP_CATCH_BEGIN` | A | push A values from `error_vals[]` |
| `OP_FINALLY_BEGIN` | A | A = offset past finally block |
| `OP_THROW` | A | pop A values, store in `error_vals`, throw |
| `OP_RERAISE` | — | re-throw current exception |

### Band 15 — Threads

`OP_THREAD` — pop closure, spawn OS thread, push thread handle.
`OP_AWAIT` — pop thread handle, block until done, push return values.
`OP_ASYNC` / `OP_YIELD` — reserved, unused.

### Band 16 — Classes

`OP_NEW_CLASS` (A) — create class object named `constants[A]`.
`OP_BIND_METHOD` (A) — bind method `constants[A]` to class on TOS.
`OP_INHERIT` — set `__index` on child to parent.

### Band 17 — Masks

`OP_MASK_PASS` — keep top (no-op). `OP_MASK_SKIP` — pop and discard top.
`OP_MASK_APPLY` (A, B) — apply bitmask B to top A values.

### Band 18 — Spreading

`OP_SPREAD_RET` — adjust `spread_extra` by `last_ret_count - 1`.
`OP_ARRAY_SPREAD` — adjust `array_extra` by `last_ret_count - 1`.

### Band 19 — Call-result comparison

`OP_TRUNCATE_RET` — keep only the first return value. `OP_EQ_SPREAD`,
`OP_NEQ_SPREAD`, `OP_LT_SPREAD`, `OP_GT_SPREAD`, `OP_LEQ_SPREAD`,
`OP_GEQ_SPREAD` — compare left operand against all return values from
the last call.

### Sentinels

`OP_NOP` — no operation. `OP_HALT` — stop VM (top-level script end).

## Call convention

For `OP_CALL A`: stack before is `[... callee, arg0, ..., argA-1]`. A new
`CandoCallFrame` is pushed with `slots = &callee`. On `OP_RETURN N`, the
top N values replace the callee slot range. `vm->last_ret_count` is set
to N.

## Multi-return spreading

`OP_SPREAD_RET` adjusts `spread_extra` by `last_ret_count - 1` to account
for functions returning more values than the single slot the call
expression normally occupies. All variable-defining opcodes
(`OP_DEF_LOCAL`, `OP_DEF_CONST_LOCAL`, `OP_DEF_GLOBAL`,
`OP_DEF_CONST_GLOBAL`) reset `spread_extra` to 0 after consuming it.

## Try/catch unwinding

On error, the VM restores `stack_top`, `frame_count`, and `loop_depth`
from `CandoTryFrame`, jumps to `catch_ip`. `OP_CATCH_BEGIN` pushes bound
values from `error_vals[]`.

Multi-argument throw/catch: `OP_THROW A` pops A values into
`error_vals[0..A-1]`. Catch binds up to its parameter count; excess values
are dropped, missing ones arrive as null.

## Eval re-entrancy

`cando_vm_exec_eval` sets `eval_stop_frame = frame_count`. When
`OP_RETURN` decrements `frame_count` to match, the VM captures results
and returns `VM_EVAL_DONE` instead of continuing.

## Stack helpers

```c
void       cando_vm_push(CandoVM *vm, CandoValue val);
CandoValue cando_vm_pop(CandoVM *vm);
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist);  // 0 = top
u32        cando_vm_stack_depth(const CandoVM *vm);
```

See [value-types.md](value-types.md) for `CandoValue`/`CdoValue`.
See [object-system.md](object-system.md) for how the VM uses objects.
