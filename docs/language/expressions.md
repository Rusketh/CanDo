# Expressions

This page covers operators, precedence, and the special expression
forms.  See [statements.md](statements.md) for control flow.

## Operator table

| Category   | Operators                                                  |
|------------|------------------------------------------------------------|
| Arithmetic | `+  -  *  /  %  ^` (power)                                 |
| Unary      | `-x  +x  !x  ~x  #x  ++x  --x`                             |
| Comparison | `==  !=  <  >  <=  >=`                                     |
| Logical    | `&&  \|\|  !`                                              |
| Bitwise    | `&  \|  \|&` (xor)  `~` (not)  `<<  >>`                    |
| Assignment | `=  +=  -=  *=  /=  %=  ^=`                                |
| Indexing   | `a[i]   obj.field   obj["field"]`                          |
| Safe       | `obj?.field   obj?[expr]`                                  |
| Call       | `f(...)   obj:method(...)   obj::method(...)`              |
| Range      | `1 -> 10`  /  `10 <- 1`                                    |
| Length     | `#x`                                                       |
| Pipe       | `arr ~> body`  /  `arr ~!> body`  /  `arr ~&> body`        |
| Conditional| `cond ? then_expr : else_expr`                             |
| Mask       | `(~.~) expr`                                               |
| Vararg     | `...args` in parameters; `...expr` in call sites           |

## Precedence

From lowest to highest (each row binds tighter than the row above it):

1. Assignment (`=`, compound `+=` …) — right-associative
2. Ternary `? :` — right-associative
3. Logical OR `||`
4. Logical AND `&&`
5. Bitwise OR `|`, XOR `|&`
6. Bitwise AND `&`
7. Equality `==`, `!=`
8. Comparison `<`, `<=`, `>`, `>=`
9. Bit shift `<<`, `>>`
10. Range `->`, `<-`
11. Additive `+`, `-`
12. Multiplicative `*`, `/`, `%`
13. Power `^` — right-associative
14. Unary `- + ! ~ # ++ --`
15. Postfix call `(...)`, member `.`, index `[…]`, method `:`, fluent
    `::`, safe `?.` / `?[`

This follows C with two adjustments: `^` is power (right-assoc), and
the pipe operators (`~>`, `~!>`, `~&>`) form their own associativity
class above range and below assignment.

## Logical `||` and `&&`

These return one of their operands verbatim — **the result is not
coerced to a boolean**.

- `a || b` returns `a` if `a` is truthy, otherwise `b`.
- `a && b` returns `a` if `a` is falsy, otherwise `b`.

`NULL`, `FALSE`, and `0` are falsy (see [types.md](types.md)).  Objects
may override their truthiness via the `__is` metamethod.

```cdo
print(FALSE || 0);            // 0          (both falsy; returns last)
print(NULL  || "default");    // default
print("a"   || "b");          // a
print(0     && "won't print");// 0
print(1     && "yes");        // yes
```

This is the idiomatic way to provide a default:

```cdo
VAR name = user.nickname || user.realname || "anonymous";
```

## Logical NOT `!`

`!x` returns a boolean: `TRUE` if `x` is falsy, `FALSE` otherwise.

## Comparison

`==` and `!=` are identity-based for objects — two separate objects
with the same contents compare unequal.  Numbers, booleans, strings,
and `null` compare by value.

```cdo
print({ a: 1 } == { a: 1 });   // false — different objects

VAR o = { a: 1 };
print(o == o);                 // true  — same object

print("hi" == "hi");           // true  — strings are interned
```

Order comparison (`<`, `<=`, `>`, `>=`) requires both sides to be
numbers (or both strings — string comparison is byte-lexicographic) or
to be objects whose class defines `__lt` / `__le`.

### Multi-comparison

```cdo
IF code == 200, 201, 204 {
    print("success");
}

IF grade > 50, 60, 70 {
    // all three relations must hold:
    // grade > 50 AND grade > 60 AND grade > 70
    print("solid pass");
}
```

`==` and `!=` use **any/none** semantics.  Ordering operators (`<`,
`<=`, `>`, `>=`) require the relation to hold against *every* right-hand
value.

This works inside `WHILE` and on right-hand-side expressions too:

```cdo
VAR is_success = code == 200, 201, 204;     // bool result
```

## Ternary `? :`

```cdo
VAR label = score >= 50 ? "pass" : "fail";
```

Right-associative, so chains read top-to-bottom:

```cdo
VAR grade = pct >= 90 ? "A"
          : pct >= 80 ? "B"
          : pct >= 70 ? "C"
          :             "F";
```

The condition is evaluated once.  Only the chosen branch is evaluated,
so `?:` short-circuits like `IF`/`ELSE`.

## Safe access `?.` and `?[]`

`obj?.field` and `obj?[expr]` evaluate the receiver first; if it is
`NULL` the whole chain short-circuits to `NULL` without evaluating the
rest of the access.  Once the chain has gone through one `?.`, every
subsequent `.`, `[`, `:`, or `(` in the same expression also
short-circuits if it sees `NULL`:

```cdo
VAR obj = { a: { b: 42 } };

print(obj?.a.b);               // 42
print(obj?.x.y.z);             // null  — stops at obj.x
print(obj?["a"].b);            // 42
print(NULL?.a.b);              // null  — never dereferences NULL
print(NULL?:method().more);    // null
```

Use `?.` to walk possibly-missing nested data without raising the
runtime "field access on non-object" error.

## Indexing

```cdo
arr[0]                         // numeric index into an array
obj.field                      // bare-identifier field access
obj["field name"]              // string-key field access
obj[any_string_expr]           // computed key
```

Bare identifiers and `["…"]` resolve to the same hash entry; `obj.foo`
and `obj["foo"]` are interchangeable.

Out-of-range indexing returns `NULL`; never an exception.  Writing past
the end of an array extends it.

## Method call: `:` and `::`

```cdo
"hello":toUpper()              // → "HELLO"
arr:push(42)                   // arr is passed as the first argument (self)
```

The double-colon variant `::` calls the method but **returns the
receiver** instead of the method's result, so calls chain:

```cdo
obj::set_x(3)::set_y(4)::set_z(5);
// equivalent to:
//   obj.set_x(obj, 3); obj.set_y(obj, 4); obj.set_z(obj, 5);
```

This is "fluent style".  The method's return value is discarded by `::`.

## Range generators

`a -> b` and `b <- a` both produce the inclusive integer range
`[a, b]`.  Most often used inside `FOR … IN`:

```cdo
FOR i IN 1 -> 5  { print(i); }      // 1 2 3 4 5
FOR i IN 5 <- 1  { print(i); }      // 5 4 3 2 1
```

A range used in a value context expands to a stack of numbers (i.e.
behaves like `RETURN 1, 2, 3, 4, 5`).

## Length `#`

`#x` returns:

- the byte length of a string,
- the integer-indexed length of an array,
- the number of own keys in a plain object,
- whatever `__len` returns, if defined.

```cdo
print(#"hello");               // 5
print(#[10, 20, 30]);          // 3
print(#{ a: 1, b: 2 });        // 2
```

## Mask selectors `(~.~)`

A mask selects positions out of a multi-value expression.  `~` keeps a
value, `.` skips it.

```cdo
// (~.~) means "keep, skip, keep"
VAR x, z = (~.~) 1, 2, 3;          // x = 1, z = 3

FUNCTION triple() { RETURN 10, 20, 30; }

VAR first, third = (~.~) triple(); // first = 10, third = 30
```

Pure-`~` masks consume exactly their width and ignore anything past it;
pure-`.` masks skip exactly their width and pass everything past them
through unchanged.  Mixed masks (`~`/`.` interleaved) apply strictly
per position.

Masks compose with comparison too — useful for asserting "the second
return value is 200":

```cdo
IF (.~..) http.get(url) == 200 {
    print("ok");
}
```

## Vararg / spread `...`

In a parameter list, `...rest` collects the remaining arguments into a
local array:

```cdo
FUNCTION log(tag, ...rest) {
    print(tag, ...rest);
}
```

In a call site, `...expr` spreads a multi-valued expression (an array,
a multi-return call, or `...rest`) into the argument list:

```cdo
VAR args = ["a", "b", "c"];
print(...args);                  // a b c
```

A multi-return call used in a larger expression list spreads
automatically — `f(g())` passes all of `g`'s return values as separate
arguments to `f`.  The explicit `...` is only needed when the spread
source is an array or a stored value rather than a fresh call.

## Pipes

`~>`, `~!>`, and `~&>` iterate the array on their left and run the
block on their right with `pipe` bound to the current element.  Full
treatment in [pipes.md](pipes.md).

```cdo
VAR ns = [1, 2, 3, 4, 5];
VAR doubled = ns ~> pipe * 2;             // [2, 4, 6, 8, 10]
VAR evens   = ns ~&> pipe % 2 == 0;        // [2, 4]
```
