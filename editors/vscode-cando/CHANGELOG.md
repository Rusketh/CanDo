# Changelog

All notable changes to the **CanDo Language** VS Code extension are
documented in this file.

## 0.8.0 -- 2026-05-11

### Added

- **Cross-function object-shape prediction.** Every user function gets
  a per-parameter write summary recorded during inference: every
  direct `param.key = value` mutation. At call sites, the summary is
  replayed onto the argument's binding, so an `init(rec)` whose body
  writes `o.x` and `o.y` makes `rec.x` and `rec.y` available to
  completion afterwards. Summaries propagate through `VAR alias =
  init` (the FunctionType reference carries them) and through object
  fields (`{ cb: init }`). Conservative on `any` / unions / non-shape
  values -- the worst case is "no extra knowledge," never a wrong
  claim. Implements the middle-ground plan we discussed: per-function
  summaries + zero-depth on-call replay, no global fixpoint.
- **Structured doc comments.** The extension now recognises a small
  JSDoc/LuaLS-flavored tag vocabulary in `///` line and `/** */`
  block comments and turns it into type information that drives
  completion, hover, signature help, and diagnostics:
  - `@param name {type} description` -- type a function parameter.
  - `@returns {type} description` -- type a return value (repeat for
    multi-return).
  - `@type {type}` -- type a `VAR` whose initialiser the inferer
    can't pin down.
  - `@field name {type} description` -- declare a class member; the
    field becomes visible on `self.name` and on instances.
  - `@shape Name { k: T, k2: T2 }` -- reusable named record type.
  - `@callback Name (a: T, b: U) -> R` -- reusable function-signature
    alias.
  - `@class Name` / `@throws {type}` / `@thread-safe` / `@see` /
    `@example` -- rendered into hover.
  - `@deprecated msg` -- marks the binding; references render
    struck-through and the completion entry is tagged.
- **Doc-type mini-language.** A small recursive-descent parser
  handles primitives (`number`, `string`, `bool`, ...), arrays
  (`T[]` / `Array<T>`), unions (`T | U`), optionals (`T?`), object
  literals (`{ k: T, k2: T2 }`), function literals
  (`(a: T) -> R`, with multi-return `(...) -> R1, R2`), and named
  references to `@shape` / `@callback` aliases declared anywhere in
  the same file (including at file scope, outside any declaration).
- **Doc diagnostics.** Three new advisory codes:
  - `doc-bad-type` -- a `{type}` annotation didn't parse.
  - `doc-unknown-tag` -- `@foo` isn't a known tag.
  - `doc-deprecated-use` -- a reference to a `@deprecated` binding.

## 0.7.0 -- 2026-05-11

### Added

- **Unused variable / parameter detection** -- locals and params with
  no reads are flagged as unnecessary (rendered faded by the editor).
  Names starting with `_` are silenced; functions / classes / globals
  are exempt because they're API surface.
- **Dead-code detection** -- statements after `RETURN`, `THROW`,
  `BREAK`, `CONTINUE`, `SETTLE` in the same block are flagged
  unreachable.
- **Argument type checking** -- when a callee's parameter has a known
  concrete type, mismatched argument types raise a warning. Permissive
  on `any` / `unknown` to avoid noise on under-typed code.
- **"Did you mean ...?" code action** -- undefined identifiers get a
  Levenshtein-bounded suggestion plus a one-click rename to the
  closest visible name (locals, scoped names, namespaces, builtins).
- **Auto-include code action** -- typing `forms.x` with `forms`
  unbound suggests adding `VAR forms = include("./forms.so");` at the
  top of the file when a matching native module is found under
  `<workspace>/modules/<name>/cando.api.json`.
- **Call hierarchy** -- right-click on a function for "Show Call
  Hierarchy". Incoming calls are aggregated by enclosing function
  across the whole workspace; outgoing calls walk the body.
- **`workspace/didChangeWatchedFiles`** -- external `.cdo` file
  changes invalidate the workspace index and refresh open documents.
- **`workspace/willRenameFiles`** -- renaming a `.cdo` file
  rewrites every workspace `include("...")` argument that pointed at
  it to the new relative path.

## 0.6.0 -- 2026-05-11

### Added

- **Document highlights** -- every occurrence of the binding under the
  cursor is highlighted (Write kind on the declaration; Read on uses).
- **CodeLens** -- function and class declarations show their workspace
  reference count inline.
- **Document formatting** -- single-pass token-aware formatter:
  normalizes internal whitespace, indents to 4 spaces, ensures a space
  after `,`, leaves strings / comments / template literals untouched.
- **`__call` metamethod** -- instances whose class defines `__call`
  are callable; the return-type comes from `__call`'s function
  signature (no more spurious "non-callable" diagnostics).
- **Operator overloads** -- binary `+ - * / % ^ == < <=` dispatch
  through `__add`, `__sub`, etc., so `Vector(1,2) + Vector(3,4)`
  infers as `Vector`.
- **Type narrowing via `type(x) == "..."`** -- inside the IF branch
  `x`'s type is locked to the named runtime tag (string/number/bool/
  array/object/null or any class / manifest type). Same for
  `x.__type == "Foo"` discrimination.
- **Member access on `any` stays `any`** -- previously degraded to
  `unknown`, which suppressed completion chains starting from
  untyped parameters.
- **Self-type inference** -- assigning `ClassOrObj.method =
  FUNCTION(self, ...) { ... };` infers `self` as the owner's instance
  type, so member access inside the method body is typed.
- **JSDoc-style `@param` / `@returns` / `@deprecated` / `@example`**
  in doc comments now render as Markdown sections in hover.
- **Richer snippets** -- adds doc-commented function, multi-var
  decl, ALSO branch, method-on-class, named/anon function, vararg
  function, throw, mask selector, default-value pattern, safe access,
  inspect. Existing snippets corrected (FOR range now uses `IN`).

## 0.5.0 -- 2026-05-11

### Added

- **Find references** -- every Ident occurrence that resolves to the
  same binding (across the workspace for file-scoped declarations).
- **Rename** -- workspace-wide, behind `prepareRename` so the editor
  only offers it on real bindings (skips `self`, `pipe`, namespaces).
- **Workspace symbols** -- searchable index of every top-level
  declaration in every `.cdo` under the workspace roots.
- **Inlay hints** -- shows inferred types on `VAR x = expr;` and
  parameter names at call sites for literal arguments.
- **Folding ranges** -- collapses every block, multi-line literal,
  IF chain, loop, TRY, and multi-line comment.
- **Selection ranges** -- smart expand (Cmd/Ctrl+Shift+Right) walks
  the AST node stack from the cursor outward.
- **Semantic tokens** -- richer highlighting that distinguishes
  parameters, locals, captured upvalues, classes, functions, and
  default-library namespaces.
- **Color provider** -- `0xRRGGBB` / `0xAARRGGBB` numeric literals
  render as inline color swatches and can be tweaked with the picker.
- **Code actions** -- quick fixes for "undeclared identifier" (inserts
  `VAR <name> = NULL;`) and "Cannot assign to CONST" (rewrites
  `CONST` to `VAR`).
- **Doc comment harvesting** -- consecutive `//`, `///`, and `/* */`
  comments immediately above a declaration are attached to the
  binding and shown in hover + completion documentation.
- **Template-string interpolations are now real expressions.** The
  parser re-lexes each `${...}` with the correct source-offset, so
  member access, completion, and references work inside template
  strings.
- **Completion ranking** -- locals win ties over globals, then
  function/class declarations, then namespaces / builtins.

### Improved

- Reference tracking added to every binding (resolver pass).
- Workspace indexer with mtime-based invalidation; refreshes when a
  visible document changes so refs always reflect the open buffers.

## 0.4.0 -- 2026-05-11

### Rewritten

- **Complete rewrite of the language server around a real parser + AST +
  scope tree + type inferer.** The previous server walked the token
  stream with shallow pattern matching; it never tracked function return
  values, lost member writes through reassignment, and couldn't see
  beyond first-occurrence symbol lookups. The new pipeline (parse →
  resolve → infer) is structurally aware:
  - **Function return types are tracked.** `VAR x = f(); x.|`
    now lists `f`'s actual return-value members.
  - **Multi-return distribution.** `VAR a, b = pair();` types `a` and
    `b` positionally from `pair`'s return tuple.
  - **Member-flow through reassignment.** `VAR x = {}; x.foo = 1;
    VAR y = x; y.|` shows `foo`.
  - **Indexing.** `arr[i]` now yields the array's element type.
  - **Class / EXTENDS chains.** `Dog EXTENDS Animal` instance lookup
    walks the prototype chain via the manifest- or in-file-defined
    `__index` parent.
  - **Fluent `::` calls.** Always type to the receiver, so chains
    survive `obj::a()::b()`.
  - **Pipes.** `arr ~> body` exposes `pipe: <element type>` in the
    body scope; `~>` yields `array<bodyType>`, `~&>` / `~!>` yield
    `array<sourceElement>`.
  - **Flow narrowing.** `IF x { x.| }` drops the `null` variant of a
    union so member completion shows the truthy side.
  - **Closures and scoping.** Block / function / file scopes with
    proper shadowing; upvalue captures are tracked.
  - **Cross-file include.** `.cdo` modules are re-analyzed; their
    top-level `RETURN value;` (or top-level binding bag) becomes the
    include's value type. Binary modules fall through to their
    `cando.api.json` manifest.

### Added

- **Semantic diagnostics** (on by default) flag `undefined-identifier`
  (advisory), `wrong-arg-count`, `non-callable-call`, and
  `assign-to-const`. Disable with `cando.diagnostics.semantic = false`.
- **Headless test suite** at `server/test/cases/*.cdo` plus
  `server/test/runner.js`. Run with `npm test`.

### Removed

- The legacy `analyzer.ts`, `types.ts`, and `crossfile.ts` modules
  (replaced by `ast.ts`, `parser.ts`, `scope.ts`, `typesys.ts`,
  `infer.ts`, and `analyze.ts`).

## 0.3.3 -- 2026-05-01

### Fixed

- **Cross-file include now uses the actual return shape, not the file's
  top-level VARs.** When a `.cdo` module did
  `VAR exports = { foo, bar }; RETURN exports;` the language server
  was leaking `helper`, `thing`, `exports` -- every top-level
  declaration in the file -- as if they were public members. The type
  tracker now parses the included file and infers the type of its
  top-level `RETURN <expr>` statement directly. Three shapes are
  recognised:
  1. `RETURN { ... }`     -- literal object
  2. `RETURN ident;`      -- a variable bound to an object literal
  3. `RETURN <chain>`     -- chained call whose type the tracker can
                              follow (e.g. another module's exports)
  The crossfile fallback no longer surfaces top-level VARs at all.
- **Function-body locals show up in identifier completion.** The
  analyzer only collects symbols at depth 0 (for the Outline view),
  so `VAR localFoo = ...` declared inside a `FUNCTION` was invisible
  to plain identifier completion. The general completion path now
  also walks the type env's bindings, which includes every
  `VAR/CONST/GLOBAL` regardless of nesting.

## 0.3.2 -- 2026-05-01

### Fixed

- **`::` is the fluent-chain operator.** CanDo distinguishes `:` (method
  call -- result is whatever the method returns) from `::` (chain --
  result is always the receiver). The lexer already tokenises `::` as a
  single op, but the type tracker was treating it like `:`. Both the
  receiver walk-back (`inferReceiverAt`) and the postfix step
  (`inferPostfix`) now recognise `::` and preserve the receiver type
  regardless of the method's declared return. So
  `t::set_v(200)::set_v(300)` keeps its type on `t`, matching
  `tests/scripts/method_call.cdo`.
- **Runtime member attachments to records.** `VAR t = { v: 100 };
  t.meth = FUNCTION(self) { ... };` -- the `meth` assignment was
  invisible to completion. The type tracker now runs a second pass and
  augments record-typed bindings with `name.member = ...` and
  `name:member = ...` patterns so `t.|` includes both `v` and `meth`.

## 0.3.1 -- 2026-05-01

Edge-case sweep for the type tracker. 63-case harness in
`server/test/edge_cases.js`; all green.

### Fixed

- **CLASS syntax.** The analyzer was assuming `CLASS Name { body }`,
  but CanDo's actual syntax is
  `CLASS Name [EXTENDS Parent] = [(params)] { body }`. The fix parses
  the constructor params, captures `self.x = …` assignments inside
  the constructor as fields/methods, and runs a second pass that
  picks up methods attached after the body via
  `Name.method = FUNCTION(self) { … }` (the canonical CanDo idiom).
- **No `NEW` keyword.** Removed a fictional `NEW` branch from the type
  tracker. Calling a class directly (`Animal("Rex")`) now produces
  an instance of the class, matching the runtime.
- **Class-name shadowing.** `CLASS Foo = { }` was being misread by
  the type tracker as a bare `Foo = { }` assignment, shadowing the
  class with an empty record. The tracker now skips past CLASS
  declarations and refuses to bind class names via bare assignment.
- **Self-typed returns.** Fluent chaining (`f:setText("…"):center()`)
  was collapsing to `Control` -- the class declaring `setText`. The
  manifest schema now supports `"returns": "self"` (alias `"this"`)
  meaning "preserve the receiver's type". 90 chainable methods in the
  forms manifest were migrated.

### Added

- `server/test/edge_cases.js`: a node-based harness covering receiver
  detection, type inference, include/path detection, manifest
  pathologies (cycles, missing types, malformed JSON), real-world
  class idioms from `tests/scripts/metamethods.cdo`, performance
  (1k-line files), CRLF/tabs, empty documents, and 50+ smaller
  shapes. Run with `node server/test/edge_cases.js` after `tsc -b`.

## 0.3.0 -- 2026-05-01

### Added

- **Module manifests (`cando.api.json`).** Each module folder can now
  ship a JSON file describing the value `include(...)` returns -- its
  exported names, their parameter signatures, and the named types
  reachable from them (`forms.TextBox`, `sql.Statement`, …). The
  language server reads it next to any binary or `.cdo` target. Schema
  is documented in `server/src/manifest.ts`. `forms`, `sql`, `sqlite`,
  `ldap`, `window`, and `draw` ship a manifest in-tree.
- **Type tracker.** The server now infers a best-effort type for every
  variable in a buffer:
  - `VAR mod = include("./forms.so")` → the manifest's exports type
  - `VAR f = forms.Form()` → `forms.Form`
  - `VAR b = forms.Button(f); b:` → completion offers every method on
    `Button`, including those inherited from `Control` and the Derma
    aliases (`SetText`, `MoveToFront`, …).
  - Method chains resolve through `returns`: `forms.createTextBox(p).`
    completes `TextBox`.
- **Inheritance.** Manifests support both `extends` (single classical
  parent) and `indexes` (single string or array, mirroring the
  runtime's `__index` prototype-chain mechanism). Both are walked
  during member lookup.
- **`__index` in user code.** Object literals with an `__index: parent`
  field are treated as inheriting the parent's members:

      VAR Animal = { speak: FUNCTION() { } };
      VAR dog    = { __index: Animal, bark: FUNCTION() { } };
      dog.       // suggests bark, speak

- **Manifest-aware completion / hover / signature help.**
  - Completion items expand with parameter snippets and show
    `member(arg: type) -> Returns` in the detail row.
  - Hover on a member shows the resolved owner type and the formatted
    signature pulled from the manifest.
  - Signature help walks the receiver chain so
    `forms.Button(f):setText("|")` highlights the right argument.
- Manifest discovery is tolerant of missing binaries: a manifest is
  found even when `forms.so` / `forms.dll` hasn't been built yet, so
  completion works on a clean checkout.

## 0.2.0 -- 2026-05-01

### Added

- Filesystem path completion inside `include("...")`. Lists workspace
  `.cdo`, `.json`, `.csv`, `.yaml`, `.yml`, `.so`, `.dylib`, and `.dll`
  files, walking up parent directories the same way the runtime resolver
  does. Folder entries re-trigger completion automatically.
- Cross-file member completion. When a variable is bound to
  `include("./mod.cdo")` or `include("./data.json")`, typing
  `name.` / `name:` offers that module's exported keys (object literal
  returned at the top level, or top-level declarations as a fallback).
  Results are cached by mtime.
- Object-literal member completion: `VAR cfg = { host: ..., port: ... };`
  followed by `cfg.` now suggests `host` / `port`.
- Class member completion: instance methods, static members, and fields
  are surfaced under both `Cls.` and `Cls:`.
- Signature help: invoking a function shows its parameter list, with the
  active parameter highlighted as you type commas. Works for in-file
  functions, the `include` / `print` / `type` / `inspect` / `toString`
  globals, and known `array.*` / `string.*` / `math.*` members.
- `include()` registered as a global builtin with hover documentation;
  go-to-definition on an `include(...)`-bound variable now jumps to the
  resolved file.
- Snippets: `include`, `includeso` (cross-platform binary loader), and
  `return`/`export` for module exports.
- Settings: `cando.completion.includePaths`,
  `cando.completion.crossFile`.

### Changed

- The analyzer now records `include(...)` bindings, top-level object
  literal keys, the last top-level `RETURN { ... }` (treated as the
  module's export shape), and class members.
- Function and method completion items now expand into snippet calls
  with each parameter as a tab stop.

## 0.1.0 -- 2026-04-29

Initial release.

### Added

- TextMate grammar (`source.cando`) covering keywords (upper- and
  lower-case forms), operators, numbers, comments, and string variants
  (double-quoted, single-quoted multi-line, backtick template strings
  with `${...}` interpolation).
- Language configuration: bracket pairs, comment toggles, indentation
  rules, region folding markers.
- File-icon contribution for `.cdo` files.
- Snippets for `IF`, `IFE`, `WHILE`, range / `IN` / `OVER` `FOR` loops,
  `FUNCTION`, `CLASS` (with and without `EXTENDS`), `TRY/CATCH/FINALY`,
  pipe operators (`~>`, `~!>`, `~&>`), ternary, `THREAD`, `ASYNC`.
- Language server with completion (keywords, global builtins, std-lib
  namespaces, namespace members, document symbols), hover, go-to-
  definition for in-file symbols, document symbols, and lightweight
  diagnostics (unterminated strings/comments, bracket-balance check).
- Settings: `cando.diagnostics.enable`,
  `cando.completion.includeBuiltins`, `cando.keywordCase`,
  `cando.trace.server`.
