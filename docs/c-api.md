# C API Reference

Reference for every symbol exported by `cando.h` and its transitively
included headers.  For task-oriented examples see
[embedding.md](embedding.md) and [writing-extensions.md](writing-extensions.md).

All public functions are declared `CANDO_API`, which expands to the
right visibility attribute for the platform.  Every function listed
here is safe to call from the thread that owns the VM; see
[threading.md](threading.md) for the rules around child VMs.

## Version

```c
#define CANDO_VERSION_MAJOR  1
#define CANDO_VERSION_MINOR  0
#define CANDO_VERSION_PATCH  0
#define CANDO_VERSION        "1.0.0"
#define CANDO_VERSION_NUM    10000        /* major*10000 + minor*100 + patch */

const char *cando_version(void);          /* returns CANDO_VERSION      */
int         cando_version_num(void);      /* returns CANDO_VERSION_NUM  */
```

## Error codes

```c
#define CANDO_OK           0
#define CANDO_ERR_FILE     1
#define CANDO_ERR_PARSE    2
#define CANDO_ERR_RUNTIME  3
```

Returned by `cando_dofile`, `cando_dostring`, and `cando_loadstring`.

## VM lifecycle

### `CandoVM *cando_open(void);`

Create and initialise a new VM.  Registers the three core natives
(`print`, `type`, `toString`) but no standard libraries.  Returns
`NULL` only if the allocation fails.  Each call must be balanced by
exactly one `cando_close`.

### `void cando_close(CandoVM *vm);`

Wait for all spawned threads, free all VM-owned resources, and —
if this was the last live VM — destroy the global string intern
table and meta-key cache.  Passing `NULL` is a no-op.

## Standard library registration

```c
void cando_openlibs(CandoVM *vm);                 /* all below             */
void cando_open_mathlib(CandoVM *vm);             /* math.*                */
void cando_open_filelib(CandoVM *vm);             /* file.*                */
void cando_open_stringlib(CandoVM *vm);           /* string prototype      */
void cando_open_arraylib(CandoVM *vm);            /* array prototype       */
void cando_open_objectlib(CandoVM *vm);           /* object.*              */
void cando_open_jsonlib(CandoVM *vm);             /* json.*                */
void cando_open_csvlib(CandoVM *vm);              /* csv.*                 */
void cando_open_threadlib(CandoVM *vm);           /* thread.*              */
void cando_open_oslib(CandoVM *vm);               /* os.*                  */
void cando_open_datetimelib(CandoVM *vm);         /* datetime.*            */
void cando_open_cryptolib(CandoVM *vm);           /* crypto.*              */
void cando_open_processlib(CandoVM *vm);          /* process.*             */
void cando_open_netlib(CandoVM *vm);              /* net.*                 */
void cando_open_evallib(CandoVM *vm);             /* eval()                */
void cando_open_includelib(CandoVM *vm);          /* include()             */
void cando_open_httplib(CandoVM *vm);             /* http.* + fetch()      */
void cando_open_httpslib(CandoVM *vm);            /* https.*               */
```

Each opener is idempotent.  `cando_openlibs` registers the libraries
in the order that satisfies internal dependencies — for example,
`http/https` is registered after `json` so that `response:json()` can
resolve `json.parse`.

## Load and execute

### `int cando_dofile(CandoVM *vm, const char *path);`

Read, compile, and execute a `.cdo` script.  The path is canonicalised
so that `include()` calls inside the script resolve relative to the
script's directory.

Returns `CANDO_OK`, `CANDO_ERR_FILE`, `CANDO_ERR_PARSE`, or
`CANDO_ERR_RUNTIME`.

### `int cando_dostring(CandoVM *vm, const char *src, const char *name);`

Compile and execute a NUL-terminated source string.  `name` appears
in error messages; `NULL` becomes `"<string>"`.

Returns `CANDO_OK`, `CANDO_ERR_PARSE`, or `CANDO_ERR_RUNTIME`.

### `int cando_loadstring(CandoVM *vm, const char *src, const char *name, CandoChunk **chunk_out);`

Compile without executing.  On success `*chunk_out` receives the
compiled `CandoChunk*`; the caller is responsible for freeing it with
`cando_chunk_free`.  Run the chunk with `cando_vm_exec`.

### `const char *cando_errmsg(const CandoVM *vm);`

Return the most recent error message.  Never returns `NULL`; returns
`""` when no error is set.  The string is valid until the next VM
call that overwrites it.

## Value types

From `core/value.h`:

```c
typedef enum {
    TYPE_NULL   = 0,
    TYPE_BOOL   = 1,
    TYPE_NUMBER = 2,
    TYPE_STRING = 3,
    TYPE_OBJECT = 4,
} TypeTag;

typedef u32 HandleIndex;
#define CANDO_INVALID_HANDLE ((HandleIndex)UINT32_MAX)

typedef struct CandoString {
    u32   ref_count;
    u32   length;
    u32   hash;
    char  data[];            /* NUL-terminated */
} CandoString;

typedef struct CandoValue {
    u8 tag;
    union {
        bool         boolean;
        f64          number;
        CandoString *string;
        HandleIndex  handle;
    } as;
} CandoValue;
```

### Constructors

```c
CandoValue cando_null(void);
CandoValue cando_bool(bool v);
CandoValue cando_number(f64 v);
CandoValue cando_string_value(CandoString *s);      /* takes ownership    */
CandoValue cando_object_value(HandleIndex h);
```

### Predicates

```c
bool cando_is_null(CandoValue v);
bool cando_is_bool(CandoValue v);
bool cando_is_number(CandoValue v);
bool cando_is_string(CandoValue v);
bool cando_is_object(CandoValue v);
```

### Operations

```c
const char *cando_value_type_name(TypeTag tag);
bool        cando_value_equal(CandoValue a, CandoValue b);
char       *cando_value_tostring(CandoValue v);     /* caller frees       */
CandoValue  cando_value_copy(CandoValue v);         /* retains strings    */
void        cando_value_release(CandoValue v);
```

### Strings

```c
CandoString *cando_string_new(const char *src, u32 length);
CandoString *cando_string_retain(CandoString *s);
void         cando_string_release(CandoString *s);
```

## VM stack and natives

### Native function type

```c
typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

- Arguments are in `args[0..argc-1]`.
- Push return values onto the stack with `cando_vm_push`.
- Return the number of values pushed, or `-1` for an error
  (call `cando_vm_error` first).

### Registration

```c
bool       cando_vm_register_native(CandoVM *vm, const char *name, CandoNativeFn fn);
CandoValue cando_vm_add_native     (CandoVM *vm, CandoNativeFn fn);
```

`cando_vm_register_native` exposes the function as a global named
`name`.  `cando_vm_add_native` just assigns it a sentinel value; use
this when attaching the function as a method on an object (as
`libutil_set_method` does).

Maximum of `CANDO_NATIVE_MAX` (128) registered natives per VM.

### Stack helpers

```c
void       cando_vm_push(CandoVM *vm, CandoValue val);
CandoValue cando_vm_pop(CandoVM *vm);
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist);     /* 0 = top    */
u32        cando_vm_stack_depth(const CandoVM *vm);
```

### Error reporting

```c
void cando_vm_error(CandoVM *vm, const char *fmt, ...);
```

Formats `fmt` into `vm->error_msg`, sets `vm->has_error`, and stages
the message as a catchable thrown value.  Your native function should
`return -1` immediately afterwards.

## Globals

```c
bool cando_vm_set_global(CandoVM *vm, const char *name, CandoValue val, bool is_const);
bool cando_vm_get_global(const CandoVM *vm, const char *name, CandoValue *out);
```

`is_const = true` prevents reassignment from scripts (attempts raise a
runtime error).

## Calling scripts from C

### `int cando_vm_call_value(CandoVM *vm, CandoValue fn_val, CandoValue *args, u32 argc);`

Call an `OBJ_FUNCTION` value with `argc` arguments.  Return values end
up on top of `vm->stack`; the function's return count is the return
value.  Returns 0 if `fn_val` is not callable.

### `CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk);`

Execute a top-level chunk (usually from `cando_loadstring`).  Returns
`VM_OK`, `VM_HALT`, or `VM_RUNTIME_ERR`.

```c
typedef enum {
    VM_OK,
    VM_RUNTIME_ERR,
    VM_HALT,
    VM_EVAL_DONE,
} CandoVMResult;
```

### `CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk, CandoValue **results_out, u32 *count_out);`

Execute a chunk compiled in eval mode.  On `VM_EVAL_DONE` the expression's
value is written to `*results_out[0]` (or a multi-value array) and
`*count_out` is set.  Safe to call re-entrantly from a native function.

### `int cando_vm_register_native(CandoVM *vm, const char *name, CandoNativeFn fn);`

See *VM stack and natives* above.

## Bridge layer

From `vm/bridge.h`.  Use these when you need to manipulate objects
directly from C.

```c
CdoObject  *cando_bridge_resolve(CandoVM *vm, HandleIndex h);
CandoValue  cando_bridge_new_object(CandoVM *vm);
CandoValue  cando_bridge_new_array(CandoVM *vm);
CdoString  *cando_bridge_intern_key(CandoString *cs);
CandoValue  cando_bridge_to_cando(CandoVM *vm, CdoValue v);
CdoValue    cando_bridge_to_cdo  (CandoVM *vm, CandoValue v);
```

`cando_bridge_new_object` allocates a `CdoObject`, adds it to the VM's
handle table, and returns a `CandoValue` of type `TYPE_OBJECT`.  Use
`cando_bridge_resolve` to get the `CdoObject *` for manipulation.

## Chunks

From `vm/chunk.h`.  Used by `cando_loadstring` and the parser.

```c
CandoChunk *cando_chunk_new(const char *name, u32 arity, bool has_vararg);
void        cando_chunk_free(CandoChunk *chunk);

void cando_chunk_emit_byte(CandoChunk *c, u8 byte, u32 line);
void cando_chunk_emit_op  (CandoChunk *c, CandoOpcode op, u32 line);
void cando_chunk_emit_op_a (CandoChunk *c, CandoOpcode op, u16 a, u32 line);
void cando_chunk_emit_op_ab(CandoChunk *c, CandoOpcode op, u16 a, u16 b, u32 line);

u32  cando_chunk_emit_jump     (CandoChunk *c, CandoOpcode op, u32 line);
void cando_chunk_patch_jump    (CandoChunk *c, u32 patch_offset);
void cando_chunk_patch_jump_to (CandoChunk *c, u32 patch_offset, u32 target);
void cando_chunk_emit_loop     (CandoChunk *c, u32 loop_start, u32 line);

u16  cando_chunk_add_const        (CandoChunk *c, CandoValue val);
u16  cando_chunk_add_string_const (CandoChunk *c, const char *str, u32 len);
```

## Disassembly

From `vm/debug.h`.  Used by the `cando … --disasm` CLI.

```c
void cando_chunk_disasm(CandoChunk *chunk, FILE *out);
```

## Limits

From `vm/vm.h`:

```c
#define CANDO_STACK_MAX        2048     /* value stack                    */
#define CANDO_FRAMES_MAX       256      /* call depth                     */
#define CANDO_TRY_MAX          64       /* try/catch nesting              */
#define CANDO_LOOP_MAX         64       /* loop nesting                   */
#define CANDO_NATIVE_MAX       128      /* registered native functions    */
#define CANDO_MAX_THROW_ARGS   32       /* values per THROW               */
#define CANDO_PROTO_DEPTH_MAX  32       /* prototype chain depth          */
```

## Thread utilities

```c
struct CdoThread *cando_current_thread(void);     /* NULL on main thread  */
void              cando_vm_wait_all_threads(CandoVM *vm);
```

`cando_current_thread` is implemented via thread-local storage; it
returns the `OBJ_THREAD` handle for the thread this native call is
running on, or `NULL` when called from the main thread.

`cando_vm_wait_all_threads` blocks until every thread that the VM has
spawned has finished.  `cando_close` calls this internally — you only
need it if you want to join before shutdown.

## Child VMs (thread internals)

```c
void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem);
void cando_vm_init_child(CandoVM *child, const CandoVM *parent);
void cando_vm_destroy(CandoVM *vm);
```

Host code rarely calls these directly.  They are used by the thread
trampoline to set up a spawned thread's VM sharing the parent's
handles and globals.
