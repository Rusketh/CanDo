# Getting Started

This guide takes you from a clean checkout to a running script in a few
minutes.

## Requirements

- A C11 compiler (gcc 7+, clang 6+, MSVC 2019+)
- CMake 3.16+ **or** GNU Make
- pthreads (POSIX) / Win32 threads (Windows)
- OpenSSL development headers (for the `https` / TLS client)

## Build

### CMake (recommended)

```bash
cmake -B build
cmake --build build
```

Artifacts appear in `build/`:

- `cando` — the CLI interpreter
- `libcando.so` (`.dll` on Windows, `.dylib` on macOS) — shared library
- `libcando_static.a` — static library

### GNU Make

```bash
make            # builds libcando.so, libcando.a, cando, and all tests
make cando      # just the interpreter
make test       # runs the unit + script tests
```

## Run your first script

Create `hello.cdo`:

```cando
print("hello, world");
```

Run it:

```bash
./build/cando hello.cdo
```

Every script run via the `cando` CLI gets all 17 standard libraries
pre-loaded.  When embedding CanDo yourself you decide which libraries
to open — see [embedding.md](embedding.md).

### See the bytecode

```bash
./build/cando hello.cdo --disasm
```

The disassembler dumps the compiled chunk before execution.  Output
includes line numbers, opcodes, and constant-pool references.  Useful
for understanding what an expression compiles to.

## A slightly larger example

```cando
/* Pythagorean triples up to N. */

FUNCTION hypot(a, b) {
    RETURN math.sqrt(a * a + b * b);
}

VAR N = 10;
FOR a IN 1 -> N {
    FOR b IN a -> N {
        VAR c = hypot(a, b);
        IF c == math.floor(c) {
            print(a, b, c);
        }
    }
}
```

Things to notice:

- Keywords (`VAR`, `FOR`, `IN`, `IF`, `FUNCTION`, `RETURN`) are
  upper-case.  Identifiers are case-sensitive.
- Ranges are inclusive at both ends: `1 -> N` iterates `1, 2, … N`.
- Blocks are always braced.  Statements end with `;`; statements *inside*
  a braced block may omit the trailing semicolon before `}`.
- `math.sqrt` comes from the standard library; `print` is a core native
  always registered by `cando_open()`.

## Where to go next

- [language-reference.md](language-reference.md) — the full syntax
- [standard-library.md](standard-library.md) — what's in `math`, `string`, `array`, `file`, `json`, `thread`, …
- [embedding.md](embedding.md) — run CanDo from your own C program
- [threading.md](threading.md) — `thread { … }` and `await`
