# `sqlite` module

SQLite bindings for CanDo — prepared statements, transactions,
user-defined functions, row iteration, and online backup.

The `sqlite` module is **embedded**: the SQLite C library is linked
into the shared library directly, so the only runtime requirement is
the host C library.

## Loading

```cdo
VAR sqlite = include("./modules/sqlite/sqlite");
```

`include()` probes `sqlite.so`, `sqlite.dylib`, `sqlite.dll` in order
when no extension is given.

## Quick example

```cdo
VAR sqlite = include("./modules/sqlite/sqlite");

VAR db = sqlite.open(":memory:");
db:exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");

VAR insert = db:prepare("INSERT INTO users (name) VALUES (?)");
insert:run("Alice");
insert:run("Bob");
insert:finalize();

FOR row OF db:prepare("SELECT * FROM users"):all() {
    print(row.id, row.name);
}

db:close();
```

## Module functions

### `sqlite.open(path) → db`

Open a database.  `path` may be a filesystem path or `":memory:"` for
an in-memory database.

### Database methods

| Method                                 | Description |
|----------------------------------------|-------------|
| `db:close()`                           | Close the connection.  Idempotent. |
| `db:exec(sql)`                         | Execute one or more SQL statements with no result set.  Multiple statements separated by `;` are allowed. |
| `db:prepare(sql) → stmt`               | Compile a parameterized statement.  Returns a statement handle. |
| `db:pragma(name, value*) → any`        | Read or write a PRAGMA.  With one argument, returns the current value; with two, sets it. |
| `db:transaction(fn)`                   | Run `fn(db)` inside a transaction.  Commits on clean return, rolls back on `THROW`. |
| `db:begin()`, `db:commit()`, `db:rollback()` | Manual transaction control. |
| `db:inTransaction() → bool`            | True if a transaction is open. |
| `db:backup(dest)`                      | Online backup to another open database. |
| `db:loadExtension(path)`               | Load a SQLite extension.  Requires `db:pragma("trusted_schema", 1)`. |
| `db:defineFunction(name, arity, fn)`   | Register a scalar SQL function. |
| `db:defineAggregate(name, arity, step, final)` | Register an aggregate function. |
| `db:bigintMode(mode)`                  | `"number"` (default), `"string"`, or `"throw"`.  Controls how integers > 2^53 are returned. |
| `db:setReadBigInts(bool)`              | Enable / disable bigint coercion on read. |

## Statement methods

A `stmt` returned by `db:prepare()` has:

| Method                          | Description |
|---------------------------------|-------------|
| `stmt:run(...args) → number`    | Execute, binding `args` positionally.  Returns rowcount (`changes()`). |
| `stmt:get(...args) → object`    | Execute and return the first row, or `NULL`. |
| `stmt:all(...args) → array`     | Execute and return every row. |
| `stmt:bind(...args)`            | Bind without executing. |
| `stmt:reset()`                  | Reset to a fresh state, keeping bindings. |
| `stmt:finalize()`               | Free the prepared statement.  Idempotent. |
| `stmt:expandedSQL() → string`   | The SQL with bindings substituted (debugging). |
| `stmt:columns() → array`        | Names of the columns in the result set. |
| `stmt:iterate(...args) → iter`  | Lazy iterator; combine with `OVER`. |

## Iteration

```cdo
FOR row OVER stmt:iterate() {
    print(row.id, row.name);
    IF row.bad { BREAK; }
}
```

`iterate()` is more memory-efficient than `all()` for large result
sets — rows are produced lazily.

## Parameter binding

Three placeholder styles are supported:

```cdo
db:prepare("SELECT * FROM users WHERE age > ?"):all(18);
db:prepare("SELECT * FROM users WHERE age > ?1"):all(18);
db:prepare("SELECT * FROM users WHERE age > :min"):all({ min: 18 });
```

For the named-parameter form, pass an object whose keys match the
parameter names (without the leading `:`).

## Transactions

```cdo
db:transaction(FUNCTION(db) {
    db:exec("INSERT INTO logs VALUES ('a')");
    db:exec("INSERT INTO logs VALUES ('b')");
    /* if this throws, both inserts roll back */
});
```

For nested transactions, use savepoints via `db:exec("SAVEPOINT s1");
… db:exec("RELEASE s1");`.

## User-defined functions

```cdo
db:defineFunction("upper_first", 1, FUNCTION(s) {
    IF s == NULL { RETURN NULL; }
    RETURN s:char(0):toUpper() + s:sub(1);
});

print(db:prepare("SELECT upper_first('hello')"):get());
// { "upper_first('hello')": "Hello" }
```

For aggregates:

```cdo
db:defineAggregate("custom_sum", 1,
    FUNCTION(state, x) { state.total = (state.total || 0) + x; RETURN state; },
    FUNCTION(state) { RETURN state.total; });
```

## Bigint handling

SQLite stores 64-bit integers but JS-style numbers lose precision past
2^53.  `db:bigintMode()` controls the policy:

- `"number"` (default) — return as `f64`.  Loss of precision is
  silent.
- `"string"` — return as a decimal string for any value out of range.
- `"throw"` — raise a runtime error if a bigint can't fit.

## Errors

All SQLite errors throw with the SQLite error message as the thrown
value.  Wrap calls in `TRY` / `CATCH` to handle.

```cdo
TRY {
    db:exec("INSERT INTO nope (x) VALUES (1)");
} CATCH (e) {
    print("sqlite:", e);
}
```

## Module surface summary

```
sqlite.open(path) → db

db:close() | exec(sql) | prepare(sql) → stmt
   pragma(name, value*) | transaction(fn)
   begin() | commit() | rollback() | inTransaction()
   backup(dest) | loadExtension(path)
   defineFunction(name, arity, fn)
   defineAggregate(name, arity, step, final)
   bigintMode(mode) | setReadBigInts(bool)

stmt:run(...) → number | get(...) → row | all(...) → rows
     bind(...) | reset() | finalize()
     expandedSQL() | columns() | iterate(...) → iter
```
