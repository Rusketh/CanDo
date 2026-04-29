# SQL Module (PostgreSQL + MySQL/MariaDB)

A binary extension that gives Cando scripts a single SQL client API
that talks to either **PostgreSQL** or **MySQL/MariaDB** servers.

The module **implements both wire protocols in pure C** and statically
links them into the resulting shared object — there are no runtime
dependencies on `libpq`, `libmysqlclient`, or any other third-party
client library.  OpenSSL (which `cando` itself already links for the
`socket` / `https` libraries) handles TLS and the cryptographic
primitives required by SCRAM-SHA-256, MD5, native_password, and
caching_sha2_password authentication.

`sql.so` / `sql.dll` ships in the same CI artefact bundle as `cando`
and the `sqlite` / `ldap` modules, so a fresh CanDo install can
connect to either engine without the user installing anything.

## Building

From the repository root:

```bash
make modules                                     # Linux / macOS host
make -C modules/sql                              # build only this module
make -C modules/sql test                         # run the C unit tests
./cando modules/sql/test_sql.cdo                 # run the script tests
```

For Windows, either build natively under MSYS2/MinGW or cross-compile:

```bash
make -C modules/sql sql.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

## Loading

```cando
VAR sql = include("./sql.so");          // or "./sql.dll" on Windows
print(sql.VERSION);                      // e.g. "0.1.0"
```

Both calling styles are supported on every handle method:

```cando
sql.exec(db, "CREATE TABLE ...");        // function-style (like modules/ldap)
db:exec("CREATE TABLE ...");              // method-style  (like node:sqlite)
```

## Quick start

```cando
VAR sql = include("./sql.so");

// PostgreSQL
VAR pg = sql.openPostgres({
    host: "db.internal", port: 5432,
    user: "alice", password: "s3cret",
    database: "app",
    tls: TRUE, tlsVerify: TRUE
});

pg:exec("CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, name TEXT)");
pg:exec("INSERT INTO users (name) VALUES ('Ada')");

VAR stmt = pg:prepare("SELECT id, name FROM users WHERE id >= ?");
VAR rows = stmt:all(1);
FOR (r IN rows) { print(`${r.id} ${r.name}`); }
stmt:finalize();

pg:close();

// MySQL / MariaDB
VAR my = sql.openMySQL({
    host: "127.0.0.1",
    user: "root", password: "",
    database: "test"
});
VAR s = my:prepare("SELECT NOW() AS now");
print(s:get().now);
s:finalize();
my:close();
```

## Manual SQL building -- `db:escape` / `db:escapeIdentifier`

Prepared statements are always preferable -- the bind path keeps
values out of the parser entirely.  When you have to build SQL by
hand (dynamic identifiers, IN-list builders, dialect-specific syntax
that doesn't take placeholders) the module exposes engine-aware
escape helpers:

```cando
VAR table = db:escapeIdentifier("user_profiles");
//            -> "user_profiles" (PG) or `user_profiles` (MySQL)

VAR who   = db:escape("o'brien");
//            -> E'o''brien' (PG) or 'o\'brien' (MySQL)

db:exec(`SELECT * FROM ${table} WHERE name = ${who}`);
```

`db:escape(value)` accepts:

| Cando value      | PostgreSQL output     | MySQL output    |
|---|---|---|
| `NULL`           | `NULL`                | `NULL`          |
| `TRUE` / `FALSE` | `TRUE` / `FALSE`      | `1` / `0`       |
| number           | decimal text          | decimal text    |
| string           | `E'...'` (`'` and `\` doubled, control chars expanded) | `'...'` (backslash escapes) |
| anything else    | error                 | error           |

`db:escapeIdentifier(name)` always wraps the name in `"..."` (PG) or
`` `...` `` (MySQL) and doubles the closing-quote character.

Strings containing a NUL byte (`\0`) cannot be embedded as a text
literal under PostgreSQL — `db:escape` throws SQLSTATE `22021` so
the caller can switch to a `bytea` bind instead.

## Native `$N` placeholders

PostgreSQL's spec-mandated `$1, $2, ...` placeholder syntax also
works unchanged for scripts that prefer it over `?`:

```cando
VAR s = pg:prepare("SELECT id FROM users WHERE name = $1 AND active = $2");
s:all("Ada", TRUE);
```

The `?` translator only rewrites placeholders that aren't already
quoted; native `$N` tokens are left intact.

## Threading model

Each connection has an internal mutex; methods serialise their wire
round-trip so a single handle can be safely shared across `thread { ... }`
blocks.  For higher throughput open one handle per thread and let the
server pool the work.

There is **no async API**.  Cando's first-class `thread { ... }` syntax
provides all the concurrency scripts need:

```cando
VAR report = thread {
    VAR s = pg:prepare("SELECT count(*) AS n FROM events");
    VAR r = s:get();
    s:finalize();
    return r.n;
};

// ... do other work on the main thread ...
VAR n = await report;
```

## API reference

### Module level

| Member | Description |
|---|---|
| `sql.open(driver, options)`        | Generic open: `driver` is `"postgres"` or `"mysql"`. |
| `sql.openPostgres(options)`        | Convenience for PostgreSQL. |
| `sql.openMySQL(options)`           | Convenience for MySQL / MariaDB. |
| `sql.VERSION`                      | Module version string (e.g. `"0.1.0"`). |
| `sql.DRIVER_POSTGRES`              | Constant `"postgres"`. |
| `sql.DRIVER_MYSQL`                 | Constant `"mysql"`. |

`open` options (all optional except where noted):

| Field | Type | Default | Meaning |
|---|---|---|---|
| `host`             | string  | `"127.0.0.1"`  | Server hostname or address. |
| `port`             | number  | 5432 / 3306    | TCP port. |
| `user`             | string  | none           | Login user. |
| `password`         | string  | none           | Login password. |
| `database`         | string  | none           | Default database / schema. |
| `applicationName`  | string  | none           | PostgreSQL only — surfaced in `pg_stat_activity`. |
| `tls`              | bool    | `FALSE`        | Request TLS. |
| `tlsVerify`        | bool    | `FALSE`        | Verify peer certificate. |
| `tlsCa`            | string  | none           | Optional CA bundle (PEM). |
| `tlsClientCert`    | string  | none           | Optional client cert (PEM) for mTLS. |
| `tlsClientKey`     | string  | none           | Optional client private key (PEM). |
| `connectTimeout`   | number  | `10000`        | Connect timeout in ms (0 = blocking). |
| `ioTimeout`        | number  | `0`            | Per-recv/send timeout in ms (0 = blocking). |
| `charset`          | string  | `"utf8mb4"`    | MySQL only — character set for the session. |

### Database handle (`db`)

| Method | Returns | Purpose |
|---|---|---|
| `db:close()`                              | `TRUE`                | Closes the connection. Idempotent. |
| `db:exec(sql)`                            | `{ affected, insertId, tag }` | Run a SQL statement (no parameters). Multiple semicolon-separated statements are supported on PostgreSQL. |
| `db:prepare(sql)`                         | `stmt`                | Compile a statement; returns a statement handle.  Use `?` placeholders for both engines (the PG driver translates them to `$1, $2, …` automatically).  `??` is the literal-`?` escape for the rare PG operators that take one. |
| `db:begin()`                              | `TRUE`                | `BEGIN`, or `SAVEPOINT cdo_sp_<n>` if already inside a transaction. |
| `db:commit()`                             | `TRUE`                | `COMMIT`, or `RELEASE SAVEPOINT` for nested savepoints. |
| `db:rollback()`                           | `TRUE`                | `ROLLBACK`, or `ROLLBACK TO ... ; RELEASE` for nested savepoints. |
| `db:inTransaction()`                      | bool                  | `TRUE` when at least one `begin` is open. |
| `db:transaction(fn[, ...args])`           | `fn`'s return value   | `BEGIN`, run `fn(...args)`, then `COMMIT`.  Rolls back and re-throws on error.  Nests via `SAVEPOINT`. |
| `db:bigintMode("number" | "string")`      | active mode           | Default is `"number"` (lossy past 2^53). `"string"` returns INTEGER overflows as decimal strings. |
| `db:ping()`                               | `TRUE`                | Issues `SELECT 1` and throws if the connection is broken. |
| `db:escape(value)`                        | quoted string literal | Escape a value for inline SQL (manual query building).  Driver-aware: PG uses `E'...'`, MySQL uses backslash escapes. |
| `db:escapeIdentifier(name)`               | quoted identifier     | Escape a column / table / schema name.  PG uses `"..."`; MySQL uses `` `...` ``. |

### Statement handle (`stmt`)

| Method | Returns | Purpose |
|---|---|---|
| `stmt:run(params...)`        | `{ affected, insertId }`       | Execute, expecting no row results. |
| `stmt:get(params...)`        | row object or `NULL`           | Execute and return the first row. |
| `stmt:all(params...)`        | array of row objects           | Execute and return every row. |
| `stmt:finalize()`            | `TRUE`                         | Free the server-side prepared statement.  Idempotent. |
| `stmt.sourceSQL`             | string                         | The SQL passed to `db:prepare`. |

#### Parameter binding

Three call shapes for `run` / `get` / `all`:

```cando
stmt:run();                              // no parameters
stmt:run(p0, p1, p2);                    // positional
stmt:run([p0, p1, p2]);                  // positional via array
```

Per-element value mapping:

| Cando value         | PostgreSQL | MySQL |
|---|---|---|
| `NULL`                       | `NULL` | `NULL` |
| `TRUE` / `FALSE`             | `boolean` | `TINYINT 1/0` |
| number (whole, \|x\| ≤ 2^53) | encoded as text integer | `LONGLONG` (binary) |
| number (other)               | encoded as text float    | `DOUBLE`   (binary) |
| string                       | text                     | `VAR_STRING` |
| `{ blob: <string> }`         | `bytea` (text-escaped)   | `LONG_BLOB` |
| anything else                | error | error |

#### Result-row mapping

PostgreSQL columns arrive in text format and are reified by OID:

| OID | Cando type |
|---|---|
| `bool`                      | bool   |
| `int2` / `int4` / `int8`    | number (or string when `bigintMode == "string"` and \|v\| > 2^53) |
| `float4` / `float8` / `numeric` | number |
| anything else (text, varchar, json, …) | string |

MySQL prepared statements use the binary protocol and reify natively:

| MySQL type                  | Cando type |
|---|---|
| `NULL`                      | `NULL` |
| `TINY` / `SHORT` / `LONG` / `LONGLONG` | number / string (per `bigintMode`) |
| `FLOAT` / `DOUBLE`          | number |
| `*BLOB`                     | string (Cando strings are byte-safe) |
| anything else               | string |

## Errors

Every method that can fail throws via Cando's multi-value catch:

```cando
TRY {
    db:exec("INSERT INTO users (id, email) VALUES (1, 'dup@example.com')");
    db:exec("INSERT INTO users (id, email) VALUES (2, 'dup@example.com')");
} CATCH (msg, code, sqlstate) {
    print(msg);              // "sql.exec: duplicate key value violates ..."
    print(code);              // PostgreSQL: integer-packed SQLSTATE; MySQL: errno
    print(sqlstate);          // "23505" (UNIQUE), "08006" (network), …
}
```

`sqlstate` is the standard 5-character SQL state code when the server
provides one; an empty string for module-side errors (missing
arguments, bad handles, pool exhaustion).

## Authentication

| Driver       | Mechanisms supported |
|---|---|
| PostgreSQL   | `AuthenticationOk` (trust), cleartext, MD5, SCRAM-SHA-256 |
| MySQL/MariaDB| `mysql_native_password`, `caching_sha2_password` (fast path; full-auth requires TLS) |

For `caching_sha2_password` over a plain TCP connection, the server
will require the cleartext password to be sent if the cache is cold;
the driver refuses to do this without TLS.  Either enable `tls: TRUE`
or run `ALTER USER ... IDENTIFIED WITH mysql_native_password BY ...`
on the server.

## Transactions

```cando
db:transaction(FUNCTION (from, to, amt) {
    db:exec(`UPDATE balances SET amt = amt - ${amt} WHERE id = ${from}`);
    db:exec(`UPDATE balances SET amt = amt + ${amt} WHERE id = ${to}`);
}, 1, 2, 100);
```

`db:transaction` runs the function inside `BEGIN ... COMMIT`.  If the
function throws, the transaction is rolled back and the same error is
re-raised.  Nested calls use `SAVEPOINT cdo_sp_<n>`.

## Layout

```
modules/sql/
├── README.md                this file
├── Makefile                 per-module build (Linux / macOS / Windows)
├── sql_module.c             script-facing API + handle pool
├── sql_pg.c / sql_pg.h      PostgreSQL wire-protocol driver
├── sql_mysql.c / sql_mysql.h MySQL/MariaDB wire-protocol driver
├── sql_buf.h                read/write buffer helpers (header-only)
├── sql_crypto.h             OpenSSL-backed digest / HMAC / PBKDF2 / base64
├── sql_driver.h             shared types (SqlConnectOpts, SqlError, …)
├── sql_net.h                TCP/TLS read/write helpers
├── test_sql.c               C unit tests (`make -C modules/sql test`)
├── test_sql.cdo             script integration tests (skip when no server)
└── test_sql_smoke.cdo       no-server smoke test
```

## Limitations

- LISTEN/NOTIFY and replication protocol are not implemented.
- COPY (PostgreSQL) and LOCAL INFILE (MySQL) are not implemented;
  the driver returns a clear `0A000` ("feature not supported") error
  if a `COPY` or `LOAD DATA` statement is executed.
- Very large result rows above 16 MiB are not supported on MySQL
  (single-frame packets only).
- `caching_sha2_password` full-auth requires TLS.
