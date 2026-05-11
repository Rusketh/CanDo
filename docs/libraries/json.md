# `json`

JSON parsing and serialization.

## Reference

### `json.parse(text) → any`

Decode a JSON string into CanDo values.  **Throws** on malformed input
— wrap in `TRY` / `CATCH` to recover.

| JSON           | CanDo value          |
|----------------|----------------------|
| `null`         | `NULL`               |
| `true`/`false` | `TRUE` / `FALSE`     |
| number         | number               |
| string         | string               |
| array          | array                |
| object         | object               |

```cdo
VAR data = json.parse('{"name":"Alice","scores":[10,20,30]}');
print(data.name);                  // Alice
print(inspect(data.scores));       // [10, 20, 30]

TRY {
    json.parse("not json");
} CATCH (e) {
    print("bad json:", e);
}
```

### `json.stringify(value) → string`

Encode a CanDo value as JSON text.

- Numbers use the shortest representation that round-trips.
- Objects serialize in **FIFO insertion order**.
- Arrays serialize by integer index; named array fields are dropped.
- Functions, threads, and natives serialize as `null`.

```cdo
print(json.stringify({ name: "Alice", age: 30 }));
// {"name":"Alice","age":30}

print(json.stringify([1, "two", TRUE, NULL]));
// [1,"two",true,null]
```

## Examples

### Round-tripping a config

```cdo
VAR cfg = { port: 8080, hosts: ["a", "b"], debug: TRUE };
file.write("config.json", json.stringify(cfg));

VAR loaded = json.parse(file.read("config.json"));
print(loaded.port);                // 8080
```

### Defensive parsing

```cdo
VAR data;
TRY {
    data = json.parse(input);
} CATCH (e) {
    data = {};
}
VAR token = data?.auth?.token || "anonymous";
```

Wrap parsing in `TRY` / `CATCH` whenever the input may be untrusted —
malformed JSON throws.

### Pretty printing

`json.stringify` always produces compact output.  For indented JSON,
use the YAML library instead, or implement formatting yourself:

```cdo
FUNCTION pretty(v, indent) {
    indent = indent || "  ";
    // ... user implementation
}
```
