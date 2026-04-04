# Writing a Module

This guide walks through creating a complete Cando library module from scratch.
We will build a small `mylib` module that exposes a global `mylib` object with
two methods: `mylib.add(a, b)` and `mylib.greet(name)`.

---

## 1. Create the header file

`source/lib/mylib.h`:

```c
#ifndef CANDO_LIB_MYLIB_H
#define CANDO_LIB_MYLIB_H

#include "../vm/vm.h"

/*
 * cando_lib_mylib_register -- register the mylib module in vm.
 *
 * Exposes a global read-only object `mylib` with:
 *   mylib.add(a, b)     → number (a + b)
 *   mylib.greet(name)   → string ("Hello, <name>!")
 */
void cando_lib_mylib_register(CandoVM *vm);

#endif /* CANDO_LIB_MYLIB_H */
```

---

## 2. Create the implementation file

`source/lib/mylib.c`:

```c
#include "mylib.h"

#include "../vm/bridge.h"
#include "../object/object.h"

#include <stdio.h>
#include <string.h>

/* =========================================================================
 * mylib.add(a, b) → number
 * ======================================================================= */

static int mylib_add(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_number(args[0]) || !cando_is_number(args[1])) {
        vm->has_error = true;
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "mylib.add: expected two numbers");
        return -1;
    }
    f64 result = args[0].as.number + args[1].as.number;
    cando_vm_push(vm, cando_number(result));
    return 1;
}

/* =========================================================================
 * mylib.greet(name) → string
 * ======================================================================= */

static int mylib_greet(CandoVM *vm, int argc, CandoValue *args)
{
    const char *name = "World";
    if (argc >= 1 && cando_is_string(args[0]))
        name = args[0].as.string->data;

    char buf[256];
    int  len = snprintf(buf, sizeof(buf), "Hello, %s!", name);

    CandoString *s = cando_string_new(buf, (u32)len);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* =========================================================================
 * Helper: add a native method to an object under `name`.
 * ======================================================================= */

static void set_method(CandoVM *vm, CdoObject *obj,
                        const char *name, CandoNativeFn fn)
{
    /* Register the native; get its sentinel value */
    CandoValue sentinel = cando_vm_add_native(vm, fn);

    /* Convert the sentinel (TYPE_NUMBER) to a CdoValue for rawset */
    CdoValue v = cdo_number(sentinel.as.number);

    /* Intern the key string in the object-layer intern table */
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));

    cdo_object_rawset(obj, key, v, FIELD_STATIC);
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_mylib_register(CandoVM *vm)
{
    /* Create the module object and resolve its pointer */
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, obj_val.as.handle);

    /* Add methods */
    set_method(vm, obj, "add",   mylib_add);
    set_method(vm, obj, "greet", mylib_greet);

    /* Make the object read-only so scripts cannot accidentally overwrite it */
    cdo_object_set_readonly(obj, true);

    /* Expose as a const global named "mylib" */
    cando_vm_set_global(vm, "mylib", obj_val, true);
}
```

---

## 3. Add to the Makefile

Open `Makefile` and add the new source file to `CANDO_SRCS`:

```makefile
CANDO_SRCS = \
    ...
    source/lib/mylib.c     \
    source/main.c
```

---

## 4. Register in main.c

```c
#include "lib/mylib.h"

// Inside main(), after cando_vm_init():
cando_lib_mylib_register(&vm);
```

---

## 5. Test it in a script

`test_mylib.cdo`:

```cando
print(mylib.add(3, 4));          // 7
print(mylib.greet("Cando"));     // Hello, Cando!
print(mylib.greet());            // Hello, World!
```

```
./cando test_mylib.cdo
7
Hello, Cando!
Hello, World!
```

---

## Patterns and conventions

### Argument helpers

Centralise argument extraction to keep native functions tidy:

```c
static f64 arg_num(CandoValue *args, int argc, int idx) {
    return (idx < argc && cando_is_number(args[idx]))
           ? args[idx].as.number : 0.0;
}

static const char *arg_str(CandoValue *args, int argc, int idx) {
    return (idx < argc && cando_is_string(args[idx]))
           ? args[idx].as.string->data : NULL;
}
```

### Pushing string results

```c
static void push_str(CandoVM *vm, const char *data, u32 len) {
    CandoString *s = cando_string_new(data, len);
    cando_vm_push(vm, cando_string_value(s));
}
```

### Returning objects with multiple fields

```c
CandoValue result = cando_bridge_new_object(vm);
CdoObject *obj   = cando_bridge_resolve(vm, result.as.handle);

CdoString *k_ok  = cdo_string_intern("ok",  2);
CdoString *k_val = cdo_string_intern("val", 3);

cdo_object_rawset(obj, k_ok,  cdo_bool(true),    FIELD_NONE);
cdo_object_rawset(obj, k_val, cdo_number(42.0),  FIELD_NONE);

cando_vm_push(vm, result);
return 1;
```

### Module object with constants

Use `FIELD_STATIC` for constant fields and `set_method` for natives:

```c
// String constant
CdoString *k     = cdo_string_intern("VERSION", 7);
CdoString *v_str = cdo_string_intern("1.0.0",   5);
cdo_object_rawset(obj, k, cdo_string_value(v_str), FIELD_STATIC);

// Numeric constant
CdoString *k_pi = cdo_string_intern("PI", 2);
cdo_object_rawset(obj, k_pi, cdo_number(3.14159265358979), FIELD_STATIC);
```

### Error reporting pattern

Always set `vm->has_error = true` AND fill `vm->error_msg` before returning `-1`:

```c
vm->has_error = true;
snprintf(vm->error_msg, sizeof(vm->error_msg),
         "mylib.foo: expected a string, got %s",
         cando_value_type_name((TypeTag)args[0].tag));
return -1;
```

---

## Enabling colon-syntax for a custom type

If your module introduces a new "type" represented as an object and you want
users to call methods with `:` syntax, set the relevant meta-key:

```c
// Set __type for identification
CdoString *k_type = cdo_string_intern("__type", 6);
CdoString *type_name = cdo_string_intern("MyType", 6);
cdo_object_rawset(instance, k_type, cdo_string_value(type_name), FIELD_STATIC);

// Set __index to point to the method table so prototype lookup works
CdoString *k_index = g_meta_index; // pre-interned by cdo_object_init()
cdo_object_rawset(instance, k_index, cdo_object_value(method_table), FIELD_NONE);
```

Then `instance:method(args)` will resolve `method` through the `__index` chain
and call it with `instance` as `args[0]`.

---

## File naming convention

| File | Purpose |
|---|---|
| `source/lib/mylib.h` | Public header — declare only `cando_lib_mylib_register` |
| `source/lib/mylib.c` | Implementation — all native functions are `static` |

Keep all native functions `static` to avoid name collisions across modules.
