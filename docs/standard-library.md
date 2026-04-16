# Standard Library

CanDo ships 17 library modules plus 3 built-in native globals.  The
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

The response object has fields `status`, `statusText`, `headers` (object), and `body` (string), plus a
`:json()` method that parses `body` using `json.parse`.

### Server

| Function | Description |
|---|---|
| `http.createServer(handler)` | Create a server; `handler(req, res)` is invoked for every connection on its own thread. |
| `https.createServer(handler, keyPath, certPath)` | TLS equivalent. |
| `server:listen(port, host*)` | Start accepting connections. |
| `server:close()` | Stop accepting new connections and let in-flight ones finish. |

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
