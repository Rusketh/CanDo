# Standard Library

CanDo ships 19 library modules plus 3 built-in native globals.  The
`cando` CLI registers all of them.  When you embed the library, call
`cando_openlibs(vm)` to register all, or an individual
`cando_open_*lib(vm)` to register only what you need — see
[embedding.md](embedding.md).

Conventions used below:

- `s`, `a`, `obj` — string, array, object
- `s:method(args)` means the method form.  Equivalent to
  `string.method(s, args)` for the string/array prototypes.
- Functions marked with `→` return a value; a list after `→` indicates
  multiple return values.
- `*` on an argument means optional with a default.

---

## Core natives (always present)

Registered unconditionally by `cando_open()`.

### `print(...)`

Writes each argument to standard output separated by spaces and
followed by a newline.  Arrays are expanded element-by-element.  Uses
`__tostring` on objects that define it.

### `type(v) → string`

Returns one of `"null"`, `"bool"`, `"number"`, `"string"`, `"object"`.
If `v` is an object with a `__type` field, returns that field's value
instead.

### `toString(v) → string`

Returns the canonical string representation.  If `v` is an object with
a `__tostring` meta-method, calls it and returns the result.

### `inspect(v, depth*) → string`

Returns a debug-friendly string showing the *contents* of arrays and
objects rather than their handle id.  Designed to be passed to `print`:

```cando
var data = { name: "Alice", scores: [10, 20, 30] };
print(inspect(data));
// { name: "Alice", scores: [10, 20, 30] }
```

Formatting:

| Value | Rendering |
|---|---|
| `null` / `true` / `false` | as-is |
| number | same as `toString(n)` |
| string | double-quoted, with `\\`, `\"`, `\n`, `\r`, `\t`, `\xNN` escapes |
| array | `[v1, v2, ...]` |
| object | `{ key: value, ... }` (FIFO insertion order; non-identifier keys are quoted) |
| function / native / thread | `<function>` / `<native>` / `<thread>` |

`depth` (default `0`) limits how many levels of nested arrays / objects
are expanded:

- `0` (default) — unlimited recursion.
- `N > 0` — nested arrays / objects beyond level `N` are truncated to
  `[...]` / `{...}`.

Cycles are detected on the current path and rendered as `<circular>`,
so `inspect` always terminates regardless of `depth`.  Two distinct
sub-trees that happen to reference the same object are *not* flagged
as cycles (they are printed twice).

```cando
var a = [1];
a:push(a);
print(inspect(a));            // [1, <circular>]

var deep = { a: { b: { c: 1 } } };
print(inspect(deep, 1));      // { a: {...} }
print(inspect(deep, 2));      // { a: { b: {...} } }
```

`inspect` does not invoke `__tostring`; it always shows raw structure,
which is what you want when debugging.  Use `toString(v)` when you do
want the meta-method.

---

## `math`

All angles are in radians.  `math.random` uses `rand()` seeded from
`time(NULL)` on first use.

| Function | Description |
|---|---|
| `math.clamp(v, min, max)` | Constrain `v` to `[min, max]`. |
| `math.min(...)` | Smallest numeric argument. |
| `math.max(...)` | Largest numeric argument. |
| `math.abs(x)` | Absolute value. |
| `math.sign(x)` | `-1`, `0`, or `1`. |
| `math.floor(x)` / `ceil(x)` / `round(x)` | Standard rounding. |
| `math.sqrt(x)` | Square root. |
| `math.pow(x, y)` | `x` raised to `y`. |
| `math.exp(x)` / `log(x)` / `log(x, base)` / `log10(x)` | Exponentials and logarithms. |
| `math.sin / cos / tan / asin / acos / atan` | Trigonometry. |
| `math.atan2(y, x)` | Two-argument arctangent. |
| `math.sinh / cosh` | Hyperbolic trig. |
| `math.rad(deg)` / `math.deg(rad)` | Angle conversion. |
| `math.random()` | Random `f64` in `[0, 1)`. |
| `math.random(max)` | Random integer in `[0, max)`. |
| `math.random(min, max)` | Random integer in `[min, max)`. |

Constants: `math.pi`, `math.tau`, `math.e`, `math.huge`.

---

## `string` (prototype)

`cando_open_stringlib` installs these as methods on the **string
prototype**, so every string value answers to them via `:` as well as
the `string.*` global.

| Method | Description |
|---|---|
| `s:length()` | Byte length of `s`. |
| `s:sub(start, end)` | Substring `[start, end)`. |
| `s:char(n)` | One-byte string at index `n` (0-based). |
| `s:chars()` | Array of one-byte strings. |
| `s:toLower()` / `s:toUpper()` | ASCII case conversion. |
| `s:trim()` | Strip ASCII whitespace from both ends. |
| `s:left(n)` / `s:right(n)` | First or last `n` bytes. |
| `s:repeat(n)` | `s` concatenated with itself `n` times. |
| `s:find(needle)` | Byte index of first match, or `-1`. |
| `s:split(sep)` | Array of parts; empty `sep` splits into characters. |
| `s:replace(old, new)` | Replace every occurrence. |
| `s:startsWith(prefix)` / `s:endsWith(suffix)` | Boolean. |
| `s:format(...)` | `%s`, `%d`, `%f`, `%%` substitution. |
| `s:match(pattern, start*, end*) → bool, array` | POSIX extended regex match; returns whether it matched and an array of capture groups. |

---

## `array` (prototype)

Array values answer to these methods via `:`.  Indices are 0-based.

| Method | Description |
|---|---|
| `a:length()` | Number of elements. |
| `a:push(v)` | Append `v`; return the new length. |
| `a:push(i, v)` | Insert `v` at index `i`; return the new length. |
| `a:pop()` | Remove and return the last element. |
| `a:remove(i)` | Remove and return the element at `i`. |
| `a:splice(start, count)` | Remove `count` elements starting at `start`; return the removed array. |
| `a:copy()` | Shallow copy. |
| `a:map(f)` | New array with `f(element)` applied to each element. |
| `a:filter(f)` | New array of the elements for which `f(element)` is truthy. |
| `a:reduce(f, init)` | Left fold: `f(acc, element)` with initial accumulator `init`. |

---

## `_meta` (global meta registry) {#meta-global-meta-registry}

`_meta` is a writable global object that holds **meta tables** — prototype
objects for built-in types.  Native libraries register a subtable per type
and stamp every instance they create with `instance.__index = _meta.<type>`,
so user code can attach methods that immediately become callable on every
instance.

```cando
print(type(_meta));                       // object
print(type(_meta.http_response));         // http_response

_meta.http_response.write = FUNCTION(self, data) {
    self.body = self.body + data;
};

http.createServer(FUNCTION(req, res) {
    res:write("Hello, world!");
    res:send();
});
```

The `_meta.<name>` subtable's `__type` field is stamped immutably (`FIELD_STATIC`)
to the type name, so `type(instance)` returns the type tag.  Method slots
(`status`, `send`, `listen`, …) use ordinary `FIELD_NONE` flags so user code
may override them.

Subtables registered by the standard library:

| Subtable | Used by |
|---|---|
| `_meta.string` | Same table as the global `string` and `vm->string_proto` -- the prototype consulted whenever a method is called on a string. |
| `_meta.array` | Same table as `array` / `vm->array_proto` -- consulted for methods on array receivers. |
| `_meta.object` | Same table as `object`.  Not auto-applied to plain objects; use `object.setPrototype(o, _meta.object)` to opt in. |
| `_meta.thread` | Per-instance methods for thread receivers (`t:done()`, `t:join()`, `t:state()`, `t:then(fn)`, …).  Aliased onto the same native sentinels exposed via `thread.<name>`. |
| `_meta.http_request` | Server-side request objects passed into `http.createServer`'s handler. |
| `_meta.http_response` | Server-side response objects.  Default methods: `status`, `setHeader`, `send`, `json`. |
| `_meta.http_server` | Server objects returned by `createServer`.  Default methods: `listen`, `close`. |
| `_meta.http_client_response` | Response objects returned by `http.get`, `https.get`, `fetch`, etc. |
| `_meta.tcp_socket` | Plain-TCP connections from the [`socket`](#socket) library.  Default methods: `connect`, `send`, `sendAll`, `recv`, `recvAll`, `recvLine`, `close`, `isOpen`, `setTimeout`, `setBlocking`, `setOption`, `fd`, `localAddress`, `remoteAddress`. |
| `_meta.tcp_server` | TCP listener objects.  Default methods: `listen`, `close`, `fd`, `localAddress`. |
| `_meta.tls_socket` | TLS connections from the [`secure_socket`](#secure_socket) library.  All `_meta.tcp_socket` methods plus `cipher`, `protocol`, `peerCertificate`. |
| `_meta.tls_server` | TLS listener objects.  Same surface as `_meta.tcp_server` with a TLS-aware `listen`. |

For `string`, `array`, `object`, and `thread` the meta table is the same
underlying CdoObject as the like-named global, so writing through either
name is observable through the other:

```cando
_meta.string.shout = FUNCTION(self) { RETURN self:toUpper() + "!"; };
print("hi":shout());        // HI!
print(string.shout("yes")); // YES!  -- same table, same method
```

You may also attach your own subtables at runtime (`_meta.foo = { ... }`)
and use them as prototypes via `object.setPrototype(instance, _meta.foo)`.

---

## `object`

Utilities for manipulating objects.  All take the object as the first
argument and skip the `__index` prototype chain unless stated.

| Function | Description |
|---|---|
| `object.lock(o)` | Acquire the re-entrant script lock on `o`. |
| `object.locked(o)` | `TRUE` if another thread holds the lock. |
| `object.unlock(o)` | Release the script lock. |
| `object.copy(o)` | Shallow copy of `o`'s own fields. |
| `object.assign(o, ...sources)` | Merge each source's own fields into `o`; return `o`. |
| `object.apply(o, ...sources)` | Like `assign`, but produces a new object. |
| `object.get(o, key)` | Raw field read; no prototype chain or meta-methods. |
| `object.set(o, key, value) → bool` | Raw field write; returns success. |
| `object.setPrototype(o, proto)` | Set `__index`.  `proto = NULL` removes it. |
| `object.getPrototype(o)` | Return `o.__index` or `NULL`. |
| `object.keys(o)` | Array of own keys in FIFO insertion order. |
| `object.values(o)` | Array of own values in FIFO insertion order. |

---

## `file`

Synchronous filesystem I/O.  Every function that can fail returns
`NULL` on error.  An optional `encoding` argument is accepted on text
functions and currently may be `"utf8"` (default) or `"binary"`.

| Function | Description |
|---|---|
| `file.read(path, encoding*)` | Read the whole file as a string. |
| `file.write(path, data, encoding*)` | Write `data`, truncating. Returns `TRUE`/`FALSE`. |
| `file.append(path, data, encoding*)` | Append. Returns `TRUE`/`FALSE`. |
| `file.exists(path) → bool` | File or directory exists? |
| `file.delete(path) → bool` | Remove a file. |
| `file.copy(src, dst) → bool` | Copy a file. |
| `file.move(src, dst) → bool` | Rename/move a file. |
| `file.size(path) → number` | Size in bytes, or `NULL`. |
| `file.lines(path, encoding*) → array` | Array of lines, without newline terminators. |
| `file.mkdir(path) → bool` | Create a directory.  Non-recursive. |
| `file.list(path) → array` | Array of directory entry names. |

---

## `json`

| Function | Description |
|---|---|
| `json.parse(text)` | Decode a JSON string into CanDo values; returns `NULL` on malformed input. |
| `json.stringify(value)` | Encode a CanDo value as JSON text. |

Objects serialise in FIFO insertion order; arrays serialise by index;
numbers use the shortest representation that round-trips.

---

## `csv`

| Function | Description |
|---|---|
| `csv.parse(text)` | Parse CSV text into an array of rows (each row is an array of string fields). |
| `csv.stringify(rows)` | Serialise an array of rows back to CSV text.  Fields containing the separator, quote, or newline are quoted. |

The parser accepts RFC 4180 quoting with doubled `""` escapes.

---

## `datetime`

| Function | Description |
|---|---|
| `datetime.now() → number` | Unix timestamp in seconds. |
| `datetime.format(timestamp, format*)` | Format using the host `strftime` syntax.  Default: `"%Y-%m-%d %H:%M:%S"`. |
| `datetime.parse(text, format)` | Inverse of `format`; returns `NULL` on mismatch.  Uses the local timezone. |

`datetime.parse` on Windows is a stub that returns `NULL` until a
proper `strptime` shim is available.

---

## `os`

| Function | Description |
|---|---|
| `os.getenv(name)` | Return the environment variable, or `NULL`. |
| `os.setenv(name, value) → bool` | Set or overwrite an environment variable. |
| `os.execute(cmd)` | Run `cmd` via the shell and return its captured stdout, or `NULL`. |
| `os.exit(code*)` | Terminate the process.  Default code is `0`. |
| `os.time() → number` | Unix timestamp in seconds. |
| `os.clock() → number` | Monotonic clock in seconds (process CPU time). |

---

## `crypto`

| Function | Description |
|---|---|
| `crypto.md5(data) → string` | Lower-case hexadecimal MD5 digest. |
| `crypto.sha256(data) → string` | Lower-case hexadecimal SHA-256 digest. |
| `crypto.base64Encode(data) → string` | Base64-encode a string. |
| `crypto.base64Decode(text) → string` | Base64-decode; returns `NULL` on invalid input. |

---

## `process`

| Function | Description |
|---|---|
| `process.pid() → number` | Current process ID. |
| `process.ppid() → number` | Parent process ID. |

---

## `net`

| Function | Description |
|---|---|
| `net.lookup(hostname) → string` | Resolve a hostname to an IPv4 address.  `NULL` on failure. |

---

## `socket`

Raw TCP sockets (IPv4 and IPv6, dual-stack via `getaddrinfo`).  The
`http` and `https` libraries are layered on top — reach for `socket`
when you need custom protocols, line-oriented servers, or any non-HTTP
network code.  See [socket.md](socket.md) for a long-form guide.

### Module functions

| Function | Description |
|---|---|
| `socket.tcp() → tcp_socket` | Create an unconnected TCP socket. |
| `socket.connect(host, port [, opts]) → tcp_socket` | Convenience: open + connect.  `opts.timeout` (ms), `opts.family` (`"inet"`/`"inet6"`/`"any"`). |
| `socket.createServer(callback) → tcp_server` | Mirrors `http.createServer`.  `callback(conn)` runs in a fresh child VM thread for every accepted connection. |
| `socket.resolve(host) → array` | Returns up to 16 IPv4/IPv6 numeric address strings, or `NULL` if resolution fails. |

### Connection methods (`_meta.tcp_socket`)

| Method | Description |
|---|---|
| `s:connect(host, port [, ms])` | Synchronous connect on an unconnected socket; throws on failure. Returns `self`. |
| `s:send(data) → bytes` | Single write; returns the number of bytes accepted. |
| `s:sendAll(data) → self` | Loop until every byte is written. |
| `s:recv(maxLen [, ms]) → string` | Up to `maxLen` bytes (default 4096; capped at 16 MB).  Returns `""` on clean EOF. |
| `s:recvAll() → string` | Read until EOF or peer close. |
| `s:recvLine([maxLen]) → string` | Read up to and including the next LF; CRLF or LF is stripped from the result.  `maxLen` defaults to 65536. |
| `s:close()` | Idempotent shutdown of the underlying transport. |
| `s:isOpen() → bool` | True iff the connection is still open. |
| `s:setTimeout(ms) → self` | Sets `SO_RCVTIMEO` and `SO_SNDTIMEO`; pass `0` to disable. |
| `s:setBlocking(bool) → self` | Toggle non-blocking mode (`O_NONBLOCK` / `FIONBIO`). |
| `s:setOption(name, value) → self` | Recognised names: `"tcp_nodelay"`, `"so_keepalive"`, `"so_reuseaddr"` (bool); `"so_rcvbuf"`, `"so_sndbuf"` (number). |
| `s:fd() → number` | Underlying file-descriptor (escape hatch for advanced use). |
| `s:localAddress() → object` | `{ host, port, family }` of the local endpoint, or `NULL`. |
| `s:remoteAddress() → object` | `{ host, port, family }` of the peer, or `NULL`. |

### Server methods (`_meta.tcp_server`)

| Method | Description |
|---|---|
| `srv:listen(port [, host [, backlog]]) → self` | Bind, listen, and spawn the accept worker.  Returns immediately; the calling script keeps running.  An optional callback may be passed in place of `host` or as the third argument. |
| `srv:close()` | Stop the accept loop and release resources. |
| `srv:fd() → number` | Listener fd. |
| `srv:localAddress() → object` | Bound address. |

### Errors

I/O failures (refused connect, peer reset, bind failure, `:recv` after
peer reset) and programmer errors (wrong types, calling `:send` on a
closed socket, unknown option name) all throw via the standard CanDo
error mechanism — wrap calls in `try { … } catch (e) { … }` to handle.
A clean EOF on `:recv` returns `""` and is *not* an error.

```cando
/* Non-blocking echo server.  Main thread continues past :listen. */
socket.createServer(FUNCTION(conn) {
    VAR data = conn:recv(4096);
    WHILE data != "" {
        conn:sendAll(data);
        data = conn:recv(4096);
    }
    conn:close();
}):listen(7000);
print("server up; main loop is free to do other work");
```

```cando
/* Extending sockets via _meta */
VAR LF = '
';
_meta.tcp_socket.writeLine = FUNCTION(self, line) { self:sendAll(line + LF); };

VAR s = socket.connect("127.0.0.1", 7000);
s:writeLine("ping");
print(s:recvLine());
s:close();
```

---

## `secure_socket`

TLS variant of `socket`.  The surface is a near mirror; the only
material differences are the additional opts on the constructors and
three TLS introspection methods.  OpenSSL is statically linked into
`libcando` so no extra runtime libraries are required beyond what
`http`/`https` already need.

### Module functions

| Function | Description |
|---|---|
| `secure_socket.tcp([opts]) → tls_socket` | Unconnected; opts seed the SSL_CTX and SNI hostname for a later `:connect`. |
| `secure_socket.connect(host, port [, opts]) → tls_socket` | Open + TLS handshake in one call.  `opts.verifyPeer` defaults to **true** (safer than `http`/`https` for the new API). |
| `secure_socket.createServer(opts, callback) → tls_server` | `opts.cert` and `opts.key` (PEM strings) are required.  Optional `opts.verifyPeer` + `opts.ca` enable client-cert verification. |

Client `opts`:

| Field | Type | Default | Description |
|---|---|---|---|
| `verifyPeer` | bool | `true` | Verify the server cert against the system trust store and the supplied CA bundle. |
| `ca` | string | — | PEM bundle of additional trust roots. |
| `cert`, `key` | strings | — | Client cert + key for mutual TLS. |
| `serverName` | string | host arg | SNI override; also used for hostname verification when `verifyPeer` is true. |
| `timeout` | number | `0` | Connect / per-call recv timeout in ms. |
| `family` | string | `"any"` | Address family. |

Server `opts`:

| Field | Type | Default | Description |
|---|---|---|---|
| `cert`, `key` | strings | — | **Required.** PEM-encoded leaf certificate (chain optional) and matching private key. |
| `verifyPeer` | bool | `false` | Require a client certificate. |
| `ca` | string | — | PEM bundle of acceptable client CAs (used with `verifyPeer`). |

### Methods (`_meta.tls_socket`, `_meta.tls_server`)

`_meta.tls_socket` inherits all of `_meta.tcp_socket`'s methods (the
shared connection surface) plus three TLS introspection helpers:

| Method | Description |
|---|---|
| `s:cipher() → string` | Negotiated cipher suite name (e.g. `"TLS_AES_256_GCM_SHA384"`), or `NULL`. |
| `s:protocol() → string` | Negotiated TLS version (e.g. `"TLSv1.3"`). |
| `s:peerCertificate() → object` | `{ subject, issuer, notBefore, notAfter, fingerprint }`.  Fingerprint is the lowercase-hex SHA-256 of the DER encoding. |

`_meta.tls_server` inherits the listener methods from
`_meta.tcp_server`; `:listen` is replaced with a TLS-aware variant that
runs the handshake on the connection's worker thread before invoking
the user callback.  By the time `callback(conn)` runs the TLS session
is established and `conn:cipher()`/`:peerCertificate()` are valid.

```cando
/* TLS client with default-on verification */
VAR s = secure_socket.connect("example.com", 443);
s:sendAll("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
print(s:recvAll());
s:close();
```

```cando
/* TLS server with cert + key */
VAR creds = include("modules/test_cert.cdo");
secure_socket.createServer({ cert: creds.cert, key: creds.key },
    FUNCTION(conn) {
        print("peer cipher:", conn:cipher());
        conn:sendAll("hello over TLS\n");
        conn:close();
    }):listen(8443);
```

> **Note.** `tls_socket` does *not* chain to `tcp_socket` via `__index`
> — that would make `type()` return `"tcp_socket"` for TLS sockets,
> which is misleading.  Methods you want available on both should be
> aliased explicitly: `_meta.tls_socket.write = _meta.tcp_socket.write;`.

---

## `thread`

See [threading.md](threading.md) for the full treatment.  The library
provides:

| Function | Description |
|---|---|
| `thread.sleep(ms)` | Block the current thread for `ms` milliseconds. |
| `thread.id()` | Numeric ID of the current thread. |
| `thread.done(t) → bool` | Has thread `t` finished (success or error)? |
| `thread.join(t)` | Block until `t` completes; return its return values. |
| `thread.cancel(t) → bool` | Request co-operative cancellation. |
| `thread.state(t) → string` | `"pending"`, `"running"`, `"done"`, `"error"`, or `"cancelled"`. |
| `thread.error(t)` | The value passed to `THROW` inside `t`, if it errored. |
| `thread.current()` | Current thread handle, or `NULL` on the main thread. |
| `thread.then(t, fn)` | Register a success callback; called with `t`'s return values. |
| `thread.catch(t, fn)` | Register an error callback; called with the thrown value. |

Every per-thread function (`done`, `join`, `cancel`, `state`, `error`,
`then`, `catch`) is also reachable through the `_meta.thread` prototype as
a method on the thread receiver itself:

```cando
VAR t = thread { RETURN 42; };
print(t:state());   // running | done
print(await t);     // 42
print(t:done());    // true
```

Because `_meta.thread` aliases the same native sentinels as `thread.<name>`,
both forms call the same underlying implementation -- and overrides applied
under either name take effect for both.

The language-level `thread { … }` expression and `await` operator are
described in [language-reference.md](language-reference.md).

---

## `http` and `https`

A blocking HTTP/1.1 client plus a thread-pool server.  `https` shares
the same implementation but requires OpenSSL and uses TLS.

### Client

| Function | Description |
|---|---|
| `http.get(url) → response` | Convenience GET. |
| `http.request(options) → response` | Arbitrary request. `options` is an object with `url`, `method`, `headers`, `body`. |
| `https.get(url) → response` | TLS equivalent. |
| `https.request(options) → response` | TLS equivalent. |
| `fetch(url, options*) → response` | Scheme-aware global; picks http vs https from the URL. |

Client responses inherit from `_meta.http_client_response`; the instance
itself carries `status`, `ok`, `body`, and `headers` fields and methods are
attached on the meta table so user code can extend it (see [`_meta`](#meta-global-meta-registry) below).

### Server

| Function | Description |
|---|---|
| `http.createServer(handler)` | Create a server; `handler(req, res)` is invoked for every connection on its own thread. |
| `https.createServer(handler, keyPath, certPath)` | TLS equivalent. |
| `server:listen(port, host*)` | Start accepting connections. |
| `server:close()` | Stop accepting new connections and let in-flight ones finish. |

Server `req` / `res` objects inherit from `_meta.http_request` and
`_meta.http_response`; `server` objects inherit from `_meta.http_server`.
The default response methods are:

| Method | Description |
|---|---|
| `res:status(code)` | Set the status code (returns the receiver for chaining). |
| `res:setHeader(name, value)` | Add a response header. |
| `res:send(body*)` | Flush the response.  If `body` is omitted, the receiver's `body` field (a string accumulator that defaults to `""`) is used. |
| `res:json(value)` | JSON-encode `value` and send it with `Content-Type: application/json`. |

`res:send()` may be invoked **synchronously from the handler**, or
**asynchronously from any other thread**.  The connection thread keeps the
TCP/TLS socket open after the handler returns and waits for `:send()` before
cleaning up.  Stash `res` on a global, hand it to a `thread { ... }`, or push
it into a queue — the response is only flushed once `:send()` runs.

```cando
VAR pending = NULL;
http.createServer(FUNCTION(req, res) {
    pending = res;            // defer
});
thread {
    thread.sleep(50);
    pending:send("delayed");  // sends from another thread
};
```

### Extending request and response objects

Because `req` and `res` follow the prototype chain, any function attached to
`_meta.http_response` (or `_meta.http_request`) becomes a method on every
instance.  This is the recommended way to build response helpers:

```cando
_meta.http_response.write = FUNCTION(self, data) {
    self.body = self.body + data;
};

http.createServer(FUNCTION(req, res) {
    res:write("Hello, ");
    res:write("world!");
    res:send();               // sends the accumulated body
});
```

---

## `eval`

### `eval(source) → any`

Compile `source` as a CanDo expression and execute it in the calling
VM.  The last expression is the return value.  Full access to enclosing
globals.  Parse or runtime errors are thrown and may be caught by the
caller.

---

## `include`

### `include(path) → any`

Load and cache a module.  Resolution rules:

- Absolute paths are canonicalised with `realpath()` and used directly.
- Relative paths are resolved relative to the **script's directory** —
  the nearest enclosing frame whose chunk name is an absolute path.
- `.cdo` files are parsed and executed; their top-level `RETURN` value
  (or the last expression) becomes the module value.
- `.so` / `.dylib` / `.dll` files are loaded with `dlopen`; the symbol
  `cando_module_init(CandoVM *) → CandoValue` is called once and its
  return value becomes the module value.  See
  [writing-extensions.md](writing-extensions.md).

Identical canonical paths share one cached value across the whole VM —
Node.js `require()` semantics.

Example:

```cando
// mylib.cdo
VAR lib = {};
lib.hello = FUNCTION(name) { RETURN `hi, ${name}`; };
RETURN lib;
```

```cando
// main.cdo
VAR my = include("./mylib.cdo");
print(my.hello("world"));           // hi, world
```
