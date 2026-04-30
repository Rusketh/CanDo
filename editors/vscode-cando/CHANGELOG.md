# Changelog

All notable changes to the **CanDo Language** VS Code extension are
documented in this file.

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
