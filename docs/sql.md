# SQL Module — PostgreSQL + MySQL

Cando ships a binary module, `sql.so` (Linux/macOS) / `sql.dll`
(Windows), that lets scripts talk to **PostgreSQL** and **MySQL /
MariaDB** servers over TCP or TLS.

The driver is implemented in pure C against the native wire
protocols.  No `libpq`, no `libmysqlclient`, no third-party connectors
need to be installed at runtime — the module ships in the same release
bundle as `cando` itself.

> See [`modules/sql/README.md`](../modules/sql/README.md) for the full
> per-method API reference.  This document focuses on usage patterns
> and how the SQL module fits into the rest of CanDo.

## Loading

```cando
VAR sql = include("./sql.so");          // or "./sql.dll" on Windows
```

The module exports one global object with three open functions and a
set of method sentinels that get attached to every database / statement
handle the module hands out.

## Connecting

There are three equivalent ways to open a connection:

```cando
// 1. Generic open with the driver name.
VAR pg = sql.open("postgres", { host: "127.0.0.1", user: "postgres",
                                 password: "secret", database: "app" });

// 2. Convenience wrapper for PostgreSQL.
VAR pg = sql.openPostgres({ host: "...", user: "...", ... });

// 3. Convenience wrapper for MySQL / MariaDB.
VAR my = sql.openMySQL({ host: "...", user: "...", ... });
```

All three return a database handle.  Methods can be invoked in two
styles, both routing to the same native function:

```cando
sql.exec(pg, "SELECT 1");                  // function-style (matches modules/ldap)
pg:exec("SELECT 1");                        // method-style  (matches node:sqlite)
```

## Querying

### Ad-hoc SQL

Use `db:exec(sql)` for one-shot statements (DDL, INSERT/UPDATE/DELETE,
multiple semicolon-separated statements on PostgreSQL):

```cando
VAR r = db:exec("INSERT INTO users (name) VALUES ('Ada'), ('Grace')");
print(r.affected);                  // 2
print(r.insertId);                   // last auto-increment id (MySQL only)
```

### Prepared statements

For parameterised queries, prepare the SQL once and execute many
times:

```cando
// PostgreSQL placeholders are $1, $2, ...
VAR insert = pg:prepare("INSERT INTO users (id, name) VALUES ($1, $2)");
insert:run(1, "Ada");
insert:run(2, "Grace");
insert:finalize();

// MySQL placeholders are ?
VAR pick = my:prepare("SELECT id, name FROM users WHERE id >= ? ORDER BY id");
VAR rows = pick:all(1);
FOR (r IN rows) { print(r.id + " " + r.name); }
pick:finalize();
```

`stmt:run(...)` is for INSERT/UPDATE/DELETE; `stmt:get(...)` returns the
first row (or `NULL`); `stmt:all(...)` returns the full result set as
an array of objects keyed by column name.

Always call `stmt:finalize()` when done — the server keeps the
prepared statement alive until you do.

### Parameter encoding

| Cando value                  | PostgreSQL                | MySQL (binary protocol)   |
|---|---|---|
| `NULL`                       | `NULL`                    | `NULL`                    |
| `TRUE` / `FALSE`             | `boolean`                 | `TINYINT 1` / `0`         |
| number (whole, \|x\| ≤ 2^53) | text integer              | `LONGLONG` binary value   |
| number (fractional / large)  | text float                | `DOUBLE` binary value     |
| string                       | text                      | `VAR_STRING`              |
| `{ blob: <bytes-string> }`   | `bytea`                   | `LONG_BLOB`               |

### Result-row reification

Each result row arrives as a Cando object keyed by column name.

PostgreSQL columns reify based on the `pg_type` OID (`bool`, `int4`,
`float8`, …); anything not explicitly numeric/boolean comes back as a
Cando string (so JSON, UUID, timestamp, etc. arrive in text form).

MySQL binary-protocol cells already carry a type byte; the driver
maps integer types to numbers, floats to numbers, blobs to strings,
and everything else to strings.

For 64-bit integers that exceed JavaScript's `Number.MAX_SAFE_INTEGER`
(2^53), call `db:bigintMode("string")` to receive them as decimal
strings instead of lossy doubles.

## Transactions

The driver supports nested transactions via `SAVEPOINT`:

```cando
db:begin();                                  // BEGIN
db:exec("INSERT ...");
    db:begin();                              // SAVEPOINT cdo_sp_1
    db:exec("INSERT ...");
    db:rollback();                            // ROLLBACK TO + RELEASE cdo_sp_1
db:commit();                                  // COMMIT
```

Or use the `transaction` helper, which begins, runs your function,
and commits — rolling back and re-throwing on any error inside:

```cando
db:transaction(FUNCTION (from, to, amount) {
    db:exec(`UPDATE balances SET amt = amt - ${amount} WHERE id = ${from}`);
    db:exec(`UPDATE balances SET amt = amt + ${amount} WHERE id = ${to}`);
}, 1, 2, 100);
```

## Errors

All methods that can fail throw via Cando's multi-value catch:

```cando
TRY {
    db:exec("INSERT INTO users(id) VALUES(1)");
    db:exec("INSERT INTO users(id) VALUES(1)");
} CATCH (msg, code, sqlstate) {
    print(msg);                  // server's message text
    print(code);                  // numeric code (driver-specific)
    print(sqlstate);              // 5-char SQL state (e.g. "23505")
}
```

When the error originates from the module itself (bad arguments,
closed handle, pool exhaustion) `code` is `0` and `sqlstate` is
`""`.

Common SQL states worth catching:

| State  | Meaning |
|---|---|
| `08001` / `08006` | Connection / network errors |
| `23505`           | UNIQUE constraint violation (PostgreSQL) |
| `23000`           | Integrity violation (MySQL family) |
| `28P01` / `28000` | Authentication / access denied |
| `42601` / `42000` | SQL syntax error |

## TLS

To open a TLS-encrypted connection, pass `tls: TRUE`.  By default the
driver does not verify the peer certificate (trust on first use) —
production deployments should also pass `tlsVerify: TRUE` and either
let the OS trust store handle verification or supply an explicit CA
bundle:

```cando
VAR pg = sql.openPostgres({
    host:      "db.example.com",
    user:      "alice",
    password:  "...",
    database:  "app",
    tls:       TRUE,
    tlsVerify: TRUE,
    tlsCa:     file.read("/etc/ssl/ca.pem")
});
```

For mutual TLS, also supply `tlsClientCert` and `tlsClientKey`.

## Threading

A single connection handle is safe to share across `thread { ... }`
blocks — every method call serialises through a per-connection mutex
so requests don't interleave on the wire.  For higher throughput open
one handle per thread and let the server pool concurrent work.

```cando
VAR results = [];
VAR threads = [];
FOR (i IN 0..10) {
    threads:push(thread {
        VAR conn = sql.openPostgres({ host: "...", ... });
        VAR row  = conn:prepare("SELECT pg_sleep(1), $1::int AS id"):get(i);
        conn:close();
        return row.id;
    });
}
FOR (t IN threads) { results:push(await t); }
```

## Limitations

- COPY (PostgreSQL) / LOCAL INFILE (MySQL) are not implemented; the
  driver throws SQLSTATE `0A000` if you try to use them.
- Async notifications (PostgreSQL `LISTEN/NOTIFY`) are not implemented.
- MySQL packets larger than 16 MiB are not supported (single-frame
  only).
- `caching_sha2_password` full-auth requires TLS — see the README.
