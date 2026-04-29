# Streams

CanDo's `stream` library is a single byte-oriented I/O abstraction that
files, sockets, HTTP bodies, subprocesses, and in-memory buffers all
implement.  Anything that can produce bytes can be `:pipeTo()`'d into
anything that can consume them, in straight-line code or inside a
`thread { … }` block — no callbacks, no event loops.

This page is a hands-on companion to the API tables in
[standard-library.md](standard-library.md).

## Mental model

Four things to keep in mind:

1. **One vtable, many transports.**  The same six methods (`:read`,
   `:write`, `:pipeTo`, `:end`, `:close`, plus a few accessors) work on
   every adapter.  Once you know how to drain a memory buffer you know
   how to drain an HTTP body or a child-process pipe.

2. **`:pipeTo` is blocking.**  It loops `read → write` until the source
   reports EOF or one side closes.  Backpressure is automatic: each
   write blocks until the sink consumes.  To run a pipe off the calling
   thread, wrap it in CanDo's existing `thread { … }` syntax — there is
   no separate async API.

3. **Streams are safe to share between threads.**  The pool slot is
   guarded so a stream handed into a child thread will not corrupt the
   parent's view.  Use `stream.channel(cap)` when you specifically want
   a *byte channel* for transferring bytes between threads with
   producer/consumer backpressure.

4. **Errors throw.**  I/O failures (`read`/`write` returning an error
   from the underlying transport) and programmer mistakes (`:write` on
   a closed stream) raise CanDo exceptions; clean EOF returns `""` from
   `:read` and stops `:pipeTo` without an error.

## Constructors

| Call                            | Returns                                         |
|---------------------------------|-------------------------------------------------|
| `stream.memory([initialBytes])` | Duplex in-memory buffer (compacts as it drains) |
| `stream.channel([capacity])`    | Bounded thread channel — backpressure built-in  |
| `file.open(path, mode)`         | File-backed stream (`r/w/a`, optional `b`/`+`)  |
| `sock:stream()`                 | Duplex view of an open TCP / TLS socket         |
| `res:stream()`                  | Writable view of a server `http_response`       |
| `clientResponse:stream()`       | Readable view of an HTTP client response body   |
| `proc:stdin()` / `:stdout()` / `:stderr()` | Pipes for a spawned subprocess (when opened with `pipe`) |

## Stream methods

```
s:read(maxLen [, timeoutMs])    -- string ("" on clean EOF)
s:readAll()                     -- drain to EOF
s:write(data)                   -- bytes consumed
s:writeAll(data)                -- loops until done; returns self
s:flush()                       -- adapter-defined
s:end()                         -- half-close write side
s:close()                       -- full close, idempotent
s:pipeTo(dst [, opts])          -- blocking copy; returns bytes copied
s:isClosed() / :error() / :bytesIn() / :bytesOut() / :kind()
```

`pipeTo` accepts an options object: `{ chunk: <bytes> }` overrides the
default 64 KiB transfer block.

> **Why `pipeTo` and not `pipe`?**  `pipe` is reserved as the implicit
> loop variable in CanDo's `~>` pipe expressions and isn't permitted as
> a method name.

## Examples

### Copy a file, no script-side buffering

```cando
VAR src = file.open("input.bin",  "rb");
VAR dst = file.open("output.bin", "wb");
src:pipeTo(dst);
src:close();
dst:close();
```

### Download an HTTP response straight to disk

```cando
VAR resp = fetch("https://example.com/big.zip");
VAR out  = file.open("big.zip", "wb");
resp:stream():pipeTo(out);
out:close();
```

### Echo server using socket streams

```cando
socket.createServer(FUNCTION(conn) {
    VAR cs = conn:stream();
    cs:pipeTo(cs);                  /* echo source -> sink, same fd */
    conn:close();
}):listen(7000);
```

### Producer / consumer across threads

```cando
VAR ch = stream.channel(64);        /* 64-byte buffer applies backpressure */

thread {
    FOR i OF 1 -> 1000 {
        ch:writeAll("line " + i + "\n");
    }
    ch:end();
};

/* Main thread drains as fast as it can; the producer blocks when full. */
VAR sink = stream.memory();
ch:pipeTo(sink);
print(sink:bytesIn());
```

### Capture a subprocess's stdout

```cando
VAR p = process.spawn(["ls", "-la"], { stdout: "pipe" });
VAR out = stream.memory();
p:stdout():pipeTo(out);
print(p:wait());                    /* exit code */
print(out:readAll());
```

### Pipe a file out as the body of an HTTP response

```cando
http.createServer(FUNCTION(req, res) {
    VAR f = file.open("download.zip", "rb");
    VAR rs = res:stream();
    f:pipeTo(rs);
    rs:end();                       /* flush the buffered response */
    f:close();
}):listen(8080);
```

## Threading and lifetimes

A stream is a thin handle into a global pool slot; its meaningful state
lives in the slot.  Capabilities (`readable`, `writable`, `seekable`)
are fixed at construction.

* The slot is reference-checked on every method call — once `:close()`
  fires, subsequent calls on any handle pointing at it see "closed".
* Adapters that may *block* inside `read`/`write` (channel, full-duplex
  socket) skip the slot lock and rely on their own internal mutex.  All
  other adapters serialise read against write via the slot lock so a
  stream handed to a child thread can't tear.
* `:pipeTo` runs on whatever thread invoked it.  To do it in the
  background, wrap it: `thread { src:pipeTo(dst); }`.  The existing
  `cando_vm_wait_all_threads` machinery ensures the process won't exit
  before the pipe finishes.

## Limitations

* **HTTP bodies are still buffered.**  Both `clientResponse:stream()`
  and `res:stream()` work over the existing buffered body.  True
  on-the-wire streaming (chunked downloads, chunked sends) is a
  follow-up that requires teaching `http_read_response` to expose the
  unread connection.
* **`process.spawn` is POSIX-only.**  The Windows path errors with
  "not implemented".  `process.pid()` / `process.ppid()` work
  everywhere.
* **No transform streams yet.**  If you need to map / filter bytes mid-
  pipe, drain into a `stream.memory()`, transform, and pipe out.  A
  proper transform adapter will follow once the use cases are clearer.
