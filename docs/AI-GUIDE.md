# AI Assistant Guide to CanDo

This document is intended for AI coding assistants editing the CanDo
repository.  Reading it once at the start of a session will save you from
the most common mistakes and dead ends.

If you are a human, read the [project Readme](../Readme.md) and the
[documentation index](README.md) first; this document assumes you already
have that orientation and focuses on workflow guidance specific to AI
edits.

---

## 1. Repository orientation in 60 seconds

```
include/cando.h       single public header — the C API surface
source/
  core/               CandoValue, memory, handle table, atomics, locks
  object/             CdoObject, arrays, strings, functions, threads
  parser/             lexer.c, parser.c (Pratt), scopes, bytecode emit
  vm/                 vm.c (dispatch loop), bridge.c, chunk.c, debug.c
  jit/                trace recorder + IR + native codegen (gated by `--jit`)
  lib/                <name>.c + <name>.h per stdlib namespace
                      (a few larger libs split into <name>_*.c -- see
                       console_term.c / console_input.c / ...)
  natives.c, natives.h        print, type, toString, inspect
  cando_lib.c                 cando_open / cando_dofile / cando_openlibs
  main.c                      CLI entry point
  compat/                     Windows / POSIX shims
modules/                      binary extension modules (one dir each)
tests/
  test_*.c            C unit tests
  scripts/*.cdo       integration scripts
  integration/        runner that executes every script
docs/                 documentation (this directory)
```

**Public API is `include/cando.h` only.**  Anything else under `source/`
is private: feel free to refactor freely as long as the symbols listed
in `cando.h` keep their signatures.

---

## 2. The two-layer value system (read this first)

CanDo has **two parallel value representations**.  Confusing them is the
single biggest source of bugs in this codebase.

### Layer 1 — `CandoValue` (the VM/script layer)

A small tagged union used on the VM stack and in native function calls.
Lives in `source/core/value.h`.

- For numbers, booleans, and `null`, the value is stored inline.
- For strings, the value is the interned string pointer.
- **For heap objects (arrays, plain objects, functions, threads),
  the value is a `HandleIndex` (u32) — not a pointer.**

This is intentional.  The GC must be able to relocate objects between
heap regions without invalidating live values held by other threads.
The handle table is the indirection that makes that safe.

### Layer 2 — `CdoObject` (the heap layer)

The actual heap-resident data.  Lives in `source/object/object.h`.  An
object owns:

- A hash table of `key → value` entries (used for plain object fields,
  class methods, prototype `__index`, meta-methods).
- A dense `values[]` array for integer-indexed storage (used by arrays).
- An `ObjectKind` tag distinguishing plain object, array, function,
  native, thread, etc.

### The bridge

`source/vm/bridge.c` and `bridge.h` translate between layers.  When you
pass a `CandoValue` to a function that operates on `CdoObject*`, you go
through the bridge — `bridge_resolve_object(vm, value)` returns the
`CdoObject*` for a given handle, or `NULL` if the value is not an
object.

**Rule of thumb when writing native code:**
- Read arguments as `CandoValue`.
- If you need to inspect or mutate an object, resolve to `CdoObject*`
  through the bridge, do your work, then return a `CandoValue` (which
  may be `cando_value_object(handle)` — never a raw pointer).
- Never store `CdoObject*` across a call that might trigger GC.  Store
  the handle and re-resolve.

---

## 3. Adding a stdlib function

The pattern is uniform across `source/lib/*.c`.  A worked example is in
[`api/extensions.md`](api/extensions.md); the short version:

1. Write a `static CandoValue my_fn(CandoVM *vm, int argc, CandoValue *argv)`.
2. Add it to the file's `static const CandoNative funcs[]` table at the
   bottom.
3. The `cando_open_<name>lib(vm)` opener is what registers the table —
   if you are adding a new file rather than a new function, the opener
   needs to call `libutil_register(vm, name, funcs, count)`.
4. If the new opener is meant to be loaded by `cando_openlibs`, add it
   to the list in `source/cando_lib.c`.
5. Add the prototype to `cando.h` only if it should be a public
   embedder entry point.

When you add a method that must be reachable through `obj:method()`,
register it on the appropriate `_meta.<type>` subtable (search for
`libutil_set_method` in the file you're working in for examples).

---

## 4. Adding a VM opcode

This is rarer; do it only when the change cannot be expressed with
existing opcodes.

1. Append the new opcode to `source/vm/opcodes.h` in the right band
   (each band groups related instructions; see the file's header
   comment).  Bands aren't load-bearing, but keeping them ordered makes
   diffs and disassembly readable.
2. Add a row to the opcode metadata table in `source/vm/opcodes.c`:
   name, operand format (`OPFMT_NONE`, `OPFMT_A`, `OPFMT_A_B`), effect
   class (`EFFECT_PURE`, `EFFECT_LOAD`, …).
3. Implement the dispatch case in `source/vm/vm.c`.  Look at an existing
   opcode in the same band as a template — they all follow the same
   prologue/epilogue convention with respect to `ip`, the value stack,
   and call-frame state.
4. Emit the opcode from the parser (`source/parser/parser.c`).  Use
   `emit_op`, `emit_op_a`, or `emit_op_ab` from
   `source/parser/emit.h`.
5. If the opcode is meant to be JITable, add a translator in
   `source/jit/`.  Otherwise mark it as an abort point so traces stop
   cleanly when they encounter it.

Every change to `opcodes.h` is **a binary-format change**.  If anyone
ships pre-compiled `.cdc` chunks (currently nobody does, but the format
is stable), you would have to bump the chunk-format version.

---

## 5. Adding a binary module

See [`api/extensions.md`](api/extensions.md) for the binary-module
contract.  The high-level flow:

1. Create `modules/<name>/`.
2. Write `<name>_module.c` that exports
   `CandoValue cando_module_init(CandoVM *vm)`.
3. Write a `Makefile` that links a single shared library (`<name>.so`,
   `.dylib`, `.dll`).
4. Add unit tests as `test_<name>.c` and integration tests as
   `test_<name>.cdo`.
5. Add `<name>` to the `MODULES =` list in the top-level `Makefile`
   and `CMakeLists.txt`.
6. Write `modules/<name>/README.md` documenting the script-facing
   surface.  See an existing module's README as a template.
7. Add a row to the module index in `modules/README.md`.

---

## 6. Conventions to keep

### C code

- C11.  No platform-specific extensions outside of `source/compat/`.
- `// single-line` comments are fine.  Doxygen-style headers are not
  used.
- Functions exported across translation units are `_Noreturn`,
  `static inline`, or plain — never `inline` without `static`.
- Public API symbols (declared in `cando.h`) start with `cando_`.
  Private cross-translation-unit symbols start with the subsystem
  name: `vm_`, `parser_`, `bridge_`, `gc_`, etc.

### Script code

- Keywords are case-insensitive but **must not be mixed-case**.  `VAR`
  and `var` are both keywords; `Var` is an identifier.  All-uppercase is
  the canonical style.
- `FINALY` is spelled with one `L`.  `FINALLY` is **not** accepted.
- Test scripts under `tests/scripts/` use **all-uppercase** keywords by
  convention.  Match that style when writing new ones.

### Tests

- C unit tests sit next to the code and are picked up automatically by
  the top-level `Makefile`.  Add new ones by following the
  `test_<area>.c` naming convention.
- Integration scripts go in `tests/scripts/`.  The runner in
  `tests/integration/` executes every `.cdo` it finds and diffs the
  output against `expected/<name>.txt` if one exists.
- `make test` runs both layers.

---

## 7. Cross-cutting things that bite people

### Strings

- Strings are immutable and reference-counted.  Methods that "modify"
  strings always return a new string.
- The `+` operator concatenates strings.  Mixing strings with non-string
  operands without a `__add` metamethod is a runtime error.  When in
  doubt, call `toString(v)` explicitly.
- `#s` returns the byte length, not the codepoint count.  String
  contents are UTF-8 but the runtime treats them as byte arrays for
  length, indexing, and substring purposes.

### Numbers

- The single numeric type is IEEE-754 `double`.  `42`, `42.0`, and
  `4.2e1` are all the same value.
- Bitwise operators (`&`, `|`, `|&`, `<<`, `>>`) coerce to `int64_t`,
  apply the operation, and coerce back to `double`.
- `^` is **power**, not XOR.  Bitwise XOR is `|&`.

### Arrays vs objects

- `[1, 2, 3]` is an array (dense, integer-indexed).
- `{ a: 1 }` is a plain object (hash, ordered by insertion).
- An array can also have named fields — they're stored in the same hash
  table that objects use.  This is intentional and useful, but can
  surprise you when iterating: `FOR k IN arr` walks named keys; `FOR v
  OF arr` walks integer-indexed values.

### Threads

- `thread { … }` and `thread.spawn(fn)` both spawn a real OS thread.
- `await t` joins.  It blocks the calling thread until `t` finishes and
  returns whatever `t` returned (or rethrows whatever `t` threw).
- Sharing mutable state between threads requires the script to
  cooperate: take `object.lock(o)` / `object.unlock(o)`, or use
  `stream.channel()` for message-passing.
- The GC is concurrent-safe; the VM is not single-threaded under the
  hood.  Don't add a "while we're at it" assumption that there is one
  active script at a time.

### `_meta` is mutable

`_meta.<type>` holds the prototype object for built-in types.  Anything
you write into `_meta.string`, `_meta.array`, `_meta.tcp_socket`, etc.
becomes a method on every existing and future instance of that type
immediately.  This is intentional — but it means a poorly chosen method
name can collide with stdlib internals.  Stick to the documented
extension points.

### Globals are implicit on assignment

Inside a function body, `x = 1` (no `VAR`) assigns a **global**.  This
is intentional, but it traps people coming from languages where
unqualified assignment is local.  Always write `VAR x = 1` for locals.

### `CONST` is a binding rule, not deep immutability

`CONST t = { count: 0 }` prevents reassigning `t`, but `t.count = 1` is
allowed.  Use `object.lock` if you need real immutability across
threads.

---

## 8. When in doubt

- The parser is in `source/parser/parser.c`.  Search there first when a
  syntax question is unclear.
- Opcode semantics are in `source/vm/vm.c`'s dispatch loop.  Each case
  is a few lines; it is the source of truth for evaluation order.
- The `tests/scripts/` directory has hundreds of small `.cdo` files.
  When adding a new feature, write a script that exercises it and put
  it there.  When investigating a question of "is this allowed?" the
  fastest path is often to write a five-line script and run it.
- For "does the C API expose this?", grep `include/cando.h` directly —
  it is the contract.

If you change a public-facing behaviour, update the relevant docs in
this folder in the same change.  Per-library reference lives in
[libraries/](libraries/README.md); language behaviour in
[language/](language/README.md); embedding and extensions in
[api/](api/README.md).
