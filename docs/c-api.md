# CanDo C API Reference

Complete reference for `include/cando.h` and the supporting headers it
re-exports.  Compile with `-Iinclude` and link with `-lcando`.

---

## Table of Contents

1. [Version macros](#version-macros)
2. [Error codes](#error-codes)
3. [High-level API (`cando.h`)](#high-level-api)
   - [State lifecycle](#state-lifecycle)
   - [Load and execute](#load-and-execute)
   - [Error inspection](#error-inspection)
   - [Standard library openers](#standard-library-openers)
   - [Version queries](#version-queries)
4. [VM API (`vm/vm.h`)](#vm-api)
   - [Types](#types)
   - [VM lifecycle](#vm-lifecycle)
   - [Execution](#execution)
   - [Stack helpers](#stack-helpers)
   - [Native function API](#native-function-api)
   - [Global variable API](#global-variable-api)
   - [Threading helpers](#threading-helpers)
5. [Value API (`core/value.h`)](#value-api)
6. [Bridge API (`vm/bridge.h`)](#bridge-api)
7. [Chunk API (`vm/chunk.h`)](#chunk-api)

---

## Version macros

Defined in `cando.h`.

| Macro | Value | Description |
|---|---|---|
| `CANDO_VERSION_MAJOR` | `1` | Major version number |
| `CANDO_VERSION_MINOR` | `0` | Minor version number |
| `CANDO_VERSION_PATCH` | `0` | Patch version number |
| `CANDO_VERSION` | `"1.0.0"` | Version string |
| `CANDO_VERSION_NUM` | `10000` | Numeric: `major*10000 + minor*100 + patch` |

Use `CANDO_VERSION_NUM` in `#if` guards:

```c
#if CANDO_VERSION_NUM >= 10000
  /* CanDo 1.0.0 or later */
#endif
```

---

## Error codes

| Constant | Value | Meaning |
|---|---|---|
| `CANDO_OK` | 0 | Success |
| `CANDO_ERR_FILE` | 1 | File could not be opened or read |
| `CANDO_ERR_PARSE` | 2 | Syntax or compilation error |
| `CANDO_ERR_RUNTIME` | 3 | Unhandled runtime error |

---

## High-level API

Declared in `include/cando.h`.  These functions are the primary interface for
embedders.

### State lifecycle

```c
CandoVM *cando_open(void);
```
Allocate and initialise a new CanDo VM.  Registers core native functions
(`print`, `type`, `toString`).  Manages a process-global reference counter
so the first call initialises the shared string intern table and the last
`cando_close` tears it down.

Returns a pointer to the new VM, or `NULL` on allocation failure.  Every
call must be paired with exactly one `cando_close`.

---

```c
void cando_close(CandoVM *vm);
```
Destroy a VM.  Waits for any threads spawned by the VM to finish, releases
all VM-owned memory, and decrements the global reference count.

The pointer is invalid after this call.

---

### Load and execute

```c
int cando_dofile(CandoVM *vm, const char *path);
```
Read the file at `path`, compile it, and execute it.

The script's canonical path (via `realpath` / `_fullpath`) is used as the
base for relative `include()` calls inside the script.

Returns `CANDO_OK` or one of `CANDO_ERR_FILE`, `CANDO_ERR_PARSE`,
`CANDO_ERR_RUNTIME`.

---

```c
int cando_dostring(CandoVM *vm, const char *src, const char *name);
```
Compile and execute the NUL-terminated source string `src`.  `name` is used
in error messages (pass `NULL` to default to `"<string>"`).

Returns `CANDO_OK`, `CANDO_ERR_PARSE`, or `CANDO_ERR_RUNTIME`.

---

```c
int cando_loadstring(CandoVM *vm, const char *src, const char *name,
                     CandoChunk **chunk_out);
```
Compile `src` without executing it.  On success stores the compiled
`CandoChunk*` in `*chunk_out` and returns `CANDO_OK`.  The caller owns the
chunk and must free it with `cando_chunk_free`.

On failure returns `CANDO_ERR_PARSE` and sets `*chunk_out = NULL`.

Use `cando_vm_exec(vm, chunk)` to run the compiled chunk.

---

### Error inspection

```c
const char *cando_errmsg(const CandoVM *vm);
```
Return the most recent error message.  Valid after any load/exec call that
returned non-zero.  The string is owned by the VM; it is valid until the
next call that modifies `vm->error_msg`.  Never returns `NULL` (returns `""`
when no error is set).

---

### Standard library openers

```c
void cando_openlibs(CandoVM *vm);
```
Open all standard library modules.  Equivalent to calling each individual
opener below.

Individual openers (call any subset for sandboxing):

| Function | Globals exposed |
|---|---|
| `cando_open_mathlib(vm)` | `math.*` |
| `cando_open_filelib(vm)` | `file.*` |
| `cando_open_stringlib(vm)` | String prototype methods |
| `cando_open_arraylib(vm)` | Array prototype methods |
| `cando_open_objectlib(vm)` | `object.*` |
| `cando_open_jsonlib(vm)` | `json.*` |
| `cando_open_csvlib(vm)` | `csv.*` |
| `cando_open_threadlib(vm)` | `thread.*` |
| `cando_open_oslib(vm)` | `os.*` |
| `cando_open_datetimelib(vm)` | `datetime.*` |
| `cando_open_cryptolib(vm)` | `crypto.*` |
| `cando_open_processlib(vm)` | `process.*` |
| `cando_open_netlib(vm)` | `net.*` |
| `cando_open_evallib(vm)` | `eval()` |
| `cando_open_includelib(vm)` | `include()` |

---

### Version queries

```c
const char *cando_version(void);   /* returns e.g. "1.0.0" */
int         cando_version_num(void); /* returns CANDO_VERSION_NUM */
```

---

## VM API

Declared in `source/vm/vm.h` (included transitively by `cando.h`).

### Types

```c
typedef struct CandoVM CandoVM;        /* interpreter state */

typedef enum {
    VM_OK,           /* execution completed normally */
    VM_RUNTIME_ERR,  /* unhandled runtime error */
    VM_HALT,         /* OP_HALT reached */
    VM_EVAL_DONE,    /* eval chunk returned (internal) */
} CandoVMResult;

typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

### VM lifecycle

```c
void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem);
```
Initialise all fields of an already-allocated `CandoVM`.  `mem` may be
`NULL` (uses default allocator).  Prefer `cando_open()` for normal use.

---

```c
void cando_vm_init_child(CandoVM *child, const CandoVM *parent);
```
Initialise a child VM for a spawned thread.  The child shares the parent's
handle table and global environment (no new heap allocation).  Native
functions are copied by value.

---

```c
void cando_vm_destroy(CandoVM *vm);
```
Release all VM-owned resources.  Does not free the `CandoVM*` itself.

---

```c
CandoClosure *cando_closure_new(CandoChunk *chunk);
void          cando_closure_free(CandoClosure *closure);
```
Manually create/free a closure around a chunk.  Used internally; prefer
`cando_vm_exec` which handles this automatically.

---

### Execution

```c
CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk);
```
Execute a top-level chunk.  Wraps it in a closure, pushes the first call
frame, and runs the dispatch loop until `OP_HALT`, `OP_RETURN` at frame 0,
or an unhandled error.

Returns `VM_OK`, `VM_HALT`, or `VM_RUNTIME_ERR`.

---

```c
CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue **results_out, u32 *count_out);
```
Execute a chunk compiled with `eval_mode = true`.  Re-entrant: safe to call
from inside a native function.  On success, `*results_out` points to an
array of `*count_out` result values.

---

```c
int cando_vm_call_value(CandoVM *vm, CandoValue fn_val,
                        CandoValue *args, u32 argc);
```
Call a CanDo function value with `argc` arguments from `args[]`.  Return
values are pushed onto `vm->stack`; the return count is returned.  Returns
`0` if `fn_val` is not callable.  Safe to call from inside a native function.

---

### Stack helpers

```c
void       cando_vm_push(CandoVM *vm, CandoValue val);
CandoValue cando_vm_pop(CandoVM *vm);
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist);  /* 0 = top */
u32        cando_vm_stack_depth(const CandoVM *vm);
```

---

### Native function API

```c
bool cando_vm_register_native(CandoVM *vm, const char *name, CandoNativeFn fn);
```
Register a C function as a named global callable in the VM.  Returns `false`
if `CANDO_NATIVE_MAX` (128) is exceeded.

---

```c
CandoValue cando_vm_add_native(CandoVM *vm, CandoNativeFn fn);
```
Register a native without exposing it as a global; returns the sentinel
`CandoValue` representing the function.  Returns `cando_null()` on overflow.

---

```c
void cando_vm_error(CandoVM *vm, const char *fmt, ...);
```
Report an error from a native function.  Sets `vm->has_error`, formats
`vm->error_msg`, and populates `vm->error_vals[0]` so the error is catchable
by a `TRY/CATCH` block.  After calling this, return `-1` from your native.

---

### Global variable API

```c
bool cando_vm_set_global(CandoVM *vm, const char *name, CandoValue val,
                         bool is_const);
```
Define or overwrite a global variable.  If `is_const` is `true`, the
variable is write-protected for scripts (but can still be overwritten from C).

---

```c
bool cando_vm_get_global(const CandoVM *vm, const char *name, CandoValue *out);
```
Look up a global by name.  Stores the value in `*out` and returns `true` on
success; returns `false` (without modifying `*out`) if not found.

---

### Threading helpers

```c
void cando_vm_wait_all_threads(CandoVM *vm);
```
Block until all threads spawned from this VM have finished.  Call after
`cando_vm_exec` returns if the script may have spawned threads.

---

```c
struct CdoThread *cando_current_thread(void);
```
Return the `CdoThread*` for the currently executing CanDo thread, or `NULL`
on the main thread.  Implemented via thread-local storage.

---

## Value API

Declared in `source/core/value.h` (included by `cando.h`).

### Type tags

```c
typedef enum {
    TYPE_NULL,
    TYPE_NUMBER,   /* double-precision float */
    TYPE_BOOL,
    TYPE_STRING,   /* pointer to CandoString (ref-counted) */
    TYPE_OBJECT,   /* handle into the handle table → CdoObject* */
} TypeTag;
```

### CandoValue struct

```c
typedef struct CandoValue {
    TypeTag type;
    union {
        double       number;
        bool         boolean;
        CandoString *string;
        HandleIndex  handle;   /* for TYPE_OBJECT */
    } as;
} CandoValue;
```

### Constructors

```c
CandoValue cando_null(void);
CandoValue cando_number(double n);
CandoValue cando_bool(bool b);
CandoValue cando_string_new(const char *data);   /* heap-allocated, retained */
```

### Type predicates

```c
bool cando_is_null(CandoValue v);
bool cando_is_number(CandoValue v);
bool cando_is_bool(CandoValue v);
bool cando_is_string(CandoValue v);
bool cando_is_object(CandoValue v);
```

### Accessors

```c
/* Convenient aliases — undefined behaviour if type is wrong */
double       cando_as_number(CandoValue v);   /* v.as.number   */
bool         cando_as_bool(CandoValue v);     /* v.as.boolean  */
CandoString *cando_as_string(CandoValue v);   /* v.as.string   */
```

### Reference counting

```c
CandoValue cando_value_copy(CandoValue v);    /* retain string/object; returns v */
void       cando_value_release(CandoValue v); /* release string/object           */
```

### Utilities

```c
const char *cando_value_type_name(CandoValue v);         /* "null", "number", etc. */
bool        cando_value_equal(CandoValue a, CandoValue b); /* structural equality */
CandoString *cando_value_tostring(CandoValue v);           /* format as string    */
```

---

## Bridge API

Declared in `source/vm/bridge.h` (included by `cando.h`).

The bridge converts between the VM value layer (`CandoValue`) and the object
layer (`CdoValue` / `CdoObject*`).

```c
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);
```
Resolve a `HandleIndex` (from `CandoValue.as.handle`) to a `CdoObject*`.
Asserts that the handle is valid and live.

---

```c
CandoValue cando_bridge_new_object(CandoVM *vm);
```
Allocate a plain `CdoObject`, register it in the VM's handle table, and
return a `CandoValue` of `TYPE_OBJECT`.

---

```c
CandoValue cando_bridge_new_array(CandoVM *vm);
```
Allocate a `CdoObject` array, register it, and return a `CandoValue`.

---

```c
CdoString *cando_bridge_intern_key(CandoString *cs);
```
Intern a `CandoString`'s data as a `CdoString` suitable for use as an
object field key.  Returns a retained `CdoString*`; caller must call
`cdo_string_release`.

---

```c
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue   cando_bridge_to_cdo(CandoVM *vm, CandoValue v);
```
Convert between the two value representations.  For object types a new
handle is allocated (to-cando) or the handle is resolved to a pointer
(to-cdo).

---

## Chunk API

Declared in `source/vm/chunk.h` (included by `cando.h`).

```c
CandoChunk *cando_chunk_new(const char *name, u32 arity, bool eval_mode);
void        cando_chunk_free(CandoChunk *chunk);
```
Allocate and free a compiled bytecode chunk.  Normally you use
`cando_loadstring` rather than creating chunks manually.

---

```c
void cando_chunk_disasm(const CandoChunk *chunk, FILE *out);
```
Print a human-readable disassembly of the chunk's bytecode to `out`
(typically `stderr`).  Also disassembles nested function chunks recursively.

Used by the `--disasm` CLI flag.
