# Functions

Functions in CanDo are **first-class** — they can be assigned to
variables, stored in arrays and objects, passed to other functions,
returned from functions, and captured by closures.

## Definition forms

### Statement form

```cdo
FUNCTION add(a, b) {
    RETURN a + b;
}

print(add(2, 3));          // 5
```

The statement form binds a global named `add`.  The function's
`__name` field is set to `"add"` for diagnostics.

### Expression form

```cdo
VAR add = FUNCTION(a, b) { RETURN a + b; };
```

An anonymous function expression.  Use this when you need a function
value but no name — for example as a callback:

```cdo
arr:map(FUNCTION(x) { RETURN x * x; });
```

### Lambda arrow

For short one-expression callbacks:

```cdo
arr:map((x) => x * x);
arr:filter((x) => x > 10);
```

The arrow form parses as `FUNCTION(args) { RETURN <expr>; }`.  Single
parameter with no parens is **not** supported — write `(x)`.

## Parameters and arguments

### Default values

CanDo does not have parameter defaults at the language level.  Use a
guard at the top of the function:

```cdo
FUNCTION greet(name) {
    name = name || "stranger";
    print(`hello ${name}`);
}
```

### Multi-return

A function may return more than one value.

```cdo
FUNCTION minmax(a, b) {
    IF a < b { RETURN a, b; }
    RETURN b, a;
}

VAR lo, hi = minmax(7, 3);
```

When a multi-return call appears in an expression list, all return
values are spread:

```cdo
FUNCTION pair() { RETURN 1, 2; }
print(pair(), 99);           // 1 2 99 — pair() spreads
print([pair(), 99]);          // [1, 2, 99]
```

If you only want the first return value, wrap the call in parentheses:

```cdo
print((pair()));              // 1
```

Or use a [mask](expressions.md#mask-selectors-):

```cdo
VAR first, third = (~.~) f();
```

### Varargs

```cdo
FUNCTION log(tag, ...rest) {
    print(tag, ...rest);       // spread back into print's argument list
}

log("INFO", "started", 42, TRUE);
```

`...name` in a parameter list collects all remaining arguments into an
array bound to `name`.  `...expr` at a call site spreads a multi-valued
expression into the argument list.

## Closures

Inner functions capture the **enclosing local bindings**:

```cdo
FUNCTION make_counter() {
    VAR n = 0;
    RETURN FUNCTION() {
        n = n + 1;
        RETURN n;
    };
}

VAR c = make_counter();
print(c(), c(), c());          // 1 2 3
```

Captured variables are tracked by **upvalues**.  When the defining
frame returns, the upvalue is "closed" — the value is moved to the
heap, where it persists for the lifetime of every closure that captured
it.  Two closures that captured the same variable share a single
storage cell:

```cdo
FUNCTION make_pair() {
    VAR n = 0;
    RETURN FUNCTION() { RETURN n; },
           FUNCTION() { n = n + 1; };
}

VAR get, inc = make_pair();
print(get());                  // 0
inc(); inc();
print(get());                  // 2
```

## Calling functions

The basic call form is `f(args)`.  Two extra call forms exist for
methods:

| Form           | What it does                                                          |
|----------------|------------------------------------------------------------------------|
| `obj:m(a, b)`  | Method call.  Looks up `m` on `obj` (walking `__index`) and invokes it with `obj` as the first argument. |
| `obj::m(a, b)` | Fluent method call.  Same dispatch, but the result is **discarded** and `obj` is returned, so calls chain. |

```cdo
print("hi":toUpper());                          // HI
arr::push(1)::push(2)::push(3);                 // mutates arr; returns arr
```

The dot form `obj.m()` is a *plain function call* with no implicit
`self` — `obj.m(arg)` calls `m` with `arg` only.  Use `:` to inject the
receiver.

## Self-recursion

Statement-form functions can call themselves by name:

```cdo
FUNCTION fact(n) {
    IF n <= 1 { RETURN 1; }
    RETURN n * fact(n - 1);
}
```

For expression-form anonymous functions, name them when you need
recursion:

```cdo
VAR fact = FUNCTION fact(n) {
    IF n <= 1 { RETURN 1; }
    RETURN n * fact(n - 1);
};
```

## Mask selectors

`(~.~)` selects positions out of a multi-value source.  `~` keeps,
`.` skips:

```cdo
// (~.~) means "keep, skip, keep"
VAR x, z = (~.~) 1, 2, 3;          // x = 1, z = 3

FUNCTION triple() { RETURN 10, 20, 30; }

VAR first, third = (~.~) triple(); // first = 10, third = 30
```

Pure-`~` masks consume exactly their width and ignore anything past
them; pure-`.` masks skip exactly their width and pass everything past
them through unchanged.

Masks compose with comparisons:

```cdo
IF (.~..) http.get(url) == 200 {
    print("ok");
}
```

## Calling non-functions

A non-function value `v` can still be called as `v(...)` if `v` is an
object with a `__call` metamethod (see [classes.md](classes.md)).  The
runtime walks the metatable chain, then dispatches to `__call(v,
...args)`.

If no `__call` is found, the runtime raises:

```
can only call functions (got <T>)
```

Class objects are themselves callable — the default `__call` runs the
constructor and returns a fresh instance.

## Tail calls

`OP_TAIL_CALL` is reserved in the bytecode but is not yet implemented.
Recursive code that needs deep stacks should be rewritten iteratively;
the runtime will throw a "call stack overflow" error past
`CANDO_FRAMES_MAX = 256` frames.

## Examples — common idioms

```cdo
// Higher-order: pick which comparator to use.
FUNCTION sort_by(list, key_fn) {
    list:sort(FUNCTION(a, b) {
        RETURN key_fn(a) < key_fn(b);
    });
    RETURN list;
}

sort_by(users, (u) => u.age);
```

```cdo
// Memoization with a closure.
FUNCTION memoize(fn) {
    VAR cache = {};
    RETURN FUNCTION(...args) {
        VAR key = json.stringify(args);
        IF cache[key] == NULL {
            cache[key] = fn(...args);
        }
        RETURN cache[key];
    };
}

VAR slow_add = FUNCTION(a, b) { thread.sleep(10); RETURN a + b; };
VAR fast = memoize(slow_add);
print(fast(1, 2));            // 10ms
print(fast(1, 2));            // ~0ms — cached
```

```cdo
// Currying.
FUNCTION curry(f) {
    RETURN FUNCTION(a) { RETURN FUNCTION(b) { RETURN f(a, b); }; };
}

VAR add = (a, b) => a + b;
VAR add5 = curry(add)(5);
print(add5(7));               // 12
```
