/*
 * CanDo language server.
 *
 * Thin LSP shell over the analyze pipeline (analyze.ts). All language
 * understanding lives in the parser / resolver / inferer; this file maps
 * LSP requests to the analyzed document and renders the result.
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

import { Range as LexRange, Token } from './lexer';
import { analyzeDocument, AnalyzedDocument, clearDocument, resolveIncludePath } from './analyze';
import {
    KEYWORDS_UPPER, PIPE_KEYWORD, GLOBAL_BUILTINS, NAMESPACES,
    namespaceByName, namespaceMemberDetail, builtinByName
} from './builtins';
import { detectIncludeString, completeIncludePath } from './paths';
import {
    TypeRef, FunctionType, MemberType,
    enumerateMembers, renderType
} from './typesys';
import { Node, Call, rangeContains, nodeAt, children as astChildren } from './ast';
import { Scope, Binding, scopeAt } from './scope';

interface CandoSettings {
    diagnostics: { enable: boolean; semantic: boolean };
    completion: { includeBuiltins: boolean; includePaths: boolean; crossFile: boolean };
    keywordCase: 'upper' | 'lower';
}

const DEFAULT_SETTINGS: CandoSettings = {
    diagnostics: { enable: true, semantic: true },
    completion: { includeBuiltins: true, includePaths: true, crossFile: true },
    keywordCase: 'upper'
};

let globalSettings: CandoSettings = DEFAULT_SETTINGS;
let workspaceRoots: string[] = [];

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments<TextDocument>(TextDocument);

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
                triggerCharacters: ['.', ':', '(', '"', '\'', '/']
            },
            hoverProvider: true,
            definitionProvider: true,
            documentSymbolProvider: true,
            signatureHelpProvider: { triggerCharacters: ['(', ','] }
        }
    };
});

connection.onDidChangeConfiguration(change => {
    const cfg = (change.settings as { cando?: Partial<CandoSettings> } | undefined)?.cando;
    globalSettings = {
        diagnostics: {
            enable:   cfg?.diagnostics?.enable   ?? DEFAULT_SETTINGS.diagnostics.enable,
            semantic: (cfg?.diagnostics as { semantic?: boolean } | undefined)?.semantic
                       ?? DEFAULT_SETTINGS.diagnostics.semantic
        },
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
documents.onDidClose(e => clearDocument(e.document.uri));

function refresh(doc: TextDocument): void {
    const analyzed = analyzeDocument(doc.uri, doc.getText(), doc.version, workspaceRoots);
    if (globalSettings.diagnostics.enable) {
        connection.sendDiagnostics({ uri: doc.uri, diagnostics: makeDiagnostics(analyzed) });
    } else {
        connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
    }
}

function getAnalyzed(uri: string): AnalyzedDocument | null {
    const doc = documents.get(uri);
    if (!doc) return null;
    return analyzeDocument(doc.uri, doc.getText(), doc.version, workspaceRoots);
}

/* ----------------------------------------------------------------------- */
/* Diagnostics                                                             */
/* ----------------------------------------------------------------------- */

function toLsp(r: LexRange): LspRange {
    return { start: r.start, end: r.end };
}

function makeDiagnostics(a: AnalyzedDocument): Diagnostic[] {
    const out: Diagnostic[] = [];
    /* Lex errors. */
    for (const t of a.tokens) {
        if (t.kind === 'error') {
            out.push({
                severity: DiagnosticSeverity.Error,
                range: toLsp(t.range),
                message: t.message ?? `Unexpected '${t.value}'`,
                source: 'cando'
            });
        }
    }
    /* Parse errors. */
    for (const e of a.parseErrors) {
        out.push({
            severity: DiagnosticSeverity.Error,
            range: toLsp(e.range),
            message: e.message,
            source: 'cando'
        });
    }
    /* Semantic diagnostics (opt-out via settings). */
    if (globalSettings.diagnostics.semantic) {
        for (const d of semanticDiagnostics(a)) out.push(d);
    }
    return out;
}

function semanticDiagnostics(a: AnalyzedDocument): Diagnostic[] {
    const out: Diagnostic[] = [];
    /* assign-to-const: scan AssignStmt targets where the binding is CONST. */
    walkAst(a.program, (n) => {
        if (n.kind === 'AssignStmt') {
            for (const t of n.targets) {
                if (t.kind === 'Ident') {
                    const scope = a.resolved.scopeOf.get(t);
                    const b = scope?.lookup(t.name);
                    if (b && b.kind === 'const' && b.decl !== n) {
                        out.push({
                            severity: DiagnosticSeverity.Error,
                            range: toLsp(t.range),
                            message: `Cannot assign to CONST binding '${t.name}'`,
                            source: 'cando'
                        });
                    }
                }
            }
        }
        if (n.kind === 'Call') {
            const calleeT = a.inferred.nodeTypes.get(n.callee);
            if (calleeT) {
                if (calleeT.kind === 'function') {
                    const required = calleeT.params.filter(p => !p.optional && !p.rest).length;
                    const hasRest = calleeT.params.some(p => p.rest);
                    const given = n.args.filter(arg => !arg.spread).length;
                    const hasSpread = n.args.some(arg => arg.spread);
                    if (!hasSpread && given < required) {
                        out.push({
                            severity: DiagnosticSeverity.Warning,
                            range: toLsp(n.range),
                            message: `Expected at least ${required} argument${required === 1 ? '' : 's'}, got ${given}`,
                            source: 'cando'
                        });
                    }
                    if (!hasSpread && !hasRest && given > calleeT.params.length) {
                        out.push({
                            severity: DiagnosticSeverity.Warning,
                            range: toLsp(n.range),
                            message: `Expected at most ${calleeT.params.length} argument${calleeT.params.length === 1 ? '' : 's'}, got ${given}`,
                            source: 'cando'
                        });
                    }
                } else if (calleeT.kind !== 'class' && !isCallableType(calleeT)) {
                    out.push({
                        severity: DiagnosticSeverity.Information,
                        range: toLsp(n.range),
                        message: `Calling a non-function value (${renderType(calleeT)})`,
                        source: 'cando'
                    });
                }
            }
        }
        if (n.kind === 'Ident') {
            const scope = a.resolved.scopeOf.get(n);
            const b = scope?.lookup(n.name);
            if (!b && !isBuiltinIdent(n.name) && n.name !== 'self' && n.name !== 'pipe') {
                out.push({
                    severity: DiagnosticSeverity.Hint,
                    range: toLsp(n.range),
                    message: `'${n.name}' is not declared in this file (will be looked up as a global)`,
                    source: 'cando'
                });
            }
        }
    });
    return out;
}

function isBuiltinIdent(name: string): boolean {
    return !!(namespaceByName(name) || builtinByName(name));
}

function isCallableType(t: TypeRef): boolean {
    if (t.kind === 'function' || t.kind === 'class' || t.kind === 'namespace') return true;
    if (t.kind === 'prim' && (t.name === 'any' || t.name === 'unknown')) return true;
    if (t.kind === 'union') return t.variants.every(isCallableType);
    if (t.kind === 'optional') return isCallableType(t.inner);
    /* Objects may have __call -- can't tell without walking. Allow. */
    if (t.kind === 'object' || t.kind === 'manifest-exports' || t.kind === 'manifest-type') return true;
    return false;
}

/* ----------------------------------------------------------------------- */
/* Completion                                                              */
/* ----------------------------------------------------------------------- */

connection.onCompletion(params => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return [];
    const text = doc.getText();
    const offset = doc.offsetAt(params.position);

    /* 1. include() path completion. */
    if (globalSettings.completion.includePaths) {
        const ictx = detectIncludeString(text, offset);
        if (ictx) return completeIncludePath(ictx, params.textDocument.uri, workspaceRoots);
    }

    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];

    /* 2. Member access after `.` or `:` or `?.`. */
    const dotInfo = receiverDotAt(text, offset);
    if (dotInfo) {
        const tokIdx = tokenIndexBefore(a.tokens, dotInfo.dotOffset, doc);
        if (tokIdx === null) return [];
        const receiverRange = receiverRangeFromTokens(a.tokens, tokIdx);
        if (!receiverRange) return [];
        const node = nodeContainingRange(a.program, receiverRange);
        if (!node) return [];
        const t = a.inferred.nodeTypes.get(node) ?? null;
        if (!t) return [];
        const members = enumerateAt(t);
        return memberMapToCompletions(members);
    }

    /* 3. Bare identifier completion. */
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

    /* Locals + globals visible at the cursor scope. */
    const scope = scopeContaining(a, params.position.line, params.position.character);
    const seen = new Set<string>();
    for (let s: Scope | null = scope; s; s = s.parent) {
        for (const [name, b] of s.bindings) {
            if (seen.has(name)) continue;
            seen.add(name);
            items.push({
                label: name,
                kind: bindingKindToCompletion(b),
                detail: renderType(b.type),
                documentation: b.kind === 'function'
                    ? { kind: MarkupKind.Markdown, value: `Defined at line ${b.declRange.start.line + 1}.` }
                    : undefined
            });
        }
    }
    return items;
});

function bindingKindToCompletion(b: Binding): CompletionItemKind {
    switch (b.kind) {
        case 'function': return CompletionItemKind.Function;
        case 'class':    return CompletionItemKind.Class;
        case 'const':    return CompletionItemKind.Constant;
        case 'param':    return CompletionItemKind.Variable;
        case 'self':     return CompletionItemKind.Variable;
        case 'pipe':     return CompletionItemKind.Variable;
        case 'catch':    return CompletionItemKind.Variable;
        case 'forvar':   return CompletionItemKind.Variable;
        default:         return CompletionItemKind.Variable;
    }
}

function memberMapToCompletions(members: Map<string, MemberType>): CompletionItem[] {
    const out: CompletionItem[] = [];
    for (const [name, m] of members) {
        const item: CompletionItem = {
            label: name,
            kind: memberCompletionKind(m),
            detail: memberDetail(m, name)
        };
        if (m.doc) item.documentation = { kind: MarkupKind.Markdown, value: m.doc };
        if (m.type.kind === 'function' && m.type.params.length > 0) {
            const placeholders = m.type.params
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

function memberCompletionKind(m: MemberType): CompletionItemKind {
    if (m.isEvent) return CompletionItemKind.Event;
    if (m.readOnly) return CompletionItemKind.Constant;
    if (m.type.kind === 'function') return CompletionItemKind.Method;
    return CompletionItemKind.Field;
}

function memberDetail(m: MemberType, name: string): string {
    if (m.type.kind === 'function') {
        const ft = m.type;
        const ps = ft.params.map(p => {
            const sigil = p.rest ? '...' : '';
            const opt = p.optional ? '?' : '';
            return `${sigil}${p.name}${opt}: ${renderType(p.type)}`;
        }).join(', ');
        const ret = ft.returnsSelf
            ? 'self'
            : ft.returns.length === 1 ? renderType(ft.returns[0])
            : ft.returns.map(r => renderType(r)).join(', ');
        return `${name}(${ps}) -> ${ret}`;
    }
    return `${name}: ${renderType(m.type)}`;
}

function enumerateAt(t: TypeRef): Map<string, MemberType> {
    return enumerateMembers(t);
}

/* ----------------------------------------------------------------------- */
/* Hover                                                                   */
/* ----------------------------------------------------------------------- */

connection.onHover((params): Hover | null => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;

    const word = wordAt(doc.getText(), doc.offsetAt(params.position));
    if (!word) return null;

    /* Keyword hover. */
    const kw = KEYWORDS_UPPER.find(k => k.name === word.value || k.name.toLowerCase() === word.value);
    if (kw) return md(`**${kw.name}** -- ${kw.detail}\n\n${kw.doc}`);
    if (word.value === 'pipe') return md(`**pipe** -- ${PIPE_KEYWORD.doc}`);

    /* Find the AST node at the cursor; use its inferred type. */
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (node) {
        if (node.kind === 'Member') {
            const recvT = a.inferred.nodeTypes.get(node.object);
            if (recvT) {
                const members = enumerateMembers(recvT);
                const m = members.get(node.property);
                if (m) {
                    const owner = renderType(recvT);
                    const detail = memberDetail(m, node.property);
                    const lines = [`**${owner}.${node.property}**`, '', '`' + detail + '`'];
                    if (m.doc) { lines.push('', m.doc); }
                    return md(lines.join('\n'));
                }
            }
        }
        if (node.kind === 'Ident') {
            const scope = a.resolved.scopeOf.get(node);
            const b = scope?.lookup(node.name);
            if (b) {
                const t = a.inferred.bindingTypes.get(b) ?? b.type;
                const lines = [`**${b.name}** -- \`${renderType(t)}\``];
                if (b.captured) lines.push('', '_(captured upvalue)_');
                return md(lines.join('\n'));
            }
            const ns = namespaceByName(node.name);
            if (ns) {
                return md(`**${ns.name}** (standard library)\n\n${ns.doc}\n\nMembers: ${ns.members.map(m => `\`${m}\``).join(', ') || '_(none)_'}`);
            }
            const bi = builtinByName(node.name);
            if (bi) return md(`**${bi.name}** -- ${bi.detail}\n\n${bi.doc}`);
        }
    }
    return null;
});

function md(text: string): Hover {
    return { contents: { kind: MarkupKind.Markdown, value: text } };
}

/* ----------------------------------------------------------------------- */
/* Signature help                                                          */
/* ----------------------------------------------------------------------- */

connection.onSignatureHelp((params): SignatureHelp | null => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return null;
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;

    /* Find the enclosing Call node whose argument range covers the cursor. */
    const target = findEnclosingCallNode(a.program, params.position.line, params.position.character);
    if (!target) return null;
    const calleeT = a.inferred.nodeTypes.get(target.call.callee);
    if (!calleeT) return null;

    let fn: FunctionType | null = null;
    if (calleeT.kind === 'function') fn = calleeT;
    else if (calleeT.kind === 'class') {
        fn = {
            kind: 'function',
            params: calleeT.ctorParams,
            returns: [calleeT.instance],
            name: calleeT.name
        };
    }
    if (!fn) return null;

    const label = formatFunctionSignature(fn);
    const sigParams: ParameterInformation[] = fn.params.map(p => ({
        label: `${p.rest ? '...' : ''}${p.name}${p.optional ? '?' : ''}: ${renderType(p.type)}`,
        documentation: p.doc ? { kind: MarkupKind.Markdown, value: p.doc } : undefined
    }));
    const sig: SignatureInformation = {
        label,
        parameters: sigParams,
        documentation: fn.doc ? { kind: MarkupKind.Markdown, value: fn.doc } : undefined
    };
    const active = Math.min(target.activeParameter, Math.max(0, fn.params.length - 1));
    return { signatures: [sig], activeSignature: 0, activeParameter: active };
});

function formatFunctionSignature(fn: FunctionType): string {
    const ps = fn.params.map(p => `${p.rest ? '...' : ''}${p.name}${p.optional ? '?' : ''}: ${renderType(p.type)}`).join(', ');
    const ret = fn.returnsSelf
        ? 'self'
        : fn.returns.length === 1 ? renderType(fn.returns[0])
        : fn.returns.map(r => renderType(r)).join(', ');
    return `${fn.name ?? 'function'}(${ps}) -> ${ret}`;
}

function findEnclosingCallNode(root: Node, line: number, character: number): { call: Call; activeParameter: number } | null {
    /* Walk the AST and pick the deepest Call whose argument list contains
     * the cursor. We treat the range *inside* the parens as the trigger
     * range; if there are no args yet, anywhere inside the call range works. */
    let best: { call: Call; activeParameter: number } | null = null;
    const visit = (n: Node): void => {
        if (n.kind === 'Call' && rangeContains(n.range, line, character)) {
            /* Compute active parameter: index of the trailing arg whose
             * range ends before the cursor, plus 1. */
            let active = 0;
            for (let i = 0; i < n.args.length; i++) {
                const arg = n.args[i];
                if (isBeforeOrAt(arg.range.end, line, character)) active = i + 1;
            }
            if (active > n.args.length) active = n.args.length;
            best = { call: n, activeParameter: active };
        }
        for (const c of astChildren(n)) visit(c);
    };
    visit(root);
    return best;
}

function isBeforeOrAt(p: { line: number; character: number }, line: number, character: number): boolean {
    if (p.line < line) return true;
    if (p.line > line) return false;
    return p.character <= character;
}

/* ----------------------------------------------------------------------- */
/* Definition + document symbols                                           */
/* ----------------------------------------------------------------------- */

connection.onDefinition(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return null;

    if (node.kind === 'Ident') {
        const scope = a.resolved.scopeOf.get(node);
        const b = scope?.lookup(node.name);
        if (b) {
            return {
                uri: params.textDocument.uri,
                range: toLsp(b.nameRange)
            } as Location;
        }
        /* Maybe an include binding: VAR foo = include("...") -- already
         * resolved by the inferer; the binding's decl range is the VarDecl. */
    }
    if (node.kind === 'Call' && node.callee.kind === 'Ident' && node.callee.name === 'include'
        && node.args.length >= 1 && node.args[0].expr.kind === 'StringLit') {
        const target = resolveIncludePath(node.args[0].expr.value, params.textDocument.uri, workspaceRoots);
        if (target) {
            return {
                uri: fsPathToUri(target),
                range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } }
            } as Location;
        }
    }
    return null;
});

connection.onDocumentSymbol(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const out: DocumentSymbol[] = [];
    for (const s of a.program.body) {
        const ds = stmtToDocSymbol(s);
        if (ds) out.push(ds);
    }
    return out;
});

function stmtToDocSymbol(s: Node): DocumentSymbol | null {
    switch (s.kind) {
        case 'FunctionDecl':
            return {
                name: s.name,
                detail: `FUNCTION ${s.name}(${s.params.map(p => p.name).join(', ')})`,
                kind: LspSymbolKind.Function,
                range: toLsp(s.range),
                selectionRange: toLsp(s.nameRange)
            };
        case 'ClassDecl': {
            const ds: DocumentSymbol = {
                name: s.name,
                detail: `CLASS ${s.name}`,
                kind: LspSymbolKind.Class,
                range: toLsp(s.range),
                selectionRange: toLsp(s.nameRange),
                children: []
            };
            return ds;
        }
        case 'VarDecl': {
            const first = s.targets[0];
            if (!first) return null;
            return {
                name: first.name,
                detail: s.keyword,
                kind: s.keyword === 'CONST' ? LspSymbolKind.Constant : LspSymbolKind.Variable,
                range: toLsp(s.range),
                selectionRange: toLsp(first.range)
            };
        }
    }
    return null;
}

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */

function wordAt(text: string, offset: number): { value: string; start: number; end: number } | null {
    const isIdentChar = (c: string): boolean => /[A-Za-z0-9_]/.test(c);
    let s = offset;
    while (s > 0 && isIdentChar(text[s - 1])) s--;
    let e = offset;
    while (e < text.length && isIdentChar(text[e])) e++;
    if (s === e) return null;
    return { value: text.slice(s, e), start: s, end: e };
}

/** Return the offset of the trailing `.` / `:` / `?.` before the cursor
 *  if the user is mid-member-access. */
function receiverDotAt(text: string, offset: number): { dotOffset: number; accessor: '.' | ':' | '?.' } | null {
    let i = offset;
    while (i > 0 && /[A-Za-z0-9_]/.test(text[i - 1])) i--;
    let j = i;
    while (j > 0 && (text[j - 1] === ' ' || text[j - 1] === '\t')) j--;
    if (j <= 0) return null;
    const c = text[j - 1];
    if (c === '.' || c === ':') {
        /* `?.` is two chars. */
        if (j >= 2 && text[j - 2] === '?' && c === '.') {
            return { dotOffset: j - 2, accessor: '?.' };
        }
        return { dotOffset: j - 1, accessor: c };
    }
    return null;
}

/** Find the token index at or just before `offset` (in document characters). */
function tokenIndexBefore(tokens: Token[], offset: number, doc: TextDocument): number | null {
    let result: number | null = null;
    for (let i = 0; i < tokens.length; i++) {
        const endOff = doc.offsetAt(tokens[i].range.end);
        if (endOff <= offset) result = i;
        else break;
    }
    return result;
}

/** Walk backwards from `endIdx` collecting tokens that form the contiguous
 *  receiver expression (identifiers, member accessors, balanced calls /
 *  indices). Returns the range from the first such token to the last. */
function receiverRangeFromTokens(tokens: Token[], endIdx: number): LexRange | null {
    if (endIdx < 0 || endIdx >= tokens.length) return null;
    let i = endIdx;
    let depthParen = 0;
    let depthBracket = 0;
    while (i >= 0) {
        const t = tokens[i];
        if (t.kind === 'eof') { i--; continue; }
        if (t.kind === 'punct' && t.value === ')') { depthParen++; i--; continue; }
        if (t.kind === 'punct' && t.value === '(') {
            if (depthParen === 0) break;
            depthParen--; i--; continue;
        }
        if (t.kind === 'punct' && t.value === ']') { depthBracket++; i--; continue; }
        if (t.kind === 'punct' && t.value === '[') {
            if (depthBracket === 0) break;
            depthBracket--; i--; continue;
        }
        if (depthParen > 0 || depthBracket > 0) { i--; continue; }
        if (t.kind === 'ident' || t.kind === 'keyword') { i--; continue; }
        if (t.kind === 'op' && (t.value === '.' || t.value === ':' || t.value === '::' || t.value === '?.')) {
            i--; continue;
        }
        break;
    }
    const start = tokens[i + 1];
    const end = tokens[endIdx];
    if (!start) return null;
    return { start: start.range.start, end: end.range.end };
}

/** Find the deepest AST node whose range fully matches `r`. */
function nodeContainingRange(root: Node, r: LexRange): Node | null {
    let best: Node | null = null;
    const startsAt = (n: Node): boolean =>
        n.range.start.line === r.start.line && n.range.start.character === r.start.character;
    const endsAt = (n: Node): boolean =>
        n.range.end.line === r.end.line && n.range.end.character === r.end.character;
    const visit = (n: Node): void => {
        if (rangeContains(n.range, r.start.line, r.start.character)
            && rangeContains(n.range, r.end.line, r.end.character)) {
            if (startsAt(n) && endsAt(n)) best = n;
            for (const c of astChildren(n)) visit(c);
        }
    };
    visit(root);
    /* If no exact match, fall back to the deepest containing node. */
    if (best) return best;
    return nodeAt(root, r.end.line, Math.max(0, r.end.character - 1));
}

function walkAst(n: Node, visit: (n: Node) => void): void {
    visit(n);
    for (const c of astChildren(n)) walkAst(c, visit);
}

function scopeContaining(a: AnalyzedDocument, line: number, character: number): Scope {
    return scopeAt(a.resolved.fileScope, line, character);
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

/* Reference imports retained for the public API surface. */
void path; void namespaceMemberDetail;

documents.listen(connection);
connection.listen();
