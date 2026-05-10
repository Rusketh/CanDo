# Writing Extensions

Two ways to expose host functionality to CanDo scripts:

1. **In-process natives.**  Functions written in C and registered into a
   VM directly.  Use this for host-specific functionality that you
   build into the executable — game APIs, application services,
   testing hooks.
2. **Binary modules.**  Standalone shared libraries discovered by
   `include()` at runtime.  Use this for reusable extensions that ship
   independently of the host.

Both paths use the same C signature and the same value/object bridge.
The difference is just *when* and *how* they're loaded.

## The native function signature

```c
#include <cando.h>

CandoValue my_native(CandoVM *vm, int argc, CandoValue *argv);
```

- `vm` is the calling VM.  Pass it through to any helpers that need it.
- `argc` is the number of arguments the script passed.  Validate.
- `argv` is the array of arguments as `CandoValue`s.
- The returned `CandoValue` is what the call expression evaluates to.
  Return `cando_value_null()` if you have nothing to say.

## Reading arguments

`CandoValue` is a tagged union.  Helpers in `cando.h` plus its included
headers let you discriminate and read:

```c
#include <cando/core/value.h>

if (argc < 1 || !cando_value_is_string(argv[0])) {
    cando_throw(vm, "expected string");      /* never returns */
}
const char *s   = cando_value_to_cstring(argv[0]);
size_t      len = cando_value_string_length(argv[0]);
```

The full list of `cando_value_is_*` / `cando_value_to_*` helpers is in
[`include/cando.h`](../../include/cando.h) and the included headers it
pulls in.  Look at `source/lib/*.c` for production examples.

## Working with objects

A `CandoValue` for an object is a `HandleIndex` — not a raw pointer.
The runtime keeps the indirection so the GC can move objects without
invalidating live values held by other threads.

To inspect or mutate the underlying object, resolve through the bridge:

```c
#include <cando/vm/bridge.h>
#include <cando/object/object.h>

CdoObject *o = bridge_resolve_object(vm, argv[0]);
if (!o) cando_throw(vm, "expected object");

CandoValue field = cdo_object_get(o, "name");
```

**Critical rule:**  do **not** store `CdoObject *` across any call
that might trigger the GC (allocation, calling back into a script,
issuing another native).  Re-resolve from the handle each time.  If
this trips you up, see the AI guide ([../AI-GUIDE.md](../AI-GUIDE.md))
for a longer treatment.

## Returning values

Constructors in `core/value.h`:

```c
CandoValue cando_value_null(void);
CandoValue cando_value_bool(bool b);
CandoValue cando_value_number(double n);
CandoValue cando_value_string(CandoVM *vm, const char *s, size_t len);
CandoValue cando_value_object(HandleIndex h);
```

For multi-return, push values onto the stack and call
`cando_native_return(vm, n)`.  See an existing native that returns
multiple values (e.g. `socket.connect`) for an example.

## Throwing errors

```c
void cando_throw(CandoVM *vm, const char *fmt, ...);   /* _Noreturn */
```

`cando_throw` raises a script-level error that scripts can catch with
`TRY` / `CATCH`.  It does not return — control transfers back into the
VM's nearest catch handler, unwinding the C stack via `longjmp`.

Use `cando_throw` whenever the script's input is wrong; do not return
sentinel values for errors unless your library explicitly documents
that it returns `NULL` on failure (the way `file.read` does).

## Registering natives — in-process

To expose a single function as a global:

```c
cando_register_native(vm, "my_fn", my_fn);
```

To register a whole library namespace (the way `math`, `string`, etc.
are registered), follow the pattern in `source/lib/*.c`:

```c
static const CandoNative funcs[] = {
    { "fn1", my_fn1 },
    { "fn2", my_fn2 },
    { NULL,  NULL    },
};

void open_mylib(CandoVM *vm) {
    libutil_register(vm, "mylib", funcs, sizeof(funcs)/sizeof(funcs[0]) - 1);
}
```

`libutil_register` is internal API — it lives in `source/lib/libutil.h`
and is shared by every standard library.

## Binary modules

A binary module is a shared library exposing one symbol:

```c
#include <cando.h>

CandoValue cando_module_init(CandoVM *vm);
```

The runtime calls this once when the module is first `include()`d.
Whatever you return becomes the module value visible to script.  Any
natives you register, methods you set on `_meta.<type>`, or globals you
populate are persisted by the VM.

Layout for a new module under `modules/`:

```
modules/<name>/
  README.md            module documentation (script-facing)
  <name>_module.c      implementation; exports cando_module_init
  <name>_helpers.h     pure-C helpers reusable in tests
  test_<name>.c        C unit tests for the helpers
  test_<name>.cdo      script-level integration tests
  Makefile             builds <name>.so / .dylib / .dll
```

Steps:

1. Copy an existing module (e.g. [`modules/sqlite/`](../../modules/sqlite/)).
2. Implement `cando_module_init`:

```c
CandoValue cando_module_init(CandoVM *vm) {
    CdoObject *mod = cdo_object_new(vm);

    static const CandoNative funcs[] = {
        { "doThing", do_thing },
        { NULL, NULL },
    };
    libutil_register_into(vm, mod, funcs, 1);

    return cando_value_object(cdo_object_handle(mod));
}
```

3. Add a `Makefile` that links a single `.so` / `.dylib` / `.dll`.
4. Add the module's directory to the top-level `Makefile`'s `MODULES =`
   list and CMake equivalent.
5. Drop the artefact next to the script that wants it, or somewhere on
   `include()`'s search path.  Load with:

```cdo
VAR mod = include("./mymodule");        // probes .so/.dylib/.dll/.cdo
mod.doThing();
```

## A complete worked example

The [`sqlite` module](../../modules/sqlite/) is small enough to read
end-to-end as a template:

- `sqlite_module.c` — the entry point and registration.
- `sqlite_helpers.h` / `.c` — pure-C glue that the C tests reuse.
- `test_sqlite.c` — C unit tests.
- `test_sqlite.cdo` — script-level integration test.
- `Makefile` — single-shared-library build.

If you're starting a new module, that is the file set to copy.

## See also

- [reference.md](reference.md) — the public symbol list.
- [embedding.md](embedding.md) — how a host opens and uses a VM.
- [../AI-GUIDE.md](../AI-GUIDE.md) — orientation for AI tools editing
  the runtime.
