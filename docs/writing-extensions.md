# Writing Native Functions and Library Modules

This document explains how to extend CanDo with C code — either as a
standalone native function or as a full library module.

---

## Table of Contents

1. [Native function signature](#native-function-signature)
2. [Return conventions](#return-conventions)
3. [Error reporting](#error-reporting)
4. [Type checking](#type-checking)
5. [Working with strings](#working-with-strings)
6. [Working with objects and arrays](#working-with-objects-and-arrays)
7. [Writing a library module](#writing-a-library-module)
8. [Registering the module with cando_openlibs](#registering-the-module-with-cando_openlibs)
9. [Complete example: a `vec2` module](#complete-example-a-vec2-module)

---

## Native function signature

```c
typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

| Parameter | Description |
|---|---|
| `vm` | The running VM — use it for stack operations, error reporting, bridge calls |
| `argc` | Number of arguments passed from the script |
| `args` | Base of the argument array; `args[0]` is the first argument |

Return value: number of values pushed onto the stack, or `-1` on error.

### Minimal example

```c
static int native_double(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_number(args[0])) {
        cando_vm_error(vm, "double: expected a number");
        return -1;
    }
    cando_vm_push(vm, cando_number(args[0].as.number * 2.0));
    return 1;
}

// Register in the VM:
cando_vm_register_native(vm, "double", native_double);
```

In CanDo:
```cando
print(double(21));  // 42
```

---

## Return conventions

Push each return value with `cando_vm_push`, then return the total count.

```c
// Return nothing (null is pushed automatically by the VM)
return 0;

// Return one value
cando_vm_push(vm, cando_number(42.0));
return 1;

// Return multiple values
cando_vm_push(vm, cando_number(3.0));   // first
cando_vm_push(vm, cando_number(4.0));   // second
return 2;
```

In CanDo, spread-receive multiple return values:
```cando
VAR a, b = myNative();
```

---

## Error reporting

Always use `cando_vm_error` — never set `vm->has_error` directly.  It
formats the message, sets the error state, and populates `vm->error_vals`
so scripts can catch the error with `TRY/CATCH`.

```c
static int native_sqrt(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1) {
        cando_vm_error(vm, "sqrt: expected 1 argument, got 0");
        return -1;
    }
    if (!cando_is_number(args[0])) {
        cando_vm_error(vm, "sqrt: expected number, got %s",
                       cando_value_type_name(args[0]));
        return -1;
    }
    double x = args[0].as.number;
    if (x < 0.0) {
        cando_vm_error(vm, "sqrt: cannot take square root of negative number");
        return -1;
    }
    cando_vm_push(vm, cando_number(sqrt(x)));
    return 1;
}
```

`cando_vm_error` is variadic — use `printf`-style formatting:
```c
cando_vm_error(vm, "expected number at arg %d, got %s", idx,
               cando_value_type_name(args[idx]));
```

### Caught in CanDo

```cando
TRY {
    sqrt(-1);
} CATCH (msg) {
    print(msg);  // "sqrt: cannot take square root of negative number"
}
```

---

## Type checking

Check types before accessing value fields:

```c
bool cando_is_null(CandoValue v)
bool cando_is_number(CandoValue v)
bool cando_is_bool(CandoValue v)
bool cando_is_string(CandoValue v)
bool cando_is_object(CandoValue v)
```

Shorthand accessors (undefined behaviour if type is wrong — check first):
```c
double       cando_as_number(CandoValue v)  /* v.as.number  */
bool         cando_as_bool(CandoValue v)    /* v.as.boolean */
CandoString *cando_as_string(CandoValue v)  /* v.as.string  */
```

The `CandoString` struct has a `data` field containing the NUL-terminated
UTF-8 string:
```c
if (cando_is_string(args[0])) {
    const char *text = args[0].as.string->data;
    size_t len = args[0].as.string->length;
}
```

### Optional arguments pattern

```c
static int native_greet(CandoVM *vm, int argc, CandoValue *args) {
    const char *name = (argc >= 1 && cando_is_string(args[0]))
                       ? args[0].as.string->data
                       : "world";
    printf("Hello, %s!\n", name);
    return 0;
}
```

---

## Working with strings

### Reading a string argument

```c
if (!cando_is_string(args[0])) {
    cando_vm_error(vm, "expected string");
    return -1;
}
const char *s = args[0].as.string->data;
size_t      n = args[0].as.string->length;
```

### Returning a string

```c
// From a C string literal or buffer
cando_vm_push(vm, cando_string_new("result text"));
return 1;
```

`cando_string_new` heap-allocates and retains a new `CandoString`.  The VM
takes ownership of it when you push it onto the stack.

### Building a string in C

```c
char buf[256];
snprintf(buf, sizeof(buf), "item_%d", id);
cando_vm_push(vm, cando_string_new(buf));
return 1;
```

---

## Working with objects and arrays

### Reading object fields

```c
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"

if (!cando_is_object(args[0])) {
    cando_vm_error(vm, "expected object");
    return -1;
}
CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);

// Look up field "x"
CdoString *key = cdo_intern_cstr("x");   // interned; no need to free
CdoValue   field;
bool found = cdo_object_get(obj, key, &field);
if (found && field.type == CDO_NUMBER)
    double x = field.as.number;
```

### Creating an object

```c
CandoValue result = cando_bridge_new_object(vm);
CdoObject *o      = cando_bridge_resolve(vm, result.as.handle);

CdoString *kx = cdo_intern_cstr("x");
CdoString *ky = cdo_intern_cstr("y");
cdo_object_set(o, kx, (CdoValue){ .type = CDO_NUMBER, .as.number = 3.0 });
cdo_object_set(o, ky, (CdoValue){ .type = CDO_NUMBER, .as.number = 4.0 });

cando_vm_push(vm, result);
return 1;
```

### Working with arrays

```c
#include "object/array.h"

CandoValue arr_val = cando_bridge_new_array(vm);
CdoObject *arr     = cando_bridge_resolve(vm, arr_val.as.handle);

// Append a number
CdoValue item = { .type = CDO_NUMBER, .as.number = 42.0 };
cdo_array_push(arr, item);

// Get element at index
CdoValue elem;
if (cdo_array_get(arr, 0, &elem))
    printf("%g\n", elem.as.number);

// Array length
u32 len = cdo_array_length(arr);

cando_vm_push(vm, arr_val);
return 1;
```

---

## Writing a library module

A library module is a `.c` file that:
1. Implements native functions
2. Has a `cando_lib_*_register(CandoVM *vm)` function that registers them

Convention: expose the functions under a namespace object (e.g., `mylib.*`).

### Module header (`source/lib/mylib.h`)

```c
#ifndef CANDO_LIB_MYLIB_H
#define CANDO_LIB_MYLIB_H

#include "../vm/vm.h"
#include "../../include/cando.h"

CANDO_API void cando_lib_mylib_register(CandoVM *vm);

#endif
```

### Module source (`source/lib/mylib.c`)

```c
#include "mylib.h"
#include "libutil.h"     // for libutil_push_cstr, libutil_set_method
#include "../vm/bridge.h"
#include <math.h>

/* --- Implementation --- */

static int mylib_clamp(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 3) {
        cando_vm_error(vm, "mylib.clamp: expected (value, min, max)");
        return -1;
    }
    double v   = args[0].as.number;
    double lo  = args[1].as.number;
    double hi  = args[2].as.number;
    cando_vm_push(vm, cando_number(v < lo ? lo : (v > hi ? hi : v)));
    return 1;
}

static int mylib_lerp(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 3) {
        cando_vm_error(vm, "mylib.lerp: expected (a, b, t)");
        return -1;
    }
    double a = args[0].as.number;
    double b = args[1].as.number;
    double t = args[2].as.number;
    cando_vm_push(vm, cando_number(a + (b - a) * t));
    return 1;
}

/* --- Registration --- */

void cando_lib_mylib_register(CandoVM *vm) {
    cando_vm_register_native(vm, "mylib.clamp", mylib_clamp);
    cando_vm_register_native(vm, "mylib.lerp",  mylib_lerp);
}
```

---

## Registering the module with cando_openlibs

If you want your module to be part of CanDo's standard library (opened by
`cando_openlibs`), add it to `source/cando_lib.c`:

1. Include the header:
   ```c
   #include "lib/mylib.h"
   ```

2. Call the register function in `cando_openlibs`:
   ```c
   void cando_openlibs(CandoVM *vm) {
       // ... existing libs ...
       cando_lib_mylib_register(vm);
   }
   ```

3. Add an individual opener in `cando_lib.c`:
   ```c
   CANDO_API void cando_open_myliblib(CandoVM *vm) {
       cando_lib_mylib_register(vm);
   }
   ```

4. Declare it in `include/cando.h`:
   ```c
   CANDO_API void cando_open_myliblib(CandoVM *vm);
   ```

5. Add the source file to `CMakeLists.txt` in `LIB_SRCS` and to the Makefile's `CANDO_LIB_SRCS`.

---

## Complete example: a `vec2` module

### `source/lib/vec2.h`

```c
#ifndef CANDO_LIB_VEC2_H
#define CANDO_LIB_VEC2_H
#include "../vm/vm.h"
#include "../../include/cando.h"
CANDO_API void cando_lib_vec2_register(CandoVM *vm);
#endif
```

### `source/lib/vec2.c`

```c
#include "vec2.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"
#include <math.h>

/* Helper: read (x,y) from object arg */
static bool read_vec(CandoVM *vm, CandoValue v, double *x, double *y) {
    if (!cando_is_object(v)) return false;
    CdoObject *o  = cando_bridge_resolve(vm, v.as.handle);
    CdoString *kx = cdo_intern_cstr("x");
    CdoString *ky = cdo_intern_cstr("y");
    CdoValue fx, fy;
    bool ok = cdo_object_get(o, kx, &fx) && cdo_object_get(o, ky, &fy);
    if (ok) { *x = fx.as.number; *y = fy.as.number; }
    return ok;
}

/* Helper: push a new vec2 object */
static int push_vec(CandoVM *vm, double x, double y) {
    CandoValue obj = cando_bridge_new_object(vm);
    CdoObject *o   = cando_bridge_resolve(vm, obj.as.handle);
    CdoString *kx  = cdo_intern_cstr("x");
    CdoString *ky  = cdo_intern_cstr("y");
    cdo_object_set(o, kx, (CdoValue){ .type = CDO_NUMBER, .as.number = x });
    cdo_object_set(o, ky, (CdoValue){ .type = CDO_NUMBER, .as.number = y });
    cando_vm_push(vm, obj);
    return 1;
}

static int vec2_new(CandoVM *vm, int argc, CandoValue *args) {
    double x = argc >= 1 ? args[0].as.number : 0.0;
    double y = argc >= 2 ? args[1].as.number : 0.0;
    return push_vec(vm, x, y);
}

static int vec2_add(CandoVM *vm, int argc, CandoValue *args) {
    double ax, ay, bx, by;
    if (!read_vec(vm, args[0], &ax, &ay) ||
        !read_vec(vm, args[1], &bx, &by)) {
        cando_vm_error(vm, "vec2.add: expected two vec2 objects");
        return -1;
    }
    return push_vec(vm, ax + bx, ay + by);
}

static int vec2_length(CandoVM *vm, int argc, CandoValue *args) {
    double x, y;
    if (!read_vec(vm, args[0], &x, &y)) {
        cando_vm_error(vm, "vec2.length: expected a vec2 object");
        return -1;
    }
    cando_vm_push(vm, cando_number(sqrt(x * x + y * y)));
    return 1;
}

void cando_lib_vec2_register(CandoVM *vm) {
    cando_vm_register_native(vm, "vec2.new",    vec2_new);
    cando_vm_register_native(vm, "vec2.add",    vec2_add);
    cando_vm_register_native(vm, "vec2.length", vec2_length);
}
```

### Usage in CanDo

```cando
VAR a = vec2.new(3, 0);
VAR b = vec2.new(0, 4);
VAR c = vec2.add(a, b);
print(vec2.length(c));   // 5.0
print(c.x);              // 3
print(c.y);              // 4
```
