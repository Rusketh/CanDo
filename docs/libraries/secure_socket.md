# `secure_socket`

TLS variant of [`socket`](socket.md).  The surface is a near-mirror;
the only material differences are the additional opts on the
constructors and three TLS introspection methods on connections.

OpenSSL is statically linked into `libcando` so no extra runtime
libraries are required beyond what `http`/`https` already need.

## Module functions

### `secure_socket.tcp(opts*) → tls_socket`

Unconnected TLS socket.  `opts` seed the SSL context and SNI hostname
for a later `:connect`.

### `secure_socket.connect(host, port, opts*) → tls_socket`

Open the TCP socket and perform the TLS handshake in one call.

```cdo
VAR s = secure_socket.connect("example.com", 443);
s:sendAll("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
print(s:recvAll());
s:close();
```

### `secure_socket.createServer(opts, callback) → tls_server`

`opts.cert` and `opts.key` (PEM strings) are required.  `opts.verifyPeer`
+ `opts.ca` enable client-cert verification.

```cdo
VAR creds = include("./certs.cdo");      // returns { cert: "...", key: "..." }
secure_socket.createServer({ cert: creds.cert, key: creds.key },
    FUNCTION(conn) {
        print("peer cipher:", conn:cipher());
        conn:sendAll("hello over TLS\n");
        conn:close();
    }):listen(8443);
```

## Client `opts`

| Field        | Type   | Default      | Description |
|--------------|--------|--------------|-------------|
| `verifyPeer` | bool   | `TRUE`       | Verify the server cert against the system trust store and the supplied CA bundle. |
| `ca`         | string | —            | PEM bundle of additional trust roots. |
| `cert`, `key`| strings| —            | Client cert + key for mutual TLS. |
| `serverName` | string | host arg     | SNI override; also used for hostname verification when `verifyPeer` is on. |
| `timeout`    | number | `0`          | Connect / per-call recv timeout in ms. |
| `family`     | string | `"any"`      | Address family. |

> `verifyPeer` defaults to **true**, which is safer than the legacy
> `http`/`https` defaults.

## Server `opts`

| Field        | Type   | Default | Description |
|--------------|--------|---------|-------------|
| `cert`, `key`| strings| —       | **Required.** PEM-encoded leaf certificate (chain optional) and matching private key. |
| `verifyPeer` | bool   | `FALSE` | Require a client certificate. |
| `ca`         | string | —       | PEM bundle of acceptable client CAs (used with `verifyPeer`). |

## Methods (`_meta.tls_socket`, `_meta.tls_server`)

`_meta.tls_socket` inherits all of `_meta.tcp_socket`'s methods (the
shared connection surface — `connect`, `send`, `recv`, `recvAll`,
`recvLine`, `close`, `setTimeout`, etc.) plus three TLS introspection
helpers:

### `s:cipher() → string | null`

Negotiated cipher suite name (e.g. `"TLS_AES_256_GCM_SHA384"`).

### `s:protocol() → string`

Negotiated TLS version (e.g. `"TLSv1.3"`).

### `s:peerCertificate() → object | null`

`{ subject, issuer, notBefore, notAfter, fingerprint }`.  `fingerprint`
is the lowercase-hex SHA-256 of the DER encoding.

```cdo
VAR cert = s:peerCertificate();
print(cert.subject);
print(cert.fingerprint);
```

`_meta.tls_server` inherits the listener methods from
`_meta.tcp_server`; `:listen` is replaced with a TLS-aware variant that
runs the handshake on the connection's worker thread before invoking
the user callback.  By the time `callback(conn)` runs the TLS session
is established and `conn:cipher()` / `:peerCertificate()` are valid.

> **Note.** `tls_socket` does **not** chain to `tcp_socket` via
> `__index` — that would make `type()` return `"tcp_socket"` for TLS
> sockets, which would be misleading.  Methods you want available on
> both should be aliased explicitly:
>
> ```cdo
> _meta.tls_socket.write = _meta.tcp_socket.write;
> ```

## Examples

### Mutual TLS client

```cdo
VAR s = secure_socket.connect("api.example.com", 443, {
    cert: file.read("client.crt"),
    key:  file.read("client.key"),
    ca:   file.read("ca.pem"),
});
s:sendAll("GET / HTTP/1.0\r\nHost: api.example.com\r\n\r\n");
print(s:recvAll());
s:close();
```

### Pinning a server certificate

```cdo
CONST EXPECTED_FP = "ab12cd34…";

VAR s = secure_socket.connect("internal.example.com", 443);
IF s:peerCertificate().fingerprint != EXPECTED_FP {
    s:close();
    THROW "certificate pin mismatch";
}
```
