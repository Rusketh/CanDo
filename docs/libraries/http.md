# `http`

A blocking HTTP/1.1 client and a thread-pool server.  `https` shares
the same implementation but requires OpenSSL and uses TLS — see
[https.md](https.md).

## Client

### `http.get(url) → response`

Convenience GET request.

```cdo
VAR res = http.get("http://example.com/");
print(res.status);                 // 200
print(res.body);
```

### `http.request(options) → response`

Arbitrary request.  `options` is an object:

| Field      | Type   | Default | Description |
|------------|--------|---------|-------------|
| `url`      | string | —       | Required.  Full URL. |
| `method`   | string | `"GET"` | `"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, `"PATCH"`, `"HEAD"`. |
| `headers`  | object | `{}`    | Request headers. |
| `body`     | string | `""`    | Request body. |
| `timeout`  | number | `0`     | Per-request timeout in ms. |
| `stream`   | bool   | `FALSE` | If `TRUE`, the response body is read lazily through `res:stream()` instead of buffered. |

```cdo
VAR res = http.request({
    url: "http://api.example.com/items",
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: json.stringify({ name: "widget" }),
});
print(res.status);                 // 201
```

### `fetch(url, options*) → response`

Scheme-aware global; picks `http` vs `https` automatically based on the
URL scheme.  Most code can use `fetch` and ignore the namespace
distinction.

```cdo
VAR res = fetch("https://api.example.com/items");
```

### Response object

The instance carries:

- `status` — number
- `ok` — `TRUE` if `status` is in `[200, 300)`
- `body` — string
- `headers` — object (lowercased keys)

…and methods inherited from `_meta.http_client_response`:

- `res:stream() → stream` — readable view of the response body.  When
  the request was issued with `{ stream: TRUE }`, the body is drained
  lazily from the live connection; otherwise it is a memory stream
  over the buffered body.

## Server

### `http.createServer(handler) → server`

Create a server.  `handler(req, res)` is invoked for every connection
on its own thread.

```cdo
http.createServer(FUNCTION(req, res) {
    res:status(200);
    res:setHeader("Content-Type", "text/plain");
    res:send("Hello, world!");
}):listen(8080);

print("listening on :8080");
```

### `server:listen(port, host*)`

Start accepting connections.  Returns the server (chainable).

### `server:close()`

Stop accepting new connections; in-flight ones finish.

### Request object (`req`)

Inherits from `_meta.http_request`.  Carries `method`, `url`, `path`,
`query`, `headers`, `body` fields.

### Response object (`res`)

Inherits from `_meta.http_response`.  Default methods:

| Method                    | Description |
|---------------------------|-------------|
| `res:status(code)`        | Set status code; returns the receiver for chaining. |
| `res:setHeader(name, v)`  | Add a response header. |
| `res:send(body*)`         | Flush the response.  If `body` is omitted, the receiver's `body` field (a string accumulator that defaults to `""`) is used. |
| `res:json(value)`         | JSON-encode `value` and send with `Content-Type: application/json`. |
| `res:stream() → stream`   | Writable view; `:end()` flushes the buffered response. |

`res:send()` may be invoked **synchronously from the handler** or
**asynchronously from any other thread**.  The connection thread keeps
the socket open after the handler returns and waits for `:send()` before
cleaning up.  Stash `res` on a global, hand it to a `thread { … }`, or
push it into a queue — the response is only flushed once `:send()` runs.

```cdo
VAR pending = NULL;
http.createServer(FUNCTION(req, res) {
    pending = res;            // defer
});
thread {
    thread.sleep(50);
    pending:send("delayed");  // sends from another thread
};
```

## Extending request and response objects

`req` and `res` follow the prototype chain.  Anything you attach to
`_meta.http_response` (or `_meta.http_request`) becomes a method on
every instance immediately:

```cdo
_meta.http_response.write = FUNCTION(self, data) {
    self.body = self.body + data;
};

http.createServer(FUNCTION(req, res) {
    res:write("Hello, ");
    res:write("world!");
    res:send();               // sends the accumulated body
}):listen(8080);
```

## Examples

### Streaming a large download to a file

```cdo
VAR res = http.request({ url: "http://example.com/big.bin", stream: TRUE });
res:stream():pipeTo(file.open("big.bin", "wb"));
```

### JSON API server

```cdo
http.createServer(FUNCTION(req, res) {
    IF req.method == "POST", "PUT" {
        VAR body = json.parse(req.body) || {};
        res:json({ ok: TRUE, echo: body });
    } ELSE {
        res:status(405):send("method not allowed");
    }
}):listen(8080);
```

### Long-polling

```cdo
VAR waiters = [];

http.createServer(FUNCTION(req, res) {
    IF req.path == "/wait" {
        waiters:push(res);                     // park
    } ELSE IF req.path == "/notify" {
        FOR w OF waiters { w:send(req.body); }
        waiters = [];
        res:send("ok");
    }
}):listen(8080);
```
