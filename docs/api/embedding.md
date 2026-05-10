# Embedding CanDo

This page is the practical guide to running CanDo from C.  For the
exhaustive list of every symbol the library exports, see
[reference.md](reference.md).

## The minimum viable host

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();           // 1
    cando_openlibs(vm);                    // 2

    int rc = cando_dofile(vm, "main.cdo"); // 3
    if (rc != CANDO_OK)
        fprintf(stderr, "%s\n", cando_errmsg(vm));

    cando_close(vm);                       // 4
    return rc == CANDO_OK ? 0 : 1;
}
```

1. **Allocate a VM.**  `cando_open` returns a fresh `CandoVM *`.  Each
   VM is independent: separate global tables, separate intern table,
   separate handle space.  You can run multiple VMs in one process —
   they don't share state.
2. **Open standard libraries.**  `cando_openlibs(vm)` registers every
   library the CLI does (`math`, `file`, `string`, `array`, `thread`,
   `socket`, `http`, `https`, …).  If you want a smaller subset, skip
   this and call `cando_open_<name>lib(vm)` for each one you need.
3. **Run a script.**  `cando_dofile` parses, compiles, and executes
   the file.  Return value is `CANDO_OK`, `CANDO_ERR_FILE`,
   `CANDO_ERR_PARSE`, or `CANDO_ERR_RUNTIME`.
4. **Free the VM.**  `cando_close` frees every resource the VM owns.

Compile with:

```bash
gcc myapp.c -lcando -Iinclude -o myapp
```

If you have CMake, add `target_link_libraries(myapp PRIVATE cando)`
after `find_package(cando)` (or after CanDo's `add_subdirectory`).

## Running scripts from a string

```c
const char *src =
    "print('hello from a buffer');\n"
    "RETURN 42;\n";

if (cando_dostring(vm, src, "<embedded>") != CANDO_OK)
    fprintf(stderr, "%s\n", cando_errmsg(vm));
```

The third argument is the chunk name — used in stack traces and error
messages to identify where the source came from.

## Selective library opening

Skip `cando_openlibs(vm)` and pull in only what you need:

```c
cando_open_mathlib(vm);
cando_open_stringlib(vm);
cando_open_arraylib(vm);
cando_open_jsonlib(vm);
```

This is useful when you're embedding CanDo into an environment where
you want to forbid filesystem or network access — just don't open the
library that exposes them.

A full list of openers is in [reference.md](reference.md).

## Passing command-line arguments

```c
int main(int argc, char **argv) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    cando_set_args(vm, argc, argv);     // populate the global `args`

    int rc = cando_dofile(vm, argv[1]);
    cando_close(vm);
    return rc;
}
```

Inside the script, the `args` global is an array of the strings.

## Error handling

Every "do" entry point returns one of:

| Code                   | Meaning |
|------------------------|---------|
| `CANDO_OK` (0)         | Success. |
| `CANDO_ERR_FILE` (1)   | I/O error opening or reading the source file. |
| `CANDO_ERR_PARSE` (2)  | Lex / parse / compile error. |
| `CANDO_ERR_RUNTIME` (3)| Runtime error or uncaught `THROW`. |

After a non-`OK` return, `cando_errmsg(vm)` returns a heap-allocated
formatted error string that stays valid until the next entry-point
call.

```c
if (cando_dofile(vm, path) != CANDO_OK) {
    fprintf(stderr, "[cando] %s\n", cando_errmsg(vm));
    cando_close(vm);
    return 1;
}
```

## Threading model

The VM is **thread-aware**.  Multiple OS threads can execute script
code concurrently inside the same VM as long as they were spawned by
that VM's threading machinery (`thread { … }`, `thread.spawn`, etc.).

You should **not** call `cando_dofile` / `cando_dostring` from multiple
host threads on the same VM — those entry points re-enter the
top-level frame.  Use one of:

- **One VM per host thread.**  Independent VMs are the simplest model.
- **Drive concurrency from inside the script.**  Call a single
  top-level entry that spawns CanDo threads itself.  This is what the
  `cando` CLI does.

GC and the value system are safe for concurrent script execution.

## Lifecycle and ownership

- The VM owns every value created inside it.  Don't try to reach into
  the heap directly from C; instead use the natives API
  (see [extensions.md](extensions.md)) which goes through the bridge
  layer.
- `cando_close(vm)` cancels any active script threads, runs all
  outstanding `FINALY` blocks, and frees memory.
- Native callbacks registered into a VM may be called from any thread
  spawned by that VM.  Make them re-entrant.

## Embedding a CanDo REPL

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    char line[4096];
    for (;;) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin)) break;

        if (cando_dostring(vm, line, "<repl>") != CANDO_OK)
            fprintf(stderr, "%s\n", cando_errmsg(vm));
    }
    cando_close(vm);
}
```

## Versioning

```c
printf("%s\n", cando_version());        // "1.0.0"
printf("%d\n", cando_version_num());    // 10000
```

`cando_version_num()` is `major*10000 + minor*100 + patch` — convenient
for `#if`-style version gating.

## Next steps

- Add custom natives to expose host functions to scripts:
  [extensions.md](extensions.md).
- Look up a specific function in [reference.md](reference.md).
- Review the [AI-GUIDE](../AI-GUIDE.md) if you're writing automation
  that edits CanDo C code.
