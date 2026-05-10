# Getting Started

This guide walks you through installing CanDo, running your first
script, and getting your editor set up.

## Prebuilt binaries

If you'd rather not build from source, every green push to the trunk
branches publishes a fresh build to a rolling release tag on GitHub.

| Platform           | Release tag       | Asset                       |
|--------------------|-------------------|-----------------------------|
| Linux (x86_64)     | `linux-latest`    | `cando-linux-x86_64.zip`    |
| Windows (x86_64)   | `windows-latest`  | `cando-windows-x86_64.zip`  |
| VS Code extension  | `vscode-latest`   | `vscode-cando.vsix`         |

Each platform zip extracts to:

- `cando` / `cando.exe` -- the standalone interpreter.
- `libcando.so` / `libcando.dll` -- the shared library, sitting next to
  the binary so `-Wl,-rpath,'$ORIGIN'` (Linux) or the default DLL search
  order (Windows) finds it at runtime.
- `libcando.a` / `libcando.lib` -- static archive / MSVC import library
  for embedding.
- `include/` -- public embedding headers (`cando.h` and the internal
  per-subsystem headers).
- `modules/<name>/` -- one `.so`/`.dll` per binary module.

The latest builds are linked from the repository's
[Releases](https://github.com/Rusketh/CanDo/releases) page.

If you want to build from source, read on.

## Requirements

- A C11 compiler (`gcc 7+`, `clang 6+`, or MSVC 2019+).
- **CMake 3.13+** (preferred) or GNU Make.
- **OpenSSL** development headers (used by `https`, `secure_socket`,
  and `crypto`).
- **pthreads** on POSIX (used by `thread`); native threads on Windows.

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libssl-dev
```

On macOS with Homebrew:

```bash
brew install cmake openssl@3
```

On Windows the easiest path is the MSYS2 mingw-w64 toolchain or Visual
Studio with the CMake integration enabled.

## Building

CMake is the supported build system.  GNU `Makefile` is provided as a
fallback for environments where CMake is unavailable.

### With CMake

```bash
git clone <repo-url> CanDo
cd CanDo
cmake -B build
cmake --build build -j
```

The build produces:

- `build/cando` (or `cando.exe`) — the standalone interpreter.
- `build/libcando.so` (or `.dylib` / `.dll`) — the shared library for
  embedding.
- `build/libcando.a` (or `cando_static.lib`) — static archive.

### With make

```bash
make            # builds tests + the cando interpreter
make cando      # builds only the interpreter (faster)
make test       # builds and runs the full test suite
make clean      # removes generated artefacts
```

### Verifying the build

Run any script from the test suite:

```bash
./cando tests/scripts/hello.cdo
```

You should see the script's output.  If the binary exits with a
"command not found" error from your shell, prepend `./`.

## Your first script

Create `hello.cdo`:

```cdo
// hello.cdo
VAR name = "CanDo";
print(`Hello, ${name}!`);

VAR squares = [1, 2, 3, 4] ~> pipe * pipe;
print("squares:", inspect(squares));
```

Run it:

```bash
./cando hello.cdo
```

Output:

```
Hello, CanDo!
squares: [1, 4, 9, 16]
```

### What's happening

- `VAR name = "CanDo"` declares a local variable.  Use `CONST` instead
  to make the binding write-protected.
- Backtick strings (`` `…` ``) interpolate `${expr}`.
- `~>` is the pipe operator: it takes the array on the left and
  evaluates the body on the right with `pipe` bound to each element,
  collecting the results into a new array.
- `print()` writes to stdout with a trailing newline; `inspect()`
  formats nested values for readable debug output.

A guided tour of the rest of the language is in
[language/README.md](language/README.md).

## Passing arguments to a script

```bash
./cando script.cdo --port 8080 input.txt
```

Inside the script, `args` is an array of the trailing arguments:

```cdo
// args.cdo
print("got", #args, "arguments");
FOR i, v IN args { print(i, "=", v); }
```

The `cando` binary's own flags (`--disasm`, `--jit`, `--no-jit`,
`--jit-stats`, `--jit-dump`) are consumed before the script starts and
**do not appear in `args`**.  See [cli.md](cli.md) for the full list.

## Running a quick eval

If you just want to evaluate an expression:

```bash
./cando -e '1 + 2 * 3'
```

(For longer one-liners, write to a file — quoting in shells gets ugly
fast.)

## Editor setup

A VS Code extension lives in
[`editors/vscode-cando/`](../editors/vscode-cando/) and provides syntax
highlighting, snippets, completion, and hover documentation.  To install
it locally:

```bash
cd editors/vscode-cando
npm install
npm run compile
npm run package        # produces vscode-cando-<version>.vsix
code --install-extension vscode-cando-*.vsix
```

For other editors, syntax highlighting is straightforward to recreate
— the keyword list is in
[language/syntax.md](language/syntax.md).

## Where to next

- A guided language tour: [language/README.md](language/README.md).
- The standard library, function-by-function:
  [libraries/README.md](libraries/README.md).
- Embedding CanDo in a C application:
  [api/embedding.md](api/embedding.md).
