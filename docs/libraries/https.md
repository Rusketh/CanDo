# `https`

TLS variant of [`http`](http.md).  Same surface; everything goes
through OpenSSL.

## Client

### `https.get(url) → response`

```cdo
VAR res = https.get("https://example.com/");
print(res.status);
```

### `https.request(options) → response`

Same fields as `http.request`, with one addition:

| Field        | Type | Default | Description |
|--------------|------|---------|-------------|
| `verifyPeer` | bool | `FALSE` | Verify the server certificate.  Defaults to `FALSE` for backwards compatibility — flip to `TRUE` for any production code, or use the [`secure_socket`](secure_socket.md) library which defaults verification on. |

```cdo
VAR res = https.request({
    url: "https://api.example.com/items",
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: json.stringify({ name: "widget" }),
    verifyPeer: TRUE,
});
```

## `fetch(url, options*) → response`

Scheme-aware global.  Picks `https` automatically when the URL begins
with `https://`.  See [http.md](http.md).

## Server

### `https.createServer(opts, handler) → server`

Like `http.createServer`, but the first argument is an options object
containing PEM-encoded `cert` and `key` **strings** (not file paths):

```cdo
VAR opts = {
    cert: file.read("server.crt"),
    key:  file.read("server.key"),
};

https.createServer(opts, FUNCTION(req, res) {
    res:json({ secure: TRUE });
}):listen(8443);
```

For more control over the TLS configuration (cipher suites, mutual
TLS, SNI, …) use [`secure_socket.createServer`](secure_socket.md).

## Notes

- The `https` namespace shares the bulk of its implementation with
  `http`.  Anything documented in [http.md](http.md) — the response
  object, the `_meta.http_response` extension story, deferred
  `res:send()`, etc. — applies to `https` too.
- Outbound `https` requests do **not** verify peer certificates by
  default.  Pass `verifyPeer: TRUE`, or use `secure_socket.connect`
  directly when verification matters.
