# SQLite Module

A binary extension that gives Cando scripts an embedded SQLite database
with an API on par with Node.js's `node:sqlite` (`DatabaseSync` +
`StatementSync`).

The module **statically links the public SQLite amalgamation** -- there
are no external runtime dependencies on either Linux or Windows.  It
ships as `sqlite.so` / `sqlite.dll` in the same CI artefact bundle as
`cando` itself.

## Building

From the repository root:

```bash
make modules                                    # Linux / macOS host
make -C modules/sqlite                          # build only this module
make -C modules/sqlite test                     # run the C unit tests
./cando modules/sqlite/test_sqlite.cdo          # run the script tests
```

For Windows, either build natively under MSYS2/MinGW or cross-compile:

```bash
make -C modules/sqlite sqlite.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

CI builds and uploads `sqlite.so` (Linux) and `sqlite.dll` (Windows)
alongside the `ldap` module.

## Loading

```cando
VAR sql = include("./sqlite.so");          // or "./sqlite.dll" on Windows
print(sql.SQLITE_VERSION);                 // e.g. "3.53.0"
```

Both calling styles are supported on every handle method:

```cando
sql.exec(db, "CREATE TABLE ...");          // function-style (like modules/ldap)
db:exec("CREATE TABLE ...");                // method-style  (like node:sqlite)
```

## Threading model

SQLite is built in **serialized mode** (`SQLITE_THREADSAFE=1`), so a
single `db` (and any statement prepared from it) is safe to use from
multiple OS threads concurrently -- SQLite serialises calls through
its own mutex, and the module guards its slot table with a per-module
mutex.

There is **no async API**.  Cando's first-class `thread { ... }` syntax
gives scripts everything they need:

```cando
VAR report = thread {
    VAR s = db:prepare("SELECT count(*) AS n FROM users");
    VAR r = s:get();
    s:finalize();
    return r.n;
};

// ... do other work on the main thread ...

VAR n = await report;
```

For atomic multi-step work across threads, wrap the handle with
`object.lock` / `object.unlock` (the language-level lock documented in
`tests/scripts/test_race_conditions.cdo`):

```cando
object.lock(db);
TRY {
    db:begin();
    db:exec("UPDATE balances SET amt = amt - 100 WHERE id = 1");
    db:exec("UPDATE balances SET amt = amt + 100 WHERE id = 2");
    db:commit();
} CATCH (e) {
    db:rollback();
    object.unlock(db);
    throw e;
}
object.unlock(db);
```

Or use the built-in helper:

```cando
db:transaction(FUNCTION (from, to, amt) {
    db:exec(`UPDATE balances SET amt = amt - ${amt} WHERE id = ${from}`);
    db:exec(`UPDATE balances SET amt = amt + ${amt} WHERE id = ${to}`);
}, 1, 2, 100);
```

## API reference

### Module level

| Member | Description |
|---|---|
| `sql.open(path[, options])`     | Open / create a database, returns a `db` handle. |
| `sql.escape(value)`             | Quote a value as a SQLite literal (`'...'` with `'` doubled).  Accepts null / bool / number / string. |
| `sql.escapeIdentifier(name)`    | Quote a column / table name as `"name"` with embedded `"` doubled. |
| `sql.VERSION`                   | Module version string (`"0.5.0"`). |
| `sql.SQLITE_VERSION`            | Vendored SQLite library version. |
| `sql.OPEN_READONLY` `sql.OPEN_READWRITE` `sql.OPEN_CREATE` `sql.OPEN_URI` `sql.OPEN_MEMORY` | Bit-flag constants. |

`open` options (object):

| Field | Type | Default | Meaning |
|---|---|---|---|
| `readonly`            | bool   | `FALSE` | Open `SQLITE_OPEN_READONLY` instead of RW+CREATE. |
| `create`              | bool   | `TRUE`  | Add `SQLITE_OPEN_CREATE` (ignored when `readonly`). |
| `uri`                 | bool   | `FALSE` | Treat `path` as a SQLite URI (`file:foo?mode=ro` etc.). |
| `timeout`             | number | `5000`  | Busy-timeout in milliseconds. |
| `enableForeignKeys`   | bool   | `TRUE`  | `PRAGMA foreign_keys = ON` immediately after open. |
| `enableLoadExtension` | bool   | `FALSE` | Allow `db:loadExtension(...)`. |

### Database handle (`db`)

| Method | Returns | Purpose |
|---|---|---|
| `db:close()`                              | `TRUE`                | Finalises any outstanding statements and closes the connection.  Idempotent. |
| `db:exec(sql)`                            | `TRUE`                | Run one or more semicolon-separated statements with no result rows. |
| `db:prepare(sql)`                         | `stmt`                | Compile a statement; returns a statement handle. |
| `db:begin()`                              | `TRUE`                | `BEGIN`, or `SAVEPOINT` if already inside a transaction. |
| `db:commit()`                             | `TRUE`                | `COMMIT`, or `RELEASE` for nested savepoints. |
| `db:rollback()`                           | `TRUE`                | `ROLLBACK`, or `ROLLBACK TO ... ; RELEASE` for nested savepoints. |
| `db:inTransaction()`                      | bool                  | `TRUE` when at least one `begin` is open. |
| `db:transaction(fn[, ...args])`           | `fn`'s return value   | `BEGIN`, run `fn(...args)`, then `COMMIT`.  Rolls back and re-throws on error.  Nests via `SAVEPOINT`. |
| `db:pragma(name[, value])`                | array of row objects  | Read or set a `PRAGMA`.  Setter accepts numbers, booleans, and alphanumeric strings. |
| `db:bigintMode("number" | "string")`      | active mode           | Default is `"number"` (lossy past 2^53). `"string"` returns INTEGER overflows as decimal strings. |
| `db:setReadBigInts(bool)`                 | `TRUE`                | node:sqlite-compat alias for `bigintMode`. |
| `db:defineFunction(name, fn)`             | `TRUE`                | Register `fn` as a SQL scalar UDF.  Variadic. |
| `db:defineAggregate(name, opts)`          | `TRUE`                | Register an aggregate UDF.  See below. |
| `db:loadExtension(path[, entryPoint])`    | `TRUE`                | Load a SQLite extension.  Requires `enableLoadExtension: TRUE` at open time. |
| `db:backup(destPath[, options])`          | `{ pages, totalPages }` | Online backup using `sqlite3_backup_*`.  Synchronous; wrap in `thread {...}` to background it. |

`backup` options:

| Field        | Default | Meaning |
|---|---|---|
| `step`       | `100`   | Pages copied per `sqlite3_backup_step` call. |
| `pages`      | `-1`    | Maximum pages to copy.  `-1` copies until done. |
| `dbName`     | `"main"`| Source attached-database name. |
| `destDbName` | `"main"`| Destination attached-database name. |

`defineAggregate` options:

| Field    | Type     | Default | Meaning |
|---|---|---|---|
| `start`  | any      | `null`  | Initial accumulator value. |
| `step`   | function | (required) | Called once per input row with `(accumulator, ...args)`; must return the new accumulator. |
| `result` | function | (none)  | If present, called once per group with the final accumulator; its return value is the aggregate's output.  Without `result`, the final accumulator is returned directly. |

### Statement handle (`stmt`)

| Method | Returns | Purpose |
|---|---|---|
| `stmt:run(params...)`        | `{ lastInsertRowid, changes }` | Execute, expecting no row results. |
| `stmt:get(params...)`        | row object or `NULL`           | Step once; return the first row. |
| `stmt:all(params...)`        | array of row objects           | Step until done; return every row. |
| `stmt:iterate(params...)`    | iterator handle                | Stream rows one at a time. |
| `stmt:bind(params)`          | `TRUE`                         | Bind without stepping. |
| `stmt:reset()`               | `TRUE`                         | Clear bindings and rewind. |
| `stmt:finalize()`            | `TRUE`                         | Free the statement.  Idempotent. |
| `stmt:expandedSQL()`         | string                         | SQL with currently-bound parameters substituted (`sqlite3_expanded_sql`). |
| `stmt:columns()`             | array of column-info objects   | One `{ name, type, table, column, database }` per result column. |
| `stmt.sourceSQL`             | string                         | The SQL passed to `db:prepare`. |

#### Iterator handle (`iter`)

| Member | Description |
|---|---|
| `iter:next()` | Returns the next row object or `NULL` once exhausted. |
| `iter.done`   | Boolean; `TRUE` once the cursor is drained. |

#### Parameter binding

Three call shapes for `run` / `get` / `all` / `bind` / `iterate`:

```cando
stmt:run();                              // no parameters
stmt:run(p0, p1, p2);                    // positional
stmt:run([p0, p1, p2]);                  // positional via array
stmt:run({ name: ..., age: ... });       // named, for :name / @name / $name
```

Per-element value mapping:

| Cando value         | SQL type |
|---|---|
| `NULL`                       | `NULL` |
| `TRUE` / `FALSE`             | `INTEGER 1` / `0` |
| number (whole, \|x\| ≤ 2^53) | `INTEGER` |
| number (other)               | `REAL` |
| string                       | `TEXT` |
| `{ blob: <string> }`         | `BLOB` |
| anything else                | error |

#### Result-row mapping

| SQL type   | Cando value |
|---|---|
| `NULL`    | `NULL` |
| `INTEGER` | `number` (or decimal string when `bigintMode == "string"` and the value overflows 2^53) |
| `REAL`    | `number` |
| `TEXT`    | `string` |
| `BLOB`    | `string` (Cando strings are byte-safe) |

## Manual SQL building -- `sql.escape` / `sql.escapeIdentifier`

Prepared statements are the safe default; for the cases where you
have to assemble SQL by hand (dynamic identifiers, IN-list builders)
the module exposes the same `escape` helpers as the `sql` (Postgres /
MySQL) module:

```cando
VAR table = sql.escapeIdentifier("user_profiles");      // -> "user_profiles"
VAR who   = sql.escape("o'brien");                       // -> 'o''brien'
db:exec(`SELECT * FROM ${table} WHERE name = ${who}`);
```

`sql.escape(value)` accepts null / bool / number / string and produces:

| Cando value      | SQLite output |
|---|---|
| `NULL`           | `NULL` |
| `TRUE` / `FALSE` | `1` / `0` |
| number           | decimal text |
| string           | `'...'` with `'` doubled |
| anything else    | error |

## Errors

Every method that can fail throws via Cando's multi-value catch:

```cando
TRY {
    db:exec("INSERT INTO users (id) VALUES (1)");
    db:exec("INSERT INTO users (id) VALUES (1)");
} CATCH (msg, code, sqlstate) {
    print(msg);              // "sqlite.exec: UNIQUE constraint failed: users.id"
    print(code);              // 1555 (SQLITE_CONSTRAINT_PRIMARYKEY)
    print(sqlstate);          // "SQLITE_CONSTRAINT_PRIMARYKEY"
}
```

`sqlstate` is the symbolic SQLite name; `code` is the **extended**
result code from `sqlite3_extended_errcode`.  Module-side errors
(missing arguments, unknown options, pool exhaustion, etc.) pass
`code = 0` and `sqlstate = ""`.

## Differences from `node:sqlite`

| Area | node:sqlite | this module |
|---|---|---|
| Function registration | `db.function(name, fn)` | `db:defineFunction(name, fn)` -- `function` is reserved in Cando. |
| Aggregate registration | `db.aggregate(name, opts)` | `db:defineAggregate(name, opts)` -- ditto. |
| Async | `db.exec` is sync; you wrap in worker threads yourself. | Same.  No `runAsync` / `execAsync`; use `thread { ... }` + `await`. |
| BigInt | `setReadBigInts(true)` returns BigInt. | `db:bigintMode("string")` returns decimal **strings** (Cando has no BigInt type). |
| Transactions | `db.transaction(fn)` returns a wrapped function you call later. | `db:transaction(fn, ...args)` runs immediately and returns `fn`'s value (rolling back + re-throwing on failure). |

## Layout

```
modules/sqlite/
├── README.md                this file
├── Makefile                 per-module build (Linux / Windows)
├── sqlite_module.c          implementation
├── sqlite_helpers.h         pure-C helpers (error code -> name)
├── vendor/
│   ├── sqlite3.c            vendored amalgamation
│   ├── sqlite3.h
│   └── sqlite3ext.h
├── test_sqlite.c            C unit tests (run by `make -C modules/sqlite test`)
├── test_sqlite.cdo          script integration tests (Linux job)
└── test_sqlite_smoke.cdo    cross-platform smoke test (Windows job)
```

## Build flags

The amalgamation is compiled with:

```
-DSQLITE_THREADSAFE=1
-DSQLITE_ENABLE_FTS5
-DSQLITE_ENABLE_RTREE
-DSQLITE_ENABLE_JSON1
-DSQLITE_ENABLE_MATH_FUNCTIONS
-DSQLITE_ENABLE_COLUMN_METADATA
-DSQLITE_DQS=0
-DSQLITE_USE_URI=1
-DSQLITE_OMIT_DEPRECATED
-DSQLITE_DEFAULT_FOREIGN_KEYS=1
```

FTS5, R-Tree, JSON1, math functions, and column metadata are all
available without extra setup.  `SQLITE_DQS=0` makes "double-quoted
strings" a syntax error on both DDL and DML, matching SQLite's
recommendation.
