# Library Reference

Per-namespace, per-function reference for CanDo's standard library.
Every namespace gets a dedicated file with a description, signature,
return value, error behaviour, and at least one worked example for every
function.

These libraries are registered automatically by the `cando` CLI.  When
embedding, call `cando_openlibs(vm)` to register all of them at once,
or `cando_open_<name>lib(vm)` for one library individually — see
[../api/embedding.md](../api/embedding.md).

## Globals (always present)

These are not in any namespace; they live as plain global functions.

| Function                     | Page |
|------------------------------|------|
| `print(...)`                 | [globals.md](globals.md) |
| `type(v)`                    | [globals.md](globals.md) |
| `toString(v)`                | [globals.md](globals.md) |
| `inspect(v, depth*)`         | [globals.md](globals.md) |
| `eval(source)`               | [eval.md](eval.md) |
| `include(path)`              | [include.md](include.md) |
| `fetch(url, options*)`       | [http.md](http.md) |
| `await expr`                 | language form, see [../language/threading.md](../language/threading.md) |

## Namespaces

| Namespace        | Page                                | What it covers |
|------------------|-------------------------------------|----------------|
| `array`          | [array.md](array.md)               | Array prototype methods — `push`/`pop`/`splice`/`map`/`filter`/`reduce` plus the full JS-style surface (`indexOf`, `includes`, `find`, `some`, `every`, `flat`, `concat`, `slice`, `join`, `sort`, `unique`, set ops, …). |
| `string`         | [string.md](string.md)             | String prototype methods, `string.format`, padding (`padStart`/`padEnd`), substring search (`indexOf`/`includes`), UTF-8 codepoint helpers. |
| `math`           | [math.md](math.md)                 | Trig, hyperbolic, log/exp, rounding (`floor`/`ceil`/`trunc`), `hypot`, `cbrt`, `log2`, random, constants. |
| `object`         | [object.md](object.md)             | Object utilities, prototypes, locks, JS-style `entries`/`fromEntries`/`has`/`freeze`/`seal`. |
| `console`        | [console.md](console.md)           | Terminal control — cursor, colour, raw-mode keyboard and mouse, line editing, async dispatcher, `attach`/`detach`/`hide`/`show` for GUI / service scripts. |
| `json`           | [json.md](json.md)                 | JSON parse and stringify. |
| `yaml`           | [yaml.md](yaml.md)                 | YAML parse and stringify. |
| `csv`            | [csv.md](csv.md)                   | CSV parse and stringify. |
| `file`           | [file.md](file.md)                 | Synchronous filesystem I/O, `stat`, path helpers (`basename`, `dirname`, `extname`, `join`, `resolve`, `realpath`). |
| `os`             | [os.md](os.md)                     | Time, environment, process exit, system info (`hostname`, `tmpdir`, `homedir`, `arch`, `platform`, `cpus`, memory). |
| `process`        | [process.md](process.md)           | Spawning child processes, pipes. |
| `datetime`       | [datetime.md](datetime.md)         | Formatting, parsing, component accessors, date math, calendar helpers. |
| `crypto`         | [crypto.md](crypto.md)             | Full Node.js `node:crypto` parity — hashes (md5/sha1-2/sha3/blake2), HMAC, KDFs (pbkdf2/scrypt/hkdf), random, symmetric ciphers (AES/ChaCha20-Poly1305), asymmetric (RSA / EC / Ed25519 / X25519), X.509, encoding helpers, `timingSafeEqual`. |
| `net`            | [net.md](net.md)                   | DNS lookup. |
| `socket`         | [socket.md](socket.md)             | TCP client and server. |
| `secure_socket`  | [secure_socket.md](secure_socket.md) | TLS client and server. |
| `http`           | [http.md](http.md)                 | HTTP/1.1 client and server. |
| `https`          | [https.md](https.md)               | TLS variant of `http`. |
| `thread`         | [thread.md](thread.md)             | Thread library functions. |
| `stream`         | [stream.md](stream.md)             | Unified byte streams and channels. |
| `app`            | [app.md](app.md)                   | Application lifecycle, quit. |
| `eval`           | [eval.md](eval.md)                 | The `eval()` global. |
| `include`        | [include.md](include.md)           | The `include()` global. |
| `gc`             | [gc.md](gc.md)                     | Garbage collector control. |
| `jit`            | [jit.md](jit.md)                   | JIT control and statistics. |
| `_meta`          | [meta.md](meta.md)                 | The metatable registry for built-in types. |

## Conventions used in these pages

- Method form `s:length()` is equivalent to `string.length(s)`.  The
  same body of code defines both; pages document the method form unless
  noted.
- Function signatures use `→` to mark return values.  An argument
  suffixed with `*` is optional.
- Errors are reported via the standard `THROW` mechanism unless a
  function is documented as returning `NULL` on failure.
