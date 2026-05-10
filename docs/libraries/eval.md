# `eval`

Compile and run a string of CanDo source in the current VM.

## Reference

### `eval(source) → any`

Compile `source` as a CanDo expression (or block of statements) and
execute it in the calling VM.  The last expression is the return
value.  The evaluated code has full access to the calling globals.

Parse or runtime errors are thrown — wrap in `TRY` / `CATCH` to handle.

```cdo
print(eval("1 + 2 * 3"));               // 7

VAR x = 10;
print(eval("x * x"));                    // 100  — sees the calling x

VAR fn = eval("FUNCTION(a, b) { RETURN a + b; }");
print(fn(2, 3));                         // 5
```

## When to use it

`eval` is occasionally useful for:

- **Templating systems** that compile user-supplied formula strings.
- **REPLs and consoles** built into a host application.
- **Testing tools** that need to run snippets dynamically.

For ordinary code organization, **prefer [`include()`](include.md)** —
it caches by path, returns module values, and reads from a file rather
than a string.  `eval` should be your last choice: it bypasses module
boundaries, has no caching, and makes the source location of bugs
harder to find.

## Examples

### A tiny REPL

```cdo
WHILE TRUE {
    print("> ");
    VAR line = file.lines("/dev/stdin"):pop();   // platform-dependent
    IF line == NULL { RETURN; }

    TRY {
        print(eval(line));
    } CATCH (e) {
        print("error:", e);
    }
}
```

### Configurable predicate

```cdo
VAR cfg = include("./config.yaml");
VAR is_eligible = eval("FUNCTION(user) { RETURN " + cfg.eligible_expr + "; }");

VAR eligible = users ~&> is_eligible(pipe);
```

(Be careful with this pattern — `eval` will happily run anything in
`cfg.eligible_expr`, so it must come from a trusted source.)

## Caveats

- `eval` does not introduce a new scope: assignments without `VAR`
  create globals just as they would in a top-level script.
- The parser is the same one used for source files, so all the syntax
  rules in [../language/syntax.md](../language/syntax.md) apply.
- `eval` is **not** sandboxed.  Do not feed it strings that came from
  the network or from untrusted users.
