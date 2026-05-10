# Language Reference

This directory contains the normative reference for the CanDo language.
Each topic gets its own file so you can link to it precisely.

The reference describes the language surface — syntax, semantics, error
messages — rather than the standard library; for per-namespace, per-
function library docs see [`../libraries/`](../libraries/README.md).

## Pages

| Page | Topic |
|---|---|
| [syntax.md](syntax.md) | Lexical structure: source encoding, comments, identifiers, keywords, literals, operator characters. |
| [types.md](types.md) | The five value types (`null`, `bool`, `number`, `string`, `object`), truthiness, type coercion. |
| [expressions.md](expressions.md) | Operators, precedence, associativity, ternary, safe access, ranges, masks. |
| [statements.md](statements.md) | `IF`, `WHILE`, `FOR`, `BREAK`, `CONTINUE`, blocks, scoping rules, multi-assignment. |
| [functions.md](functions.md) | Definition forms, closures, multi-return, varargs and spreading, mask selectors. |
| [classes.md](classes.md) | `CLASS`, `EXTENDS`, prototype chain, metamethods, the three class forms. |
| [error-handling.md](error-handling.md) | `TRY` / `CATCH` / `FINALY` / `THROW`, error message catalogue. |
| [threading.md](threading.md) | The `thread { … }` expression, `await`, cancellation, shared state. |
| [pipes.md](pipes.md) | `~>` map, `~!>` filter+map, `~&>` predicate filter, the `pipe` keyword. |
| [modules.md](modules.md) | `include()` resolution rules, caching, file-extension dispatch. |

## Conventions used in these pages

- Code blocks are tagged `cdo` for CanDo source and `c` for C.
- `s`, `a`, `obj`, `t` mean string, array, object, and thread receivers.
- Argument names suffixed with `*` are optional.
- A `→` after a function signature shows the return type, or a list of
  return types for multi-return.

## Reading order

If you are new to CanDo, the recommended reading order is:

1. [syntax.md](syntax.md) — what does the source look like?
2. [types.md](types.md) — what values exist?
3. [expressions.md](expressions.md) and [statements.md](statements.md) —
   how do you compose them?
4. [functions.md](functions.md) — the most-used construct in real code.
5. [classes.md](classes.md) — when you need user-defined types.
6. [error-handling.md](error-handling.md), [threading.md](threading.md),
   [pipes.md](pipes.md), [modules.md](modules.md) — pick as needed.
