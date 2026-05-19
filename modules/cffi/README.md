# `cffi` module

C foreign-function interface — load arbitrary C-ABI shared libraries
and call into them from CanDo scripts.  Built on `libffi` for
ABI-correct calls and `dlopen` / `LoadLibrary` for symbol resolution.

This is the role LuaJIT's `ffi`, Python's `ctypes`, and Node's `koffi`
play in their respective ecosystems.  The model is the smallest one
that works: load a library, name a symbol, give it a signature, call
it.

## Platforms

| Platform | Backend | Notes |
|---|---|---|
| Linux   | `libffi` + `dlopen` | `apt install libffi-dev` |
| macOS   | `libffi` + `dlopen` | ships in the SDK |
| Windows | `libffi` (statically linked) + `LoadLibrary` | no extra DLLs to ship |

## Loading

```cdo
VAR ffi = include("./modules/cffi/cffi");
```

## Quick example

Bind to `libc` and call a few functions:

```cdo
VAR ffi  = include("./modules/cffi/cffi");
VAR libc = ffi.load("libc.so.6");          // "libSystem.B.dylib" on macOS,
                                           // "msvcrt.dll" on Windows

// Direct call, signature given inline:
print(libc:call("getpid", "int()"));
print(libc:call("strlen", "size_t(const char*)", "hello"));   // 5

// Pre-bound, signature parsed once:
VAR strlen = libc:bind("strlen", "size_t(const char*)");
print(strlen:call("the quick brown fox"));                    // 19

libc:close();
```

## Module-level API

| Function | Description |
|---|---|
| `ffi.load(path)`            | Open a shared library.  Returns a library handle. |
| `ffi.load(NULL)` / `ffi.current()` | Resolve symbols from the current process. |
| `ffi.call(lib, name, sig, args…)` | Direct call.  Same as `lib:call(name, sig, args…)`. |
| `ffi.sizeof(type)`          | Bytes used by `type` (a C type string). |
| `ffi.alignof(type)`         | Alignment in bytes. |
| `ffi.alloc(nbytes)`         | Allocate a zeroed, GC-tracked buffer.  Returns a buffer handle. |
| `ffi.string(ptr, len*)`     | Copy bytes from `ptr` into a CanDo string.  Without `len`, reads up to a NUL. |
| `ffi.errno()` / `ffi.errno(n)` | Read or set `errno` on the calling thread. |

Constants:

| Name | Value |
|---|---|
| `VERSION`        | module version string |
| `SIZEOF_VOIDP`   | `sizeof(void*)` |
| `SIZEOF_LONG`    | `sizeof(long)` (4 on Windows LLP64, 8 on Linux/macOS LP64) |
| `SIZEOF_SIZE_T`  | `sizeof(size_t)` |

## Library handle (`lib`)

| Method | Description |
|---|---|
| `lib:call(name, sig, args…)` | Resolve `name` and call it with `sig`. |
| `lib:bind(name, sig)`        | Resolve + parse once; returns a binding handle. |
| `lib:close()`                | `dlclose`.  Idempotent.  Bindings derived from this library throw on subsequent calls. |

Both `lib:close()` and `ffi.close(lib)` work — every method on the
library handle is also callable function-style.

## Binding handle

A binding caches the resolved symbol and the parsed signature so
repeated calls don't re-resolve.

| Field / Method | Description |
|---|---|
| `bnd:call(args…)`  | Invoke the bound function. |
| `bnd.name`         | The symbol name (read-only). |
| `bnd.signature`    | The signature string (read-only). |

## Buffer handle

`ffi.alloc(n)` returns a buffer.  Pointers returned from C (e.g. from
`strchr`, `malloc`, `strdup`) are also buffer handles — the runtime
just doesn't know their length.

| Method | Description |
|---|---|
| `buf:size()`            | Length in bytes (0 for foreign pointers — runtime doesn't know). |
| `buf:address()`         | Numeric address — for debugging or interop with non-CanDo code. |
| `buf:isNull()`          | True if the pointer is NULL or the buffer was freed. |
| `buf:read(off, len)`    | Copy `len` bytes from `off` into a CanDo string. |
| `buf:read(len)`         | Shorthand for `buf:read(0, len)`. |
| `buf:read()`            | Read the whole buffer (owned buffers only). |
| `buf:write(bytes)`      | Copy `bytes` into the buffer at offset 0. |
| `buf:write(off, bytes)` | Copy `bytes` into the buffer at offset `off`. |
| `buf:free()`            | Release immediately.  Idempotent; subsequent reads throw. |

## Type strings

Used in every `sig` / `type` argument.  The grammar is plain C:

- Primitive: `void`, `bool`, `_Bool`, `char`, `signed char`,
  `unsigned char`, `short`, `unsigned short`, `int`, `unsigned int`,
  `long`, `unsigned long`, `long long`, `unsigned long long`,
  `float`, `double`, `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`,
  `uintptr_t`, `int8_t` … `int64_t`, `uint8_t` … `uint64_t`.
- Pointer: any of the above followed by `*`, with optional `const` /
  `volatile` / `restrict` qualifiers.  `char*` and `const char*` get
  string-friendly marshalling (CanDo strings convert automatically);
  every other pointer is opaque.
- Signature: `return_type ( arg_type , arg_type , ... )`.  Use
  `(void)` or `()` for no-arg.

Long-double is rejected — its width and ABI differ across the
platforms cffi targets.  Structs, unions, fixed-size arrays, and
function pointers are not yet supported.

## Argument conversion

| C parameter | What a script may pass |
|---|---|
| Integer (`int`, `size_t`, `uint8_t`, …) | a CanDo number or boolean |
| Floating-point (`float`, `double`) | a CanDo number |
| `const char*` / `char*` | a CanDo string (passed as the NUL-terminated bytes), `NULL`, or a buffer |
| `void*` / `T*` | a buffer handle, a CanDo string (raw bytes), `NULL`, or a number (raw address) |

A type mismatch raises a script-level error before C is entered.

## Examples

### Round-trip through `memcpy`

```cdo
VAR libc = ffi.load("libc.so.6");

VAR src = ffi.alloc(16);
src:write("abcdefghij");

VAR dst = ffi.alloc(16);
libc:call("memcpy", "void*(void*, const void*, size_t)", dst, src, 10);

print(dst:read(0, 10));        // "abcdefghij"

src:free();
dst:free();
```

### Reading a pointer return

```cdo
VAR p = libc:call("strchr", "char*(const char*, int)", "hello, world", 119);
IF p == NULL {
    print("not found");
} ELSE {
    print(ffi.string(p));      // "world"
}
```

### Reading `errno`

```cdo
ffi.errno(0);                                 // clear before the call
libc:call("atoi", "int(const char*)", "0x7f");
print(`errno after atoi: ${ffi.errno()}`);
```

## Errors

Every cffi function throws a script-level error on failure.  Common
shapes:

- `ffi.load("..."): dlopen failed: ...`        — bad path / missing library.
- `ffi.call: symbol "..." not found in library` — typo in the name.
- `ffi.call: bad signature "..." at offset N: ...` — signature parse error.
- `ffi.call: signature expects N arguments, got M` — argument-count mismatch.
- `ffi.call: argument K: expected ...`         — argument type mismatch.
- `ffi: library was closed`                    — using a binding after `:close()`.
- `ffi: buffer was freed`                      — passing a freed buffer to C.

Wrap calls in `TRY` / `CATCH` to handle these gracefully.

## What's not supported yet

This module covers the MVP — milestones 1–3 of the design doc in
[`PLAN.md`](PLAN.md).  Future milestones add:

- `ffi.new("Struct", …)` — declared struct types with field access.
- `ffi.callback(fn)` — pass a CanDo function as a C function pointer.
- `lib.declare(c_source)` — paste a C header fragment, every prototype
  becomes a property on the library handle.

Until those land, work around them with one of:
- Allocate a struct-sized buffer with `ffi.alloc(ffi.sizeof("..."))`
  and read fields out at fixed offsets via `buf:read(off, len)`.
- For "C calls back into me", wrap the C library in a helper `.c`
  that the cffi module can call into.

See [`PLAN.md`](PLAN.md) for the full roadmap.
