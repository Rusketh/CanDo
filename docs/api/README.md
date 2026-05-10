# Embedding API

CanDo is built to be embedded.  The runtime is exposed as a single
header (`include/cando.h`) and a shared library (`libcando.so` /
`.dylib` / `.dll`) — link against the library, include the header, and
you have a complete embeddable scripting language.

This directory is the embedder's documentation.

## Pages

| Page                              | Topic |
|-----------------------------------|-------|
| [embedding.md](embedding.md)      | The lifecycle: opening a VM, running scripts, exchanging values, error handling. |
| [reference.md](reference.md)      | Reference for every public symbol in `cando.h`. |
| [extensions.md](extensions.md)    | Native functions, in-process libraries, and binary extension modules. |

## Repository pointers

- **`include/cando.h`** — the only public header.  If a symbol isn't
  declared here, treat it as private.
- **`source/cando_lib.c`** — implementation of the public API.
- **`modules/`** — production examples of binary extension modules.

## Hello world

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    if (cando_dofile(vm, "game/main.cdo") != CANDO_OK)
        fprintf(stderr, "%s\n", cando_errmsg(vm));

    cando_close(vm);
    return 0;
}
```

Build:

```bash
gcc myapp.c -lcando -Iinclude -o myapp
```

That's the whole minimum-viable embedding.  See [embedding.md](embedding.md)
for everything else — passing arguments, exchanging values, registering
custom natives, structuring host-script integrations.
