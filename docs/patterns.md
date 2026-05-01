# Common Patterns and Idioms

A field guide for everyday CanDo scripting.  These are not new features —
they are the conventions that experienced CanDo authors reach for.

The cross-references go to [language-reference.md](language-reference.md)
for syntax details and [standard-library.md](standard-library.md) for the
library functions used.

---

## Variables and scope

### Always use `VAR` inside functions

A bare assignment to an undeclared name creates a global.  This is
convenient at the top level, but inside a function it leaks state across
calls and across threads.

```cando
FUNCTION bad(n) {
    sum = 0;            // global! shared by every call
    FOR i IN 1 -> n { sum = sum + i; }
    RETURN sum;
}

FUNCTION good(n) {
    VAR sum = 0;        // local — fresh on each call
    FOR i IN 1 -> n { sum = sum + i; }
    RETURN sum;
}
```

### Use `CONST` for things that don't change

Module-level configuration is the prototypical case:

```cando
CONST MAX_RETRIES = 5;
CONST TIMEOUT_MS  = 30000;
CONST ENDPOINT    = "https://api.example.com";
```

Reassigning a `CONST` is a runtime error, which catches mistakes early.

### Block-scope short-lived bindings

Variables declared inside `{ … }` (an `IF` body, a loop body, an explicit
block expression) disappear at the closing brace.  Use this to keep
helper variables out of the surrounding scope:

```cando
IF need_token() {
    VAR tok = fetch_token();
    headers.authorization = `Bearer ${tok}`;
}
// `tok` is gone here
```

---

## Strings

### Use backticks for templates

Backtick strings interpolate `${expr}` and span multiple lines.  Reach
for them whenever you build a user-facing message:

```cando
VAR name = "Alice";
VAR n    = 3;
print(`Hello ${name}, you have ${n} new message${IF n == 1 { "" } ELSE { "s" }}.`);
```

### Use single quotes for raw multiline blobs

Single-quoted strings have no escape processing.  They are perfect for
SQL queries, regex source, and embedded scripts:

```cando
VAR query = '
    SELECT id, name, email
    FROM users
    WHERE active = 1
      AND created_at > ?
';
```

### Coerce numbers with unary `+`

`+s` parses a numeric string into a number.  Use it on CSV/JSON-string
fields and on `args[]`:

```cando
VAR threshold = +args[0];
VAR row_score = +row.score;
```

Returns `NULL` if the string isn't a valid number — combine with a
`IF v == NULL` guard.

---

## Numbers

### Integer math via `math.floor`

Numbers are always doubles.  When you want integer division, do the
divide and floor:

```cando
VAR pages = math.floor(#items / page_size);
VAR last  = #items - pages * page_size;
```

### Random integer in a range

`math.random()` returns `[0, 1)`.  Scale and floor:

```cando
FUNCTION rand_between(lo, hi) {
    RETURN lo + math.floor(math.random() * (hi - lo + 1));
}
print(rand_between(1, 6));    // d6
```

---

## Arrays

### `map`, `filter`, `reduce`

The functional trio comes from the array prototype:

```cando
VAR doubled = [1, 2, 3]:map(FUNCTION(x) { RETURN x * 2; });
VAR evens   = [1, 2, 3, 4]:filter(FUNCTION(x) { RETURN x % 2 == 0; });
VAR sum     = [1, 2, 3, 4]:reduce(FUNCTION(a, b) { RETURN a + b; }, 0);
```

For simple element-wise transforms reach for the pipe instead — it reads
better and avoids the closure allocation:

```cando
VAR doubled = [1, 2, 3] ~> pipe * 2;
VAR evens   = [1, 2, 3, 4] ~!> { IF pipe % 2 == 0 { RETURN pipe; } };
```

### Stack and queue

`push` / `pop` give you a stack; `push` / `shift` give you a queue:

```cando
VAR stack = [];
stack:push(1); stack:push(2); stack:push(3);
print(stack:pop());      // 3 — LIFO

VAR queue = [];
queue:push("a"); queue:push("b"); queue:push("c");
print(queue:shift());    // a — FIFO
```

### Build then return

Inside a function, build into a local array and return it.  Avoid the
temptation to mutate a parameter — that surprises callers.

```cando
FUNCTION grades_for(rows) {
    VAR out = [];
    FOR r OF rows {
        VAR g = "F";
        IF r.score >= 90      { g = "A"; }
        ELSE IF r.score >= 80 { g = "B"; }
        ELSE IF r.score >= 70 { g = "C"; }
        out:push({ name: r.name, grade: g });
    }
    RETURN out;
}
```

---

## Objects

### Use objects as named records

Objects are key-value maps with insertion-order iteration and prototype
inheritance.  They double as records:

```cando
VAR user = {
    id:    42,
    name:  "Alice",
    email: "alice@example.com",
    roles: ["admin", "editor"]
};
```

### Sentinels and defaults

Looking up an absent field returns `NULL`.  Combine with an `OR`-style
default:

```cando
VAR port = config.port;
IF port == NULL { port = 8080; }
```

### Merge two objects

There is no built-in `Object.assign` — write the loop:

```cando
FUNCTION merge(dst, src) {
    FOR k IN src { dst[k] = src[k]; }
    RETURN dst;
}

VAR opts = merge(merge({}, defaults), overrides);
```

### Use a prototype to share methods

If two objects need the same methods, set the methods on a parent and
chain them via `__index`:

```cando
VAR shape_proto = {};
shape_proto.area = FUNCTION(self) { RETURN self.w * self.h; };

VAR a = { w: 3, h: 4 };
VAR b = { w: 5, h: 6 };
object.setPrototype(a, shape_proto);
object.setPrototype(b, shape_proto);

print(a:area(), b:area());     // 12 30
```

`CLASS` does this for you (see *Classes* below).

---

## Functions

### Default arguments via `OR NULL` test

CanDo has no default-parameter syntax — write the test in the body:

```cando
FUNCTION greet(name, greeting) {
    IF greeting == NULL { greeting = "Hello"; }
    RETURN `${greeting}, ${name}!`;
}
print(greet("Alice"));                  // Hello, Alice!
print(greet("Bob", "Hi"));              // Hi, Bob!
```

### Returning failure with multi-return

Two-value `(result, err)` returns are an idiomatic Go-style failure
channel.  Don't `THROW` for *expected* failures — return them:

```cando
FUNCTION read_config(path) {
    IF !file.exists(path) { RETURN NULL, "no such file"; }
    VAR text = file.read(path);
    IF text == NULL       { RETURN NULL, "cannot read"; }
    RETURN json.parse(text), NULL;
}

VAR cfg, err = read_config("config.json");
IF err != NULL { print("error:", err); os.exit(1); }
```

### Higher-order helpers

A function that returns a function is the classic factory:

```cando
FUNCTION multiplier_by(n) {
    RETURN FUNCTION(x) { RETURN x * n; };
}

VAR triple = multiplier_by(3);
print(triple(7));            // 21
```

### The pipe-as-DSL

The pipe variable `pipe` plus a block makes one-liners read like a
dataflow:

```cando
VAR usernames = users ~> pipe.name;
VAR active    = users ~!> { IF pipe.active { RETURN pipe; } };
VAR shouts    = words ~> pipe:toUpper();
```

---

## Error handling

### Catch what you expect, let the rest crash

Wrap the small section that can fail.  Don't wrap the whole function —
that hides programmer errors:

```cando
FUNCTION load_settings(path) {
    VAR raw;
    TRY {
        raw = file.read(path);
    } CATCH (msg) {
        RETURN NULL;             // file may be absent — caller decides
    }
    RETURN json.parse(raw);      // a JSON error here is a *bug*, not a config issue
}
```

### Re-throw with extra context

After cleaning up or logging, throw a richer error:

```cando
TRY {
    do_step();
} CATCH (msg) {
    log.write("step failed: " + msg);
    THROW "while loading config: " + msg;
}
```

### Multi-value throws

`THROW` accepts multiple values, mirrored on the `CATCH` parameter list:

```cando
TRY {
    THROW "validation", 422, "missing field 'name'";
} CATCH (kind, code, detail) {
    print(kind, code, detail);
}
```

If the catch has fewer parameters than the throw, the extras are
discarded; if it has more, the extras are `NULL`.

### Always release with `FINALY`

`FINALY` runs whether the `TRY` succeeded, threw, returned early, or was
broken out of:

```cando
VAR fh = file.open("data.bin", "rb");
TRY {
    process(fh);
} FINALY {
    fh:close();
}
```

> Spelled with one **L**.

---

## Classes

### Constructor in `()`, methods after

`CLASS` body is the constructor.  Methods are field assignments:

```cando
CLASS Stack = (self) {
    self.items = [];
}

Stack.push = FUNCTION(self, v) { self.items:push(v); };
Stack.pop  = FUNCTION(self) {
    IF #self.items == 0 { THROW "stack empty"; }
    RETURN self.items:pop();
};
Stack.peek = FUNCTION(self) {
    IF #self.items == 0 { RETURN NULL; }
    RETURN self.items[#self.items - 1];
};
Stack.size = FUNCTION(self) { RETURN #self.items; };
```

### Inheritance with `EXTENDS`

```cando
CLASS Animal = (self, name) {
    self.name = name;
}
Animal.speak = FUNCTION(self) { RETURN self.name + " makes a sound"; };

CLASS Dog EXTENDS Animal = (self, name, breed) {
    Animal.__constructor(self, name);   // super-style call
    self.breed = breed;
}
Dog.speak = FUNCTION(self) {
    RETURN Animal.speak(self) + " (woof)";
};
```

There is no `super` keyword.  Call the parent's method via the parent
class object — `Animal.speak(self)` — and the parent's constructor via
`Animal.__constructor(self, ...)`.

### Operator overloading

A few `__op` fields turn a record into something that looks built-in.
See [metamethods.md](metamethods.md).

```cando
CLASS Vec2 = (self, x, y) { self.x = x; self.y = y; }
Vec2.__add      = FUNCTION(a, b) { RETURN Vec2(a.x + b.x, a.y + b.y); };
Vec2.__tostring = FUNCTION(self) { RETURN `(${self.x}, ${self.y})`; };

VAR p = Vec2(1, 2);
VAR q = Vec2(3, 4);
print(toString(p + q));          // (4, 6)
```

---

## Modules

### Single-export pattern

Build a namespace object inside the module and `RETURN` it:

```cando
// strings_extra.cdo
VAR M = {};

M.titleCase = FUNCTION(s) { ... };
M.camelCase = FUNCTION(s) { ... };

RETURN M;
```

```cando
// main.cdo
VAR strx = include("./strings_extra.cdo");
print(strx:titleCase("hello world"));
```

### Caching is automatic

`include(path)` runs the file the first time and caches its return value
keyed by canonical path.  Subsequent `include`s of the same path return
the cached object — useful for shared singletons.

### Hot-loaded YAML / JSON

`include()` can also load `.yaml` / `.json` directly into a value:

```cando
CONST cfg = include("./config.yaml");
print(cfg.server.host);
```

See [yaml.md](yaml.md) for the loading rules.

---

## Threads

### Fire-and-forget vs. await

Use `await` when you need the return value or want backpressure.  Skip
it for background tasks that own their own results (sockets, queues):

```cando
// joining
VAR result = await thread { return long_compute(); };

// fire-and-forget — kept alive by the runtime until it returns
thread {
    log_async("started at " + datetime.now());
};
```

### Worker pool

A common pattern: fan out N workers, each pulling from a shared queue:

```cando
VAR jobs    = [...];                 // input queue
VAR results = [];

VAR lock = {};
FUNCTION pop_job() {
    object.lock(lock);
    VAR j = jobs:shift();
    object.unlock(lock);
    RETURN j;
}

FUNCTION push_result(r) {
    object.lock(lock);
    results:push(r);
    object.unlock(lock);
}

VAR workers = [];
FOR i IN 1 -> 4 {
    workers:push(thread {
        WHILE TRUE {
            VAR j = pop_job();
            IF j == NULL { BREAK; }
            push_result(handle(j));
        }
    });
}

FOR w OF workers { await w; }
```

### Avoid sharing closures over plain locals

Each thread observes the **same** captured variable — read/write races
will happen.  Wrap shared mutable state in an object and lock it.

See [threading.md](threading.md) for the full treatment.

---

## I/O

### Read-then-process is fine for small files

Up to a few MiB, just read it all:

```cando
VAR text = file.read("input.txt");
VAR rows = text:split("\n");
```

### Stream large inputs

For larger or unknown-sized inputs use [streaming.md](streaming.md):

```cando
VAR src = stream.openFile("big.csv");
VAR sink = stream.openFile("out.bin", "w");
src:pipe(sink);
src:close(); sink:close();
```

### Writing a JSON report atomically

Write to a temp path, then rename — readers either see the old or the
new contents, never a half-written file:

```cando
VAR tmp = path + ".tmp";
file.write(tmp, json.stringify(report));
file.rename(tmp, path);
```

---

## CLI scripts

### Required arg with friendly error

```cando
IF #args < 1 {
    print("usage: cando script.cdo <input.csv>");
    os.exit(1);
}

VAR input = args[0];
```

### Flag parsing

CanDo doesn't ship a `getopt`.  For trivial flags walk `args` directly;
for non-trivial CLIs write your own dispatcher:

```cando
VAR opts = { verbose: FALSE, output: "out.txt" };
VAR rest = [];
VAR i = 0;
WHILE i < #args {
    VAR a = args[i];
    IF a == "-v", "--verbose" { opts.verbose = TRUE; }
    ELSE IF a == "-o" { i = i + 1; opts.output = args[i]; }
    ELSE { rest:push(a); }
    i = i + 1;
}
```

See [cli.md](cli.md) for the surrounding `cando` invocation conventions.
