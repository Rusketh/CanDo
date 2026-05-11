# `stream`

The `stream` library is a single byte-oriented I/O abstraction shared
by files, sockets, HTTP bodies, subprocess pipes, in-memory buffers,
and thread channels.

A stream is duplex: it has read methods and write methods, but
particular adapters may make one side read-only or write-only.  Streams
are reference-counted and clean themselves up when the last reference
is dropped — `:close()` is idempotent.

## Constructors

### `stream.memory(initialBytes*) → stream`

Duplex in-memory buffer.  Auto-compacts as the reader drains.  Useful
as a glue between producers and consumers in the same thread.
`initialBytes` is clamped to `[64, 64 MiB]`.

```cdo
VAR buf = stream.memory();
buf:write("hello");
print(buf:read(5));            // hello
```

### `stream.channel(capacity*) → stream`

Bounded thread channel.  Reads block while empty; writes block while
full.  `capacity` is clamped to `[64, 64 MiB]`.

```cdo
VAR ch = stream.channel(100);

VAR producer = thread {
    FOR i IN 1 -> 1000 { ch:write(`item ${i}\n`); }
    ch:end();
};

VAR consumer = thread {
    WHILE TRUE {
        VAR msg = ch:read(64);
        IF msg == "" { RETURN; }      // EOF
        process(msg);
    }
};

await producer;
await consumer;
```

### `stream.transform(fn) → stream`

Pipes every chunk through `fn(chunk) → chunk`.  Returning `NULL` (or a
non-string) drops the chunk.  Errors thrown inside `fn` are **logged
and swallowed**, not surfaced to the writer or reader — the offending
chunk is simply dropped.  Use a regular function call (not
`stream.transform`) if you need errors to propagate.

```cdo
VAR upcase = stream.transform(FUNCTION(chunk) { RETURN chunk:toUpper(); });
upcase:write("hello world");
print(upcase:read(11));         // HELLO WORLD
```

## Methods (`_meta.stream`)

### `s:read(maxLen*) → string`

Read up to `maxLen` bytes (default `4096`, capped at 16 MiB).  Returns
`""` on clean EOF.  Throws if the stream is not readable or has been
closed with an error.

### `s:readAll() → string`

Drain to EOF and return everything.

### `s:write(data) → number`

Write.  Returns the number of bytes consumed (less than `#data` only on
non-blocking adapters).

### `s:writeAll(data) → stream`

Loop until all bytes are written.  Returns `self`.

### `s:flush() → stream`

Adapter-defined.  No-op for memory, channel, and socket adapters.
Forces a buffered file adapter to commit.

### `s:end() → stream`

Half-close the write side.  Signals EOF to readers.

### `s:close() → stream`

Full close.  Idempotent.

### `s:pipeTo(dst, opts*) → number`

Blocking copy from `s` to `dst` until `s` reports EOF.  Returns the
total bytes copied.  `opts.chunk` overrides the 64 KiB transfer block
size.

For non-blocking pipe-through behaviour, wrap `pipeTo` in `thread { …
}`.

> **Naming note:** the method is `:pipeTo`, not `:pipe`, because `pipe`
> is reserved as the implicit loop variable in CanDo's `~>`/`~!>`/`~&>`
> expressions.

### `s:isClosed() → bool`

`TRUE` after `:close()` or once the underlying transport is gone.

### `s:error() → string`

Last error message reported by the adapter; `""` if none.

### `s:bytesIn() → number`, `s:bytesOut() → number`

Total bytes read or written through any method.

### `s:kind() → string`

Adapter name: `"memory"`, `"file"`, `"tcp"`, `"tls"`, `"channel"`,
`"transform"`, `"http_body"`, `"http_response"`.

## Adapters on existing handles

| Call                                            | Returns |
|-------------------------------------------------|---------|
| `file.open(path, mode)`                         | File-backed stream. |
| `tcp_socket:stream()` / `tls_socket:stream()`   | Duplex view of a connected socket.  Does not own the socket. |
| `res:stream()` (server)                         | Writable view of a server `http_response`; `:end()` flushes the buffered response. |
| `clientResponse:stream()`                       | Readable view of an HTTP client response body. |
| `proc:stdin()` / `:stdout()` / `:stderr()`      | Pipes for a spawned subprocess. |

## Examples

### File copy

```cdo
file.open("src.bin", "rb"):pipeTo(file.open("dst.bin", "wb"));
```

### Run a streaming transform

```cdo
VAR src   = file.open("input.txt", "r");
VAR upper = stream.transform((chunk) => chunk:toUpper());
VAR dst   = file.open("UPPER.txt", "w");

src:pipeTo(upper);
upper:end();
upper:pipeTo(dst);
```

### Multi-producer / single-consumer queue

```cdo
VAR queue = stream.channel(0);

VAR producers = [];
FOR i IN 1 -> 4 {
    producers:push(thread {
        FOR j IN 1 -> 100 { queue:write(`P${i}:${j}\n`); }
    });
}

VAR consumer = thread {
    WHILE TRUE {
        VAR msg = queue:read(64);
        IF msg == "" { RETURN; }
        print(msg);
    }
};

FOR p OF producers { await p; }
queue:end();
await consumer;
```

### Backpressure for a slow consumer

A `stream.channel(N)` with a small `N` provides natural backpressure:
producers block once the channel fills, so they can't outrun a slow
consumer.
