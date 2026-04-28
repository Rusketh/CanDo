# SQLite Module

A binary extension module that gives Cando scripts an embedded SQLite
database with an API on par with Node.js's `node:sqlite`
(`DatabaseSync` + `StatementSync`).

The module statically links the public **SQLite amalgamation** -- there
are no external runtime dependencies on either Linux or Windows.

> **Status:** scaffold only.  Chunk 1 of the implementation lands the
> Makefile, vendored amalgamation, and a stub `cando_module_init` that
> exposes only `VERSION` and `SQLITE_VERSION`.  The full `open` /
> `prepare` / `run` / `get` / `all` / `transaction` surface is wired up
> in subsequent chunks; see `/root/.claude/plans/we-dont-need-async-calm-dragon.md`
> for the chunk-by-chunk roadmap.

## Building

From the repository root:

```bash
make modules                                    # Linux / macOS host
make -C modules/sqlite                          # build only this module
make -C modules/sqlite test                     # run the C unit tests
```

For Windows, either build natively under MSYS2/MinGW, or cross-compile:

```bash
make -C modules/sqlite sqlite.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

CI uploads the artefacts (`sqlite.so` for Linux, `sqlite.dll` for
Windows) from each workflow run.

## Threading model

SQLite is built in serialized mode (`SQLITE_THREADSAFE=1`) so a single
database handle can be used from multiple OS threads concurrently --
SQLite serialises calls internally, and the module guards its slot
table with a per-module mutex.  Cando scripts therefore parallelise
work using the existing thread syntax:

```cando
VAR sql = include("./sqlite.so");
VAR db  = sql.open("./app.db");

VAR job = thread {
    VAR s = db:prepare("SELECT count(*) AS n FROM users");
    VAR r = s:get();
    s:finalize();
    return r.n;
};

VAR n = await job;
print(n);
```

There is deliberately no `runAsync` / `execAsync` API.

## Layout

```
modules/sqlite/
├── README.md                this file
├── Makefile                 per-module build
├── sqlite_module.c          implementation
├── vendor/
│   ├── sqlite3.c            vendored amalgamation
│   ├── sqlite3.h
│   └── sqlite3ext.h
├── test_sqlite.c            C unit tests
└── test_sqlite_smoke.cdo    cross-platform smoke test
```
