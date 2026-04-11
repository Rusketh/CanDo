# CanDo Language Reference

CanDo is a C-style embeddable scripting language. Scripts use the `.cdo` file extension.

## Table of Contents

1. [Running Scripts](#running-scripts)
2. [Comments](#comments)
3. [Types and Literals](#types-and-literals)
4. [Variables and Constants](#variables-and-constants)
5. [Operators](#operators)
   - [Arithmetic](#arithmetic-operators)
   - [Comparison](#comparison-operators)
   - [Logical](#logical-operators)
   - [Bitwise](#bitwise-operators)
   - [Assignment](#assignment-operators)
   - [Unary](#unary-operators)
   - [Range](#range-operators)
   - [Operator Precedence](#operator-precedence)
6. [Strings](#strings)
7. [Arrays](#arrays)
8. [Objects](#objects)
9. [Control Flow](#control-flow)
10. [Loops](#loops)
11. [Functions](#functions)
12. [Error Handling](#error-handling)
13. [Pipe and Filter](#pipe-and-filter)
14. [Mask Syntax](#mask-syntax)
15. [Method Call Syntax](#method-call-syntax)
16. [Threading](#threading)
17. [Built-in Globals](#built-in-globals)
18. [Module Loading](#module-loading)

---

## Running Scripts

```bash
./cando script.cdo           # Run a script
./cando script.cdo --disasm  # Run and print disassembled bytecode
```

---

## Comments

```cando
// Single-line comment

/* Multi-line
   comment */
```

---

## Types and Literals

CanDo has six value types:

| Type    | Examples                                          |
|---------|---------------------------------------------------|
| Number  | `42`, `3.14`, `-5`, `0`                           |
| String  | `"hello"`, `'multiline'`, `` `interpolated` ``    |
| Boolean | `TRUE`, `FALSE`                                   |
| Null    | `NULL`                                            |
| Array   | `[1, 2, 3]`, `[]`                                 |
| Object  | `{ x: 1, y: 2 }`, `{}`                           |

### Number Literals

Numbers are double-precision floating-point values. Integer and decimal forms are both supported:

```cando
VAR a = 42;
VAR b = 3.14;
VAR c = -100;
```

### String Literals

CanDo has three kinds of string literals:

**Double-quoted** — standard single-line strings with escape sequences (`\n`, `\t`, `\\`, `\"`, etc.):

```cando
VAR s = "hello\nworld";
```

**Single-quoted** — multiline strings; whitespace and newlines are preserved as-is:

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

### Boolean Literals

`TRUE` and `FALSE` (uppercase keywords).

### Null Literal

`NULL` (uppercase keyword) represents the absence of a value.

### Array Literals

```cando
VAR empty = [];
VAR nums  = [1, 2, 3];
VAR mixed = ["a", 1, TRUE, NULL];
```

### Object Literals

Keys are bare identifiers; values can be any expression:

```cando
VAR empty = {};
VAR point = { x: 10, y: 20 };
VAR nested = { a: { b: 42 } };
```

---

## Variables and Constants

Declare mutable variables with `VAR` and immutable constants with `CONST`:

```cando
VAR x = 10;
CONST PI = 3.14159;

x = 99;    // ok
// PI = 1; // runtime error — constants cannot be reassigned
```

### Multiple Assignment

Declare or assign multiple variables in one statement using a comma-separated list. Values are matched positionally:

```cando
VAR a, b = 1, 2;
// a = 1, b = 2
```

This also works with multi-return function calls:

```cando
FUNCTION minmax(a, b) { RETURN a < b ? a, b : b, a; }
VAR lo, hi = minmax(7, 3);
```

### Scope

Variables are **block-scoped**: a variable declared inside `{ }` is not visible outside it.

---

## Operators

### Arithmetic Operators

| Operator | Description            | Example           |
|----------|------------------------|-------------------|
| `+`      | Addition               | `1 + 2` → `3`    |
| `-`      | Subtraction            | `5 - 3` → `2`    |
| `*`      | Multiplication         | `4 * 5` → `20`   |
| `/`      | Division               | `15 / 3` → `5`   |
| `%`      | Modulo (remainder)     | `10 % 3` → `1`   |
| `^`      | Power / XOR (context)  | `2 ^ 8` → `256`  |

`+` also concatenates strings: `"hello" + " world"` → `"hello world"`.

### Comparison Operators

| Operator | Description           |
|----------|-----------------------|
| `==`     | Equal                 |
| `!=`     | Not equal             |
| `<`      | Less than             |
| `>`      | Greater than          |
| `<=`     | Less than or equal    |
| `>=`     | Greater than or equal |

### Logical Operators

| Operator | Description  |
|----------|--------------|
| `&&`     | Logical AND  |
| `\|\|`   | Logical OR   |
| `!`      | Logical NOT  |

Both `&&` and `||` short-circuit: the right-hand side is not evaluated if the result is already determined by the left.

### Bitwise Operators

| Operator | Description  |
|----------|--------------|
| `&`      | Bitwise AND  |
| `\|`     | Bitwise OR   |
| `^`      | Bitwise XOR  |
| `~`      | Bitwise NOT  |
| `<<`     | Left shift   |
| `>>`     | Right shift  |

### Assignment Operators

```cando
x = 5;    // assign
x += 2;   // x = x + 2
x -= 2;   // x = x - 2
x *= 2;   // x = x * 2
x /= 2;   // x = x / 2
x %= 2;   // x = x % 2
x ^= 2;   // x = x ^ 2
x++;      // x = x + 1  (postfix increment)
x--;      // x = x - 1  (postfix decrement)
```

### Unary Operators

| Operator | Description                 | Example          |
|----------|-----------------------------|------------------|
| `-`      | Numeric negation            | `-x`             |
| `!`      | Logical NOT                 | `!TRUE`          |
| `#`      | Length of string or array   | `#"hello"` → `5` |
| `...`    | Unpack all return values    | `...fn()`        |

### Range Operators

| Operator | Description                               | Example         |
|----------|-------------------------------------------|-----------------|
| `->`     | Ascending inclusive range (`start -> end`) | `1 -> 5`       |
| `<-`     | Descending inclusive range (`start <- end`)| `5 <- 1`       |

Range values are used with `FOR IN` or `FOR OF` loops:

```cando
FOR i OF 1 -> 5 { print(i); }   // 1 2 3 4 5
FOR i OF 5 <- 1 { print(i); }   // 5 4 3 2 1
```

### Operator Precedence

Standard mathematical precedence applies. Use parentheses to override:

```cando
print(2 + 3 * 4);    // 14  (multiplication first)
print((2 + 3) * 4);  // 20  (parentheses override)
```

---

## Strings

Strings are **immutable**. Use `+` to concatenate and `#` to get the length:

```cando
VAR s = "hello" + " " + "world";
print(#s);    // 11
```

Convert other types to string with `toString()`:

```cando
VAR msg = "count: " + toString(42);
print(msg);   // count: 42
```

String functions from the `string` module can be called as methods using the colon (`:`) syntax:

```cando
"hello":toUpper()       // "HELLO"
"  hi  ":trim()         // "hi"
"hello world":split(" ") // ["hello", "world"]
```

See the standard library reference for the full `string` module API.

---

## Arrays

Arrays are **ordered, zero-indexed, mutable** lists of values. The `#` operator returns the length.

```cando
VAR nums = [10, 20, 30];
print(nums[0]);   // 10
print(#nums);     // 3

nums[1] = 99;     // update element
```

Arrays can be nested:

```cando
VAR matrix = [[1, 2], [3, 4]];
print(matrix[0][1]);  // 2
```

Build arrays incrementally by assigning to `[#arr]`:

```cando
VAR result = [];
FOR i OF 1 -> 5 {
    result[#result] = i * i;
}
```

Iterate with `FOR IN` (indices) or `FOR OF` (values):

```cando
VAR fruits = ["apple", "banana", "cherry"];
FOR i IN fruits { print(i); }   // 0  1  2
FOR v OF fruits { print(v); }   // apple  banana  cherry
```

Array methods (from the `array` module) are available via colon syntax:

```cando
nums:push(40);
nums:pop();
nums:map(FUNCTION(x) { RETURN x * 2; });
```

---

## Objects

Objects are **key-value maps** where keys are strings. Values can be any type.

```cando
VAR obj = { name: "Alice", age: 30 };
print(obj.name);   // Alice
print(obj.age);    // 30
```

Add or update fields with dot notation:

```cando
obj.city = "NYC";  // add new field
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

Objects are **passed by reference**: mutating an object inside a function affects the original.

Iterate with `FOR IN` (field names) or `FOR OF` (field values):

```cando
VAR point = { x: 10, y: 20 };
FOR k IN point { print(k); }   // x  y
FOR v OF point { print(v); }   // 10  20
```

### Prototype Chain

Objects support prototype-based inheritance via the `__index` field. When a field lookup fails on an object, the VM automatically searches `__index` for the field. Use `object.setPrototype()` / `object.getPrototype()` to manage the chain.

```cando
VAR base = { greet: FUNCTION(self) { print("Hi, " + self.name); } };
VAR obj  = { name: "Alice" };
object.setPrototype(obj, base);
obj:greet();   // Hi, Alice
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

### Multi-Value Comparisons

A single comparison operator can be tested against multiple right-hand values in the same `IF` condition using a comma-separated list:

- For `>`, `<`, `>=`, `<=`, `!=`: the condition passes only if the comparison holds for **all** values.
- For `==`: the condition passes if the comparison holds for **any** value.

```cando
VAR x = 5;
IF x > 1, 2, 3  { print("pass"); }  // x > 1 AND x > 2 AND x > 3
IF x > 1, 10    { print("fail"); }  // x is not > 10
IF x == 3, 5    { print("pass"); }  // x equals 5 (any match)
IF x != 3, 5    { print("fail"); }  // x equals 5, so not != all
```

To test against all return values of a function call, prefix with `...`:

```cando
FUNCTION bounds() { RETURN 3, 100; }
IF x > ...bounds() { print("fail"); }  // x > 3 AND x > 100
```

To select specific return values with a mask:

```cando
FUNCTION triple() { RETURN 1, 999, 2; }
IF x > (~.~) triple() { print("pass"); }  // x > 1 AND x > 2 (skips 999)
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

`FOR IN` yields **keys** (array indices as numbers; object field names as strings).
`FOR OF` yields **values** (array elements; object field values).

```cando
VAR arr = ["a", "b", "c"];
FOR i IN arr { print(i); }   // 0  1  2
FOR v OF arr { print(v); }   // a  b  c

VAR obj = { x: 1, y: 2 };
FOR k IN obj { print(k); }   // x  y
FOR v OF obj { print(v); }   // 1  2
```

### FOR with Ranges

```cando
FOR i OF 1 -> 5  { print(i); }   // 1 2 3 4 5  (ascending)
FOR i OF 5 <- 1  { print(i); }   // 5 4 3 2 1  (descending)
```

### FOR OVER (Iterator Protocol)

`FOR OVER` implements a Lua-style iterator protocol. The expression must produce a triplet: `(iterator_function, state, initial_control_value)`.

At each step, `iterator_function(state, control)` is called:
1. The **first** return value becomes the new control value.
2. **Subsequent** return values are assigned to the loop variables.
3. The loop terminates when the first return value is `NULL`.

```cando
FUNCTION pairs(t) {
    RETURN FUNCTION(s, c) {
        IF c >= #s { RETURN NULL; }
        RETURN c + 1, c, s[c];   // (next_control, index, value)
    }, t, 0;
}

VAR arr = [10, 20, 30];
FOR idx, val OVER pairs(arr) {
    print(idx, val);   // 0 10  / 1 20  / 2 30
}
```

Up to 16 loop variables are supported. Extra variables are padded with `NULL` if the iterator returns fewer values.

### BREAK and CONTINUE

```cando
WHILE TRUE {
    IF done  { BREAK; }     // exit loop
    IF skip  { CONTINUE; }  // jump to next iteration
}
```

Both work in `FOR IN`, `FOR OF`, and `FOR OVER` loops.

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

Functions are first-class values and can be stored in variables, passed as arguments, and returned from other functions:

```cando
VAR square = FUNCTION(x) { RETURN x * x; };
print(square(5));  // 25

FUNCTION apply(fn, x) { RETURN fn(x); }
print(apply(square, 4));  // 16
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

Match the number of variables in `CATCH` to the number of values thrown. Extra catch variables are `NULL`; extra thrown values are discarded:

```cando
TRY {
    THROW 404, "not found";
} CATCH (code, msg) {
    print(code);  // 404
    print(msg);   // not found
}
```

### Runtime Errors

Runtime errors (division by zero, undefined variable, type errors, etc.) are catchable the same way:

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

## Pipe and Filter

The **pipe** (`~>`) and **filter** (`~!>`) operators apply a transformation to every element of an array and return a new array. Inside the body, the special keyword `pipe` refers to the current element.

### Pipe (Map)

Expression form:

```cando
VAR nums = [1, 2, 3, 4, 5];
VAR tens = nums ~> pipe * 10;   // [10, 20, 30, 40, 50]
```

Block form:

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

If the body has no explicit `RETURN NULL`, elements that do not match are simply dropped:

```cando
VAR big = nums ~!> {
    IF pipe > 3 { RETURN pipe; }
};
// big == [4, 5]
```

### Combining with Threads

Pipe is sequential. For parallel processing, combine with `thread`/`await`:

```cando
// Sequential:
VAR results = items ~> complexFn(pipe);

// Parallel:
VAR handles = items ~> thread complexFn(pipe);
VAR done    = handles ~> await pipe;
```

---

## Mask Syntax

The mask syntax selects specific values from a multi-value expression. A mask is a parenthesised pattern of `~` (pass) and `.` (skip) bits placed immediately before a value list.

### Mixed Mask

Each position corresponds to one value. `~` passes that value; `.` discards it:

```cando
// Select 1st and 3rd; discard 2nd
VAR a, b = (~.~) 1, 3, 5;
// a = 1, b = 5
```

### All-tilde Mask

Consume the first N values; extras are skipped:

```cando
VAR c = (~) 1, 2;         // c = 1; 2 is ignored
VAR d, e = (~~) 1, 2, 3;  // d = 1, e = 2; 3 is ignored
```

### All-dot Mask

Skip the first N values; remaining values are passed through:

```cando
VAR f = (.) 1, 2;        // skip 1; f = 2
VAR g = (..) 1, 2, 3;    // skip 1, skip 2; g = 3
```

### Masks with Function Calls

```cando
FUNCTION triple() { RETURN 1, 999, 2; }
VAR x = 5;

IF x > (~.~) triple() { print("pass"); }  // x > 1 AND x > 2
IF x > (~~~) triple() { print("fail"); }  // x > 1 AND x > 999 AND x > 2
```

### Unpack Operator

To pass **all** return values from a function, use the `...` unpack operator:

```cando
FUNCTION bounds() { RETURN 3, 100; }
IF x > ...bounds() { /* checks x > 3 AND x > 100 */ }
```

Without `...`, a bare function call on the right side of a comparison contributes only its first return value.

---

## Method Call Syntax

The colon (`:`) operator passes the left-hand value as the first argument to the function call:

```cando
// Equivalent calls:
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

String methods and array methods use this syntax pervasively. All functions in the `string` module accept the string as their first argument and can be called as methods on any string value. Similarly, all `array` module functions work as methods on any array.

---

## Threading

CanDo supports true OS-level concurrency via the `thread` and `await` keywords.

### thread expression

`thread` is a unary prefix keyword that wraps an expression or block as a closure and immediately spawns it as a new OS thread. It returns a thread handle.

```cando
VAR t  = thread { RETURN 42; };
VAR t2 = thread file.read("data.txt");
VAR t3 = thread add(3, 4);
```

The spawned closure has full access to all variables in scope at the point of the `thread` expression.

### await expression

`await` blocks the calling thread until the given thread handle finishes and returns the thread's return values:

```cando
VAR result = await t;
VAR a, b   = await t2;
```

If the thread returned no values, `await` produces `NULL`.

### Thread library

The global `thread` object provides thread management utilities. See the standard library reference for the full `thread` module API.

---

## Built-in Globals

These functions are available everywhere without any import.

### `print(...)`

Prints all arguments to stdout, space-separated, followed by a newline. Arrays are expanded element-by-element:

```cando
print("hello");       // hello
print(1, 2, 3);       // 1 2 3
print("x =", x);
```

### `type(value)`

Returns the type name of a value as a string. If the value is an object with a `__type` field, that field's value is returned instead:

```cando
print(type("hi"));    // string
print(type(42));      // number
print(type(TRUE));    // bool
print(type(NULL));    // null
print(type([]));      // array
print(type({}));      // object
```

### `toString(value)`

Converts a value to its string representation. If the value is an object with a `__tostring` meta-method, that method is called:

```cando
print(toString(42));      // 42
print(toString(3.14));    // 3.14
print(toString(TRUE));    // true
print(toString(NULL));    // null
```

---

## Module Loading

### `include(path)`

Loads and executes a CanDo script (`.cdo`) or a binary extension module (`.so` / `.dylib` / `.dll`) by path. Returns the values produced by the module's top-level `RETURN` statement.

```cando
VAR mymod = include("utils.cdo");
VAR x, y, z = include("module.cdo");   // multiple return values
```

Relative paths are resolved relative to the calling script's directory. Absolute paths are used as-is.

Modules are **cached** by their absolute canonical path. Subsequent calls to `include()` with the same path return the cached values without re-executing the script.

For binary modules, the shared library must export a `cando_module_init(CandoVM*)` function that registers natives and returns the module export value.

### `eval(code [, options])`

Compiles and executes a string of CanDo source code at runtime. Returns the result of the last expression (or `NULL` for statement-only code).

```cando
VAR result = eval("1 + 2 + 3");   // 6
VAR a, b   = eval("RETURN 10, 20");
```

Options object:

| Option    | Type   | Default    | Description                                          |
|-----------|--------|------------|------------------------------------------------------|
| `name`    | string | `"<eval>"` | Name shown in error messages and stack traces        |
| `sandbox` | bool   | `false`    | Run in an isolated environment; outer globals hidden |

```cando
eval("1 / 0", { name: "my_script" });
// runtime error: division by zero [my_script line 1]

eval("print('hello')", { sandbox: true });
// outer user globals not visible; native functions still accessible
```
