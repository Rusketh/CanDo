# Sockets

CanDo's `socket` and `secure_socket` libraries expose raw TCP and
TLS-wrapped TCP at the script level.  They sit alongside the higher-
level `http` / `https` libraries and use the same underlying
implementation (`source/lib/sockutil.c`); reach for the socket libraries
when you need a custom protocol, line-oriented servers, or any non-
HTTP network code.

This page is a hands-on companion to the API tables in
[standard-library.md](standard-library.md).

## Mental model

Three things to keep in mind:

1. **Servers do not block the calling thread.**  `:listen(port)`
   returns immediately, exactly like `http.createServer(cb):listen(port)`
   does today.  A background worker accepts connections and spawns a
   fresh child VM thread for each one; that thread runs your callback.
   Blocking `:recv` / `:sendAll` inside the callback only block *that*
   connection's worker thread.

2. **The connection callback owns the connection's lifetime.**  When
   the callback returns, the underlying socket is closed and the pool
   slot reused.  Run a `WHILE` loop inside the callback for streaming
   protocols.

3. **Errors throw.**  I/O failures (refused connect, peer reset, bind
   collision, broken pipe) and programmer errors (calling `:send` on a
   closed socket, unknown options) raise CanDo exceptions; wrap calls
   in `try { … } catch (e) { … }` to handle them.  A clean EOF on
   `:recv` returns the empty string `""` and is *not* an error.

## A complete echo server

```cando
VAR LF = '
';

socket.createServer(FUNCTION(conn) {
    /* This runs in its own child VM thread. */
    VAR line = conn:recvLine();
    WHILE line != "" {
        conn:sendAll(line + LF);
        line = conn:recvLine();
    }
    conn:close();
}):listen(7000);

print("listening on 7000");

/* Main thread is free.  Do other work here, await a signal, etc. */
```

## A line-based protocol client

```cando
VAR LF = '
';

VAR s = socket.connect("127.0.0.1", 7000);
s:sendAll("ping" + LF);
s:sendAll("pong" + LF);

print(s:recvLine());   /* "ping"  -- echoed by the server above */
print(s:recvLine());   /* "pong" */
s:close();
```

## Extending sockets via `_meta`

The same `_meta` registry that powers `_meta.http_response.write` works
for sockets.  Add a method once and it is visible on every socket
instance, including those created in a child VM:

```cando
VAR LF = '
';

_meta.tcp_socket.writeLine = FUNCTION(self, line) {
    self:sendAll(line + LF);
};

socket.createServer(FUNCTION(conn) {
    conn:writeLine("hi");
    conn:close();
}):listen(7001);
```

Methods on `_meta.tls_socket` are *not* shared with `_meta.tcp_socket`
by default — `tls_socket` does not chain to `tcp_socket` via `__index`
because doing so would make `type()` return `"tcp_socket"` for TLS
sockets.  If you want a method available on both, alias it:

```cando
_meta.tls_socket.writeLine = _meta.tcp_socket.writeLine;
```

## TLS with `secure_socket`

`secure_socket` is a near-mirror of `socket` plus a small set of TLS
options on the constructors and three introspection methods on the
connection.  The biggest behavioural difference from `https`: the
client default is **`verifyPeer: true`**.  Pass `verifyPeer: false`
explicitly for self-signed dev environments.

```cando
/* Verified connection to a public site (system trust store). */
VAR s = secure_socket.connect("example.com", 443);
s:sendAll("GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n");
print(s:recvAll());
print(s:cipher());      /* e.g. TLS_AES_256_GCM_SHA384 */
print(s:protocol());    /* e.g. TLSv1.3 */
s:close();
```

For a TLS server, supply PEM `cert` and `key` strings.  These can be
read from disk with `file.read(...)` or vendored into a module so the
script is self-contained:

```cando
VAR creds = include("modules/test_cert.cdo");
secure_socket.createServer({ cert: creds.cert, key: creds.key },
    FUNCTION(conn) {
        conn:sendAll("hello over TLS\n");
        conn:close();
    }):listen(8443);
```

To require client certificates ("mTLS"), set `verifyPeer: true` on the
server opts and supply `ca` (a PEM bundle of acceptable client CAs).

## OS portability

The libraries aim to behave identically on Linux, macOS, and Windows
(MinGW).  A few things to know:

- `signal(SIGPIPE, SIG_IGN)` is installed once at first use of either
  library, so writes to a peer that has closed return `-1`/`EPIPE`
  instead of killing the process.  This is process-wide; if your
  embedding application relies on `SIGPIPE` for something else, it
  must restore its own handler after `cando_open()`.
- `WSAStartup` is called once at first use of either library on
  Windows; you do not need to call it from your embedding code.
- IPv6 dual-stack listeners use `IPV6_V6ONLY=0` so a bind to `::`
  catches IPv4-mapped clients too.  Pass `family: "inet"` if you
  want strict IPv4 only.

## Best practices

- Always check whether `:recv` returned `""` and break out of read
  loops on clean EOF.
- Prefer `:sendAll` over `:send` when you have a complete message —
  partial writes happen, and `:sendAll` loops for you.
- `socket.lastError()` is *not* part of the API.  Errors throw; catch
  them with `try` / `catch`.
- Don't reuse a socket after `:close`.  Allocate a new one with
  `socket.tcp()` or `socket.connect(...)`.
- The pool is sized for 256 concurrent socket instances (combined
  across `socket` and `secure_socket`).  If you build a busy server,
  monitor for the `"too many active sockets"` error from
  `socket.createServer` and similar — that is the pool-exhaustion
  signal.
- Ports below 1024 require elevated privileges on POSIX; pick a high
  port (4096+) for dev work to avoid surprises.

## Embedding API

To open the libraries selectively from C, link against `libcando` and
call:

```c
#include <cando.h>

CandoVM *vm = cando_open();
cando_open_metalib(vm);          /* required by socket / secure_socket */
cando_open_socketlib(vm);
cando_open_secure_socketlib(vm);
```

`cando_openlibs(vm)` opens both alongside every other standard library.

## See also

- [standard-library.md](standard-library.md#socket) — full method
  tables.
- [standard-library.md](standard-library.md#secure_socket) — TLS
  variant.
- [metamethods.md](metamethods.md) — how `_meta` dispatch works.
- [`tests/scripts/lib_socket.cdo`](../tests/scripts/lib_socket.cdo) and
  [`tests/scripts/lib_secure_socket.cdo`](../tests/scripts/lib_secure_socket.cdo)
  for working integration scripts.
