# CanDo Standard Library Reference

CanDo ships with 17 standard library modules plus 3 built-in globals.
All modules are opened with `cando_openlibs(vm)` or individually with
`cando_open_*lib(vm)`.  See [embedding.md](embedding.md) for selective loading.

---

## Table of Contents

- [Built-in globals](#built-in-globals) — `print`, `type`, `toString`
- [math](#math) — Mathematical functions
- [string](#string) — String methods (prototype)
- [array](#array) — Array methods (prototype)
- [object](#object) — Object utilities
- [file](#file) — File I/O
- [json](#json) — JSON encode/decode
- [csv](#csv) — CSV parsing
- [os](#os) — Operating system utilities
- [datetime](#datetime) — Date and time
- [crypto](#crypto) — Hashing and encoding
- [process](#process) — Process information
- [net](#net) — DNS lookup
- [thread](#thread) — Thread management
- [eval](#eval) — Dynamic code execution
- [include](#include) — Module loading

---

## Built-in globals

These are always available — no `cando_openlibs` required.

### `print(...)`

Print arguments to stdout, space-separated, followed by a newline.  Accepts
any number of values of any type.

```cando
print("hello", "world");   // hello world
print(42, TRUE, NULL);     // 42 true null
```

### `type(value)`

Return the type name of `value` as a string.

| Value | Returns |
|---|---|
| `NULL` | `"null"` |
| number | `"number"` |
| boolean | `"bool"` |
| string | `"string"` |
| object / array / function | `"object"` (or the object's `__type` field if set) |

```cando
print(type(42));       // number
print(type("hi"));     // string
print(type([]));       // object
```

### `toString(value)`

Convert any value to its string representation.  Calls `__tostring` if defined.

```cando
print(toString(42));       // "42"
print(toString(TRUE));     // "true"
print(toString([1, 2]));   // "[1, 2]"
```

---

## math

Open with: `cando_open_mathlib(vm)` | Access: `math.*`

Constants:

| Name | Value |
|---|---|
| `math.pi` | 3.14159265358979… |
| `math.e` | 2.71828182845904… |
| `math.huge` | `Infinity` |
| `math.nan` | `NaN` |

| Function | Description |
|---|---|
| `math.abs(x)` | Absolute value |
| `math.sqrt(x)` | Square root |
| `math.pow(x, y)` | `x` raised to `y` |
| `math.exp(x)` | e^x |
| `math.log(x)` | Natural logarithm |
| `math.log10(x)` | Base-10 logarithm |
| `math.floor(x)` | Round down to integer |
| `math.ceil(x)` | Round up to integer |
| `math.round(x)` | Round to nearest integer |
| `math.sign(x)` | `1`, `-1`, or `0` |
| `math.min(a, b)` | Smaller of two values |
| `math.max(a, b)` | Larger of two values |
| `math.clamp(v, lo, hi)` | Clamp `v` to `[lo, hi]` |
| `math.sin(x)` | Sine (radians) |
| `math.cos(x)` | Cosine (radians) |
| `math.tan(x)` | Tangent (radians) |
| `math.asin(x)` | Arcsine |
| `math.acos(x)` | Arccosine |
| `math.atan(x)` | Arctangent |
| `math.atan2(y, x)` | Two-argument arctangent |
| `math.sinh(x)` | Hyperbolic sine |
| `math.cosh(x)` | Hyperbolic cosine |
| `math.rad(deg)` | Degrees → radians |
| `math.deg(rad)` | Radians → degrees |
| `math.random()` | Uniform float in `[0, 1)` |

```cando
print(math.sqrt(16));          // 4
print(math.clamp(-5, 0, 100)); // 0
print(math.round(3.7));        // 4
print(math.sin(math.pi / 2));  // 1
```

---

## string

Open with: `cando_open_stringlib(vm)` | Access: method calls on string values

String methods are called using the colon (`:`) syntax or via `string.method(str)`.

| Method | Description |
|---|---|
| `s:length()` | Number of bytes in the string |
| `s:sub(start, end)` | Substring from `start` to `end` (1-based, inclusive) |
| `s:char(index)` | Single character at `index` (1-based) |
| `s:chars()` | Array of individual characters |
| `s:toLower()` | Lowercase copy |
| `s:toUpper()` | Uppercase copy |
| `s:trim()` | Remove leading and trailing whitespace |
| `s:left(n)` | First `n` characters |
| `s:right(n)` | Last `n` characters |
| `s:repeat(n)` | String repeated `n` times |
| `s:find(pattern)` | Index of first match (or `null`) |
| `s:split(delimiter)` | Array of substrings split by `delimiter` |
| `s:replace(old, new)` | Replace all occurrences of `old` with `new` |
| `s:startsWith(prefix)` | `true` if string starts with `prefix` |
| `s:endsWith(suffix)` | `true` if string ends with `suffix` |
| `s:format(...)` | `printf`-style formatting |
| `s:match(pattern)` | Returns captures from a regex match (or `null`) |

```cando
VAR s = "  Hello, World!  ";
print(s:trim());                // "Hello, World!"
print(s:trim():toLower());      // "hello, world!"
print("abc":repeat(3));         // "abcabcabc"

VAR parts = "a,b,c":split(",");
print(parts[0]);                // "a"

print("%d + %d = %d":format(1, 2, 3));  // "1 + 2 = 3"
```

---

## array

Open with: `cando_open_arraylib(vm)` | Access: method calls on array values

| Method | Description |
|---|---|
| `a:length()` | Number of elements |
| `a:push(value)` | Append `value` to the end; returns the array |
| `a:pop()` | Remove and return the last element |
| `a:splice(start, count)` | Remove `count` elements starting at `start`; returns removed elements |
| `a:remove(index)` | Remove element at `index`; returns it |
| `a:copy()` | Shallow copy of the array |
| `a:map(fn)` | New array with `fn` applied to each element |
| `a:filter(fn)` | New array with elements where `fn(el)` is truthy |
| `a:reduce(fn, initial)` | Reduce to a single value using `fn(acc, el)` |

```cando
VAR arr = [3, 1, 4, 1, 5];
print(arr:length());     // 5
arr:push(9);
print(arr[5]);           // 9

VAR doubled = arr:map(FUNCTION(x) { RETURN x * 2; });
VAR evens   = arr:filter(FUNCTION(x) { RETURN x % 2 == 0; });
VAR sum     = arr:reduce(FUNCTION(acc, x) { RETURN acc + x; }, 0);
```

---

## object

Open with: `cando_open_objectlib(vm)` | Access: `object.*`

| Function | Description |
|---|---|
| `object.lock(o)` | Acquire exclusive write lock on `o` (re-entrant per thread) |
| `object.unlock(o)` | Release one level of the write lock |
| `object.locked(o)` | `true` if `o` is currently write-locked |
| `object.copy(o)` | Shallow copy of `o`'s own fields |
| `object.assign(o, ...sources)` | Copy fields from each source into `o`; returns `o` |
| `object.apply(o, ...sources)` | New object with `o`'s fields plus each source merged in |
| `object.get(o, key)` | Read field `key` directly (bypasses `__index`) |
| `object.set(o, key, value)` | Write field `key` directly (bypasses `__newindex`); returns `bool` |
| `object.setPrototype(o, proto)` | Set `o`'s prototype (`__index`); pass `null` to remove |
| `object.getPrototype(o)` | Return `o`'s prototype, or `null` |
| `object.keys(o)` | Array of own field names (insertion order) |
| `object.values(o)` | Array of own field values (insertion order) |

```cando
VAR a = { x: 1, y: 2 };
print(object.keys(a)[0]);       // x

VAR b = object.copy(a);
b.z = 3;
print(a.z);                     // null (a is unchanged)

// Prototype chain
VAR proto = { greet: FUNCTION() { RETURN "hi"; } };
object.setPrototype(a, proto);
print(a:greet());               // hi

// Thread-safe access
object.lock(a);
a.x = a.x + 1;
object.unlock(a);
```

---

## file

Open with: `cando_open_filelib(vm)` | Access: `file.*`

| Function | Description |
|---|---|
| `file.read(path)` | Read entire file as a string |
| `file.write(path, content)` | Write string to file (overwrite) |
| `file.append(path, content)` | Append string to file |
| `file.exists(path)` | `true` if file or directory exists |
| `file.delete(path)` | Delete file; returns `true` on success |
| `file.copy(src, dst)` | Copy file; returns `true` on success |
| `file.move(src, dst)` | Move/rename file; returns `true` on success |
| `file.size(path)` | File size in bytes |
| `file.lines(path)` | Array of lines (without newlines) |
| `file.mkdir(path)` | Create directory (and parents); returns `true` on success |
| `file.list(path)` | Array of entry names in directory |

```cando
// Read and write
VAR content = file.read("data.txt");
file.write("output.txt", "result: " + content);

// Check and create directories
IF !file.exists("logs") {
    file.mkdir("logs");
}

// Process lines
FOR VAR line OVER file.lines("config.txt") {
    print(line);
}

// List directory
VAR entries = file.list(".");
FOR VAR name OVER entries { print(name); }
```

---

## json

Open with: `cando_open_jsonlib(vm)` | Access: `json.*`

| Function | Description |
|---|---|
| `json.parse(str)` | Parse a JSON string into CanDo values |
| `json.stringify(value)` | Convert a CanDo value to a JSON string |

Supported types: strings, numbers, booleans, `null`, arrays, objects.
Functions and non-JSON values are converted to `null`.

```cando
VAR data = json.parse('{"name": "Alice", "age": 30, "scores": [95, 87]}');
print(data.name);         // Alice
print(data.scores[0]);    // 95

VAR output = json.stringify({ x: 1, y: [2, 3] });
print(output);            // {"x":1,"y":[2,3]}
```

---

## csv

Open with: `cando_open_csvlib(vm)` | Access: `csv.*`

| Function | Description |
|---|---|
| `csv.parse(str)` | Parse a CSV string into an array of arrays |
| `csv.stringify(rows)` | Convert an array of arrays to a CSV string |

```cando
VAR rows = csv.parse("a,b,c\n1,2,3\n4,5,6");
print(rows[0][0]);  // a
print(rows[1][1]);  // 2

VAR out = csv.stringify([["name", "age"], ["Alice", "30"]]);
print(out);  // name,age\nAlice,30
```

---

## os

Open with: `cando_open_oslib(vm)` | Access: `os.*`

`os.name` is a pre-set string: `"unix"` on Linux/macOS or `"windows"` on Windows.

| Function | Description |
|---|---|
| `os.getenv(name)` | Get environment variable; returns `null` if not set |
| `os.setenv(name, value)` | Set environment variable |
| `os.execute(cmd)` | Run a shell command; returns exit code |
| `os.exit(code)` | Exit the process with given exit code |
| `os.time()` | Unix timestamp (seconds since epoch) as a number |
| `os.clock()` | CPU time used by the process (in seconds) |

```cando
print(os.name);               // unix
print(os.getenv("HOME"));     // /home/user
print(os.time());             // 1712345678

VAR rc = os.execute("ls -la");
print(rc);                    // 0 (success)
```

---

## datetime

Open with: `cando_open_datetimelib(vm)` | Access: `datetime.*`

`datetime.now` is a pre-set string containing the current UTC date/time in
ISO 8601 format at the moment the module was initialised.

| Function | Description |
|---|---|
| `datetime.now()` | Current UTC datetime as an object |
| `datetime.format(dt, fmt)` | Format a datetime object using `strftime`-style format string |
| `datetime.parse(str, fmt)` | Parse a datetime string with a format specifier |

```cando
VAR now = datetime.now();
print(now.year);          // 2026
print(now.month);         // 4
print(now.day);           // 10

VAR fmt = datetime.format(now, "%Y-%m-%d %H:%M:%S");
print(fmt);               // 2026-04-10 21:00:00
```

---

## crypto

Open with: `cando_open_cryptolib(vm)` | Access: `crypto.*`

| Function | Description |
|---|---|
| `crypto.md5(data)` | MD5 hash of `data` as a hex string |
| `crypto.sha256(data)` | SHA-256 hash of `data` as a hex string |
| `crypto.base64Encode(data)` | Base-64 encode a string |
| `crypto.base64Decode(b64)` | Decode a base-64 string |

```cando
print(crypto.md5("hello"));     // 5d41402abc4b2a76b9719d911017c592
print(crypto.sha256("hello"));  // 2cf24dba…

VAR enc = crypto.base64Encode("hello world");
print(enc);                     // aGVsbG8gd29ybGQ=
print(crypto.base64Decode(enc)); // hello world
```

---

## process

Open with: `cando_open_processlib(vm)` | Access: `process.*`

| Function | Description |
|---|---|
| `process.pid()` | Current process ID |
| `process.ppid()` | Parent process ID |

```cando
print(process.pid());   // 12345
print(process.ppid());  // 12300
```

---

## net

Open with: `cando_open_netlib(vm)` | Access: `net.*`

| Function | Description |
|---|---|
| `net.lookup(hostname)` | DNS lookup; returns array of IP address strings |

```cando
VAR ips = net.lookup("localhost");
print(ips[0]);  // 127.0.0.1
```

---

## thread

Open with: `cando_open_threadlib(vm)` | Access: `thread.*`

See also: [threading.md](threading.md) for the `thread`/`await` language syntax.

| Function | Description |
|---|---|
| `thread.sleep(ms)` | Sleep calling thread for `ms` milliseconds |
| `thread.id()` | Numeric ID of the calling thread (non-zero) |
| `thread.done(t)` | `true` if thread `t` has finished (non-blocking poll) |
| `thread.join(t)` | Block until `t` finishes; return its result values |
| `thread.cancel(t)` | Request cancellation of `t`; returns `true` if state changed |
| `thread.state(t)` | State string: `"pending"`, `"running"`, `"done"`, `"error"`, `"cancelled"` |
| `thread.error(t)` | The error value if state is `"error"`, else `null` |
| `thread.current()` | The current thread's handle, or `null` on the main thread |
| `thread.then(t, fn)` | Register `fn` as a success callback (fires with return values) |
| `thread.catch(t, fn)` | Register `fn` as an error callback (fires with the error value) |

```cando
VAR t = thread { thread.sleep(50); RETURN 42; };

// Poll without blocking
WHILE thread.state(t) == "running" {
    print("waiting...");
    thread.sleep(10);
}
VAR result = await t;
print(result);   // 42

// Callbacks
VAR t2 = thread { RETURN 99; };
thread.then(t2,  FUNCTION(r)   { print("done: " + r); });
thread.catch(t2, FUNCTION(err) { print("error: " + err); });
await t2;
```

---

## eval

Open with: `cando_open_evallib(vm)` | Access: `eval()`

| Function | Description |
|---|---|
| `eval(code)` | Compile and execute `code`; return the last expression result |
| `eval(code, opts)` | Same with options: `name` (string), `sandbox` (bool) |

Options:

| Option | Default | Description |
|---|---|---|
| `name` | `"<eval>"` | Name used in error messages |
| `sandbox` | `false` | Run in isolated globals (outer globals hidden) |

```cando
VAR result = eval("1 + 2 + 3");
print(result);  // 6

// Custom name in error messages
eval("bad code here", { name: "my_script" });

// Sandbox mode — outer globals invisible
VAR secret = 42;
TRY {
    eval("secret", { sandbox: true });  // throws
} CATCH (e) {
    print("sandboxed");
}

// Multiple returns
VAR a, b = eval("RETURN 10, 20");
print(a, b);  // 10  20
```

---

## include

Open with: `cando_open_includelib(vm)` | Access: `include()`

```cando
include(path)
```

Load and execute a `.cdo` file, returning its exported value.  The first call
compiles and runs the module; subsequent calls with the same resolved path
return the cached result without re-executing (Node.js `require` semantics).

The path is resolved relative to the **including script's directory**, not the
current working directory.

```cando
// mymodule.cdo
CONST VERSION = "1.0";
RETURN { version: VERSION, greet: FUNCTION(n) { RETURN "Hello, " + n; } };

// main.cdo
VAR mod = include("mymodule.cdo");
print(mod.version);          // 1.0
print(mod.greet("Alice"));   // Hello, Alice

// Include again — returns cached result (module is NOT re-executed)
VAR mod2 = include("mymodule.cdo");
```

Binary modules (`.so` / `.dll`) are also supported via `dlopen`/`LoadLibrary`.
The module must export a `cando_module_init(CandoVM*)` function.
