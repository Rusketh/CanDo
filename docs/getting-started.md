# Getting Started with CanDo

This guide walks you through building CanDo from source, running your first
script, and linking the library into a host program.

---

## Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| GCC or Clang | GCC 7 / Clang 5 | Must support C11 and `_Atomic` |
| CMake | 3.16 | Recommended build system |
| GNU Make | any | Alternative; `Makefile` ships in the repo |
| pthreads | POSIX | Included on Linux/macOS; `winpthreads` on Windows |
| Python 3 | 3.7+ | Optional — only needed to regenerate the icon |

On Ubuntu / Debian:
```bash
sudo apt install build-essential cmake
```

On macOS:
```bash
xcode-select --install
brew install cmake
```

---

## Building from source

### CMake (recommended)

```bash
git clone https://github.com/rusketh/cando
cd cando
cmake -B build
cmake --build build -j$(nproc)
```

Build outputs in `build/`:

| File | Description |
|---|---|
| `libcando.so` (Linux) / `libcando.dylib` (macOS) | Shared library |
| `libcando.a` | Static library |
| `cando` | CLI interpreter |
| `test_core`, `test_vm`, … | Unit test binaries |

Run the tests:
```bash
cd build && ctest --output-on-failure
```

### GNU Make (alternative)

```bash
make            # builds libcando.so, libcando.a, cando, and all unit tests
make cando      # builds only the executable
make test       # builds and runs unit + integration tests
make clean      # removes all build artifacts
```

### Windows cross-compile

From Linux with `mingw-w64` installed:
```bash
sudo apt install mingw-w64
make cando.exe libcando.dll
```

---

## Installing

### System-wide (Linux)

```bash
cmake --build build --target install   # installs to /usr/local by default
```

Or with a custom prefix:
```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build --target install
```

This installs:
- `$PREFIX/bin/cando`
- `$PREFIX/lib/libcando.so`
- `$PREFIX/lib/libcando.a`
- `$PREFIX/include/cando.h`

---

## Running your first script

Create `hello.cdo`:
```cando
print("Hello, CanDo!");
```

Run it:
```bash
./build/cando hello.cdo
```

Output:
```
Hello, CanDo!
```

---

## CLI usage

```
cando <file.cdo>           Execute a script
cando <file.cdo> --disasm  Disassemble bytecode to stderr, then execute
```

`--disasm` is useful during development to see what bytecode the compiler
generates for a piece of code:

```bash
./cando hello.cdo --disasm
```

---

## Writing a CanDo script

Full syntax reference: [language-reference.md](language-reference.md).

Quick example covering common features:

```cando
// Variables and constants
VAR name = "World";
CONST PI = 3.14159;

// Functions
FUNCTION greet(who) {
    RETURN "Hello, " + who + "!";
}
print(greet(name));

// Arrays and objects
VAR nums = [1, 2, 3, 4, 5];
VAR person = { name: "Alice", age: 30 };

// Loops
FOR VAR i = 0; i < nums.length; i++ {
    print(nums[i]);
}

FOR VAR item OVER nums {
    print(item);
}

// Error handling
TRY {
    VAR data = json.parse(file.read("config.json"));
    print(data.version);
} CATCH (err) {
    print("config error: " + err);
}
```

---

## Embedding CanDo in a C program

Add `include/` to your include path and link against `libcando`:

```c
#include <cando.h>
#include <stdio.h>

int main(void) {
    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    if (cando_dofile(vm, "scripts/main.cdo") != CANDO_OK)
        fprintf(stderr, "error: %s\n", cando_errmsg(vm));

    cando_close(vm);
    return 0;
}
```

Compile:
```bash
gcc -o myapp myapp.c -Ipath/to/cando/include -Lpath/to/cando/build -lcando \
    -Wl,-rpath,path/to/cando/build
```

For the full embedding guide see [embedding.md](embedding.md).
