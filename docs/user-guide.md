# User Guide

A hands-on introduction to CanDo.  Work through the sections in order —
each one builds on the previous.  For lookup tables covering every
function and operator see [language-reference.md](language-reference.md)
and [standard-library.md](standard-library.md).

## Running a script

Save a file with the `.cdo` extension and pass it to `cando`:

```bash
cando hello.cdo
```

```cando
// hello.cdo
print("hello, world!");
```

Keywords are case-insensitive (`VAR`, `var`, and `Var` all work), but the
convention is uppercase.  This guide uses uppercase throughout.

## Values and types

CanDo has five types:

```cando
print(type(NULL));       // "null"
print(type(TRUE));       // "bool"
print(type(42));         // "number"
print(type("hi"));       // "string"
print(type({}));         // "object"
```

Numbers are always 64-bit doubles.  There is no integer type.

## Variables

`VAR` declares a mutable variable.  `CONST` declares one that cannot be
reassigned.

```cando
VAR name = "Alice";
CONST MAX = 100;

name = "Bob";           // fine
// MAX = 200;           // runtime error: assignment to const
```

You can declare and assign several variables at once:

```cando
VAR x, y, z = 1, 2, 3;
print(x, y, z);         // 1 2 3
```

Variables are block-scoped:

```cando
IF TRUE {
    VAR secret = 42;
    print(secret);       // 42
}
// `secret` does not exist here
```

Assigning to an undeclared name creates a global.  Inside functions,
always use `VAR` to keep things local.

## Strings

Three quote styles, each with different powers:

```cando
VAR a = "hello\n";          // double-quoted: escape sequences, single line
VAR b = 'raw
multiline';                  // single-quoted: no escapes, multiline
VAR c = `2 + 2 = ${2+2}`;   // backtick: escapes, multiline, interpolation
```

Strings are immutable.  Methods that "transform" return new strings.

```cando
VAR s = "Hello, World!";

print(#s);                   // 13 (byte length)
print(s:toUpper());          // HELLO, WORLD!
print(s:toLower());          // hello, world!
print(s:sub(0, 5));          // Hello
print(s:find("World"));      // 7
print(s:replace("World", "CanDo"));  // Hello, CanDo!
print(s:split(", "));        // ["Hello", "World!"]
print(s:startsWith("Hello"));// TRUE
```

The `:` syntax calls a method on the value.  `s:toUpper()` is the same as
`string.toUpper(s)`.

String concatenation uses `+`:

```cando
print("score: " + toString(100));  // score: 100
```

## Numbers and math

```cando
print(1 + 2);           // 3
print(10 / 3);          // 3.3333...
print(10 % 3);          // 1
print(2 ^ 10);          // 1024 (power, not xor)

print(math.sqrt(9));    // 3
print(math.floor(3.7)); // 3
print(math.ceil(3.2));  // 4
print(math.abs(-5));    // 5
print(math.pi);         // 3.14159...
print(math.random());   // random number in [0, 1)
```

## Arrays

Arrays are 0-indexed.

```cando
VAR fruits = ["apple", "banana", "cherry"];

print(fruits[0]);        // apple
print(#fruits);          // 3

fruits[1] = "blueberry";
print(fruits[1]);        // blueberry
```

### Array methods

```cando
VAR a = [1, 2, 3];

a:push(4);               // [1, 2, 3, 4]
VAR last = a:pop();      // last = 4, a = [1, 2, 3]

VAR doubled = a:map(FUNCTION(x) { RETURN x * 2; });
print(doubled);          // [2, 4, 6]

VAR evens = [1,2,3,4,5]:filter(FUNCTION(x) { RETURN x % 2 == 0; });
print(evens);            // [2, 4]

VAR sum = [1,2,3,4]:reduce(FUNCTION(acc, x) { RETURN acc + x; }, 0);
print(sum);              // 10
```

## Objects

Objects are key-value maps.  Fields are accessed with `.` or `["key"]`.

```cando
VAR person = { name: "Alice", age: 30 };

print(person.name);      // Alice
print(person["age"]);    // 30

person.city = "NYC";     // add a new field
print(person.city);      // NYC
```

Objects preserve insertion order when iterated.

### Nested objects

```cando
VAR config = {
    server: { host: "localhost", port: 8080 },
    debug: TRUE
};

print(config.server.host);   // localhost
print(config.server.port);   // 8080
```

## Control flow

### IF / ELSE

```cando
VAR age = 25;

IF age >= 18 {
    print("adult");
} ELSE IF age >= 13 {
    print("teenager");
} ELSE {
    print("child");
}
```

Multi-comparison — check one value against several alternatives:

```cando
VAR code = 200;
IF code == 200, 201, 204 {
    print("success");
}
```

### WHILE

```cando
VAR i = 0;
WHILE i < 5 {
    print(i);
    i = i + 1;
}
```

### FOR loops

```cando
// Range (inclusive both ends)
FOR i IN 1 -> 5 {
    print(i);            // 1 2 3 4 5
}

// Descending range
FOR i IN 5 <- 1 {
    print(i);            // 5 4 3 2 1
}

// Iterate array values
FOR fruit OF ["apple", "banana", "cherry"] {
    print(fruit);
}

// Iterate object keys
VAR obj = { a: 1, b: 2, c: 3 };
FOR key IN obj {
    print(key);          // a b c
}
```

### BREAK and CONTINUE

```cando
FOR i IN 1 -> 10 {
    IF i == 5 { BREAK; }
    IF i % 2 == 0 { CONTINUE; }
    print(i);            // 1 3
}
```

`BREAK 2` exits two levels of nesting.

## Functions

```cando
FUNCTION greet(name) {
    print(`hello, ${name}!`);
}

greet("world");          // hello, world!
```

### Return values

```cando
FUNCTION add(a, b) {
    RETURN a + b;
}

print(add(3, 4));        // 7
```

### Multiple return values

Functions can return several values at once:

```cando
FUNCTION minmax(a, b) {
    IF a < b { RETURN a, b; }
    RETURN b, a;
}

VAR lo, hi = minmax(7, 3);
print(lo, hi);           // 3 7
```

### Anonymous functions

```cando
VAR square = FUNCTION(x) { RETURN x * x; };
print(square(5));        // 25

// Useful as callbacks
[1,2,3]:map(FUNCTION(x) { RETURN x * 10; });
```

### Closures

Inner functions capture variables from their enclosing scope:

```cando
FUNCTION make_counter() {
    VAR n = 0;
    RETURN FUNCTION() {
        n = n + 1;
        RETURN n;
    };
}

VAR c = make_counter();
print(c(), c(), c());   // 1 2 3
```

### Varargs

```cando
FUNCTION log(tag, ...rest) {
    print(tag, ...rest);
}

log("INFO", "server started on port", 8080);
```

### Recursion

```cando
FUNCTION fib(n) {
    IF n <= 1 { RETURN n; }
    RETURN fib(n - 1) + fib(n - 2);
}

print(fib(10));          // 55
```

## Method calls

The `:` operator calls a method, passing the left-hand side as the first
argument:

```cando
VAR obj = { value: 10 };

obj.double = FUNCTION(self) {
    RETURN self.value * 2;
};

print(obj:double());     // 20
```

The `::` operator does the same but returns the receiver, enabling fluent
chains:

```cando
VAR builder = { parts: [] };

builder.add = FUNCTION(self, part) {
    self.parts:push(part);
};

builder::add("header")::add("body")::add("footer");
print(#builder.parts);   // 3
```

## Error handling

### TRY / CATCH / FINALY

```cando
TRY {
    VAR result = risky_operation();
} CATCH (err) {
    print("error:", err);
} FINALY {
    print("cleanup done");
}
```

> `FINALY` is spelled with one L.

### THROW

Throw one or more values:

```cando
TRY {
    THROW 404, "not found";
} CATCH (code, msg) {
    print(code, msg);    // 404 not found
}
```

Runtime errors (division by zero, calling a non-function) are also
catchable:

```cando
TRY {
    VAR x = 1 / 0;
} CATCH (msg) {
    print("caught:", msg);
}
```

## Pipe and filter

The `~>` operator maps over an array.  The special variable `pipe` holds
each element:

```cando
VAR nums = [1, 2, 3, 4, 5];

VAR tens = nums ~> pipe * 10;
print(tens);             // [10, 20, 30, 40, 50]

VAR labels = nums ~> `item_${pipe}`;
print(labels);           // ["item_1", "item_2", ...]
```

The `~!>` operator filters — return a value to keep, or `NULL` to skip:

```cando
VAR evens = nums ~!> {
    IF pipe % 2 == 0 { RETURN pipe; }
};
print(evens);            // [2, 4]
```

## Classes

`CLASS` creates a prototype-based object:

```cando
CLASS Point {
    FUNCTION make(x, y) {
        RETURN { x: x, y: y };
    }

    FUNCTION dist(self) {
        RETURN math.sqrt(self.x ^ 2 + self.y ^ 2);
    }

    FUNCTION add(self, other) {
        RETURN Point.make(self.x + other.x, self.y + other.y);
    }
}

VAR a = Point.make(3, 0);
VAR b = Point.make(0, 4);
VAR c = a:add(b);
print(c:dist());         // 5
print(c.x, c.y);        // 3 4
```

Methods called with `:` receive the object as the first argument (`self`
by convention).

## Threads

CanDo threads are real OS threads.

```cando
// Spawn a thread
VAR t = thread {
    thread.sleep(10);
    RETURN "done";
};

// Wait for the result
VAR result = await t;
print(result);           // done
```

### Multiple concurrent threads

```cando
FUNCTION compute(n) {
    VAR sum = 0;
    FOR i IN 1 -> n { sum = sum + i; }
    RETURN sum;
}

VAR t1 = thread compute(1000);
VAR t2 = thread compute(2000);
VAR t3 = thread compute(3000);

print(await t1);         // 500500
print(await t2);         // 2001000
print(await t3);         // 4501500
```

### Thread callbacks

```cando
VAR t = thread { RETURN 42; };

thread.then(t, FUNCTION(result) {
    print("success:", result);
});

thread.catch(t, FUNCTION(err) {
    print("error:", err);
});
```

### Shared state and locking

Threads share global variables and heap objects.  For read-modify-write
patterns, use explicit locking:

```cando
VAR counter = { n: 0 };

VAR workers = [];
FOR i IN 1 -> 10 {
    workers:push(thread {
        FOR j IN 1 -> 100 {
            object.lock(counter);
            counter.n = counter.n + 1;
            object.unlock(counter);
        }
    });
}

FOR w OF workers { await w; }
print(counter.n);        // 1000
```

See [threading.md](threading.md) for the full treatment.

## Modules

Use `include()` to load another script.  The module's `RETURN` value is
cached by path:

```cando
// mathutil.cdo
VAR util = {};
util.clamp = FUNCTION(v, lo, hi) {
    IF v < lo { RETURN lo; }
    IF v > hi { RETURN hi; }
    RETURN v;
};
RETURN util;
```

```cando
// main.cdo
VAR mu = include("./mathutil.cdo");
print(mu.clamp(15, 0, 10));  // 10
```

Calling `include()` with the same path again returns the cached value
without re-executing the file.

## File I/O

```cando
// Write a file
file.write("output.txt", "hello from CanDo\n");

// Read it back
VAR content = file.read("output.txt");
print(content);

// Read as lines
VAR lines = file.lines("output.txt");
FOR line OF lines { print(line); }

// Check existence
print(file.exists("output.txt"));  // TRUE

// List a directory
VAR entries = file.list(".");
FOR name OF entries { print(name); }
```

## JSON

```cando
VAR data = { name: "Alice", scores: [95, 87, 92] };

// Encode
VAR text = json.stringify(data);
print(text);             // {"name":"Alice","scores":[95,87,92]}

// Decode
VAR parsed = json.parse(text);
print(parsed.name);      // Alice
print(parsed.scores[0]); // 95
```

## HTTP client

The `fetch` global picks HTTP or HTTPS from the URL:

```cando
VAR res = fetch("https://httpbin.org/get");
print(res.status);       // 200
print(res.body);         // response body as string

// Parse JSON response
VAR data = res:json();
print(data.url);

// POST with options
VAR res2 = fetch("https://httpbin.org/post", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: json.stringify({ key: "value" })
});
```

## HTTP server

```cando
VAR server = http.createServer(FUNCTION(req, res) {
    res.status = 200;
    res.headers["Content-Type"] = "text/plain";
    res.body = `Hello! You requested ${req.path}`;
});

server:listen(8080);
print("listening on :8080");
```

Each request runs on its own thread.  Call `server:close()` to shut down.

## Crypto

```cando
print(crypto.sha256("hello"));
// 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824

print(crypto.base64Encode("hello world"));
// aGVsbG8gd29ybGQ=

print(crypto.base64Decode("aGVsbG8gd29ybGQ="));
// hello world
```

## Date and time

```cando
VAR now = datetime.now();
print(datetime.format(now));
// 2026-04-16 14:30:00  (example)

print(datetime.format(now, "%Y/%m/%d"));
// 2026/04/16
```

## OS interaction

```cando
print(os.getenv("HOME"));       // /home/user
print(os.time());               // Unix timestamp

VAR output = os.execute("ls -la");
print(output);

print(process.pid());           // current process ID
```

## eval

Compile and execute a string as CanDo code at runtime:

```cando
VAR result = eval("2 + 2");
print(result);           // 4

VAR x = 10;
print(eval("x * 3"));   // 30 (accesses enclosing globals)
```

## Mask syntax

Masks select positions from a multi-value expression.  `~` keeps a value,
`.` skips it:

```cando
FUNCTION triple() { RETURN 10, 20, 30; }

VAR first, third = (~.~) triple();
print(first, third);     // 10 30

VAR a, b = (~~) 1, 2, 3;  // keep first two, ignore rest
print(a, b);             // 1 2

VAR c = (..) 1, 2, 3;     // skip first two, keep rest
print(c);                // 3
```

## FOR ... OVER (generic iterators)

`FOR ... OVER` uses a Lua-style iterator protocol — three values:
an iterator function, a state, and an initial control value:

```cando
FUNCTION pairs(t) {
    RETURN FUNCTION(s, c) {
        IF c >= #s { RETURN NULL; }
        RETURN c + 1, c, s[c];
    }, t, 0;
}

FOR idx, val OVER pairs([10, 20, 30]) {
    print(idx, val);
    // 0 10
    // 1 20
    // 2 30
}
```

Iteration stops when the iterator returns `NULL` as the new control value.

## Putting it together

A small program that reads a CSV file, processes the data, and writes
a JSON report:

```cando
// Read and parse a CSV file
VAR text = file.read("scores.csv");
IF text == NULL { print("file not found"); os.exit(1); }

VAR rows = csv.parse(text);
VAR header = rows[0];

// Process data rows (skip header)
VAR results = [];
FOR i IN 1 -> #rows - 1 {
    VAR row = rows[i];
    VAR name = row[0];
    VAR score = +row[1];         // +x coerces string to number

    VAR grade = "F";
    IF score >= 90 { grade = "A"; }
    ELSE IF score >= 80 { grade = "B"; }
    ELSE IF score >= 70 { grade = "C"; }

    results:push({ name: name, score: score, grade: grade });
}

// Sort by score (simple bubble sort)
FOR i IN 0 -> #results - 2 {
    FOR j IN 0 -> #results - 2 - i {
        IF results[j].score < results[j+1].score {
            VAR tmp = results[j];
            results[j] = results[j+1];
            results[j+1] = tmp;
        }
    }
}

// Write JSON report
file.write("report.json", json.stringify(results));
print("wrote report.json with", #results, "entries");
```

## Quick reference

| Topic | Where to look |
|---|---|
| Every operator, keyword, and syntax rule | [language-reference.md](language-reference.md) |
| All library functions | [standard-library.md](standard-library.md) |
| Threading in depth | [threading.md](threading.md) |
| Building the interpreter | [getting-started.md](getting-started.md) |
