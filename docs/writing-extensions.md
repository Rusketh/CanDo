# Writing Extensions

This guide covers three ways to extend CanDo with C:

1. Adding a **single native function** from your host program.
2. Building an **in-process library module** that ships with `libcando`.
3. Packaging a **binary module** (`.so`/`.dylib`/`.dll`) that scripts load
   with `include()`.

For the raw API listings, see [c-api.md](c-api.md).  For embedding basics,
see [embedding.md](embedding.md).

## The native function contract

```c
#include <cando.h>

typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);
```

- `vm` — the calling VM.  Pass it to every `cando_vm_*` and
  `cando_bridge_*` helper.
- `argc` — number of arguments on the stack.
- `args[0..argc-1]` — the arguments, in order.
- **Return value**: the number of values you pushed onto the stack
  (zero or more), or `-1` to signal an error after a
  `cando_vm_error` call.

Register the function under a global name:

```c
cando_vm_register_native(vm, "greet", native_greet);
```

and it becomes callable from any script:

```cando
greet("world");
```

## Argument extraction

Include `lib/libutil.h` for the tiny inline helpers the built-in libraries
use:

```c
#include "lib/libutil.h"

static int add(CandoVM *vm, int argc, CandoValue *args) {
    f64 a = libutil_arg_num_at(args, argc, 0, 0.0);
    f64 b = libutil_arg_num_at(args, argc, 1, 0.0);
    cando_vm_push(vm, cando_number(a + b));
    return 1;
}
```

| Helper | Returns |
|---|---|
| `libutil_arg_num_at(args, argc, i, def)` | `f64` at index `i`, or `def` if missing/non-number |
| `libutil_arg_cstr_at(args, argc, i)` | `const char *` at index `i`, or `NULL` |
| `libutil_arg_str_at(args, argc, i)` | `CandoString *` at index `i`, or `NULL` |

If you need stricter type checking, use the predicates directly:

```c
if (argc < 1 || !cando_is_number(args[0])) {
    cando_vm_error(vm, "sqrt: expected number, got %s",
                   cando_value_type_name(args[0].tag));
    return -1;
}
```

## Pushing return values

`cando_vm_push` accepts any `CandoValue`.  Build values with the
constructors from `core/value.h`:

```c
cando_vm_push(vm, cando_null());
cando_vm_push(vm, cando_bool(true));
cando_vm_push(vm, cando_number(3.14));
cando_vm_push(vm, cando_string_value(cando_string_new("hello", 5)));
```

To return multiple values, push them left-to-right and return the count:

```c
cando_vm_push(vm, cando_number(1));
cando_vm_push(vm, cando_number(2));
return 2;                       // caller sees `a, b = native()`
```

## Reporting errors

`cando_vm_error` is `printf`-style.  Call it and `return -1`:

```c
if (x < 0.0) {
    cando_vm_error(vm, "sqrt: negative input %g", x);
    return -1;
}
```

The formatted message becomes a throwable value — scripts can catch it:

```cando
TRY { sqrt(-1); } CATCH (msg) { print(msg); }
```

## Building objects and arrays

The bridge layer converts between the VM's `CandoValue` and the underlying
`CdoObject`/`CdoValue` heap layer.  To construct data for a script:

```c
#include "vm/bridge.h"
#include "object/object.h"
#include "object/array.h"
#include "object/string.h"
#include "object/value.h"          /* cdo_number, cdo_bool, cdo_null */

CandoValue v  = cando_bridge_new_object(vm);
CdoObject *o  = cando_bridge_resolve(vm, v.as.handle);

/* keys must be interned CdoString*s */
CdoString *kx = cdo_string_intern("x", 1);
CdoString *ky = cdo_string_intern("y", 1);

cdo_object_rawset(o, kx, cdo_number(3.0), FIELD_NONE);
cdo_object_rawset(o, ky, cdo_number(4.0), FIELD_NONE);

cdo_string_release(kx);            /* the object retains its own ref */
cdo_string_release(ky);

cando_vm_push(vm, v);
return 1;
```

Arrays use `cando_bridge_new_array` + `cdo_array_push`:

```c
CandoValue av = cando_bridge_new_array(vm);
CdoObject *a  = cando_bridge_resolve(vm, av.as.handle);

cdo_array_push(a, cdo_number(10.0));
cdo_array_push(a, cdo_number(20.0));

cando_vm_push(vm, av);
return 1;
```

Reading is the mirror image — `cdo_object_rawget` for fields,
`cdo_array_rawget_idx` / `cdo_array_len` for arrays:

```c
if (!cando_is_object(args[0])) {
    cando_vm_error(vm, "expected object");
    return -1;
}
CdoObject *o  = cando_bridge_resolve(vm, args[0].as.handle);
CdoString *kx = cdo_string_intern("x", 1);
CdoValue  fx;
bool has_x = cdo_object_rawget(o, kx, &fx);
cdo_string_release(kx);
```

## The library module pattern

Every built-in library under `source/lib/` follows the same shape: build
a fresh object, attach the natives as methods, install it as a global.
This is the `libutil_set_method` pattern:

```c
#include "mylib.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

static int mylib_square(CandoVM *vm, int argc, CandoValue *args) {
    f64 x = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(x * x));
    return 1;
}

static int mylib_lerp(CandoVM *vm, int argc, CandoValue *args) {
    f64 a = libutil_arg_num_at(args, argc, 0, 0.0);
    f64 b = libutil_arg_num_at(args, argc, 1, 0.0);
    f64 t = libutil_arg_num_at(args, argc, 2, 0.0);
    cando_vm_push(vm, cando_number(a + (b - a) * t));
    return 1;
}

void cando_lib_mylib_register(CandoVM *vm) {
    CandoValue tbl  = cando_bridge_new_object(vm);
    CdoObject *obj  = cando_bridge_resolve(vm, tbl.as.handle);

    libutil_set_method(vm, obj, "square", mylib_square);
    libutil_set_method(vm, obj, "lerp",   mylib_lerp);

    cando_vm_set_global(vm, "mylib", tbl, /*is_const=*/true);
}
```

`libutil_set_method` assigns each function a sentinel value with
`cando_vm_add_native` and stores that sentinel under the given key on the
object.  The VM recognises the sentinel and dispatches the call when the
method is invoked with `:` or `.`.

### Joining the built-in set

To ship the library as part of `libcando`:

1. Add `source/lib/mylib.c` and `source/lib/mylib.h`.
2. Declare `cando_open_mylib(CandoVM *)` in `include/cando.h`.
3. In `source/cando_lib.c` implement the public opener:
   ```c
   CANDO_API void cando_open_mylib(CandoVM *vm) {
       cando_lib_mylib_register(vm);
   }
   ```
   and call it from `cando_openlibs`.
4. Add `source/lib/mylib.c` to `LIB_SRCS` in `CMakeLists.txt`
   (and `CANDO_LIB_SRCS` in the Makefile).

## Binary extension modules

Scripts can load a shared library at runtime with `include("./mymod.so")`.
The loader calls a single exported symbol:

```c
CandoValue cando_module_init(CandoVM *vm);
```

`cando_module_init` is invoked exactly once per VM; its return value
becomes the module value cached by `include()`.

A minimal extension:

```c
/* mymod.c — build with:
 *   gcc -shared -fPIC mymod.c -lcando -o mymod.so
 */
#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "lib/libutil.h"

static int mymod_hello(CandoVM *vm, int argc, CandoValue *args) {
    const char *who = libutil_arg_cstr_at(args, argc, 0);
    if (!who) who = "world";
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "hello, %s!", who);
    cando_vm_push(vm,
        cando_string_value(cando_string_new(buf, (u32)n)));
    return 1;
}

CandoValue cando_module_init(CandoVM *vm) {
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);
    libutil_set_method(vm, obj, "hello", mymod_hello);
    return tbl;
}
```

From a script:

```cando
VAR m = include("./mymod.so");
print(m:hello("Ada"));            // hello, Ada!
```

The same file works cross-platform; `include()` picks `.so`, `.dylib`, or
`.dll` from the suffix you pass.

## Worked example — a `vec2` library

A complete two-function library that creates and adds 2D vectors.

```c
/* lib/vec2.c */
#include "../object/object.h"
#include "../object/string.h"
#include "../object/value.h"
#include "../vm/bridge.h"
#include "libutil.h"
#include <math.h>

static bool read_xy(CandoVM *vm, CandoValue v, f64 *x, f64 *y) {
    if (!cando_is_object(v)) return false;
    CdoObject *o  = cando_bridge_resolve(vm, v.as.handle);
    CdoString *kx = cdo_string_intern("x", 1);
    CdoString *ky = cdo_string_intern("y", 1);
    CdoValue fx, fy;
    bool ok = cdo_object_rawget(o, kx, &fx)
           && cdo_object_rawget(o, ky, &fy)
           && fx.type == CDO_NUMBER && fy.type == CDO_NUMBER;
    cdo_string_release(kx);
    cdo_string_release(ky);
    if (ok) { *x = fx.as.number; *y = fy.as.number; }
    return ok;
}

static int push_vec(CandoVM *vm, f64 x, f64 y) {
    CandoValue v  = cando_bridge_new_object(vm);
    CdoObject *o  = cando_bridge_resolve(vm, v.as.handle);
    CdoString *kx = cdo_string_intern("x", 1);
    CdoString *ky = cdo_string_intern("y", 1);
    cdo_object_rawset(o, kx, cdo_number(x), FIELD_NONE);
    cdo_object_rawset(o, ky, cdo_number(y), FIELD_NONE);
    cdo_string_release(kx);
    cdo_string_release(ky);
    cando_vm_push(vm, v);
    return 1;
}

static int vec2_new(CandoVM *vm, int argc, CandoValue *args) {
    f64 x = libutil_arg_num_at(args, argc, 0, 0.0);
    f64 y = libutil_arg_num_at(args, argc, 1, 0.0);
    return push_vec(vm, x, y);
}

static int vec2_add(CandoVM *vm, int argc, CandoValue *args) {
    f64 ax, ay, bx, by;
    if (argc < 2 || !read_xy(vm, args[0], &ax, &ay)
                 || !read_xy(vm, args[1], &bx, &by)) {
        cando_vm_error(vm, "vec2.add: expected two vec2 objects");
        return -1;
    }
    return push_vec(vm, ax + bx, ay + by);
}

static int vec2_length(CandoVM *vm, int argc, CandoValue *args) {
    f64 x, y;
    if (argc < 1 || !read_xy(vm, args[0], &x, &y)) {
        cando_vm_error(vm, "vec2.length: expected a vec2");
        return -1;
    }
    cando_vm_push(vm, cando_number(sqrt(x * x + y * y)));
    return 1;
}

void cando_lib_vec2_register(CandoVM *vm) {
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);
    libutil_set_method(vm, obj, "new",    vec2_new);
    libutil_set_method(vm, obj, "add",    vec2_add);
    libutil_set_method(vm, obj, "length", vec2_length);
    cando_vm_set_global(vm, "vec2", tbl, true);
}
```

```cando
VAR a = vec2.new(3, 0);
VAR b = vec2.new(0, 4);
VAR c = vec2.add(a, b);
print(vec2.length(c));            // 5
print(c.x, c.y);                  // 3 4
```

## Things to watch out for

- **Always check `argc` before reading `args[i]`.**  The VM does not
  pad missing arguments.
- **Strings are reference-counted.**  `cando_string_new` returns a
  retained string; pushing it onto the stack transfers ownership.
  If you drop the value without pushing, release it yourself with
  `cando_string_release`.
- **Keys must be interned.**  `cdo_object_rawset`/`rawget` compare keys
  by pointer identity.  Always go through `cdo_string_intern`.
- **Do not push values and then return `-1`.**  The VM discards native
  pushes on error; leaving the stack untouched is the convention.
- **Do not call scripts while you hold a raw `CdoObject *`.**  Re-entry
  through `cando_vm_call_value` or `cando_vm_exec_eval` may rehash the
  handle table; resolve the handle afresh after such a call.
