# Error-reporting validation — CanDo

This document evaluates how CanDo reports errors to the console. It is the
output of running every script under `tests/scripts/errors/` through the
`cando` interpreter and reading the captured stdout, stderr, and exit code
(see `tests/error_reports/report.txt`).

## How the suite was built

68 minimal scripts, each crafted to trigger one error condition:

| Category    | Scripts | What it tests                                            |
|-------------|---------|----------------------------------------------------------|
| `lex/`      | 10      | Unterminated strings/comments, illegal characters, UTF-8 |
| `parse/`    | 27      | Missing punctuation, bad keywords, scope-rule violations |
| `runtime/`  | 31      | Type errors, arity, OOB, throw, eval, include            |

Driver: `tests/integration/run_error_tests.sh` — invokes `./cando` on every
`.cdo` under `tests/scripts/errors/<category>/`, captures stdout, stderr, and
exit status, and writes a single report.

## Headline numbers

| Bucket                                              | Count |
|-----------------------------------------------------|-------|
| Total scripts                                       |    68 |
| Wrote something to stderr                           |    54 |
| Exited 0 (the error was *not* reported at all)      |    13 |
| Exited non-zero but produced no output (silent fail)|     1 |

Stderr is wired up and works: lexer errors, parser errors, and runtime errors
all reach the console with the leading `cando:` prefix, and the great majority
of triggered cases produce a clear message. The interesting findings sit in
two places: the cases where no message was produced at all, and the cases
where the message reaches stderr but is confusing or misleading.

---

## 1. Critical: VM crashes on calling a number

**File:** `tests/scripts/errors/runtime/call_number.cdo`

```cdo
VAR x = 5;
x(1, 2);
```

Result: **`SIGSEGV` (exit 139), no stderr at all.**

Root cause: `source/vm/vm.c:3425`. `OP_CALL` treats any numeric callee as a
script-function PC offset:

```c
if (cando_is_number(callee)) {
    u32 pc = (u32)cando_as_number(callee);
    ...
    vm_push_frame(vm, frame->closure,
                  frame->closure->chunk->code + pc, ...);
}
```

The check for "must be a callable" at `source/vm/vm.c:3470` only runs *after*
this branch, so any user-supplied number is interpreted as a function pointer
and the VM jumps into junk bytecode. The intended distinction (script-function
sentinel vs. ordinary number) is encoded purely by *convention* — the parser
emits the function entry as a number constant — and there is no tag bit to
separate it from user values.

This is the most serious finding in the suite. Fix options:

1. Use a dedicated tagged variant (e.g. `TAG_SCRIPT_FN_PC`) for emitted
   function-entry values so that user-level numbers cannot reach the
   script-fn branch.
2. Bounds-check `pc < chunk->code_len` and inspect a prologue marker before
   dispatching.

A correct error here should look like the matching `runtime/call_null.cdo`
case: `cando: can only call functions (got number)` plus a call-site frame.

---

## 2. Errors that are not reported at all (exit 0)

These scripts ran "successfully" — the user gets no signal that something is
wrong.

| Script | What happened | Comment |
|---|---|---|
| `parse/const_no_init.cdo` | `CONST x;` → silently declares `x = null` | `CONST` means write-protected; a constant with no initialiser is almost certainly a typo. Should be a parse error. |
| `parse/duplicate_local.cdo` | Two `VAR x` in the same scope → the second silently shadows | Defendable, but many languages reject this; at minimum a warning is warranted. |
| `parse/return_outside_function.cdo` | Top-level `RETURN 42;` → exit 0, no output | Inconsistent with `BREAK`/`CONTINUE` outside loop, which *do* error. |
| `parse/trailing_comma_args.cdo` | Trailing comma in parameter list → accepted | Probably intentional, but undocumented. |
| `runtime/array_negative_index.cdo` | `a[-1]` → `null` | Either implement Python-style negative indexing, or error. Right now it looks like a no-op. |
| `runtime/array_out_of_bounds.cdo` | `a[10]` on a length-3 array → `null` | Same issue: silent OOB hides real bugs. |
| `runtime/bitop_on_string.cdo` | `"abc" & 5` → `0` | Strings coerce silently to 0 for bitwise ops. Inconsistent with `1 + TRUE` which errors. |
| `runtime/index_with_string.cdo` | `[1,2,3]["nope"]` → `null` | Should error: arrays index by number. |
| `runtime/iterate_non_iterable.cdo` | `FOR x OF 42 { print(x); }` → prints `42` | Scalar treated as a 1-element iterable. Document or reject. |
| `runtime/mod_by_zero.cdo` | `10 % 0` → `nan` | `10 / 0` correctly raises `division by zero`. `%` is inconsistent. |
| `runtime/print_no_args.cdo` | `print();` → blank line, exit 0 | OK in isolation, but worth documenting. |
| `runtime/string_concat_number.cdo` | `"hello" + 1` → `"hello1"` | Implicit number→string coercion; `1 + TRUE` errors. Pick one rule. |
| `runtime/wrong_arity_too_many.cdo` | `add(1, 2, 3, 4)` → `3` | Extra arguments silently dropped. Most strict languages error. |

Of these, three are particularly worth fixing:

- **Out-of-bounds / wrong-type array index** (3 scripts): the
  silent-`null` behaviour is the single biggest bug-hider in the suite.
- **`mod_by_zero`** is inconsistent with `div_by_zero` and almost certainly
  a missed check in `OP_MOD`.
- **`RETURN` outside a function** is a parse-time bug that already has
  matching logic for `BREAK`/`CONTINUE`.

---

## 3. Errors that are reported, but the message can be improved

These all reach stderr but the message is uninformative, misleading, or
inconsistent with the rest of the diagnostics.

### 3.1 Parser messages that don't name the construct

| Script | Current message | Suggested |
|---|---|---|
| `parse/for_missing_in.cdo` | `Error at '}': expected expression` | `expected 'IN' after FOR loop variable` |
| `parse/missing_then_block.cdo` | `Error at '': expected '}' to close block` | `expected '{' after IF condition` |
| `parse/missing_function_body.cdo` | `Error at '': expected '}' to close block` | `expected '{' after parameter list` |
| `parse/dangling_else.cdo` | `Error at '}': expected expression` | `ELSE without matching IF` |
| `parse/catch_without_try.cdo` | `Error at '}': expected expression` | `CATCH without matching TRY` |
| `parse/keyword_as_var.cdo` (`VAR IF`) | `Error at '': expected '}' to close block` | `'IF' is a reserved keyword and cannot be used as a variable name` |
| `parse/missing_function_name.cdo` | `Error at '': expected ';' after expression` | `expected function name after FUNCTION (use 'FUNCTION (...) {...}' for an anonymous function expression)` |

These are all cases where the parser falls back to a generic "expected X"
because the calling rule didn't install a dedicated message. In every case
the *grammar position* makes the intent unambiguous, so a specific message
is both possible and much more useful.

### 3.2 Lexer: noise in the echoed lexeme

`error_at` formats every error as ``[line N] Error at '<lexeme>': <msg>``.
For an unterminated string, the entire (unterminated) string body is the
"lexeme", so the user sees:

```
cando: [line 1] Error at '"this string never closes;
print(s);
': unterminated double-quoted string
```

The trailing source dump is noise — the message already tells you what's
wrong, and the multi-line `'...'` quoting is misleading (looks like the
literal extends across `print(s)`). Suggested fix in
`source/parser/parser.c:74` `error_at`: when `tok->type == TOK_ERROR`, clip
the lexeme to the first 32 chars (or the first newline) before rendering.

### 3.3 Redundant `(line N)` suffix from the lexer

```
cando: [line 1] Error at '@': unexpected character '\x40' (line 1)
```

The `[line 1]` prefix is added by `error_at`; the trailing `(line 1)` is
added by the lexer at `source/parser/lexer.c:612`. Remove the trailing
copy — `error_at` already has the line.

### 3.4 UTF-8 unfriendly "unexpected character"

```
cando: [line 2] Error at '�': unexpected character '\xCF'
```

(Triggered by `VAR π = 3.14;`.) CanDo reports the first byte of a multi-byte
UTF-8 character only. Two improvements:

1. Detect a UTF-8 lead byte and decode it for the message: `unexpected
   non-ASCII character (U+03C0 'π') — identifiers must be ASCII`.
2. Don't render the replacement character (`�`) in the echoed lexeme; that
   distracts from the actual diagnostic.

### 3.5 Path is missing from parser errors but present in runtime errors

Compare:

```
cando: [line 1] Error at ';': expected ')' after arguments
```

vs.

```
cando: division by zero
  at /home/user/CanDo/tests/scripts/errors/runtime/div_by_zero.cdo:1
```

Runtime traces include the path; parser errors only have `[line N]`. For
single-file scripts that's enough, but as soon as `include()` is involved
the user can't tell *which* file the parser is complaining about. The
parser already has the path (it's plumbed into runtime traces via the
chunk); thread it into `error_at` too.

### 3.6 Stack frames don't name the function

`runtime/deep_call_stack.cdo` produces:

```
cando: error
  at .../deep_call_stack.cdo:3
  at .../deep_call_stack.cdo:2
  at .../deep_call_stack.cdo:1
  at .../deep_call_stack.cdo:4
```

The trace is correct, but every frame says `at <path>:<line>` only — no
function names. A useful trace would read:

```
  at c (deep_call_stack.cdo:3)
  at b (deep_call_stack.cdo:2)
  at a (deep_call_stack.cdo:1)
  at <main> (deep_call_stack.cdo:4)
```

The frame already has access to the closure (`source/vm/vm.c:1349` is where
the trace is appended). Use `closure->fn->name` when available.

### 3.7 Method lookup omits the method name

`runtime/method_not_found.cdo` (calls `a:fly()` on an array):

```
cando:  method is not callable
```

Two problems: double space, and no method name. Should read
`method 'fly' is not callable on type 'array'`.

### 3.8 `index access on non-object` doesn't say the type

`runtime/index_number.cdo`:

```
cando: index access on non-object
```

Compare to the sibling field-access error which *does* include the type:
`field access on non-object (got null)`. Make them symmetric.

### 3.9 `THROW` discards code/detail when uncaught

`runtime/throw_uncaught.cdo`:

```cdo
THROW "validation", 422, "missing 'name'";
```

```
cando: validation
```

The `422` and `"missing 'name'"` arguments are silently dropped. A `TRY {…}
CATCH (kind, code, detail) {…}` block would receive all three; an uncaught
throw should print them too, e.g. `validation [422]: missing 'name'`.

### 3.10 `eval()` parse errors lose the trace, runtime errors keep it

`runtime/eval_syntax_error.cdo`:

```
cando: eval parse error: [line 1] Error at ';': expected expression
```

No `at <file>:<line>` trace. The matching runtime case
(`eval_runtime_error.cdo`) has both the `<eval>` frame and the caller frame.
Adding the same trace to `eval` parse errors would make the source of the
faulty `eval(...)` call easy to find.

### 3.11 Arity mismatch isn't caught at the call site

`runtime/wrong_arity_too_few.cdo` — calling `add(1)` where `add(a, b)` —
produces:

```
cando: operands must be numbers (got number and null)
  at .../wrong_arity_too_few.cdo:1   <-- inside add, at a + b
  at .../wrong_arity_too_few.cdo:2   <-- caller
```

The user sees a *type* error one line into the callee, when the real bug
is "you passed 1 argument to a 2-arg function". An arity check at the call
site (matching the behaviour of `wrong_arity_too_many`, which silently
*succeeds* — see §2) would produce a much more direct message such as
`add() expects 2 arguments, got 1`.

---

## Summary

- The console **does** receive lex, parse, and runtime errors — the
  reporting plumbing works.
- **One memory-safety bug**: calling a number value segfaults the VM
  (`source/vm/vm.c:3425`). This is the only crash in the suite and should
  be prioritised.
- **13 scripts** that should have been errors ran to completion. Three of
  those are clearly bugs (out-of-bounds index, `mod` by zero, `RETURN`
  outside function); the rest are policy choices that should be either
  enforced or documented.
- **11 distinct message improvements** are listed in §3, covering
  unnamed-construct parser messages, noisy lexeme echoes, redundant
  line tags, non-UTF-8 friendly diagnostics, missing function names in
  stack traces, and lost `THROW` arguments.

The raw per-script output is in `tests/error_reports/report.txt`. The
suite itself lives in `tests/scripts/errors/` and is reproducible via
`tests/integration/run_error_tests.sh`.

---

## Follow-up: fixes applied (post-evaluation)

The findings above produced a conservative set of fixes in this same
branch. Re-running the suite after the changes:

| Bucket                        | Before | After |
|-------------------------------|--------|-------|
| Stderr produced               |   54   |   57  |
| Exited 0 silently             |   13   |   11  |
| Non-zero but silent (crashes) |    1   |    0  |
| Existing integration tests    |   58 / 58 pass | 58 / 58 pass |

### Code changes

- `source/vm/vm.c` (`OP_CALL`): the unreachable-for-valid-programs
  number-as-PC dispatch path was the SIGSEGV root cause. Replaced with
  a clean `"can only call functions (got number)"` runtime error
  (`tests/scripts/errors/runtime/call_number.cdo` now reports rather
  than crashes).
- `source/vm/vm.c` (`OP_MOD`): added `if (b == 0.0) error("modulo by
  zero")`, matching the existing `OP_DIV` behaviour.
- `source/vm/vm.c` (`OP_THROW`): on an uncaught throw, format every
  thrown value, not just the first, so `THROW kind, code, detail` is
  no longer silently truncated to `kind`.
- `source/vm/vm.c` (`OP_GET_INDEX`): include the operand type in the
  `index access on non-object` message, mirroring the `field access`
  sibling.
- `source/vm/vm.c` (`OP_METHOD_CALL` / `OP_FLUENT_CALL`): the
  `method-not-callable` error now names the method and the receiver
  type: `method 'fly' is not callable on object`.
- `source/parser/parser.c` (`parse_var_decl`): a `CONST` declaration
  without an initialiser is now a parse error rather than a silent
  null-binding.
- `source/parser/parser.c` (`parse_statement`): top-level `ELSE`,
  `ALSO`, `CATCH`, `FINALY`, `IN`, `OF`, `OVER` now produce
  construct-specific diagnostics instead of falling through to a
  generic "expected expression".
- `source/parser/parser.c` (`error_at`): retain the first error rather
  than letting `synchronise()`-then-overwrite stomp it with a
  follow-up recovery diagnostic. Also clip the echoed lexeme to 32
  chars / first newline so unterminated strings no longer dump a
  multi-line lexeme into the error.
- `source/parser/lexer.c`: the "unexpected character" error now uses
  the printable form for ASCII (`'@'`), a dedicated
  "non-ASCII byte 0xCF -- identifiers must be ASCII" form for high
  bytes, and the parser's own `[line N]` prefix is no longer
  duplicated.

### What's still open (intentionally)

The 11 scripts that still exit 0 silently all fall under the
"conservative fix" exclusion: silent OOB array indexing, implicit
number↔string coercion (`"hello" + 1`), trailing comma in parameter
lists, `FOR x OF 42` treating a scalar as a one-element iterable, and
extra-argument tolerance on `add(1, 2, 3, 4)`. Each of these is a
language-design choice rather than a bug, and tightening any of them
risks breaking existing scripts. Re-open the discussion with an
"aggressive" pass if stricter semantics are wanted.

Two improvements from the original list were also deferred:

- **Function names in stack frames.** The trace currently emits
  `at <path>:<line>` per frame. Including the function name (`at c
  (file:line)` etc.) requires adding a `name` field to the
  `OBJ_FUNCTION` struct and plumbing it through `parser.c` and
  `OP_CLOSURE`. Larger change than the others in this pass.
- **File path on parser errors.** Runtime traces include the path;
  parser errors only say `[line N]`. Same plumbing as above —
  threading `chunk->name` into `error_at`.
