# `datetime`

Time formatting and parsing.  Timestamps are Unix seconds (UTC); the
local timezone is used for parsing and `%Z`-style format directives.

## Reference

### `datetime.now() → number`

Unix timestamp in seconds.  Equivalent to `os.time()`.

```cdo
print(datetime.now());             // 1714400000
```

### `datetime.format(timestamp, format*) → string`

Format a timestamp using the host `strftime` syntax.  Default format:
`"%Y-%m-%d %H:%M:%S"`.

```cdo
VAR now = datetime.now();
print(datetime.format(now));                     // 2026-05-10 17:53:00
print(datetime.format(now, "%a %b %d, %Y"));     // Sun May 10, 2026
print(datetime.format(now, "%H:%M"));            // 17:53
```

Common directives (host `strftime` provides the full set):

| Directive | Meaning                                 |
|-----------|-----------------------------------------|
| `%Y`      | 4-digit year                            |
| `%m`      | 2-digit month                           |
| `%d`      | 2-digit day of month                    |
| `%H`      | 2-digit hour (24h)                      |
| `%M`      | 2-digit minute                          |
| `%S`      | 2-digit second                          |
| `%a` / `%A` | Abbreviated / full weekday name       |
| `%b` / `%B` | Abbreviated / full month name         |
| `%Z`      | Timezone name                           |
| `%s`      | Unix timestamp                          |

### `datetime.parse(text, format) → number | null`

Inverse of `format`.  Returns a Unix timestamp, or `NULL` on mismatch.
Uses the **local timezone**.

```cdo
VAR ts = datetime.parse("2026-05-10 12:30:00", "%Y-%m-%d %H:%M:%S");
print(ts);                         // Unix timestamp

VAR bad = datetime.parse("not a date", "%Y-%m-%d");
print(bad);                         // null
```

> On Windows `datetime.parse` is currently a stub that returns `NULL`
> until a proper `strptime` shim is wired in.

## Examples

### Logging timestamps

```cdo
FUNCTION log(msg) {
    VAR stamp = datetime.format(datetime.now(), "%Y-%m-%d %H:%M:%S");
    print(`[${stamp}] ${msg}`);
}
log("hello");                      // [2026-05-10 17:53:00] hello
```

### Computing a duration

```cdo
VAR t0 = datetime.now();
expensive_work();
VAR seconds = datetime.now() - t0;
print(`took ${seconds}s`);
```

### Round-tripping ISO 8601

```cdo
VAR ts   = datetime.now();
VAR text = datetime.format(ts, "%Y-%m-%dT%H:%M:%S");
VAR back = datetime.parse(text, "%Y-%m-%dT%H:%M:%S");
print(ts == back);                  // true (timezone-dependent on Windows)
```
