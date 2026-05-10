# `sql` module

Bindings to MySQL / MariaDB and PostgreSQL through their official C
client libraries (`libmysqlclient` and `libpq`).  The interface
mirrors [`sqlite`](../sqlite/README.md) where possible; the differences
are connection setup and database-specific PRAGMA-equivalent settings.

## Loading

```cdo
VAR sql = include("./modules/sql/sql");
```

Requires the corresponding client library to be installed at runtime
(`libmysqlclient.so` or `libpq.so`).  If the client library is
missing, `include()` throws a `dlopen`-style error.

## Connecting

### `sql.openMySQL(opts) → db`

Open a MySQL / MariaDB connection.  `opts`:

| Field      | Type   | Default       | Description |
|------------|--------|---------------|-------------|
| `host`     | string | `"127.0.0.1"` | |
| `port`     | number | `3306`        | |
| `user`     | string | —             | Required. |
| `password` | string | `""`          | |
| `database` | string | —             | Required. |
| `socket`   | string | —             | Unix socket path. |
| `ssl`      | object | —             | `{ ca, cert, key, verify }` |

```cdo
VAR db = sql.openMySQL({
    host: "db.example.com",
    user: "appuser",
    password: os.getenv("DB_PASS"),
    database: "app",
});
```

### `sql.openPostgres(opts) → db`

Open a PostgreSQL connection.  Accepts the same shape with
PostgreSQL-relevant defaults (`port: 5432`).

## Database methods

The shape mirrors [`sqlite`](../sqlite/README.md) — same names for
common operations:

| Method                                 | Description |
|----------------------------------------|-------------|
| `db:close()`                           | Close the connection. |
| `db:ping() → bool`                     | Round-trip a no-op to verify the connection. |
| `db:prepare(sql) → stmt`               | Compile a parameterized statement. |
| `db:exec(sql)`                         | One-off execute with no result set. |
| `db:run(sql, ...args) → number`        | Execute with bindings; return rowcount. |
| `db:get(sql, ...args) → row`           | First row, or `NULL`. |
| `db:all(sql, ...args) → rows`          | Every row. |
| `db:begin()`, `db:commit()`, `db:rollback()` | Manual transaction control. |
| `db:transaction(fn)`                   | Auto-commit / auto-rollback wrapper. |
| `db:inTransaction() → bool`            | |
| `db:escape(s) → string`                | Quote a string literal for safe interpolation. |
| `db:escapeIdentifier(s) → string`      | Quote a column / table name. |
| `db:bigintMode(mode)`                  | `"number"` / `"string"` / `"throw"`. |

## Statement methods

| Method                       | Description |
|------------------------------|-------------|
| `stmt:run(...args) → number` | Execute; return rowcount. |
| `stmt:get(...args) → row`    | First row, or `NULL`. |
| `stmt:all(...args) → rows`   | Every row. |
| `stmt:bind(...args)`         | Bind without executing. |
| `stmt:finalize()`            | Free the statement. |

## Parameter binding

MySQL uses `?` placeholders; PostgreSQL uses `$1`, `$2`, … .  The
module routes each style to the matching driver:

```cdo
// MySQL
db:run("INSERT INTO users (name, age) VALUES (?, ?)", "Alice", 30);

// PostgreSQL
db:run("INSERT INTO users (name, age) VALUES ($1, $2)", "Alice", 30);
```

## Transactions

```cdo
db:transaction(FUNCTION(db) {
    db:exec("INSERT INTO logs VALUES ('a')");
    db:exec("INSERT INTO logs VALUES ('b')");
});
```

## Errors

Driver-level errors throw with the formatted error message.  Wrap in
`TRY` / `CATCH` to handle:

```cdo
TRY {
    db:exec("INSERT INTO nope (x) VALUES (1)");
} CATCH (e) {
    print("sql:", e);
}
```

## Differences from `sqlite`

- No `pragma()`; use the database's session settings via plain SQL.
- No `backup()`; use the database's native dump/restore tooling.
- No `defineFunction` / `defineAggregate`; install user functions
  through SQL DDL on the server.
- `escape()` and `escapeIdentifier()` are needed when you can't use
  parameter binding (rare, but DDL with dynamic identifiers needs it).

## Connection lifetime and threading

- Each call to `openMySQL` / `openPostgres` opens a fresh connection.
- A connection is **not** thread-safe; use one per thread, or guard
  with `object.lock(db)` if you must share.
- `db:close()` is idempotent.
