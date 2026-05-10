# C API Reference

This is the reference for every public symbol declared in
[`include/cando.h`](../../include/cando.h).  The header is the contract:
nothing not declared there is part of the embedding API.

## Version

```c
#define CANDO_VERSION_MAJOR 1
#define CANDO_VERSION_MINOR 0
#define CANDO_VERSION_PATCH 0
#define CANDO_VERSION       "1.0.0"
#define CANDO_VERSION_NUM   10000   /* major*10000 + minor*100 + patch */

const char *cando_version(void);    /* returns CANDO_VERSION */
int         cando_version_num(void); /* returns CANDO_VERSION_NUM */
```

## Error codes

Returned by every "do" / "load" entry point.

| Constant              | Value | Meaning |
|-----------------------|-------|---------|
| `CANDO_OK`            | 0     | Success. |
| `CANDO_ERR_FILE`      | 1     | I/O error reading the source. |
| `CANDO_ERR_PARSE`     | 2     | Syntax / compile error. |
| `CANDO_ERR_RUNTIME`   | 3     | Runtime error or uncaught `THROW`. |

## State lifecycle

```c
CandoVM *cando_open(void);
void     cando_close(CandoVM *vm);
```

`cando_open` allocates a fresh VM (separate globals, intern table,
handle space) and returns it.  `cando_close` cancels any active script
threads, runs `FINALY` blocks, and frees the VM.

## Standard library openers

`cando_openlibs` opens everything; the per-library openers exist for
embedders that want a smaller surface.

```c
void cando_openlibs(CandoVM *vm);    /* opens every library */

void cando_open_mathlib(CandoVM *vm);
void cando_open_filelib(CandoVM *vm);
void cando_open_stringlib(CandoVM *vm);
void cando_open_arraylib(CandoVM *vm);
void cando_open_objectlib(CandoVM *vm);
void cando_open_jsonlib(CandoVM *vm);
void cando_open_csvlib(CandoVM *vm);
void cando_open_yamllib(CandoVM *vm);
void cando_open_threadlib(CandoVM *vm);
void cando_open_oslib(CandoVM *vm);
void cando_open_datetimelib(CandoVM *vm);
void cando_open_cryptolib(CandoVM *vm);
void cando_open_processlib(CandoVM *vm);
void cando_open_netlib(CandoVM *vm);
void cando_open_socketlib(CandoVM *vm);
void cando_open_secure_socketlib(CandoVM *vm);
void cando_open_evallib(CandoVM *vm);
void cando_open_includelib(CandoVM *vm);
void cando_open_httplib(CandoVM *vm);
void cando_open_httpslib(CandoVM *vm);
void cando_open_metalib(CandoVM *vm);
void cando_open_streamlib(CandoVM *vm);
```

After opening a library the corresponding namespace and any global
functions are visible to scripts.  Each opener is idempotent; calling
it twice is harmless.

## Loading and executing

```c
int cando_dofile  (CandoVM *vm, const char *path);
int cando_dostring(CandoVM *vm, const char *src,  const char *name);
int cando_loadstring(CandoVM *vm, const char *src, const char *name,
                     CandoChunk **chunk_out);
```

- `cando_dofile` reads `path`, parses, compiles, and runs.  Return is
  one of the error codes above.
- `cando_dostring` does the same for an in-memory source buffer.
  `name` is used in stack traces.
- `cando_loadstring` parses and compiles only.  `*chunk_out` is set to
  the compiled chunk; you can then re-execute it on demand.  Returns
  `CANDO_OK` or `CANDO_ERR_PARSE`.

```c
int cando_set_args(CandoVM *vm, int argc, char **argv);
```

Populate the script-visible global `args` array.

## Error inspection

```c
const char *cando_errmsg(CandoVM *vm);
```

Returns a formatted error message describing the most recent failure.
Valid until the next entry-point call.  After a successful run, the
message is `""`.

## Included sub-APIs

`cando.h` includes a curated set of internal headers so embedders can
build their own natives:

```c
#include <cando/core/common.h>
#include <cando/core/value.h>
#include <cando/vm/vm.h>
#include <cando/vm/bridge.h>
#include <cando/vm/chunk.h>
#include <cando/vm/debug.h>
#include <cando/object/object.h>
#include <cando/object/array.h>
#include <cando/object/string.h>
#include <cando/parser/parser.h>
#include <cando/natives.h>
```

These give you the types you need to write a native:

| Type                         | Defined in                | Role |
|------------------------------|---------------------------|------|
| `CandoVM`                    | `vm/vm.h`                 | The VM state. |
| `CandoValue`                 | `core/value.h`            | Tagged value used on stack and in calls. |
| `CdoObject`                  | `object/object.h`         | Heap object. |
| `CdoString`, `CdoArray`      | `object/string.h`, `array.h` | Concrete object subtypes. |
| `CandoChunk`                 | `vm/chunk.h`              | Compiled bytecode unit. |
| `CandoOpcode`                | `vm/opcodes.h`            | Bytecode instruction enum. |
| `CandoToken`                 | `parser/parser.h`         | Lexer output. |
| `CandoNativeFn`              | `natives.h`               | Native-callable signature. |

For details on writing natives — including the value/object bridge
that's central to operating on script values from C — see
[extensions.md](extensions.md).

## What is *not* in the API

The following are intentionally not part of the public API:

- The **GC interface**.  Allocation timing is owned by the runtime;
  reach for `gc.collect()` from script code if you need to force a
  collection.
- The **opcode emission helpers** (`emit_op` etc.).  These are reserved
  for the parser; embedders that want to programmatically build chunks
  should use `cando_dostring` against generated source instead.
- The **JIT internals**.  The JIT is observable through `jit.stats()`
  but otherwise opaque.
- Anything under `source/compat/`.  These shims are for the runtime's
  use only.
