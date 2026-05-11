# `file`

Synchronous filesystem I/O.  Every function that can fail returns
`NULL` on error rather than throwing.

The optional `encoding` argument on text functions accepts `"utf8"`
(default) or `"binary"`.  In binary mode, no transformation is applied
to the bytes.

## Reference

### `file.read(path, encoding*) → string | null`

Read the whole file as a string.  Returns `NULL` if the file doesn't
exist or can't be opened.

```cdo
VAR text = file.read("hello.txt");
IF text == NULL { THROW "could not read hello.txt"; }
print(text);
```

### `file.write(path, data, encoding*) → bool`

Write `data`, **truncating** any existing file.  Returns `TRUE` /
`FALSE`.

```cdo
file.write("hello.txt", "Hello, world!\n");
```

### `file.append(path, data, encoding*) → bool`

Append to a file, creating it if missing.

```cdo
file.append("log.txt", `[${datetime.now()}] event\n`);
```

### `file.exists(path) → bool`

`TRUE` if the path exists (file or directory).

### `file.delete(path) → bool`

Remove a file.  Returns `TRUE` on success.

### `file.copy(src, dst) → bool`, `file.move(src, dst) → bool`

Copy or rename/move a file.

### `file.size(path) → number | null`

Size in bytes, or `NULL` if the file doesn't exist.

### `file.lines(path, encoding*) → array | null`

Array of lines, with newline terminators stripped.  Returns `NULL` on
read failure.

```cdo
FOR line OF file.lines("/etc/hosts") {
    IF line:startsWith("#") { CONTINUE; }
    print(line);
}
```

### `file.list(path) → array | null`

Array of entry names (filenames only, not full paths) in the given
directory.  Returns `NULL` if `path` is not a directory.  `.` and `..`
are excluded.

```cdo
FOR name OF file.list(".") { print(name); }
```

### `file.mkdir(path) → bool`

Create a directory.  **Non-recursive** — the parent must exist.
Returns `TRUE` if the directory was created **or already existed**
(`EEXIST` is treated as success); other failures return `FALSE`.

### `file.open(path, mode) → stream | null`

Open a file as a [`stream`](stream.md).  Modes:

- `"r"` — read.
- `"w"` — write, truncate.
- `"a"` — append.
- `"r+"` / `"w+"` / `"a+"` — read+write variants.
- Append `"b"` for binary mode (Windows-significant).

Returns a stream handle, or `NULL` on failure.

```cdo
VAR f = file.open("data.bin", "wb");
f:writeAll(payload);             // payload is a string of bytes
f:close();
```

For most cases `file.read` / `file.write` is shorter; reach for
`file.open` when you need streaming I/O, large files, or random access.

## Examples

### Atomic write with rename

```cdo
FUNCTION atomic_write(path, data) {
    VAR tmp = path + ".tmp";
    IF !file.write(tmp, data) { RETURN FALSE; }
    RETURN file.move(tmp, path);
}
```

### Recursive directory walk

```cdo
FUNCTION walk(dir, fn) {
    FOR name OF (file.list(dir) || []) {
        VAR path = dir + "/" + name;
        IF file.exists(path) {
            fn(path);
            // descend if it looks like a directory: open it as a list.
            IF file.list(path) != NULL { walk(path, fn); }
        }
    }
}

walk(".", FUNCTION(p) { print(p); });
```

### Streaming a large file

```cdo
VAR src = file.open("big.log", "r");
VAR sink = file.open("copy.log", "w");
src:pipeTo(sink);
src:close(); sink:close();
```
