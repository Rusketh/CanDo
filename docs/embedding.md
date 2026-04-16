# Embedding CanDo

This guide shows how to use `libcando` from a C program — loading
scripts, exchanging values with your host, registering natives, and
handling errors.  The formal reference for every `cando_*` symbol is
in [c-api.md](c-api.md); the same information but grouped by task
is here.

## Linking

A single header:

```c
#include <cando.h>
```

Link with either the shared library (`-lcando`) or the static library
(`-lcando_static`).  Scripts pull in pthreads and OpenSSL through the
library, so your host does not need to link them directly unless it
uses them itself.

```
gcc myapp.c -lcando -Iinclude -o myapp
```

## VM lifecycle

```c
CandoVM *vm = cando_open();        // allocate + initialise
cando_openlibs(vm);                // register all 17 standard libraries
// … run scripts …
cando_close(vm);                   // tear down and free
```

`cando_open` returns `NULL` only if the process is out of memory.  The
call does **not** register standard libraries; call `cando_openlibs` or
one of the `cando_open_*lib` functions after it.

`cando_close` blocks until every thread spawned by scripts running on
`vm` has finished, then frees every VM-owned resource.

Multiple VMs can coexist in the same process.  The library uses an
internal refcount so the global string intern table lives for as long
as *any* VM is open.

## Selective library loading

```c
cando_open_mathlib(vm);            // just math
cando_open_stringlib(vm);          // just the string prototype
cando_open_jsonlib(vm);
// skip file, os, process, net, http, https, …
```

Opening a library is idempotent — the second call is a no-op.
Available openers, each corresponding to a global:

```
math      string     array       object       thread
os        datetime   crypto      process      net
file      json       csv         include      eval
http      https
```

`include`, `eval`, and the `http`/`https` server mean you usually want
a full `cando_openlibs` for trusted scripts and a curated set for
untrusted input.

## Running scripts

### From a file

```c
int rc = cando_dofile(vm, "game/main.cdo");
if (rc != CANDO_OK) {
    fprintf(stderr, "%s\n", cando_errmsg(vm));
}
```

`cando_dofile` canonicalises the path with `realpath()` so that
relative `include()` calls from within the script resolve relative to
the script's directory, not the process CWD.

### From a string

```c
int rc = cando_dostring(vm, "print('hello')", "inline");
```

The `name` argument appears in error messages.  Pass `NULL` for
`"<string>"`.

### Compile now, execute later

```c
CandoChunk *chunk = NULL;
if (cando_loadstring(vm, src, "config", &chunk) != CANDO_OK) {
    fprintf(stderr, "%s\n", cando_errmsg(vm));
    return;
}

cando_vm_exec(vm, chunk);          // execute
// optionally run it again, or inspect chunk->code
cando_chunk_free(chunk);
```

## Error handling

Every load-or-run function returns one of:

| Code | Meaning |
|---|---|
| `CANDO_OK` (0) | Success |
| `CANDO_ERR_FILE` (1) | File could not be opened or read |
| `CANDO_ERR_PARSE` (2) | Syntax / compile error |
| `CANDO_ERR_RUNTIME` (3) | Uncaught runtime error |

On any non-zero result, `cando_errmsg(vm)` returns a human-readable
message.  The pointer is owned by the VM and valid until the next call
that writes `vm->error_msg`.

```c
int rc = cando_dofile(vm, path);
switch (rc) {
    case CANDO_OK:          break;
    case CANDO_ERR_FILE:    report("cannot read %s", path); return 1;
    case CANDO_ERR_PARSE:   report("syntax: %s", cando_errmsg(vm)); return 1;
    case CANDO_ERR_RUNTIME: report("runtime: %s", cando_errmsg(vm)); return 1;
}
```

You usually want to **open a fresh VM per script run** if you care
about isolating globals.  Alternatively, save and restore globals
yourself.

## Global variables from C

Set a global before a script runs:

```c
cando_vm_set_global(vm, "DEBUG",   cando_bool(true),    /*is_const=*/false);
cando_vm_set_global(vm, "VERSION", cando_number(1.2),   /*is_const=*/true);
```

Read a global after:

```c
CandoValue score;
if (cando_vm_get_global(vm, "score", &score) && cando_is_number(score)) {
    printf("score = %g\n", score.as.number);
}
```

`cando_vm_set_global` with `is_const = true` prevents the script from
reassigning the binding.

## Creating objects for scripts

Use the bridge layer to build objects, arrays, and strings in C and
expose them as globals:

```c
#include "vm/bridge.h"

CandoValue cfg_val   = cando_bridge_new_object(vm);
CdoObject *cfg       = cando_bridge_resolve(vm, cfg_val.as.handle);

/* Fields via the raw object API. */
CdoString *k = cdo_string_intern("width", 5);
cdo_object_rawset(cfg, k, cdo_number(1920.0), FIELD_NONE);
cdo_string_release(k);

cando_vm_set_global(vm, "config", cfg_val, true);
```

Arrays work the same way via `cando_bridge_new_array` plus
`cdo_array_push`.

## Registering native functions

Native functions have the prototype:

```c
int my_fn(CandoVM *vm, int argc, CandoValue *args);
```

The return value is the number of values pushed onto the VM stack, or
`-1` to signal an error (after calling `cando_vm_error`).

```c
static int native_add(CandoVM *vm, int argc, CandoValue *args) {
    double a = argc > 0 && cando_is_number(args[0]) ? args[0].as.number : 0;
    double b = argc > 1 && cando_is_number(args[1]) ? args[1].as.number : 0;
    cando_vm_push(vm, cando_number(a + b));
    return 1;
}

cando_vm_register_native(vm, "add", native_add);
```

A full walkthrough — including multi-value returns, errors, and
packaging as a reusable library module — is in
[writing-extensions.md](writing-extensions.md).

## Calling script functions from C

```c
CandoValue fn;
if (cando_vm_get_global(vm, "on_event", &fn)) {
    CandoValue args[2] = {
        cando_string_value(cando_string_new("click", 5)),
        cando_number(3.14),
    };
    int n = cando_vm_call_value(vm, fn, args, 2);
    // n return values sit on top of vm->stack.
    for (int i = n - 1; i >= 0; i--) {
        CandoValue v = cando_vm_pop(vm);
        // … consume v …
    }
}
```

`cando_vm_call_value` is safe to invoke from inside a native function
(it re-enters the dispatch loop).  It returns 0 if the value is not
callable.

## Thread safety

One `CandoVM` is owned by one OS thread at a time — the main thread.
The spawned threads created by `thread { … }` get their own child
`CandoVM` wired up to the parent's globals and handle table with
proper reader-writer locking.  You do not need to serialise access
from C unless you deliberately share a VM pointer between threads.

If you want cross-process isolation, run multiple `CandoVM`s —
either in the same thread (round-robin) or one per worker thread.

## Cleaning up

Resources cleaned up by `cando_close`:

- The value stack, call frames, upvalue list
- The global environment
- The module cache (including any `dlopen` handles)
- The thread registry (after all threads have joined)
- The VM's handle table

When the last live VM closes, the global string intern table and
cached meta-key strings are also destroyed.

## Minimal complete example

```c
#include <cando.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }

    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    int rc = cando_dofile(vm, argv[1]);
    if (rc != CANDO_OK) {
        fprintf(stderr, "%s\n", cando_errmsg(vm));
    }

    cando_close(vm);
    return rc == CANDO_OK ? 0 : 1;
}
```

That is, verbatim, the job done by `source/main.c` — the `cando` CLI
itself is just this plus a `--disasm` option.
