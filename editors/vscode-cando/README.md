# CanDo Language for VS Code

Syntax highlighting, snippets and IntelliSense for the **CanDo** scripting
language ([`.cdo`](../../tests/scripts) files).

## Features

- **Syntax highlighting** for keywords (upper- or lower-case), operators
  (`~>`, `~!>`, `~&>`, `?.`, `?[`, `->`, `<-`, `::`, `=>`, ...), numbers,
  strings (double-quoted, single-quoted multi-line, and backtick template
  strings with `${...}` interpolation), comments (`//`, `/* */`).
- **File-icon association** for `.cdo` files using the project icon.
- **Snippets** for common constructs: `IF`, `IFE`, `WHILE`, `FOR` (range,
  `IN`, `OVER`), `FUNCTION`, `CLASS`, `TRY/CATCH/FINALY`, lambdas (`=>`),
  pipes (`~>`, `~!>`, `~&>`), and more.
- **Language Server** (`vscode-cando` LSP) providing:
  - Completion of keywords, global builtins (`print`, `type`, `toString`,
    `inspect`) and standard-library namespaces (`array`, `string`, `math`,
    `json`, `csv`, `file`, `os`, `datetime`, `crypto`, `http`, `https`,
    `socket`, `secure_socket`, `net`, `thread`, `process`, `object`, `app`).
  - Member completion after `name.` and `name:` for known namespaces.
  - Document symbols and go-to-definition for in-file `FUNCTION`, `CLASS`,
    `VAR`, `CONST`, `GLOBAL`.
  - Hover docs for keywords, builtins and namespaces.
  - Lightweight diagnostics: unterminated strings/comments and bracket
    mismatch.
- **Smart editing**: bracket pairs, auto-closing quotes/backticks, comment
  toggling, indentation rules, region folding (`// region` / `// endregion`).

## Settings

| Setting                              | Default | Description                                                         |
|--------------------------------------|---------|---------------------------------------------------------------------|
| `cando.diagnostics.enable`           | `true`  | Toggle the lightweight syntax diagnostics.                          |
| `cando.completion.includeBuiltins`   | `true`  | Suggest stdlib namespaces and global builtins.                      |
| `cando.keywordCase`                  | `upper` | Case used when inserting keywords (`upper` or `lower`).             |
| `cando.trace.server`                 | `off`   | LSP trace level (`off`, `messages`, `verbose`).                     |

## Installation

### From a packaged `.vsix`

1. Build the extension package (see **Build from source** below) to produce
   `vscode-cando-<version>.vsix`.
2. In VS Code, open the Command Palette
   (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> /
   <kbd>Cmd</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd>) and run
   **Extensions: Install from VSIX...**, selecting the file you just built.
3. Reload VS Code. Open any `.cdo` file -- highlighting, snippets and the
   language server activate automatically.

You can also install from the CLI:

```bash
code --install-extension vscode-cando-0.1.0.vsix
```

### Build from source

Prerequisites: **Node.js 18+** and **npm**.

```bash
cd editors/vscode-cando
npm install
npm run compile          # builds client/out and server/out
npm run package          # produces vscode-cando-<version>.vsix
```

`npm run package` uses `@vscode/vsce`. The first time you run it on a new
machine, vsce may ask for a publisher; the bundled `package.json` already
sets `publisher = "CandoProject"`.

### Run from source (development)

1. Open `editors/vscode-cando/` in VS Code.
2. `npm install && npm run compile` (or `npm run watch` for incremental
   rebuilds).
3. Press <kbd>F5</kbd> to launch the **Extension Development Host**. A new
   VS Code window opens with the extension loaded; open any `.cdo` file in
   it.
4. To debug the language server itself, attach the **Node Attach** debug
   configuration to port `6009` (the server is launched with
   `--inspect=6009` in debug mode).

## Project layout

```
editors/vscode-cando/
  package.json                 -- extension manifest
  language-configuration.json  -- brackets, comments, indent rules
  syntaxes/cando.tmLanguage.json
  snippets/cando.code-snippets
  icons/cando.png              -- copied from /assets
  client/src/extension.ts      -- LSP client (extension host)
  server/src/                  -- LSP server (Node)
    server.ts                  -- LSP entry, request handlers
    lexer.ts                   -- JS port of source/parser/lexer.c
    analyzer.ts                -- builds the per-file symbol table
    builtins.ts                -- keyword + stdlib metadata
```

## Updating the language metadata

Whenever the runtime grows a new keyword, builtin or stdlib member, three
files need to be kept in sync:

| Source of truth                         | Update in extension                          |
|-----------------------------------------|----------------------------------------------|
| `source/parser/lexer.c` (KEYWORDS)      | `syntaxes/cando.tmLanguage.json` + `server/src/builtins.ts` (`KEYWORDS_UPPER`) |
| `source/natives.c` (`cando_native_names`) | `server/src/builtins.ts` (`GLOBAL_BUILTINS`)  |
| `source/lib/*.c` (`libutil_set_method`) | `server/src/builtins.ts` (`NAMESPACES`)       |

A future improvement is to generate `builtins.ts` directly from the C
sources at build time; for now it is a hand-maintained mirror.

## Known limitations

- The LSP's lexer is a simplified port and may differ from the runtime in
  edge cases (unusual numeric literals, exotic escape sequences). It is
  good enough for editor tooling, not for running scripts.
- Diagnostics cover lexical errors and bracket balance only -- there is no
  parser-level checking yet. Run `./cando script.cdo` for the canonical
  errors.
- Cross-file go-to-definition for `INCLUDE`d files is not implemented.
- Smart completion of namespace members (`ns.foo`) only works for the
  built-in namespaces; user-defined object members are not indexed.

## License

This extension ships as part of the CanDo project. See the project root
for the project license.
