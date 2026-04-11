# Embedding CanDo in a C Application

CanDo is designed to be embedded in host applications the same way Lua is:
compile the library, include one header, and your application can load and
execute CanDo scripts, expose C functions to scripts, and exchange data
across the language boundary.

---

## Table of Contents

1. [Minimal example](#minimal-example)
2. [Building and linking](#building-and-linking)
3. [VM lifecycle](#vm-lifecycle)
4. [Loading and executing scripts](#loading-and-executing-scripts)
5. [Error handling](#error-handling)
6. [Selecting standard libraries](#selecting-standard-libraries)
7. [Registering native functions](#registering-native-functions)
8. [Exchanging data](#exchanging-data)
9. [Calling CanDo functions from C](#calling-cando-functions-from-c)
10. [Creating objects and arrays from C](#creating-objects-and-arrays-from-c)
11. [Thread safety](#thread-safety)
12. [Multiple VMs in one process](#multiple-vms-in-one-process)

---

## Minimal example

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    if (cando_dofile(vm, "script.cdo") != CANDO_OK)
        fprintf(stderr, "error: %s\n", cando_errmsg(vm));

    cando_close(vm);
    return 0;
}
```

Compile and link:

```bash
# Shared library (recommended)
gcc -o myapp myapp.c -Iinclude -Lbuild -lcando -Wl,-rpath,build

# Static library (no runtime dependency)
gcc -o myapp myapp.c -Iinclude build/libcando.a -lm -ldl -lpthread
```

---

## Building and linking

### Using pkg-config (after `make install`)

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs cando)
```

### CMake project

```cmake
find_package(cando REQUIRED)
target_link_libraries(myapp PRIVATE cando::cando)
```

Or, if you include CanDo as a subdirectory:

```cmake
add_subdirectory(vendor/cando)
target_link_libraries(myapp PRIVATE libcando)
target_include_directories(myapp PRIVATE vendor/cando/include)
```

### Manual compile flags

```bash
# include path (single header)
-I/path/to/cando/include

# link flags (shared)
-L/path/to/cando/build -lcando -Wl,-rpath,/path/to/cando/build

# link flags (static — must also link cando's deps)
/path/to/cando/build/libcando.a -lm -ldl -lpthread
```

---

## VM lifecycle

```c
// Create a new VM instance
CandoVM *vm = cando_open();
if (!vm) { /* out of memory */ }

// ... use the VM ...

// Destroy the VM (also waits for any spawned threads to finish)
cando_close(vm);
```

**Important**: `cando_open` / `cando_close` are reference-counted. The first
`cando_open` call in the process initialises the global string intern table.
The last `cando_close` tears it down.  You may create as many VMs as you need;
each is fully independent.

```c
// Two independent VMs in the same process — perfectly valid
CandoVM *vm1 = cando_open();
CandoVM *vm2 = cando_open();
cando_openlibs(vm1);
cando_openlibs(vm2);
cando_dofile(vm1, "a.cdo");
cando_dofile(vm2, "b.cdo");
cando_close(vm1);
cando_close(vm2);
```

---

## Loading and executing scripts

### From a file

```c
int rc = cando_dofile(vm, "scripts/game.cdo");
if (rc == CANDO_ERR_FILE)    { /* file not found or unreadable */ }
if (rc == CANDO_ERR_PARSE)   { /* syntax error */ }
if (rc == CANDO_ERR_RUNTIME) { /* runtime error */ }
```

The script's directory is used as the base for relative `include()` calls
inside the script.

### From a string

```c
const char *src = "print('hello from C!')";
int rc = cando_dostring(vm, src, "my_snippet");  // name appears in errors
```

### Compile only (no execution)

```c
CandoChunk *chunk = NULL;
int rc = cando_loadstring(vm, src, "my_snippet", &chunk);
if (rc == CANDO_OK) {
    // Inspect, cache, or execute later:
    CandoVMResult result = cando_vm_exec(vm, chunk);
    cando_chunk_free(chunk);
}
```

Compiling once and executing many times avoids re-parsing:

```c
// Compile once at startup
CandoChunk *chunk = NULL;
cando_loadstring(vm, long_script, "config", &chunk);

// Execute many times (e.g., once per game frame)
for (int frame = 0; frame < 1000; frame++) {
    cando_vm_exec(vm, chunk);
}
cando_chunk_free(chunk);
```

---

## Error handling

All execution functions return `CANDO_OK` (0) on success:

| Constant | Value | Meaning |
|---|---|---|
| `CANDO_OK` | 0 | Success |
| `CANDO_ERR_FILE` | 1 | File could not be opened or read |
| `CANDO_ERR_PARSE` | 2 | Syntax or compilation error |
| `CANDO_ERR_RUNTIME` | 3 | Unhandled runtime error |

After a non-zero return, `cando_errmsg(vm)` returns the human-readable error
string including source file, line number, and description.

```c
if (cando_dofile(vm, "game.cdo") != CANDO_OK) {
    fprintf(stderr, "CanDo error: %s\n", cando_errmsg(vm));
    // e.g. "game.cdo:42: undefined variable 'playe'"
}
```

The error message is valid until the next call that modifies `vm->error_msg`.
Copy it if you need to retain it longer:

```c
char saved_error[512];
snprintf(saved_error, sizeof(saved_error), "%s", cando_errmsg(vm));
```

---

## Selecting standard libraries

`cando_openlibs` opens all 15 standard library modules at once.  For security
or size reasons you may want to open only specific modules:

```c
CandoVM *vm = cando_open();

// Open only safe, sandboxed libraries
cando_open_mathlib(vm);
cando_open_stringlib(vm);
cando_open_arraylib(vm);
cando_open_objectlib(vm);
cando_open_jsonlib(vm);
// Note: NOT opening filelib, processlib, netlib for untrusted scripts
```

Individual opener functions:

| Function | Module | Globals exposed |
|---|---|---|
| `cando_open_mathlib(vm)` | math | `math.*` |
| `cando_open_filelib(vm)` | file | `file.*` |
| `cando_open_stringlib(vm)` | string | String prototype methods |
| `cando_open_arraylib(vm)` | array | Array prototype methods |
| `cando_open_objectlib(vm)` | object | `object.*` |
| `cando_open_jsonlib(vm)` | json | `json.*` |
| `cando_open_csvlib(vm)` | csv | `csv.*` |
| `cando_open_threadlib(vm)` | thread | `thread.*` |
| `cando_open_oslib(vm)` | os | `os.*` |
| `cando_open_datetimelib(vm)` | datetime | `datetime.*` |
| `cando_open_cryptolib(vm)` | crypto | `crypto.*` |
| `cando_open_processlib(vm)` | process | `process.*` |
| `cando_open_netlib(vm)` | net | `net.*` |
| `cando_open_evallib(vm)` | eval | `eval()` |
| `cando_open_includelib(vm)` | include | `include()` |

---

## Registering native functions

Native functions let CanDo scripts call C code.  The signature is:

```c
typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

- `argc` — number of arguments passed from the script
- `args[0]..args[argc-1]` — argument values
- **Return value**: the number of values pushed onto the stack, or `-1` on error

### Simple example

```c
// C function exposed to CanDo
static int native_add(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 2 || !cando_is_number(args[0]) || !cando_is_number(args[1])) {
        cando_vm_error(vm, "add: expected two numbers");
        return -1;
    }
    double result = args[0].as.number + args[1].as.number;
    cando_vm_push(vm, cando_number(result));
    return 1;  // pushed 1 value
}

// Register before running scripts
cando_vm_register_native(vm, "add", native_add);
```

In CanDo:
```cando
VAR sum = add(3, 4);  // calls native_add
print(sum);           // 7
```

### Type checking helpers

```c
cando_is_null(v)      // v.type == TYPE_NULL
cando_is_number(v)    // v.type == TYPE_NUMBER
cando_is_bool(v)      // v.type == TYPE_BOOL
cando_is_string(v)    // v.type == TYPE_STRING
cando_is_object(v)    // v.type == TYPE_OBJECT

cando_as_number(v)    // v.as.number  (double)
cando_as_bool(v)      // v.as.boolean (bool)
```

### Returning multiple values

Push each value and return the total count:

```c
static int native_minmax(CandoVM *vm, int argc, CandoValue *args) {
    double a = args[0].as.number, b = args[1].as.number;
    cando_vm_push(vm, cando_number(a < b ? a : b));  // min
    cando_vm_push(vm, cando_number(a > b ? a : b));  // max
    return 2;
}
```

In CanDo:
```cando
VAR lo, hi = minmax(7, 3);  // lo=3, hi=7
```

### Namespaced library pattern

Group related functions under a single global object:

```c
static int native_vec_length(CandoVM *vm, int argc, CandoValue *args) {
    // ... implementation ...
}

static int native_vec_dot(CandoVM *vm, int argc, CandoValue *args) {
    // ... implementation ...
}

void register_vec_library(CandoVM *vm) {
    // Create a global "vec" object with these functions
    cando_vm_register_native(vm, "vec.length", native_vec_length);
    cando_vm_register_native(vm, "vec.dot",    native_vec_dot);
}
```

In CanDo:
```cando
VAR v = { x: 3, y: 4 };
print(vec.length(v));  // 5.0
```

---

## Exchanging data

### Reading and writing global variables

```c
// Set a global (last arg: is_const — true makes it read-only)
cando_vm_set_global(vm, "playerHealth", cando_number(100.0), false);
cando_vm_set_global(vm, "playerName",   cando_string_new("Alice"), false);

// Read a global (returns false if the variable is not defined)
CandoValue health;
if (cando_vm_get_global(vm, "playerHealth", &health))
    printf("health = %g\n", health.as.number);
```

### Value constructors

```c
CandoValue v_null   = cando_null();
CandoValue v_num    = cando_number(3.14);
CandoValue v_true   = cando_bool(true);
CandoValue v_false  = cando_bool(false);
CandoValue v_str    = cando_string_new("hello");  // heap-allocated, retained
```

String values must be released when no longer needed if you hold them across
call boundaries:

```c
CandoValue s = cando_string_new("hello");
// ... use s ...
cando_value_release(s);  // decrements refcount; frees if zero
```

Values returned by `cando_vm_get_global` are snapshots — they do not need
explicit release unless you retain them with `cando_value_copy`.

---

## Calling CanDo functions from C

```c
// Retrieve a function defined in a script
CandoValue fn;
if (!cando_vm_get_global(vm, "onUpdate", &fn))
    return;  /* variable not defined */

// Build argument array
CandoValue args[2] = { cando_number(delta_time), cando_number(frame_count) };

// Call it — returns number of return values pushed onto the stack
int nret = cando_vm_call_value(vm, fn, args, 2);
if (nret < 0) {
    fprintf(stderr, "script error: %s\n", cando_errmsg(vm));
}

// Retrieve return values from the stack
for (int i = 0; i < nret; i++) {
    CandoValue ret = cando_vm_pop(vm);
    // ...
}
```

> See [c-api.md](c-api.md) for the full `cando_vm_call_value` signature.

---

## Creating objects and arrays from C

Use the bridge API to create CanDo objects from C:

```c
#include "vm/bridge.h"

// Create an empty object
CandoValue obj = cando_bridge_new_object(vm);

// Resolve to CdoObject* so you can set fields
CdoObject *o = cando_bridge_resolve(vm, obj.as.handle);
CdoString *key = cando_bridge_intern_key(cando_string_new("x"));
cdo_object_set(o, key, (CdoValue){ .type = CDO_NUMBER, .as.number = 42.0 });
cdo_string_release(key);

// Pass to a script function or set as a global
cando_vm_set_global(vm, "myObj", obj);
```

In CanDo:
```cando
print(myObj.x);  // 42
```

Similarly for arrays:
```c
CandoValue arr = cando_bridge_new_array(vm);
CdoObject *a = cando_bridge_resolve(vm, arr.as.handle);
// Use cdo_array_* functions to populate it
```

---

## Thread safety

**One VM = one thread at a time.**  Do not call VM functions from multiple
OS threads simultaneously on the same `CandoVM*`.

You may:
- Create multiple `CandoVM*` instances, each used from a single thread.
- Use `thread`/`await` in CanDo scripts — the VM manages its own child threads
  internally with appropriate locking.

```c
// Thread-per-VM pattern — fully safe
void *worker(void *arg) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);
    cando_dofile(vm, (const char *)arg);
    cando_close(vm);
    return NULL;
}

// In main thread:
pthread_t t1, t2;
pthread_create(&t1, NULL, worker, "worker1.cdo");
pthread_create(&t2, NULL, worker, "worker2.cdo");
pthread_join(t1, NULL);
pthread_join(t2, NULL);
```

---

## Multiple VMs in one process

Multiple `CandoVM` instances coexist safely.  They share the global string
intern table (protected by a mutex) but are otherwise independent — each has
its own value stack, global table, call-frame stack, and native function
registry.

```c
// Separate VMs for different subsystems
CandoVM *game_vm   = cando_open();  // loads game mods
CandoVM *ui_vm     = cando_open();  // loads UI scripts
CandoVM *config_vm = cando_open();  // loads config files

cando_open_mathlib(game_vm);
cando_open_mathlib(ui_vm);
// config_vm gets no stdlib — it only parses data

cando_dofile(game_vm,   "mods/quest.cdo");
cando_dofile(ui_vm,     "ui/hud.cdo");
cando_dofile(config_vm, "config/settings.cdo");

cando_close(config_vm);
cando_close(ui_vm);
cando_close(game_vm);
```
