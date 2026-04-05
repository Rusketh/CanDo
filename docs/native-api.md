# Native Function API

## Signature

```c
typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

| Parameter | Description |
|---|---|
| `vm` | Pointer to the running VM.  Use for stack helpers, error reporting, and bridge calls. |
| `argc` | Number of arguments passed by the caller. |
| `args` | Base pointer of the argument array on the VM stack.  `args[0]` is the first argument. |

**Return value**: the number of values pushed onto the stack as return values,
or `-1` on error.

---

## Return convention

1. Push each return value onto the stack using `cando_vm_push(vm, val)`.
2. Return the count of values pushed.

```c
// Return a single number
static int my_fn(CandoVM *vm, int argc, CandoValue *args)
{
    cando_vm_push(vm, cando_number(42.0));
    return 1;
}
```

Returning `0` is valid — the VM will normalise the call site by pushing a
`null` so `OP_POP` does not underflow.  You do NOT need to push a null
yourself when returning 0.

---

## Multiple return values

Push each value separately; return the total count:

```c
static int fn_two_vals(CandoVM *vm, int argc, CandoValue *args)
{
    cando_vm_push(vm, cando_bool(true));
    cando_vm_push(vm, cando_number(3.14));
    return 2;
}
```

In a script, spread-receive the results:

```cando
VAR ok, pi = fn_two_vals();
```

---

## Error reporting

Call `cando_vm_error()`, then return `-1`.  This sets `vm->has_error`,
formats `vm->error_msg`, and populates `vm->error_vals[0]` so the error
is catchable by a `TRY/CATCH` block:

```c
static int my_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_number(args[0])) {
        cando_vm_error(vm, "my_fn: expected a number argument");
        return -1;
    }
    // ...
}
```

**Do not** set `vm->has_error` or `vm->error_msg` directly — doing so
leaves `vm->error_vals[]` unpopulated and makes the error un-catchable.

The VM will unwind to the nearest `TRY/CATCH` block, passing the error
message string as the first caught value.  If no handler exists, execution
terminates with a runtime error.

### Catching native errors in scripts

Because `cando_vm_error()` stores the message in `error_vals[0]`, a single
catch parameter receives the full error string:

```cando
TRY {
    file.read(NULL);
} CATCH (msg) {
    print("caught: " + msg);  /* "file.read: path must be a string" */
}
```

---

## Type checking helpers

```c
bool cando_is_null(CandoValue v)
bool cando_is_bool(CandoValue v)
bool cando_is_number(CandoValue v)
bool cando_is_string(CandoValue v)
bool cando_is_object(CandoValue v)
```

Check `argc` first, then the tag of each argument.  Do not access `args[i]`
when `i >= argc`.

---

## Stack helpers

```c
void       cando_vm_push(CandoVM *vm, CandoValue val);
CandoValue cando_vm_pop(CandoVM *vm);
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist); // 0 = top
u32        cando_vm_stack_depth(const CandoVM *vm);
```

Most natives only need `cando_vm_push`.  Avoid `pop` / `peek` inside a native
unless you are implementing something unusual — the `args` pointer already
gives direct access to the argument values.

---

## Working with objects

Objects are `TYPE_OBJECT` values containing a `HandleIndex`.  Use bridge
functions to work with them:

```c
// Resolve the object pointer
CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);

// Read a field (uses prototype chain)
CdoString *key = cdo_string_intern("myField", 7);
CdoValue   raw;
if (cdo_object_get(obj, key, &raw)) {
    CandoValue v = cando_bridge_to_cando(vm, raw);
    // use v ...
}

// Write a field
CdoValue val = cando_bridge_to_cdo(vm, some_cando_value);
cdo_object_rawset(obj, key, val, FIELD_NONE);
```

### Creating a new object to return

```c
CandoValue obj_val = cando_bridge_new_object(vm);
CdoObject *obj = cando_bridge_resolve(vm, obj_val.as.handle);
CdoString *k = cdo_string_intern("x", 1);
cdo_object_rawset(obj, k, cdo_number(1.0), FIELD_NONE);
cando_vm_push(vm, obj_val);
return 1;
```

### Creating a new array to return

```c
CandoValue arr_val = cando_bridge_new_array(vm);
CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);
cdo_array_push(arr, cdo_number(1.0));
cdo_array_push(arr, cdo_number(2.0));
cando_vm_push(vm, arr_val);
return 1;
```

---

## Working with strings

```c
// Read a string argument
if (cando_is_string(args[0])) {
    CandoString *s = args[0].as.string;
    // s->data is a NUL-terminated char*
    // s->length is the byte count (excluding NUL)
}

// Push a string result
CandoString *result = cando_string_new("hello", 5);
cando_vm_push(vm, cando_string_value(result));
return 1;
// Note: cando_string_value() takes ownership; you no longer need to release result
```

---

## Registering a native function

```c
// Expose as a global variable (most common)
cando_vm_register_native(vm, "myFunc", my_fn);

// Register without exposing as global (returns the sentinel value)
CandoValue sentinel = cando_vm_add_native(vm, my_fn);
```

`cando_vm_register_native` assigns the next sentinel (`-(count+1).0`), stores
the function in `vm->native_fns[]`, and calls `cando_vm_set_global(vm, name, sentinel, true)`.

The VM supports at most `CANDO_NATIVE_MAX = 64` registered native functions.

---

## Calling eval from a native

```c
#include "../parser/parser.h"
#include "../vm/vm.h"

static int native_eval(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *src = args[0].as.string;

    CandoChunk *chunk = cando_chunk_new("<eval>", 0, false);
    CandoParser parser;
    cando_parser_init(&parser, src->data, src->length, chunk);
    parser.eval_mode = true;

    if (!cando_parse(&parser)) {
        cando_vm_error(vm, "eval parse error: %s",
                       cando_parser_error(&parser));
        cando_chunk_free(chunk);
        return -1;
    }

    CandoValue *results = NULL;
    u32         count = 0;
    CandoVMResult r = cando_vm_exec_eval(vm, chunk, &results, &count);
    cando_chunk_free(chunk);

    if (r == VM_RUNTIME_ERR) return -1;

    if (count == 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    for (u32 i = 0; i < count; i++) {
        cando_vm_push(vm, results[i]);
    }
    cando_free(results);
    return (int)count;
}
```

See `source/lib/eval.c` for the full implementation.
