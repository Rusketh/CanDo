# Changelog

All notable changes to the **CanDo Language** VS Code extension are
documented in this file.

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
