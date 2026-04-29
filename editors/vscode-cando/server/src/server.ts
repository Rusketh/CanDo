/*
 * CanDo language server.
 *
 * Provides:
 *   - Diagnostics: unterminated strings/comments, stray characters, naive
 *                  bracket-balance check.
 *   - Completion:  keywords, global builtins, std-lib namespaces,
 *                  namespace member completion after `name.` or `name:`,
 *                  document symbols.
 *   - Hover:       short docs for keywords, builtins and namespace members.
 *   - Document symbols + go-to-definition for in-file functions, classes,
 *                  vars, constants, globals.
 */

import {
    createConnection,
    TextDocuments,
    ProposedFeatures,
    InitializeParams,
    InitializeResult,
    TextDocumentSyncKind,
    CompletionItem,
    CompletionItemKind,
    Diagnostic,
    DiagnosticSeverity,
    Hover,
    MarkupKind,
    Location,
    DocumentSymbol,
    SymbolKind as LspSymbolKind,
    Range as LspRange
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';

import { Lexer, Token, Range as LexRange } from './lexer';
import { analyze, Symbol as CandoSymbol } from './analyzer';
import {
    KEYWORDS_UPPER,
    PIPE_KEYWORD,
    GLOBAL_BUILTINS,
    NAMESPACES,
    namespaceByName
} from './builtins';

interface CandoSettings {
    diagnostics: { enable: boolean };
    completion: { includeBuiltins: boolean };
    keywordCase: 'upper' | 'lower';
}

const DEFAULT_SETTINGS: CandoSettings = {
    diagnostics: { enable: true },
    completion: { includeBuiltins: true },
    keywordCase: 'upper'
};

let globalSettings: CandoSettings = DEFAULT_SETTINGS;

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments<TextDocument>(TextDocument);

interface CachedAnalysis {
    tokens: Token[];
    symbols: CandoSymbol[];
    byName: Map<string, CandoSymbol>;
    version: number;
}
const analysisCache = new Map<string, CachedAnalysis>();

connection.onInitialize((_params: InitializeParams): InitializeResult => {
    return {
        capabilities: {
            textDocumentSync: TextDocumentSyncKind.Incremental,
            completionProvider: {
                resolveProvider: false,
                triggerCharacters: ['.', ':']
            },
            hoverProvider: true,
            definitionProvider: true,
            documentSymbolProvider: true
        }
    };
});

connection.onDidChangeConfiguration(change => {
    const cfg = (change.settings as { cando?: Partial<CandoSettings> } | undefined)?.cando;
    globalSettings = {
        diagnostics: { enable: cfg?.diagnostics?.enable ?? DEFAULT_SETTINGS.diagnostics.enable },
        completion: { includeBuiltins: cfg?.completion?.includeBuiltins ?? DEFAULT_SETTINGS.completion.includeBuiltins },
        keywordCase: cfg?.keywordCase ?? DEFAULT_SETTINGS.keywordCase
    };
    documents.all().forEach(refresh);
});

documents.onDidChangeContent(e => refresh(e.document));
documents.onDidClose(e => analysisCache.delete(e.document.uri));

function refresh(doc: TextDocument): void {
    const tokens = new Lexer(doc.getText()).tokenize();
    const { symbols, byName } = analyze(tokens);
    analysisCache.set(doc.uri, { tokens, symbols, byName, version: doc.version });
    if (globalSettings.diagnostics.enable) {
        connection.sendDiagnostics({ uri: doc.uri, diagnostics: makeDiagnostics(tokens) });
    } else {
        connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
    }
}

/* -- Diagnostics ---------------------------------------------------------- */

function toLsp(r: LexRange): LspRange {
    return {
        start: { line: r.start.line, character: r.start.character },
        end: { line: r.end.line, character: r.end.character }
    };
}

function makeDiagnostics(tokens: Token[]): Diagnostic[] {
    const out: Diagnostic[] = [];
    const stack: { open: string; tok: Token }[] = [];
    const matches: Record<string, string> = { '(': ')', '[': ']', '{': '}' };

    for (const t of tokens) {
        if (t.kind === 'error') {
            out.push({
                severity: DiagnosticSeverity.Error,
                range: toLsp(t.range),
                message: t.message ?? `Unexpected '${t.value}'`,
                source: 'cando'
            });
            continue;
        }
        if (t.kind === 'punct') {
            if (t.value in matches) {
                stack.push({ open: t.value, tok: t });
            } else if (t.value === ')' || t.value === ']' || t.value === '}') {
                const top = stack.pop();
                if (!top || matches[top.open] !== t.value) {
                    out.push({
                        severity: DiagnosticSeverity.Error,
                        range: toLsp(t.range),
                        message: top
                            ? `Mismatched bracket: expected '${matches[top.open]}'`
                            : `Unexpected closing '${t.value}'`,
                        source: 'cando'
                    });
                }
            }
        }
    }
    for (const u of stack) {
        out.push({
            severity: DiagnosticSeverity.Error,
            range: toLsp(u.tok.range),
            message: `Unclosed '${u.open}'`,
            source: 'cando'
        });
    }
    return out;
}

/* -- Completion ----------------------------------------------------------- */

connection.onCompletion(params => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return [];
    const text = doc.getText();
    const offset = doc.offsetAt(params.position);

    /* Look back to detect `name.` or `name:` */
    const before = text.slice(Math.max(0, offset - 64), offset);
    const dotMatch = /([A-Za-z_][A-Za-z0-9_]*)\s*[.:]\s*$/.exec(before);
    if (dotMatch) {
        const ns = namespaceByName(dotMatch[1]);
        if (ns) {
            return ns.members.map<CompletionItem>(m => ({
                label: m,
                kind: CompletionItemKind.Function,
                detail: `${ns.name}.${m}`,
                documentation: { kind: MarkupKind.Markdown, value: `Member of \`${ns.name}\`.` }
            }));
        }
        // Document-symbol member completion not yet implemented; fall through.
    }

    const items: CompletionItem[] = [];
    const kwCase = globalSettings.keywordCase;

    for (const k of KEYWORDS_UPPER) {
        const label = kwCase === 'lower' ? k.name.toLowerCase() : k.name;
        items.push({
            label,
            kind: CompletionItemKind.Keyword,
            detail: k.detail,
            documentation: { kind: MarkupKind.Markdown, value: k.doc }
        });
    }
    items.push({
        label: PIPE_KEYWORD.name,
        kind: CompletionItemKind.Keyword,
        detail: PIPE_KEYWORD.detail,
        documentation: { kind: MarkupKind.Markdown, value: PIPE_KEYWORD.doc }
    });

    if (globalSettings.completion.includeBuiltins) {
        for (const b of GLOBAL_BUILTINS) {
            items.push({
                label: b.name,
                kind: CompletionItemKind.Function,
                detail: b.detail,
                documentation: { kind: MarkupKind.Markdown, value: b.doc }
            });
        }
        for (const ns of NAMESPACES) {
            items.push({
                label: ns.name,
                kind: CompletionItemKind.Module,
                detail: `${ns.name} (std)`,
                documentation: { kind: MarkupKind.Markdown, value: ns.doc }
            });
        }
    }

    const cached = analysisCache.get(params.textDocument.uri);
    if (cached) {
        for (const s of cached.symbols) {
            items.push({
                label: s.name,
                kind: docSymbolKindToCompletion(s.kind),
                detail: s.detail
            });
        }
    }

    return items;
});

function docSymbolKindToCompletion(k: CandoSymbol['kind']): CompletionItemKind {
    switch (k) {
        case 'function':  return CompletionItemKind.Function;
        case 'class':     return CompletionItemKind.Class;
        case 'constant':  return CompletionItemKind.Constant;
        case 'parameter': return CompletionItemKind.Variable;
        default:          return CompletionItemKind.Variable;
    }
}

/* -- Hover ---------------------------------------------------------------- */

connection.onHover((params): Hover | null => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const text = doc.getText();
    const offset = doc.offsetAt(params.position);

    const word = wordAt(text, offset);
    if (!word) return null;

    const kw = KEYWORDS_UPPER.find(k => k.name === word.value || k.name.toLowerCase() === word.value);
    if (kw) {
        return md(`**${kw.name}** -- ${kw.detail}\n\n${kw.doc}`);
    }
    if (word.value === 'pipe') {
        return md(`**pipe** -- ${PIPE_KEYWORD.doc}`);
    }
    const bi = GLOBAL_BUILTINS.find(b => b.name === word.value);
    if (bi) {
        return md(`**${bi.name}** -- ${bi.detail}\n\n${bi.doc}`);
    }
    const ns = namespaceByName(word.value);
    if (ns) {
        return md(`**${ns.name}** (standard library)\n\n${ns.doc}\n\nMembers: ${ns.members.map(m => `\`${m}\``).join(', ') || '_(none registered)_'}`);
    }

    /* Namespace member: `ns.name` or `ns:name` */
    const before = text.slice(Math.max(0, word.start - 64), word.start);
    const m = /([A-Za-z_][A-Za-z0-9_]*)\s*[.:]\s*$/.exec(before);
    if (m) {
        const owner = namespaceByName(m[1]);
        if (owner && owner.members.includes(word.value)) {
            return md(`**${owner.name}.${word.value}**\n\nMember of the \`${owner.name}\` standard library.`);
        }
    }

    const cached = analysisCache.get(params.textDocument.uri);
    const sym = cached?.byName.get(word.value);
    if (sym) {
        return md(`**${sym.name}** -- ${sym.detail ?? sym.kind}`);
    }

    return null;
});

function md(text: string): Hover {
    return { contents: { kind: MarkupKind.Markdown, value: text } };
}

/* -- Definition + symbols ------------------------------------------------- */

connection.onDefinition(params => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const word = wordAt(doc.getText(), doc.offsetAt(params.position));
    if (!word) return null;
    const cached = analysisCache.get(params.textDocument.uri);
    const sym = cached?.byName.get(word.value);
    if (!sym) return null;
    const loc: Location = { uri: params.textDocument.uri, range: toLsp(sym.range) };
    return loc;
});

connection.onDocumentSymbol(params => {
    const cached = analysisCache.get(params.textDocument.uri);
    if (!cached) return [];
    return cached.symbols.map(toDocumentSymbol);
});

function toDocumentSymbol(s: CandoSymbol): DocumentSymbol {
    return {
        name: s.name,
        detail: s.detail,
        kind: mapKind(s.kind),
        range: toLsp(s.range),
        selectionRange: toLsp(s.selectionRange),
        children: s.children?.map(toDocumentSymbol)
    };
}

function mapKind(k: CandoSymbol['kind']): LspSymbolKind {
    switch (k) {
        case 'function':  return LspSymbolKind.Function;
        case 'class':     return LspSymbolKind.Class;
        case 'constant':  return LspSymbolKind.Constant;
        case 'parameter': return LspSymbolKind.Variable;
        default:          return LspSymbolKind.Variable;
    }
}

/* -- Helpers -------------------------------------------------------------- */

function wordAt(text: string, offset: number): { value: string; start: number; end: number } | null {
    const isIdentChar = (c: string): boolean => /[A-Za-z0-9_]/.test(c);
    let s = offset;
    while (s > 0 && isIdentChar(text[s - 1])) s--;
    let e = offset;
    while (e < text.length && isIdentChar(text[e])) e++;
    if (s === e) return null;
    return { value: text.slice(s, e), start: s, end: e };
}

documents.listen(connection);
connection.listen();
