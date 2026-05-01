/*
 * CanDo language server.
 *
 * Provides:
 *   - Diagnostics: unterminated strings/comments, stray characters, naive
 *                  bracket-balance check.
 *   - Completion:  keywords, global builtins, std-lib namespaces,
 *                  namespace member completion after `name.` or `name:`,
 *                  document symbols, cross-file member completion for
 *                  `include(...)` bindings, object-literal members,
 *                  filesystem path completion inside `include("...")`.
 *   - Hover:       short docs for keywords, builtins, namespace members,
 *                  document symbols, and resolved include paths.
 *   - Signature help: parameter list for the function call surrounding the
 *                  cursor (in-file functions and namespace methods).
 *   - Document symbols + go-to-definition for in-file functions, classes,
 *                  vars, constants, globals; goto for `include(...)` jumps
 *                  to the included file.
 */

import * as path from 'path';
import {
    createConnection,
    TextDocuments,
    ProposedFeatures,
    InitializeParams,
    InitializeResult,
    TextDocumentSyncKind,
    CompletionItem,
    CompletionItemKind,
    InsertTextFormat,
    Diagnostic,
    DiagnosticSeverity,
    Hover,
    MarkupKind,
    Location,
    DocumentSymbol,
    SymbolKind as LspSymbolKind,
    Range as LspRange,
    SignatureHelp,
    SignatureInformation,
    ParameterInformation
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';

import { Lexer, Token, Range as LexRange } from './lexer';
import { analyze, Symbol as CandoSymbol, IncludeBinding } from './analyzer';
import {
    KEYWORDS_UPPER,
    PIPE_KEYWORD,
    GLOBAL_BUILTINS,
    NAMESPACES,
    NamespaceInfo,
    namespaceByName,
    namespaceMemberDetail,
    builtinByName
} from './builtins';
import {
    detectIncludeString,
    completeIncludePath,
    resolveIncludePath
} from './paths';
import { getIncludeExports } from './crossfile';
import {
    buildTypeEnv,
    inferReceiverAt,
    listMembers,
    describeTypeForHover,
    formatMemberDetail,
    TypeEnv,
    TypeRef
} from './types';
import { MemberSpec } from './manifest';

interface CandoSettings {
    diagnostics: { enable: boolean };
    completion: {
        includeBuiltins: boolean;
        includePaths: boolean;
        crossFile: boolean;
    };
    keywordCase: 'upper' | 'lower';
}

const DEFAULT_SETTINGS: CandoSettings = {
    diagnostics: { enable: true },
    completion: { includeBuiltins: true, includePaths: true, crossFile: true },
    keywordCase: 'upper'
};

let globalSettings: CandoSettings = DEFAULT_SETTINGS;
let workspaceRoots: string[] = [];

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments<TextDocument>(TextDocument);

interface CachedAnalysis {
    tokens: Token[];
    symbols: CandoSymbol[];
    byName: Map<string, CandoSymbol>;
    includes: Map<string, IncludeBinding>;
    objectLiterals: Map<string, string[]>;
    moduleExports: string[];
    typeEnv: TypeEnv;
    version: number;
}
const analysisCache = new Map<string, CachedAnalysis>();

connection.onInitialize((params: InitializeParams): InitializeResult => {
    workspaceRoots = (params.workspaceFolders ?? [])
        .map(f => uriToFsPath(f.uri))
        .filter((p): p is string => !!p);
    if (!workspaceRoots.length && params.rootUri) {
        const r = uriToFsPath(params.rootUri);
        if (r) workspaceRoots.push(r);
    }
    return {
        capabilities: {
            textDocumentSync: TextDocumentSyncKind.Incremental,
            completionProvider: {
                resolveProvider: false,
                /* `(`, `"`, `'`, `/` cover include() string completion;
                 * `.` and `:` drive member access; space allows keyword
                 * follow-ups (e.g. after `EXTENDS `). */
                triggerCharacters: ['.', ':', '(', '"', '\'', '/']
            },
            hoverProvider: true,
            definitionProvider: true,
            documentSymbolProvider: true,
            signatureHelpProvider: {
                triggerCharacters: ['(', ',']
            }
        }
    };
});

connection.onDidChangeConfiguration(change => {
    const cfg = (change.settings as { cando?: Partial<CandoSettings> } | undefined)?.cando;
    globalSettings = {
        diagnostics: { enable: cfg?.diagnostics?.enable ?? DEFAULT_SETTINGS.diagnostics.enable },
        completion: {
            includeBuiltins: cfg?.completion?.includeBuiltins ?? DEFAULT_SETTINGS.completion.includeBuiltins,
            includePaths:    cfg?.completion?.includePaths    ?? DEFAULT_SETTINGS.completion.includePaths,
            crossFile:       cfg?.completion?.crossFile       ?? DEFAULT_SETTINGS.completion.crossFile
        },
        keywordCase: cfg?.keywordCase ?? DEFAULT_SETTINGS.keywordCase
    };
    documents.all().forEach(refresh);
});

documents.onDidChangeContent(e => refresh(e.document));
documents.onDidClose(e => analysisCache.delete(e.document.uri));

function refresh(doc: TextDocument): void {
    const tokens = new Lexer(doc.getText()).tokenize();
    const result = analyze(tokens);
    const typeEnv = buildTypeEnv(tokens, result.symbols, doc.uri, workspaceRoots);
    analysisCache.set(doc.uri, {
        tokens,
        symbols: result.symbols,
        byName: result.byName,
        includes: result.includes,
        objectLiterals: result.objectLiterals,
        moduleExports: result.moduleExports,
        typeEnv,
        version: doc.version
    });
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

    /* 1. Path completion inside `include("...")`. */
    if (globalSettings.completion.includePaths) {
        const ictx = detectIncludeString(text, offset);
        if (ictx) {
            return completeIncludePath(ictx, params.textDocument.uri, workspaceRoots);
        }
    }

    /* 2. Member access after `.` or `:`. We use the type tracker to walk
     * back through the full receiver expression, so chains like
     * `forms.createTextBox(parent).` resolve to the TextBox type. */
    const dotIndex = receiverDotIndex(text, offset);
    if (dotIndex !== null) {
        const cached = analysisCache.get(params.textDocument.uri);
        if (!cached) return [];

        const tokenIdx = tokenIndexAt(doc, cached.tokens, dotIndex);
        if (tokenIdx === null) return [];

        /* Try the type tracker first -- handles manifest types, in-file
         * classes, anonymous records, and chained member calls. */
        const ref = inferReceiverAt(cached.tokens, tokenIdx, cached.typeEnv, workspaceRoots);
        const members = listMembers(ref, cached.typeEnv);
        if (members.size > 0) return memberMapToCompletions(members);

        /* Fallback path: handle the bare-name shapes the analyzer indexed
         * directly, even if the type tracker bailed. */
        const name = bareReceiverName(text, dotIndex);
        if (name) {
            const ns = namespaceByName(name);
            if (ns) return namespaceMemberCompletions(ns);

            /* Cross-file include with no manifest -> use the harvested
             * exports list. */
            const inc = cached.includes.get(name);
            if (inc && globalSettings.completion.crossFile) {
                const ex = getIncludeExports(inc.path, params.textDocument.uri, workspaceRoots);
                if (ex && ex.members.length) {
                    return ex.members.map<CompletionItem>(m => ({
                        label: m,
                        kind: CompletionItemKind.Field,
                        detail: `${name} (from ${path.basename(ex.path)})`
                    }));
                }
            }
        }

        return [];
    }

    /* 3. General completion: keywords + builtins + namespaces + symbols. */
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
            const item: CompletionItem = {
                label: b.name,
                kind: CompletionItemKind.Function,
                detail: b.detail,
                documentation: { kind: MarkupKind.Markdown, value: b.doc }
            };
            if (b.snippet) {
                item.insertText = b.snippet;
                item.insertTextFormat = InsertTextFormat.Snippet;
            }
            items.push(item);
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
        const seen = new Set<string>();
        for (const s of cached.symbols) {
            const item: CompletionItem = {
                label: s.name,
                kind: docSymbolKindToCompletion(s.kind),
                detail: s.detail
            };
            if (s.parameters !== undefined && s.parameters.length === 0) {
                item.insertText = `${s.name}()$0`;
                item.insertTextFormat = InsertTextFormat.Snippet;
            } else if (s.parameters !== undefined) {
                const placeholders = s.parameters
                    .map((p, i) => `\${${i + 1}:${p}}`)
                    .join(', ');
                item.insertText = `${s.name}(${placeholders})$0`;
                item.insertTextFormat = InsertTextFormat.Snippet;
            }
            items.push(item);
            seen.add(s.name);
        }
        /* Function-body locals -- the analyzer only records top-level
         * symbols, but the type tracker walks every VAR / CONST / GLOBAL
         * declaration in the file. Surface those here so identifier
         * completion works inside `FUNCTION` bodies and class
         * constructors. */
        for (const [name, ref] of cached.typeEnv.bindings) {
            if (seen.has(name)) continue;
            items.push({
                label: name,
                kind: CompletionItemKind.Variable,
                detail: describeTypeForHover(ref)
            });
            seen.add(name);
        }
    }

    return items;
});

function namespaceMemberCompletions(ns: NamespaceInfo): CompletionItem[] {
    return ns.members.map<CompletionItem>(m => ({
        label: m,
        kind: CompletionItemKind.Function,
        detail: namespaceMemberDetail(ns, m) ?? `${ns.name}.${m}`,
        documentation: { kind: MarkupKind.Markdown, value: `Member of \`${ns.name}\`.` }
    }));
}

function memberMapToCompletions(members: Map<string, MemberSpec>): CompletionItem[] {
    const out: CompletionItem[] = [];
    for (const [name, spec] of members) {
        const item: CompletionItem = {
            label: name,
            kind: memberKindToCompletion(spec),
            detail: formatMemberDetail(spec, name)
        };
        if (spec.doc) {
            item.documentation = { kind: MarkupKind.Markdown, value: spec.doc };
        }
        if (spec.kind === 'function' && spec.params) {
            const placeholders = spec.params
                .filter(p => !p.optional && !p.rest)
                .map((p, i) => `\${${i + 1}:${p.name}}`)
                .join(', ');
            item.insertText = `${name}(${placeholders})$0`;
            item.insertTextFormat = InsertTextFormat.Snippet;
        }
        out.push(item);
    }
    return out;
}

function memberKindToCompletion(spec: MemberSpec): CompletionItemKind {
    switch (spec.kind) {
        case 'function': return CompletionItemKind.Method;
        case 'event':    return CompletionItemKind.Event;
        case 'constant': return CompletionItemKind.Constant;
        case 'value':
        default:         return CompletionItemKind.Field;
    }
}

/* ----- Receiver-detection helpers --------------------------------------- */

/**
 * Return the character offset of the trailing `.` or `:` before the cursor
 * if the user is typing a member-access expression, otherwise null.
 * Skips trailing identifier characters so `forms.cre|ate` still resolves to
 * the dot at `forms.`.
 */
function receiverDotIndex(text: string, offset: number): number | null {
    let i = offset;
    while (i > 0 && /[A-Za-z0-9_]/.test(text[i - 1])) i--;
    if (i <= 0) return null;
    /* Allow whitespace between the dot and the partial member name. */
    let j = i;
    while (j > 0 && (text[j - 1] === ' ' || text[j - 1] === '\t')) j--;
    if (j <= 0) return null;
    const c = text[j - 1];
    if (c !== '.' && c !== ':') return null;
    return j - 1;
}

/** If the receiver is a single bare identifier, return it. */
function bareReceiverName(text: string, dotIndex: number): string | null {
    let i = dotIndex;
    while (i > 0 && (text[i - 1] === ' ' || text[i - 1] === '\t')) i--;
    const end = i;
    while (i > 0 && /[A-Za-z0-9_]/.test(text[i - 1])) i--;
    if (i === end) return null;
    return text.slice(i, end);
}

/**
 * Find the index of the first token whose start offset is greater than or
 * equal to `offset`. Returns null when no such token exists. Used to
 * locate the receiver expression preceding a `.` or `:` in the buffer.
 */
function tokenIndexAt(doc: TextDocument, tokens: Token[], offset: number): number | null {
    for (let i = 0; i < tokens.length; i++) {
        const startOffset = doc.offsetAt(tokens[i].range.start);
        if (startOffset >= offset) return i;
    }
    return null;
}

function docSymbolKindToCompletion(k: CandoSymbol['kind']): CompletionItemKind {
    switch (k) {
        case 'function':  return CompletionItemKind.Function;
        case 'method':    return CompletionItemKind.Method;
        case 'class':     return CompletionItemKind.Class;
        case 'constant':  return CompletionItemKind.Constant;
        case 'parameter': return CompletionItemKind.Variable;
        case 'field':     return CompletionItemKind.Field;
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
    const bi = builtinByName(word.value);
    if (bi) {
        return md(`**${bi.name}** -- ${bi.detail}\n\n${bi.doc}`);
    }
    const ns = namespaceByName(word.value);
    if (ns) {
        return md(`**${ns.name}** (standard library)\n\n${ns.doc}\n\nMembers: ${ns.members.map(m => `\`${m}\``).join(', ') || '_(none registered)_'}`);
    }

    /* Member access hover: walk back through the receiver chain via the
     * type tracker, then look the word up on the resolved type. */
    const before = text.slice(Math.max(0, word.start - 64), word.start);
    const dotMatch = /[.:]\s*$/.test(before);
    if (dotMatch) {
        const cached = analysisCache.get(params.textDocument.uri);
        if (cached) {
            const dotIdx = receiverDotIndex(text, word.start);
            if (dotIdx !== null) {
                const tokIdx = tokenIndexAt(doc, cached.tokens, dotIdx);
                if (tokIdx !== null) {
                    const ref = inferReceiverAt(cached.tokens, tokIdx, cached.typeEnv, workspaceRoots);
                    const member = listMembers(ref, cached.typeEnv).get(word.value);
                    if (member) {
                        const ownerLabel = describeTypeForHover(ref);
                        const detail = formatMemberDetail(member, word.value);
                        const lines = [`**${ownerLabel}.${word.value}**`, '', '`' + detail + '`'];
                        if (member.doc) { lines.push('', member.doc); }
                        return md(lines.join('\n'));
                    }
                }
            }
        }
    }

    const cached = analysisCache.get(params.textDocument.uri);
    const sym = cached?.byName.get(word.value);
    if (sym) {
        const lines = [`**${sym.name}** -- ${sym.detail ?? sym.kind}`];
        const inc = cached?.includes.get(sym.name);
        if (inc) {
            const resolved = resolveIncludePath(inc.path, params.textDocument.uri, workspaceRoots);
            lines.push('');
            lines.push(resolved
                ? `Module: \`${inc.path}\` -> \`${resolved}\``
                : `Module: \`${inc.path}\` *(not found)*`);
        }
        return md(lines.join('\n'));
    }

    return null;
});

function md(text: string): Hover {
    return { contents: { kind: MarkupKind.Markdown, value: text } };
}

/* -- Signature help ------------------------------------------------------- */

connection.onSignatureHelp((params): SignatureHelp | null => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const text = doc.getText();
    const offset = doc.offsetAt(params.position);

    const ctx = findEnclosingCall(text, offset);
    if (!ctx) return null;

    /* Chained call: walk back from the function name through the receiver
     * expression so `forms.createTextBox(parent).setText("|")` finds the
     * TextBox.setText signature. */
    const cached = analysisCache.get(params.textDocument.uri);
    const headMatch = /([A-Za-z_][A-Za-z0-9_]*)$/.exec(text.slice(0, ctx.callerEnd));
    if (cached && headMatch) {
        const fnName = headMatch[1];
        const fnStart = ctx.callerEnd - fnName.length;
        const dotIdx = receiverDotIndex(text, fnStart);
        if (dotIdx !== null) {
            const tokIdx = tokenIndexAt(doc, cached.tokens, dotIdx);
            if (tokIdx !== null) {
                const ref = inferReceiverAt(cached.tokens, tokIdx, cached.typeEnv, workspaceRoots);
                const spec = listMembers(ref, cached.typeEnv).get(fnName);
                if (spec) {
                    const sig = formatMemberDetail(spec, fnName);
                    return makeSignature(sig, ctx.activeParameter);
                }
            }
            /* Fall through to the std-library namespace heuristic. */
            const dotName = /([A-Za-z_][A-Za-z0-9_]*)\s*[.:]\s*$/.exec(text.slice(0, dotIdx + 1));
            if (dotName) {
                const ns = namespaceByName(dotName[1]);
                if (ns) {
                    const sig = namespaceMemberDetail(ns, fnName) ?? `${ns.name}.${fnName}(...)`;
                    return makeSignature(sig, ctx.activeParameter);
                }
            }
        }
    }

    /* Bare `name(` */
    const bareMatch = /([A-Za-z_][A-Za-z0-9_]*)$/.exec(text.slice(0, ctx.callerEnd));
    if (!bareMatch) return null;
    const name = bareMatch[1];

    const sym = cached?.byName.get(name);
    if (sym && sym.parameters !== undefined) {
        const sig = `${name}(${sym.parameters.join(', ')})`;
        return makeSignature(sig, ctx.activeParameter);
    }

    const bi = builtinByName(name);
    if (bi) return makeSignature(bi.detail, ctx.activeParameter);

    return null;
});

interface CallContext {
    /* Index of the character immediately before the opening `(`. */
    callerEnd: number;
    /* Zero-based index of the parameter the cursor is currently on. */
    activeParameter: number;
}

function findEnclosingCall(text: string, offset: number): CallContext | null {
    let depth = 0;
    let commas = 0;
    for (let i = offset - 1; i >= 0; i--) {
        const c = text[i];
        if (c === ')') depth++;
        else if (c === '(') {
            if (depth === 0) return { callerEnd: i, activeParameter: commas };
            depth--;
        } else if (c === ',' && depth === 0) {
            commas++;
        } else if (c === '"' || c === '\'') {
            /* Skip a balanced string by walking backwards to its match. */
            const quote = c;
            i--;
            while (i >= 0 && text[i] !== quote) i--;
        }
    }
    return null;
}

function makeSignature(label: string, activeParameter: number): SignatureHelp {
    const params: ParameterInformation[] = [];
    const open = label.indexOf('(');
    const close = label.lastIndexOf(')');
    if (open >= 0 && close > open) {
        const inner = label.slice(open + 1, close);
        if (inner.trim().length) {
            for (const part of inner.split(',')) params.push({ label: part.trim() });
        }
    }
    const sig: SignatureInformation = { label, parameters: params };
    return {
        signatures: [sig],
        activeSignature: 0,
        activeParameter: Math.min(activeParameter, Math.max(0, params.length - 1))
    };
}

/* -- Definition + symbols ------------------------------------------------- */

connection.onDefinition(params => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const word = wordAt(doc.getText(), doc.offsetAt(params.position));
    if (!word) return null;
    const cached = analysisCache.get(params.textDocument.uri);

    /* Jump to the included file when the user invokes goto on a name bound
     * to `include(...)`. */
    const inc = cached?.includes.get(word.value);
    if (inc) {
        const resolved = resolveIncludePath(inc.path, params.textDocument.uri, workspaceRoots);
        if (resolved) {
            return {
                uri: fsPathToUri(resolved),
                range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }
            } as Location;
        }
    }

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
        case 'method':    return LspSymbolKind.Method;
        case 'class':     return LspSymbolKind.Class;
        case 'constant':  return LspSymbolKind.Constant;
        case 'parameter': return LspSymbolKind.Variable;
        case 'field':     return LspSymbolKind.Field;
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

function uriToFsPath(uri: string): string | null {
    if (!uri.startsWith('file://')) return null;
    let p = uri.slice('file://'.length);
    const slash = p.indexOf('/');
    if (slash > 0) p = p.slice(slash);
    try { p = decodeURIComponent(p); } catch { /* keep raw */ }
    if (/^\/[A-Za-z]:/.test(p)) p = p.slice(1);
    return p;
}

function fsPathToUri(p: string): string {
    let normalized = p.replace(/\\/g, '/');
    if (!normalized.startsWith('/')) normalized = '/' + normalized;
    return 'file://' + encodeURI(normalized).replace(/[#?]/g, c => encodeURIComponent(c));
}

documents.listen(connection);
connection.listen();
