# `cffi` module — proposal

A binary include module for calling into arbitrary C-ABI shared
libraries from CanDo scripts.  Loaded with
`include("./modules/cffi/cffi")` exactly like `ldap` and `forms`.

The model is the one LuaJIT got right: **paste the C header,
then call the functions like they were CanDo functions.**  No
per-symbol bind step, no separate type objects, no `restype` /
`argtypes` ceremony.

---

## The driving example

A real one.  You have an image-processing script and need to load a
PNG.  CanDo's standard library doesn't decode PNG, but every system
ships `libpng` and the `stb_image` single-header library exists as a
tiny `.so` everywhere else.  You want to call into it.

Here is the whole script.  Read it top to bottom — every concept the
module needs is in here:

```cdo
VAR ffi = include("./modules/cffi/cffi");

// 1. Load the library and tell the runtime what's in it.
//    The string is plain C: copy it out of the header, paste it here.
VAR stb = ffi.load("./libstb_image.so");
stb.declare(`
    unsigned char* stbi_load(const char* filename,
                             int* x, int* y, int* channels,
                             int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
`);

// 2. Out-parameters: allocate something the C function can write into.
VAR w = ffi.new("int");
VAR h = ffi.new("int");
VAR c = ffi.new("int");

// 3. Call it like a normal CanDo function.
//    Strings auto-convert.  ffi.new("int") is passed as `int*`.
VAR pixels = stb.stbi_load("photo.png", w, h, c, 4);
IF pixels == NULL {
    THROW "decode failed";
}

print(`${w.value}x${h.value}, ${c.value} channels`);

// 4. Read the pixel bytes out.  pixels is a typed pointer to
//    unsigned char; treat it as a buffer.
VAR rgba = pixels:read(w.value * h.value * 4);

// 5. Hand the memory back.  No GC magic — C allocated it, we free it.
stb.stbi_image_free(pixels);

// `rgba` is now an ordinary CanDo byte string.
file.write("dump.bin", rgba);
```

That's the whole surface in one screen.  No other example introduces
any new concept; the rest of this doc just expands the pieces.

---

## What the script just did

Five mechanics, in order of appearance.

### 1. `ffi.load(path)` + `lib.declare(C source)`

`load` is `dlopen` / `LoadLibrary`.  `declare` parses the C text
once and attaches every prototype it sees as a property on `lib`.
After `stb.declare(...)`, `stb.stbi_load` *is* the function — calling
it does the FFI dance.

`declare` takes plain C.  Typedefs, structs, function prototypes,
`#define` of integer constants — all lifted.  No preprocessor, no
`#include` chasing.  You paste the parts you need from the upstream
header.  If a library has 200 functions and you use 4, you declare 4.

A library is a script value like any other: pass it around, store it
in a variable, close it with `lib:close()` when done.

### 2. `ffi.new("type", init*)` — a value C can write into

`ffi.new("int")` returns a one-word cell holding a zero `int`.  The
script side accesses it as `cell.value`; the C side sees `int*`.

It scales straight up to structs:

```cdo
stb.declare(`struct Color { unsigned char r, g, b, a; };`);
VAR red = ffi.new("Color", { r: 255, g: 0, b: 0, a: 255 });
print(red.r);                  // 255
red.a = 128;
```

Reads and writes go through the layout the declaration installed.  No
type objects in script — the *string* `"Color"` is the type identity,
because that's what's already in the C header.

For raw bytes, `ffi.new("char[1024]")` or the shorthand
`ffi.alloc(1024)`.  Both return a pointer; both are GC-tracked and
freed when the last reference drops.

### 3. Argument conversion is implicit

| C parameter | What a script passes |
|---|---|
| `const char*` | a CanDo string |
| `int`, `double`, `size_t`, … | a CanDo number |
| `bool` | a CanDo boolean |
| `int*`, `Foo*` | the value `ffi.new("int")` / `ffi.new("Foo")` returned |
| `void*` | any pointer or buffer |
| `Foo` (by value) | the value `ffi.new("Foo")` returned |
| `int (*)(int)` | a CanDo function — wrapped on the fly (see below) |
| `NULL` | the literal `NULL` |

If the conversion is impossible (string passed where an int is
expected, etc.), the call throws before C is entered.

### 4. Pointers act like buffers

The `unsigned char*` `stbi_load` returns is a typed pointer.  Pointers
support:

```cdo
p:read(n)              // copy n bytes out as a CanDo string
p:write(bytes)         // copy bytes in
p:slice(off, len)      // no-copy view that keeps p alive
p[i]                   // typed indexing (uses the element type)
p[i] = v               // typed write
p:address()            // numeric address, for debugging
#p                     // length, if it has one (ffi.alloc / ffi.new arrays do)
p == NULL              // null check
```

That's it.  There is no separate "Buffer" type; a `char*` from
`ffi.alloc(n)` and a `char*` from `stbi_load` use the same operations.

### 5. Lifetime is honest

The script `free`s what C allocated.  The runtime doesn't pretend it
knows what `stbi_image_free` does — it can't.  Memory that came in
through `ffi.new` / `ffi.alloc` *is* GC-tracked; memory C handed us is
not.  This is the same deal Lua, Python, and Node all settled on, and
trying to be cleverer than that always ends in tears.

If you forget to free, you leak.  The README says so in bold.

---

## Callbacks, the only other shape

Sometimes C calls *you*.  `qsort`, GUI event loops, audio buffers.
This is the only place the module does anything non-obvious, and it
hides behind the same calling convention as everything else: pass a
CanDo function where a function pointer is expected, the module wraps
it.

```cdo
VAR libc = ffi.load("libc.so.6");
libc.declare(`
    void qsort(void* base, size_t nmemb, size_t size,
               int (*compar)(const void*, const void*));
`);

VAR nums = ffi.new("int[6]", [5, 2, 8, 1, 9, 3]);

libc.qsort(nums, 6, ffi.sizeof("int"), FUNCTION(a, b) {
    RETURN a[0] - b[0];           // a and b are int* into nums
});

print(nums:toArray());            // [1, 2, 3, 5, 8, 9]
```

The wrapping is the libffi closure machinery — a real C function
pointer that re-enters the VM.  The closure is freed when the
enclosing call returns; long-lived callbacks (`atexit`, GTK signals)
use the explicit form `ffi.callback(fn)` which returns a pointer the
script must keep alive itself.

---

## API surface

Module-level:

| | |
|---|---|
| `ffi.load(path)` | open a shared library |
| `ffi.current()` | the host process's own symbols |
| `ffi.new(type, init*)` | allocate a value of `type` |
| `ffi.alloc(bytes)` | allocate a `bytes`-sized buffer; alias for `ffi.new("char[n]")` |
| `ffi.sizeof(type)` / `ffi.alignof(type)` / `ffi.offsetof(type, field)` | layout queries |
| `ffi.callback(fn)` | long-lived callback wrapper |
| `ffi.string(ptr, len*)` | copy C bytes into a CanDo string |
| `ffi.errno()` | read `errno` on this thread |
| `ffi.NULL` | the null pointer |

On a library handle:

| | |
|---|---|
| `lib.declare(c_source)` | parse declarations, lift functions onto `lib` |
| `lib.<name>(args…)` | call a declared function |
| `lib:symbol(name)` | raw pointer to a global (for variables, vtables) |
| `lib:close()` | `dlclose`.  Idempotent.  Closed libraries throw on call. |

On a pointer / `ffi.new` value:

| | |
|---|---|
| `p.field` | struct field read |
| `p.field = v` | struct field write |
| `p[i]` / `p[i] = v` | element indexing |
| `p:read(n)` / `p:write(bytes)` | byte-level I/O |
| `p:slice(off, len)` | no-copy view |
| `p:address()` | numeric address |
| `p:free()` | release early; otherwise GC does it |
| `#p` | element count (if known) |

For scalar cells (`ffi.new("int")` and friends), `.value` reads/writes
the single element — purely shorthand for `p[0]`.

That's the whole user-facing surface.

---

## Why this shape

A few specific choices that fall out of the example above:

- **One verb for declaration.**  Every other FFI splits "tell me the
  type" and "give me the function" into two steps.  This one doesn't.
  The C declaration *is* the binding.
- **Types are strings, not objects.**  The script's source of truth
  for "what is `Color`?" is the `declare` call, not a `ffi.struct`
  return value the user has to thread around.  This kills the
  `ffi.types.Color` namespace the previous draft had.
- **Pointer == buffer.**  Pointers and byte buffers were two concepts
  in the previous draft pulling the same weight.  Collapse them.
  `char* x = malloc(n)` and `ffi.alloc(n)` are the same thing.
- **No type cast operator.**  If you need to reinterpret memory,
  declare the right struct and `ffi.new` it over the pointer (`new`
  takes an optional pointer-to-cast-from second form).  Skipping the
  ambient `ffi.cast` removes a footgun and a screen of doc.
- **Implicit callback wrapping at call sites.**  The 80% case (one-
  shot callbacks like `qsort`) reads like ordinary code.  The 20%
  case (long-lived callbacks) opts into explicit `ffi.callback`.

Match what the user already knows from reading C, do not invent a
parallel script-side type system on top of it.

---

## Lifetime, written out

Three categories of memory, three rules:

1. **`ffi.new` / `ffi.alloc` — GC-tracked.**  The pool slot pattern
   from `ldap_module.c` / `source/lib/socket.c`.  When the wrapping
   `CdoObject` is collected, the backing allocation is freed.
2. **Returned from C as `T*` — *not* GC-tracked.**  The script must
   pass it to the matching C `free`-style function.  The module does
   not guess.
3. **CanDo strings as `const char*` — pinned for one call.**  The
   shim pins the string before the call and unpins on return.  C code
   that stashes the pointer past the call is using freed memory.

Callbacks pin the wrapped function (and its closure environment) for
as long as the callback object is reachable.  Closing a library
invalidates every binding derived from it; calling a closed binding
throws `"library was closed"`.

---

## Platforms

| Platform | Backend |
|---|---|
| Linux / macOS | `libffi` + `dlopen` / `dlsym` |
| Windows | `libffi` + `LoadLibraryW` / `GetProcAddress` |

`libffi` is the only system dependency.  Distros all package it; on
Windows we vendor the static lib (~80 KB).  No hand-rolled assembly
trampolines — every architecture worth supporting has a working
libffi port and reinventing that wheel is what the previous draft
was secretly proposing.

---

## File layout

Mirrors the convention in `modules/README.md`:

```
modules/cffi/
  README.md            user-facing docs (the stb_image story above)
  cando.api.json       LSP manifest
  cffi_module.c        cando_module_init + all natives
  cffi_parse.c/.h      C-header parser used by lib.declare()
  cffi_layout.c/.h     sizeof / alignof / struct field layout
  cffi_marshal.c/.h    CandoValue <-> C value conversion
  cffi_closure.c/.h    libffi closure plumbing for callbacks
  Makefile             builds cffi.so / cffi.dylib / cffi.dll, links -lffi
  test_cffi.c          unit tests for the parser and layout engine
  test_cffi.cdo        integration: libc.so getpid, strlen, qsort
  test_cffi_smoke.cdo  feature-detect + load + close
```

`test_cffi.cdo` runs against `libc` (Linux: `libc.so.6`; macOS:
`libSystem.B.dylib`; Windows: `msvcrt.dll`) — all present on every CI
runner.  No third-party libs in the test matrix.

Top-level wiring: add `cffi` to `MODULES =` in `/Makefile`, the
`CMakeLists.txt` modules block, and `.github/workflows/ci.yml`.  Add
a row to the index in `modules/README.md`.

---

## Implementation milestones

Each ships as a separately-mergeable PR.

1. **Skeleton.**  `ffi.load`, `lib:close`, `ffi.current`.  Smoke test
   loads `libc` and closes it.  No calls yet.
2. **Scalar calls + `declare`.**  Header parser for prototypes
   involving primitive types and `const char*`.  Worked tests: `getpid`,
   `atoi`, `strlen`, `abs`.
3. **Pointers and `ffi.new`.**  Out-parameters work (`gettimeofday`,
   `stbi_load`).  `ffi.alloc`, indexing, `:read`/`:write`.
4. **Structs.**  Layout engine, field access, by-value passing.  Test
   covers `struct timeval` end-to-end.
5. **Callbacks.**  Implicit and explicit forms.  `qsort` test.  This
   is the highest-risk milestone; budget time for the re-entrancy
   review (GC during callback, throw from inside callback,
   non-VM-thread entry).
6. **Polish.**  `cando.api.json`, README, Windows build, varargs
   support (`printf` family via libffi's `ffi_prep_cif_var`).

MVP is milestones 1–3.  4 unlocks the bulk of real C libraries.  5
unlocks GUI and game-engine bindings.

---

## Open questions

Pin down before milestone 1, since they change the public surface:

1. **64-bit integers.**  CanDo numbers are IEEE-754 `double` — 53 bits
   of integer precision.  Returning `uint64_t` from C will silently
   round above 2⁵³.  Options: (a) document the limit and let it slide,
   (b) add a boxed-int type returned for `(u)int64_t` fields, (c)
   return such values as decimal strings.  Recommend (a) for the MVP
   and revisit if a real library bites us.
2. **Pointer arithmetic.**  `p[i]` uses the element type, that's
   clear.  Should `p + n` also work?  Recommend no — it's ambiguous
   (bytes vs elements?) and `p:slice(n * ffi.sizeof(T), …)` is
   unambiguous.
3. **Thread affinity for callbacks.**  Can C fire a callback on a
   non-VM thread (audio buffer fill, etc.)?  Recommend "no, that
   throws" for the MVP; revisit when an actual user hits it.
4. **Unions.**  Easy to add (libffi handles them); not in any real
   example we've sketched.  Defer until a milestone needs them.
