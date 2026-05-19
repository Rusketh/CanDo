# `cffi` module — proposal

A binary include module that lets CanDo scripts call into arbitrary
C-ABI shared libraries at runtime — the same role LuaJIT's `ffi`,
Python's `ctypes`, or Node's `koffi` play in their respective
ecosystems.  Loaded with `include("./modules/cffi/cffi")` exactly like
`ldap` and `forms`.

The goal of this document is to fix the **script-facing API** by
showing how users will write code against it.  Implementation notes
follow at the end.

---

## 1. Loading

```cdo
VAR ffi = include("./modules/cffi/cffi");
```

Requires `libffi` at runtime on POSIX; Windows ships its own
fallback.  See "Platforms" below.

---

## 2. Worked examples

These are the shape the module is being designed to support.  Each
example is a complete, copy-pasteable script.

### 2.1 Calling a standard libc function

```cdo
VAR ffi = include("./modules/cffi/cffi");

VAR libc = ffi.open("libc.so.6");          // or "msvcrt.dll" on Windows

VAR getpid = libc:bind("getpid", "int (void)");
print(`pid = ${getpid()}`);

VAR atoi = libc:bind("atoi", "int (const char*)");
print(atoi("42"));                          // 42
```

`libc:bind(name, signature)` parses the C-style signature once and
returns a callable.  Calls are ordinary CanDo function calls.

### 2.2 Working with structs

```cdo
VAR ffi = include("./modules/cffi/cffi");
VAR libc = ffi.open("libc.so.6");

// Declare a struct.  The body is plain C grammar; the return value
// is a type descriptor that doubles as a constructor.
VAR timeval = ffi.struct("struct timeval { long tv_sec; long tv_usec; }");

VAR gettimeofday = libc:bind("gettimeofday",
    "int (struct timeval*, void*)");

VAR tv = timeval.new();                     // zeroed instance
gettimeofday(tv, NULL);
print(`${tv.tv_sec}.${tv.tv_usec}`);
```

Field access is a property on the script-facing handle; reads and
writes go through the type descriptor.

### 2.3 Pointers and buffers

```cdo
VAR ffi = include("./modules/cffi/cffi");

// Allocate an off-heap byte buffer.  The returned value is an opaque
// handle that the GC tracks; it is freed when the last reference goes
// away (or sooner via :free()).
VAR buf = ffi.malloc(1024);

buf:write_u32(0, 0xdeadbeef);
print(buf:read_u32(0):toHex());             // "deadbeef"

// Reinterpret the same memory through a typed pointer.
VAR u32p = ffi.cast("uint32_t*", buf);
print(u32p[0]:toHex());
u32p[1] = 0xfeedface;

buf:free();                                 // optional, GC will too
```

`buf:slice(offset, len)` returns a no-copy view.  `buf:toString()`
copies the bytes out as a CanDo string.

### 2.4 Strings in / strings out

CanDo strings are immutable and reference-counted; pointers handed to
C live as long as the call.

```cdo
VAR strlen = libc:bind("strlen", "size_t (const char*)");
print(strlen("hello"));                     // 5

VAR strdup = libc:bind("strdup", "char* (const char*)");
VAR p = strdup("copy me");
print(ffi.string(p));                        // "copy me"  -- copies into a CanDo string
libc:bind("free", "void (void*)")(p);
```

`ffi.string(ptr, len*)` copies a C string into a CanDo string.  Without
`len` it reads up to a NUL.

### 2.5 Callbacks (C → CanDo)

```cdo
VAR ffi = include("./modules/cffi/cffi");
VAR libc = ffi.open("libc.so.6");

VAR qsort = libc:bind("qsort",
    "void (void*, size_t, size_t, int (*)(const void*, const void*))");

VAR arr = ffi.array("int", [9, 3, 7, 1, 5]);

VAR cmp = ffi.callback("int (const void*, const void*)", FUNCTION(a, b) {
    VAR ia = ffi.cast("int*", a)[0];
    VAR ib = ffi.cast("int*", b)[0];
    RETURN ia - ib;
});

qsort(arr, #arr, ffi.sizeof("int"), cmp);
print(arr:toArray());                       // [1, 3, 5, 7, 9]

cmp:free();                                 // releases the trampoline
```

The callback is a real C function pointer (libffi closure or a
hand-rolled trampoline) that re-enters the VM on the calling thread.

### 2.6 Loading a third-party library

```cdo
VAR ffi = include("./modules/cffi/cffi");
VAR z = ffi.open("libz.so.1");

VAR crc32 = z:bind("crc32", "unsigned long (unsigned long, const char*, unsigned)");

VAR data = "the quick brown fox";
print(crc32(0, data, #data):toHex());
```

### 2.7 Header-style batch declaration

For larger surfaces, declare many symbols at once.  The argument is a
fragment of C (typedefs, structs, function prototypes); no
preprocessor, no `#include` resolution — just enough grammar to lift
declarations.

```cdo
VAR ffi = include("./modules/cffi/cffi");
VAR sdl = ffi.open("libSDL2-2.0.so.0");

sdl:declare(`
    typedef struct SDL_Window  SDL_Window;
    typedef struct SDL_Surface SDL_Surface;

    int   SDL_Init(unsigned flags);
    void  SDL_Quit(void);
    SDL_Window* SDL_CreateWindow(const char* title,
                                  int x, int y, int w, int h,
                                  unsigned flags);
    void  SDL_DestroyWindow(SDL_Window*);
    int   SDL_Delay(unsigned ms);
`);

sdl.SDL_Init(0x20);                          // SDL_INIT_VIDEO
VAR w = sdl.SDL_CreateWindow("hi", 100, 100, 640, 480, 0x4);
sdl.SDL_Delay(2000);
sdl.SDL_DestroyWindow(w);
sdl.SDL_Quit();
```

After `:declare()`, each function shows up as a property on the
library handle — no separate `bind` step.

### 2.8 Errors

```cdo
TRY {
    VAR bogus = ffi.open("./does-not-exist.so");
} CATCH (e) {
    print(`could not load: ${e}`);
}

TRY {
    libc:bind("nope_not_a_symbol", "int (void)");
} CATCH (e) {
    print(`missing symbol: ${e}`);
}
```

`dlerror()` / `GetLastError()` messages are forwarded verbatim.  Bad
signature strings throw a parse error pointing at the offending token.

---

## 3. API surface

### 3.1 Module-level

| Function | Returns | Description |
|---|---|---|
| `ffi.open(path)` | `Library` | `dlopen` / `LoadLibrary`.  `path` is searched the same way `include()` searches (cwd, then platform loader). |
| `ffi.current()` | `Library` | Handle for the host process — resolves symbols already linked in. |
| `ffi.struct(decl)` | `Type` | Declare a struct/union from a C fragment. |
| `ffi.typedef(decl)` | nothing | Register a typedef for later signature strings. |
| `ffi.sizeof(type)` | number | Bytes. |
| `ffi.alignof(type)` | number | Alignment. |
| `ffi.offsetof(type, field)` | number | Field offset. |
| `ffi.malloc(n)` / `ffi.calloc(n)` | `Buffer` | GC-tracked off-heap buffer. |
| `ffi.cast(type, value)` | typed pointer | Reinterpret an integer / buffer / pointer as a typed pointer. |
| `ffi.callback(sig, fn)` | `Callback` | Wrap a CanDo function as a C function pointer. |
| `ffi.array(elemType, init)` | `Buffer` | Allocate `Buffer` sized for `init` and populate. |
| `ffi.string(ptr, len*)` | string | Copy C bytes into a CanDo string. |
| `ffi.errno()` / `ffi.errno(n)` | number | Read / clear `errno` on the calling thread. |
| `ffi.NULL` | pointer | The null pointer constant. |

### 3.2 `Library`

| Method | Description |
|---|---|
| `lib:bind(name, signature)` | Return a callable bound to that symbol. |
| `lib:declare(c_fragment)` | Lift every prototype in `c_fragment` onto `lib` as a property. |
| `lib:symbol(name)` | Raw pointer to the symbol (for variables, vtables). |
| `lib:close()` | `dlclose`.  Idempotent.  All bindings derived from `lib` become invalid. |

### 3.3 `Buffer`

| Method | Description |
|---|---|
| `b:read_<u8\|i8\|u16\|i16\|u32\|i32\|u64\|i64\|f32\|f64>(offset)` | Typed scalar read. |
| `b:write_<…>(offset, value)` | Typed scalar write. |
| `b:slice(offset, len)` | No-copy view (keeps the parent alive). |
| `b:toString(offset*, len*)` | Copy bytes to a CanDo string. |
| `b:address()` | Numeric address (debugging / interop). |
| `b:free()` | Release immediately.  Subsequent access throws. |
| `#b` | Length in bytes. |

### 3.4 `Type` (struct / typedef)

| Method | Description |
|---|---|
| `t.new(init*)` | Zeroed instance, optionally populated from a CanDo object. |
| `t.size`, `t.align` | Layout. |
| `t.fields` | Array of `{ name, type, offset }`. |
| Instance `obj.field` | Property access — reads / writes through the descriptor. |

### 3.5 `Callback`

| Method | Description |
|---|---|
| `cb:free()` | Release the trampoline.  Calling C with a freed callback is undefined; the module guards against use-after-free by parking freed callbacks in a small dead-pool and throwing on entry. |

### 3.6 Type strings

Accepted in any `signature` / `type` argument:

- Primitive: `void`, `bool`, `char`, `signed char`, `unsigned char`,
  `short`, `unsigned short`, `int`, `unsigned int`, `long`,
  `unsigned long`, `long long`, `unsigned long long`, `float`,
  `double`, `size_t`, `ssize_t`, `intptr_t`, `uintptr_t`,
  `int8_t`…`int64_t`, `uint8_t`…`uint64_t`.
- Pointer: any of the above followed by `*`, possibly `const`-qualified.
- Struct: `struct Name` after `ffi.struct(...)`.
- Function pointer: `ret (args)` or `ret (*)(args)`.
- Array: `T[n]` (fixed size, decays to `T*` at call boundaries).

`const` is parsed and ignored at the ABI level — it exists only so
copy-pasted C headers tokenize without edits.

---

## 4. Memory & lifetime model

This is the bit that will go wrong if it isn't pinned down up front.

1. **`Buffer` is GC-tracked.**  The `CdoObject` carries a `__cffi_buf`
   slot keyed into a module-private pool (the pattern `ldap` uses for
   `__ldap_slot`, `source/lib/socket.c` for sockets).  When the object
   is collected the pool entry `free()`s the backing allocation.
2. **Pointers from C are *not* GC-tracked.**  `strdup`'s return is a
   raw `void*` wrapped in a typed-pointer value; if you don't pass it
   to `free` it leaks.  This matches every other FFI in existence and
   is documented loudly.
3. **CanDo strings handed to C are valid for the duration of one call
   only.**  The native shim pins the string for the call and unpins
   on return.  Storing the `const char*` past the call is a use-after.
4. **Callbacks pin their CanDo function.**  The closure object holds
   a strong reference to the function and the function's enclosing
   environment so the VM can re-enter cleanly.
5. **`Library:close()` invalidates all derived bindings.**  Calling a
   freed binding throws `"library was closed"`.  This is the
   pattern `ldap`'s `unbind` and `sqlite`'s `close` already follow.

The module never stores `CdoObject*` across a call — it stores the
handle index and re-resolves (the rule from `docs/AI-GUIDE.md` §2).

---

## 5. Platforms

| Platform | Backend |
|---|---|
| Linux / macOS | `libffi` (`-lffi`), `dlopen` / `dlsym` / `dlclose` |
| Windows | `libffi` if available; otherwise a tiny native trampoline for the four common ABIs (System V / SysV-x64, Win64, cdecl, stdcall).  `LoadLibraryW` / `GetProcAddress` / `FreeLibrary`. |

`libffi` is the canonical choice — covers every ABI we care about,
ships with every distro, has a stable API since 3.0.  Adding the
dependency keeps the module's implementation under ~1500 lines.

If `libffi` is genuinely undesirable, the alternative is to hand-roll
the call thunk for x86-64 SysV, x86-64 Win64, and AArch64 SysV — the
three live ABIs.  That's a few hundred lines of assembly per ABI,
which the rest of the runtime intentionally avoids; libffi pays for
itself immediately.

---

## 6. Files

Following the layout convention in `modules/README.md`:

```
modules/cffi/
  README.md            script-facing docs (mirrors the worked examples above)
  cando.api.json       LSP manifest (mirrors §3 of this plan)
  cffi_module.c        cando_module_init + every native
  cffi_types.c/.h      type-string parser, struct layout, sizeof/alignof
  cffi_marshal.c/.h    CandoValue <-> C value conversion
  cffi_callback.c/.h   libffi closure plumbing + dead-pool
  cffi_lib.c/.h        dlopen wrapper, symbol table, library handle pool
  Makefile             builds cffi.so / cffi.dylib / cffi.dll, links -lffi
  test_cffi.c          unit tests for the type parser and marshaller
  test_cffi.cdo        integration tests: libc.so getpid/strlen/qsort
  test_cffi_smoke.cdo  feature detect + load + close
```

`test_cffi.cdo` runs against `libc.so.6` on Linux, `libSystem.dylib`
on macOS, and `msvcrt.dll` on Windows — all present on every CI
runner.  No third-party libraries pulled into the test matrix.

Top-level wiring:
- Add `cffi` to `MODULES =` in `/Makefile`.
- Add `cffi` to `CMakeLists.txt`.
- Add a row to the index in `modules/README.md`.
- Add the build step to `.github/workflows/ci.yml`.

---

## 7. Implementation milestones

Each milestone is a separately-mergeable PR:

1. **Skeleton + module loader.**  Empty `cffi_module.c`,
   `ffi.open` / `ffi.current` / `lib:close`, no calling yet.  Smoke
   test loads `libc` and closes it.
2. **Primitive calls.**  Type-string parser for scalars + pointers,
   `lib:bind`, single-arg / single-return calls.  `test_cffi.cdo`
   covers `getpid`, `atoi`, `strlen`.
3. **Buffers and strings.**  `ffi.malloc` / `Buffer` methods,
   `ffi.string`, automatic C-string marshalling for `const char*`.
4. **Structs.**  `ffi.struct`, instance read/write, `sizeof` /
   `alignof` / `offsetof`.
5. **Callbacks.**  `ffi.callback`, `qsort` test.  This is the
   highest-risk milestone — schedule a review focused on the
   re-entrancy story (GC during a callback, throwing from inside a
   callback, multi-threaded calls).
6. **`:declare()` batch parser.**  Reuse the type-string parser; lift
   every prototype onto the library handle.
7. **Polish.**  `cando.api.json`, README, `make modules-windows`
   build, dark-corner tests (varargs, unions if we go that far).

Milestones 1-3 are the MVP.  4 unlocks most real-world C libraries.
5 unlocks GUI / async-callback libraries.

---

## 8. Open questions to settle before milestone 1

These are the calls that change the public surface, so worth pinning
down now:

1. **Varargs.**  `printf` family.  Easy to support (libffi has
   `ffi_prep_cif_var`); worth a `lib:bind_vararg(name, ret_type,
   fixed_args)` returning a closure that takes the variable arguments
   as a script-side array?
2. **Unions.**  Probably yes for completeness, with the same syntax as
   `ffi.struct`.
3. **Pointer arithmetic in script.**  `p + 4` is ambiguous (bytes or
   elements?) — propose `p:offset(n_elems)` and `p:byte_offset(n)`
   instead of overloading arithmetic.
4. **64-bit integers.**  CanDo numbers are `double` (52-bit mantissa).
   Returning `uint64_t` from C lossily fits in a number; on overflow
   we'd want a `BigInt`-style boxed integer.  Punt to a follow-up
   milestone; document the precision limit in the README.
5. **Thread affinity.**  Can a callback fire on a non-VM thread?  The
   safe answer is "no, you wrap the C-callee on the VM thread and any
   other thread re-entry throws."  Sufficient for the common case
   (qsort, GUI event loop on the main thread); insufficient for
   audio callbacks etc.  Explicitly document the limitation.

---

## 9. Why a binary module, not stdlib

`cffi` could in principle live in `source/lib/cffi.c` and ship in the
main binary.  Reasons not to:

- It pulls in `libffi`, which not every embedder wants.
- It is the textbook "load on demand" surface — most scripts never
  touch C bindings.
- The platform shim is non-trivial and benefits from being isolated
  in its own subtree, with its own CI matrix.

Loading as `include("./modules/cffi/cffi")` matches the precedent set
by every other module that depends on a system library
(`ldap` → libldap, `sql` → libmysql/libpq, etc.).
