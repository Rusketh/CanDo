# Cando Language User Guide

Cando is a C-style scripting language with a clean syntax for general-purpose programming. Scripts use the `.cdo` file extension.

## Table of Contents

1. [Running Scripts](#running-scripts)
2. [Comments](#comments)
3. [Types & Literals](#types--literals)
4. [Variables & Constants](#variables--constants)
5. [Operators](#operators)
6. [Strings](#strings)
7. [Arrays](#arrays)
8. [Objects](#objects)
9. [Control Flow](#control-flow)
10. [Loops](#loops)
11. [Functions](#functions)
12. [Error Handling](#error-handling)
13. [Pipe & Filter](#pipe--filter)
14. [Mask Syntax](#mask-syntax)
15. [Built-in Functions](#built-in-functions)
16. [Standard Library](#standard-library)
    - [math](#math-module)
    - [string](#string-module)
    - [file](#file-module)
    - [eval](#eval)
17. [Threading](#threading)
    - [thread expression](#thread-expression)
    - [await expression](#await-expression)
    - [thread library](#thread-library)

---

## Running Scripts

```bash
./cando script.cdo          # Run a script
./cando script.cdo --disasm # Run and print disassembled bytecode
```

---

## Comments

```cando
// Single-line comment

/* Multi-line
   comment */
```

---

## Types & Literals

| Type    | Examples                        |
|---------|---------------------------------|
| Number  | `42`, `3.14`, `-5`, `0`         |
| String  | `"hello"`, `'multiline'`, `` `interpolated` `` |
| Boolean | `TRUE`, `FALSE`                 |
| Null    | `NULL`                          |
| Array   | `[1, 2, 3]`, `[]`               |
| Object  | `{ x: 1, y: 2 }`, `{}`         |

### String Literals

Cando has three kinds of string literals:

**Double-quoted** — standard single-line strings with escape sequences:
```cando
VAR s = "hello\nworld";
```

**Single-quoted** — multiline strings, whitespace is preserved as-is:
```cando
VAR s = 'line one
line two
line three';
```

**Backtick** — interpolated strings; embed any expression with `${...}`:
```cando
VAR name = "Alice";
VAR greeting = `Hello, ${name}! You are ${20 + 5} years old.`;
```

---

## Variables & Constants

Declare mutable variables with `VAR` and immutable constants with `CONST`:

```cando
VAR x = 10;
CONST PI = 3.14159;

x = 99;    // ok
// PI = 1; // error — constants cannot be reassigned
```

### Multiple Assignment

Declare or assign multiple variables in one statement using a comma-separated list:

```cando
VAR a, b = 1, 2;
```

Variables are block-scoped: a variable declared inside `{ }` is not visible outside it.

---

## Operators

### Arithmetic

| Operator | Description        | Example      |
|----------|--------------------|--------------|
| `+`      | Addition           | `1 + 2` → `3` |
| `-`      | Subtraction        | `5 - 3` → `2` |
| `*`      | Multiplication     | `4 * 5` → `20` |
| `/`      | Division           | `15 / 3` → `5` |
| `%`      | Modulo (remainder) | `10 % 3` → `1` |
| `^`      | Power / XOR        | `2 ^ 8` → `256` (when used as power) |

`+` also concatenates strings: `"hello" + " world"`.

### Comparison

| Operator | Description              |
|----------|--------------------------|
| `==`     | Equal                    |
| `!=`     | Not equal                |
| `<`      | Less than                |
| `>`      | Greater than             |
| `<=`     | Less than or equal       |
| `>=`     | Greater than or equal    |

### Logical

| Operator | Description |
|----------|-------------|
| `&&`     | Logical AND |
| `\|\|`   | Logical OR  |
| `!`      | Logical NOT |

Both `&&` and `||` short-circuit — the right side is not evaluated if the result is already determined by the left.

### Bitwise

| Operator | Description   |
|----------|---------------|
| `&`      | Bitwise AND   |
| `\|`     | Bitwise OR    |
| `^`      | Bitwise XOR   |
| `~`      | Bitwise NOT   |
| `<<`     | Left shift    |
| `>>`     | Right shift   |

### Assignment

```cando
x = 5;    // assign
x += 2;   // x = x + 2
x -= 2;   // x = x - 2
x *= 2;   // x = x * 2
x /= 2;   // x = x / 2
x %= 2;   // x = x % 2
x ^= 2;   // x = x ^ 2
x++;      // x = x + 1  (postfix)
x--;      // x = x - 1  (postfix)
```

### Unary

| Operator | Description              | Example       |
|----------|--------------------------|---------------|
| `-`      | Negate                   | `-x`          |
| `!`      | Logical NOT              | `!TRUE`       |
| `#`      | Length of string/array   | `#"hello"` → `5` |

### Operator Precedence

Standard mathematical precedence applies. Use parentheses to override:

```cando
print(2 + 3 * 4);    // 14  (multiplication first)
print((2 + 3) * 4);  // 20  (parentheses first)
```

---

## Strings

Strings are immutable. Use `+` to concatenate and `#` to get the length:

```cando
VAR s = "hello" + " " + "world";
print(#s);    // 11
```

Convert other types to string with `toString()`:

```cando
VAR msg = "count: " + toString(42);
print(msg);   // count: 42
```

### Method Syntax

Functions from the `string` module can be called as methods using the colon (`:`) operator:

```cando
"hello":toUpper();       // "HELLO"
"  hi  ":trim();         // "hi"
```

See the [string module](#string-module) for the full list of available functions.

---

## Arrays

Arrays are ordered lists of values. Elements are accessed with zero-based indexing.

```cando
VAR nums = [10, 20, 30];
print(nums[0]);   // 10
print(#nums);     // 3  (length)

nums[1] = 99;     // update element
print(nums[1]);   // 99
```

Arrays can be nested:

```cando
VAR matrix = [[1, 2], [3, 4]];
print(matrix[0][1]);  // 2
```

Build arrays incrementally:

```cando
VAR result = [];
FOR i OF 1 -> 5 {
    result[#result] = i * i;
}
```

Iterate indices with `FOR IN`, values with `FOR OF`:

```cando
VAR fruits = ["apple", "banana", "cherry"];

FOR i IN fruits { print(i); }   // 0  1  2
FOR v OF fruits { print(v); }   // apple  banana  cherry
```

---

## Objects

Objects are key-value stores. Keys are identifiers; values can be any type.

```cando
VAR obj = { name: "Alice", age: 30 };
print(obj.name);   // Alice
print(obj.age);    // 30
```

Add or update fields by assignment:

```cando
obj.city = "NYC";  // new field
obj.age  = 31;     // update existing field
```

Objects can be nested:

```cando
VAR person = {
    name: "Bob",
    addr: { street: "Main St", zip: 12345 }
};
print(person.addr.street);  // Main St
```

Objects are passed by reference — mutating an object inside a function affects the original.

Iterate field names with `FOR IN`, field values with `FOR OF`:

```cando
VAR point = { x: 10, y: 20 };

FOR k IN point { print(k); }   // x  y
FOR v OF point { print(v); }   // 10  20
```

---

## Control Flow

### IF / ELSE IF / ELSE

Braces are required. The condition does not need parentheses.

```cando
IF x > 0 {
    print("positive");
} ELSE IF x < 0 {
    print("negative");
} ELSE {
    print("zero");
}
```

### Multi-Comparison

A single comparison operator can be tested against multiple right-hand values. The condition passes only if the comparison holds against **all** of them (or, for `==`, against **any** of them):

```cando
VAR x = 5;
IF x > 1, 2, 3 { print("pass"); }  // x > 1 AND x > 2 AND x > 3
IF x > 1, 10   { print("fail"); }  // x is not > 10
IF x == 3, 5   { print("pass"); }  // x equals 5 (any match)
IF x != 3, 5   { print("fail"); }  // x equals 5, so not != all
```

#### Function calls in comparisons

A bare function call on the right-hand side only contributes its **first** return value:

```cando
FUNCTION bounds() { RETURN 3, 100; }

VAR x = 5;
IF x > bounds() { print("pass"); }  // only checks x > 3; 100 is discarded
```

To compare against **all** return values, prefix the call with the unpack operator `...`:

```cando
IF x > ...bounds() { print("fail"); }  // x > 3 AND x > 100 — fails
```

To compare against a **selected subset** of return values, use a mask:

```cando
FUNCTION triple() { RETURN 1, 999, 2; }

// (~.~) keeps 1st and 3rd, discards 2nd
IF x > (~.~) triple() { print("pass"); }  // x > 1 AND x > 2 — passes
IF x > (~~~) triple() { print("fail"); }  // x > 1 AND x > 999 AND x > 2 — fails
```

In a comma list, each call is individually truncated to its first return:

```cando
IF x > bounds(), 4 { print("pass"); }  // x > 3 AND x > 4
```

---

## Loops

### WHILE

```cando
VAR i = 0;
WHILE i < 5 {
    print(i);
    i++;
}
```

### FOR IN and FOR OF

`FOR IN` yields **keys**: array indices (numbers) or object field names (strings).  
`FOR OF` yields **values**: array elements or object field values.

```cando
VAR arr = ["a", "b", "c"];
FOR i IN arr { print(i); }   // 0  1  2      (indices)
FOR v OF arr { print(v); }   // a  b  c      (values)

VAR obj = { x: 1, y: 2 };
FOR k IN obj { print(k); }   // x  y         (keys)
FOR v OF obj { print(v); }   // 1  2         (values)
```

### FOR with Ranges

The `->` operator creates an **ascending** inclusive range; `<-` creates a **descending** inclusive range. Use `OF` (or `IN`) — both produce the numeric values.

```cando
FOR i OF 1 -> 5 {
    print(i);   // 1 2 3 4 5
}

FOR i OF 5 <- 1 {
    print(i);   // 5 4 3 2 1
}
```

### BREAK and CONTINUE

```cando
WHILE TRUE {
    IF done  { BREAK; }     // exit loop
    IF skip  { CONTINUE; }  // jump to next iteration
    // ... body ...
}
```

Both work inside `FOR IN` and `FOR OF` loops as well.

---

## Functions

### Declaring and Calling

```cando
FUNCTION greet(name) {
    print("Hello, " + name + "!");
}

greet("Alice");
```

### Returning Values

```cando
FUNCTION add(a, b) {
    RETURN a + b;
}
VAR result = add(3, 4);  // 7
```

### Multiple Return Values

A function can return a comma-separated list of values:

```cando
FUNCTION minmax(a, b) {
    IF a < b { RETURN a, b; }
    RETURN b, a;
}

VAR lo, hi = minmax(7, 3);
print(lo);  // 3
print(hi);  // 7
```

### Recursion

```cando
FUNCTION fib(n) {
    IF n <= 1 { RETURN n; }
    RETURN fib(n - 1) + fib(n - 2);
}
print(fib(10));  // 55
```

### Anonymous Functions

Functions are first-class values and can be stored in variables:

```cando
VAR square = FUNCTION(x) { RETURN x * x; };
print(square(5));  // 25
```

### Closures

Nested functions close over variables from the enclosing scope:

```cando
FUNCTION make_counter() {
    VAR count = 0;
    RETURN FUNCTION() {
        count++;
        RETURN count;
    };
}

VAR counter = make_counter();
print(counter());  // 1
print(counter());  // 2
print(counter());  // 3
```

### Colon (Method) Call Syntax

Calling a function with `:` passes the left-hand value as the first argument:

```cando
// These two calls are equivalent:
string.toUpper("hello");
"hello":toUpper();
```

This works with any function stored on an object:

```cando
VAR dog = { name: "Rex" };

FUNCTION bark(self) {
    print(self.name + " says woof!");
}

dog.bark = bark;
dog:bark();   // Rex says woof!
```

---

## Error Handling

### TRY / CATCH / FINALY

```cando
TRY {
    // code that may fail
} CATCH (e) {
    print("caught: " + e);
} FINALY {
    print("always runs");
}
```

`FINALY` is optional. It always executes regardless of whether an error was thrown.

### THROW

`THROW` raises an error with one or more values:

```cando
THROW "something went wrong";
THROW 404, "not found";
```

### Catching Multiple Values

Match the number of variables in `CATCH` to the number of values thrown. Extra catch variables are `NULL`; extra thrown values are dropped:

```cando
TRY {
    THROW 404, "not found";
} CATCH (code, msg) {
    print(code);  // 404
    print(msg);   // not found
}
```

### Runtime Errors

Runtime errors (e.g. division by zero) are catchable the same way:

```cando
TRY {
    VAR bad = 1 / 0;
} CATCH (msg) {
    print("caught: " + msg);
}
```

### Rethrowing

Inside a `CATCH` block, `THROW` re-raises to the next handler:

```cando
TRY {
    TRY {
        THROW "inner error";
    } CATCH (e) {
        THROW "rethrown: " + e;
    }
} CATCH (e) {
    print(e);  // rethrown: inner error
}
```

---

## Pipe & Filter

The pipe (`~>`) and filter (`~!>`) operators apply a transformation to every element of an array and return a new array.

Inside the pipe/filter body, `pipe` is a special keyword that refers to the current element.

### Pipe (Map)

`~>` with an expression:

```cando
VAR nums = [1, 2, 3, 4, 5];
VAR tens = nums ~> pipe * 10;
print(tens[0]);  // 10
print(tens[4]);  // 50
```

`~>` with a block:

```cando
VAR doubled = nums ~> {
    RETURN pipe * 2;
};
```

### Filter

`~!>` keeps only elements for which the body returns a non-`NULL` value:

```cando
VAR evens = nums ~!> {
    IF pipe % 2 == 0 { RETURN pipe; }
    RETURN NULL;
};
// evens == [2, 4]
```

If the body has no explicit `RETURN NULL`, elements that don't match are simply dropped:

```cando
VAR big = nums ~!> {
    IF pipe > 3 { RETURN pipe; }
};
// big == [4, 5]
```

---

## Mask Syntax

The mask syntax selects specific values from a multi-value expression. A mask is written as a parenthesised pattern of `~` (pass/consume) and `.` (skip) bits placed before the value list. Wrapping the pattern in `()` keeps it distinct from the unpack operator `...`.

### Mixed Mask

Each position in the mask corresponds to a value. `~` passes that value to a variable; `.` discards it:

```cando
// Select 1st and 3rd values; discard 2nd
VAR a, b = (~.~) 1, 3, 5;
print(a);  // 1
print(b);  // 5
```

### Pure `~` Mask

Consume the first N values; extras are skipped:

```cando
VAR c = (~) 1, 2;        // c = 1; 2 is ignored
VAR d, e = (~~) 1, 2, 3; // d = 1, e = 2; 3 is ignored
```

### Pure `.` Mask

Skip the first N values; extras are passed through:

```cando
VAR f = (.) 1, 2;        // skip 1; f = 2
VAR g = (..) 1, 2, 3;    // skip 1, skip 2; g = 3
```

---

## Built-in Functions

These functions are available globally without any import.

### `print(...)`

Prints all arguments to stdout, space-separated, followed by a newline:

```cando
print("hello");           // hello
print(1, 2, 3);           // 1 2 3
print("x =", x);
```

### `type(value)`

Returns the type name of a value as a string:

```cando
print(type("hi"));    // string
print(type(42));      // number
print(type(TRUE));    // bool
print(type(NULL));    // null
print(type([]));      // array
print(type({}));      // object
```

### `toString(value)`

Converts a value to its string representation:

```cando
print(toString(42));      // 42
print(toString(3.14));    // 3.14
print(toString(TRUE));    // true
```

---

## Standard Library

### Include

The `include(path)` function loads and executes a Cando script or binary extension module.

```cando
VAR mymod = include("utils.cdo");
```

#### Multiple Return Values

If the included script returns multiple values, `include()` returns all of them.

```cando
// module.cdo
RETURN 1, 2, 3;

// main.cdo
VAR x, y, z = include("module.cdo");
```

#### Caching

Modules are cached by their absolute canonical path. Subsequent calls to `include()` with the same path will return the cached values without re-executing the script. All return values are correctly cached and restored.

---

### Math Module

Access via `math.`:

```cando
print(math.abs(-7));         // 7
print(math.floor(3.9));      // 3
print(math.ceil(3.1));       // 4
print(math.round(2.5));      // 3
print(math.sqrt(25));        // 5
print(math.pow(2, 10));      // 1024
print(math.log(1));          // 0
print(math.min(5, 3, 8, 1)); // 1
print(math.max(5, 3, 8, 1)); // 8
print(math.clamp(15, 0, 10));// 10
print(math.clamp(-5, 0, 10));// 0
print(math.clamp( 5, 0, 10));// 5
```

**Constants:**

```cando
math.pi  // 3.14159...
math.e   // 2.71828...
```

| Function                    | Description                         |
|-----------------------------|-------------------------------------|
| `math.abs(x)`               | Absolute value                      |
| `math.floor(x)`             | Round down                          |
| `math.ceil(x)`              | Round up                            |
| `math.round(x)`             | Round to nearest integer            |
| `math.sqrt(x)`              | Square root                         |
| `math.pow(x, y)`            | `x` raised to the power `y`         |
| `math.log(x)`               | Natural logarithm                   |
| `math.min(a, b, ...)`       | Smallest of all arguments           |
| `math.max(a, b, ...)`       | Largest of all arguments            |
| `math.clamp(x, min, max)`   | Clamp `x` to `[min, max]`           |
| `math.pi`                   | π ≈ 3.14159                         |
| `math.e`                    | e ≈ 2.71828                         |

---

### String Module

Access via `string.` or using the colon method syntax on any string value:

```cando
print(string.length("hello"));            // 5
print(string.sub("hello world", 6, 11));  // world
print(string.toLower("HELLO"));           // hello
print(string.toUpper("hello"));           // HELLO
print(string.trim("  hi  "));             // hi
print(string.left("abcdef", 3));          // abc
print(string.right("abcdef", 3));         // def
print(string.repeat("ab", 3));            // ababab
print(string.find("hello world", "world")); // 6
VAR parts = string.split("a,b,c", ",");
print(parts[0]);  // a
print(parts[1]);  // b
print(parts[2]);  // c
```

| Function                        | Description                                    |
|---------------------------------|------------------------------------------------|
| `string.length(s)`              | Number of characters                           |
| `string.sub(s, start, end)`     | Substring from `start` to `end` (1-based)     |
| `string.toLower(s)`             | Convert to lowercase                           |
| `string.toUpper(s)`             | Convert to uppercase                           |
| `string.trim(s)`                | Remove leading and trailing whitespace         |
| `string.left(s, n)`             | First `n` characters                           |
| `string.right(s, n)`            | Last `n` characters                            |
| `string.repeat(s, n)`           | Repeat `s` exactly `n` times                  |
| `string.find(s, sub)`           | Index of first occurrence of `sub` in `s`     |
| `string.split(s, sep)`          | Split `s` by separator; returns an array       |

All functions are also available as methods:

```cando
"Hello World":toLower()      // hello world
"  spaces  ":trim()          // spaces
"abc":repeat(3)              // abcabcabc
```

---

### File Module

Read and write files using `file.read` and `file.write`:

```cando
// Read entire file contents as a string
VAR contents = file.read("data.txt");
print(contents);

// Write a string to a file (overwrites existing content)
file.write("output.txt", "hello from cando\n");
```

| Function                   | Description                         |
|----------------------------|-------------------------------------|
| `file.read(path)`          | Read file at `path`, return string  |
| `file.write(path, content)`| Write `content` string to `path`   |

---

### Eval

`eval` compiles and executes a string of Cando source code at runtime, returning the result of the last expression (or `null` for statement-only code):

```cando
VAR result = eval("1 + 2 + 3");
print(result);  // 6

eval("print('dynamic code!')");
```

An optional second argument is an options table:

| Option      | Type   | Default      | Description                                              |
|-------------|--------|--------------|----------------------------------------------------------|
| `name`      | string | `"<eval>"`   | Script name shown in error messages and stack traces     |
| `sandbox`   | bool   | `false`      | Run in an isolated global environment (see below)        |

```cando
// Custom name appears in runtime error messages
eval("1 / 0", { name: "my_script" });
// runtime error: division by zero [my_script line 1]

// Sandbox mode: outer globals are hidden; new VAR declarations are discarded
VAR secret = 42;
TRY {
    eval("secret", { sandbox: true });  // throws: undefined variable 'secret'
} CATCH (e) {
    print("isolated");  // outer globals not visible in sandbox
}

eval("VAR tmp = 99", { sandbox: true });
// tmp is not defined here — it was discarded when the sandbox exited

// Native functions (print, math, eval, etc.) remain accessible in sandbox mode
eval("print('hello from sandbox')", { sandbox: true });
```

| Function               | Description                                                    |
|------------------------|----------------------------------------------------------------|
| `eval(code)`           | Execute `code` string; return all results from the last expression |
| `eval(code, options)`  | Same, with `name` and/or `sandbox` options                     |

#### Multiple Return Values in Eval

`eval` can return multiple values if the evaluated code ends with a `RETURN` statement containing multiple values or an expression that produces multiple results.

```cando
VAR a, b = eval("RETURN 10, 20");
print(a); // 10
print(b); // 20
```

---

## Threading

Cando supports true OS-level concurrency via the `thread` and `await` keywords and the built-in `thread` management object.

### Thread expression

`thread` is a unary prefix that wraps any expression or block as an anonymous closure and immediately spawns it as a new OS thread. It returns a thread handle.

```cando
// Spawn a block
var t = thread {
    return 42;
};

// Spawn a single expression (function call, arithmetic, etc.)
var t2 = thread file.read("data.txt");
var t3 = thread add(3, 4);
```

The spawned code has full access to variables in scope at the point of the `thread` expression, plus all globals — the same access it would have if called normally. Shared variables and objects are automatically protected by read/write locks.

### Await expression

`await` blocks the current thread until the given thread handle finishes and unpacks its return values.

```cando
var result = await t;       // single return value
var a, b   = await t2;      // multi-return
```

If the thread returned no values, `await` produces `null`.

**await is not parallel with `~>`** — the pipe operator (`~>`) is always sequential. Use explicit `thread`/`await` for parallel work:

```cando
// Sequential (one at a time):
var results = items ~> complexFn(pipe);

// Parallel (all at once):
var handles = items ~> thread complexFn(pipe);
var done    = handles ~> await pipe;
```

### Thread library

The global `thread` object provides thread management utilities:

| Method                | Description                                                        |
|-----------------------|--------------------------------------------------------------------|
| `thread.sleep(ms)`    | Sleep the calling thread for `ms` milliseconds                     |
| `thread.id()`         | Return the calling thread's numeric ID (non-zero)                  |
| `thread.done(t)`      | Return `true` if thread `t` has finished (non-blocking poll)       |
| `thread.join(t)`      | Block until `t` finishes; return its result values (same as `await`) |
| `thread.cancel(t)`    | Request cancellation of `t`; returns `true` if state changed       |
| `thread.state(t)`     | Return state string: `"pending"`, `"running"`, `"done"`, `"error"`, `"cancelled"` |
| `thread.error(t)`     | Return the error value if state is `"error"`, else `null`          |
| `thread.current()`    | Return the current thread's handle, or `null` on the main thread   |
| `thread.then(t, fn)`  | Register `fn` as a success callback; fires with return values      |
| `thread.catch(t, fn)` | Register `fn` as an error callback; fires with the error value     |

```cando
// Poll state without blocking
var t = thread { thread.sleep(100); return 42; };
while thread.state(t) == "running" {
    print("still running...");
    thread.sleep(10);
}
var result = await t;

// Read error after failure
var t2 = thread { throw "bad input"; };
await t2;
if thread.state(t2) == "error" {
    print(thread.error(t2));  // bad input
}

// Get the current thread from inside a thread body
var t3 = thread {
    var me = thread.current();
    print(thread.state(me));  // running
    return 1;
};
await t3;

// Cancel a slow thread
var slow = thread { thread.sleep(60000); return 0; };
thread.cancel(slow);   // returns true if the cancel was accepted

// Promise-style callbacks
var t4 = thread { return 99; };
thread.then(t4,  function(r)   { print("done: "  + r); });
thread.catch(t4, function(err) { print("error: " + err); });
await t4;

// Identify threads
var t5 = thread { print("child id: " + thread.id()); };
print("parent id: " + thread.id());
await t5;
```

> For a deeper look at the threading runtime, locking model, and multi-thread design patterns, see [docs/threading.md](threading.md).
