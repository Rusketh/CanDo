# `socket`

Raw TCP sockets (IPv4 and IPv6, dual-stack via `getaddrinfo`).  The
[`http`](http.md) and [`https`](https.md) libraries are layered on top
— reach for `socket` when you need custom protocols, line-oriented
servers, or any non-HTTP network code.

For TLS connections see [`secure_socket`](secure_socket.md).

## Module functions

### `socket.tcp() → tcp_socket`

Create an unconnected TCP socket.

```cdo
VAR s = socket.tcp();
s:connect("example.com", 80, 5000);
```

### `socket.connect(host, port, opts*) → tcp_socket`

Convenience: open a socket and connect in one call.

`opts`:

| Field      | Type   | Default | Description |
|------------|--------|---------|-------------|
| `timeout`  | number | `0`     | Connect timeout (ms).  `0` is no timeout. |
| `family`   | string | `"any"` | `"inet"`, `"inet6"`, or `"any"`. |

```cdo
VAR s = socket.connect("example.com", 80);
s:sendAll("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
print(s:recvAll());
s:close();
```

### `socket.createServer(callback) → tcp_server`

Mirror of [`http.createServer`](http.md).  `callback(conn)` runs in a
fresh child VM thread for every accepted connection.

```cdo
socket.createServer(FUNCTION(conn) {
    VAR data = conn:recv(4096);
    WHILE data != "" {
        conn:sendAll(data);
        data = conn:recv(4096);
    }
    conn:close();
}):listen(7000);

print("echo server up");
```

### `socket.resolve(host) → array | null`

Returns up to 16 IPv4/IPv6 numeric address strings, or `NULL` if
resolution fails.

```cdo
print(inspect(socket.resolve("example.com")));
// ["93.184.216.34", "2606:2800:220:1:248:1893:25c8:1946"]
```

## Connection methods (`_meta.tcp_socket`)

### `s:connect(host, port, ms*) → tcp_socket`

Synchronous connect on an unconnected socket.  Throws on failure.
Returns `self`.  `ms` is the timeout in milliseconds (default `0`).

### `s:send(data) → number`

Single write.  Returns the number of bytes accepted (which may be less
than `#data` for non-blocking sockets).

### `s:sendAll(data) → tcp_socket`

Loop until every byte is written.  Returns `self`.

### `s:recv(maxLen*, ms*) → string`

Read up to `maxLen` bytes (default 4096; capped at 16 MB).  Returns
`""` on clean EOF.

### `s:recvAll() → string`

Read until EOF or peer close.

### `s:recvLine(maxLen*) → string`

Read up to and including the next LF.  CRLF or LF is stripped from the
returned string.  `maxLen` defaults to 65536.

### `s:close()`

Idempotent shutdown of the underlying transport.

### `s:isOpen() → bool`

`TRUE` if the connection is still open.

### `s:setTimeout(ms) → tcp_socket`

Sets `SO_RCVTIMEO` and `SO_SNDTIMEO`.  Pass `0` to disable.

### `s:setBlocking(bool) → tcp_socket`

Toggle non-blocking mode.

### `s:setOption(name, value) → tcp_socket`

Recognised names:

| Name             | Type | Notes                          |
|------------------|------|--------------------------------|
| `tcp_nodelay`    | bool | Disable Nagle.                 |
| `so_keepalive`   | bool | TCP keepalive probes.          |
| `so_reuseaddr`   | bool | `SO_REUSEADDR` for the listener. |
| `so_rcvbuf`      | num  | `SO_RCVBUF` size hint.         |
| `so_sndbuf`      | num  | `SO_SNDBUF` size hint.         |

### `s:fd() → number`

Underlying file descriptor.  Escape hatch for advanced use.

### `s:localAddress() → object | null`, `s:remoteAddress() → object | null`

`{ host, port, family }` of the local or peer endpoint, or `NULL`.

## Server methods (`_meta.tcp_server`)

### `srv:listen(port, host*, backlog*) → tcp_server`

Bind, listen, and spawn the accept worker.  Returns immediately; the
calling script keeps running.

An optional callback may be passed in place of `host` or as the third
argument:

```cdo
socket.createServer():listen(7000, "0.0.0.0", FUNCTION(conn) {
    // ...
});
```

### `srv:close()`

Stop the accept loop and release resources.

### `srv:fd() → number`

Listener fd.

### `srv:localAddress() → object`

Bound address.

## Errors

I/O failures (refused connect, peer reset, bind failure, `:recv` after
peer reset) and programmer errors (wrong types, calling `:send` on a
closed socket, unknown option name) all throw via the standard CanDo
error mechanism.

A clean EOF on `:recv` returns `""` and is **not** an error.

```cdo
TRY {
    VAR s = socket.connect("nowhere.invalid", 80, 1000);
} CATCH (e) {
    print("connect failed:", e);
}
```

## Examples

### Echo server

```cdo
socket.createServer(FUNCTION(conn) {
    VAR data = conn:recv(4096);
    WHILE data != "" {
        conn:sendAll(data);
        data = conn:recv(4096);
    }
    conn:close();
}):listen(7000);
```

### Line-oriented protocol

```cdo
VAR LF = "\n";
_meta.tcp_socket.writeLine = FUNCTION(self, line) {
    self:sendAll(line + LF);
};

VAR s = socket.connect("127.0.0.1", 7000);
s:writeLine("ping");
print(s:recvLine());          // "pong"
s:close();
```

### Working through a stream

`tcp_socket:stream()` returns a [`stream`](stream.md) view of the socket
— useful when you want to feed it into `pipeTo`:

```cdo
VAR s = socket.connect("api.example.com", 80);
s:sendAll("GET / HTTP/1.0\r\n\r\n");
s:stream():pipeTo(file.open("response.txt", "w"));
```
