# New Standard Libraries Documentation

## `os` Module
Access via `os.`.

| Function | Description |
|---|---|
| `os.getenv(name)` | Returns the value of environment variable `name`, or `NULL`. |
| `os.setenv(name, val)` | Sets environment variable `name` to `val`. Returns `TRUE` on success. |
| `os.execute(command)` | Runs a shell command. Returns the exit status. |
| `os.exit(code)` | Terminates the current process with `code`. |
| `os.time()` | Returns current epoch seconds. |
| `os.clock()` | Returns CPU time in seconds. |
| `os.name` | Constant string: `"unix"` or `"windows"`. |

## `datetime` Module
Access via `datetime.`.

| Function | Description |
|---|---|
| `datetime.now()` | Returns current epoch seconds. |
| `datetime.format(ts, fmt)` | Formats timestamp `ts` according to `fmt` (strftime). |
| `datetime.parse(s, fmt)` | Parses string `s` using `fmt` (strptime). Returns epoch seconds or `NULL`. |

## `array` Module
Access via `array.` or method syntax `:`.

| Function | Description |
|---|---|
| `a:length()` | Returns number of elements. |
| `a:push(val)` | Appends `val` to array. |
| `a:pop()` | Removes and returns last element. |
| `a:map(fn)` | Returns new array with `fn(item)` applied to each element. |
| `a:filter(fn)` | Returns new array with items where `fn(item)` is truthy. |
| `a:reduce(fn, init)` | Reduces array using `fn(acc, item)`, starting with `init`. |

## `crypto` Module
Access via `crypto.`.

| Function | Description |
|---|---|
| `crypto.md5(s)` | Returns MD5 hex hash of `s` (currently mocked). |
| `crypto.sha256(s)` | Returns SHA256 hex hash of `s` (currently mocked). |
| `crypto.base64Encode(s)`| Returns Base64 encoded string. |

## `process` Module
Access via `process.`.

| Function | Description |
|---|---|
| `process.pid()` | Returns current process ID. |
| `process.ppid()` | Returns parent process ID. |

## `net` Module
Access via `net.`.

| Function | Description |
|---|---|
| `net.lookup(host)` | Returns an array of IP strings for `host`. |
