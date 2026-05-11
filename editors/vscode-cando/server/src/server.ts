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
    SymbolInformation,
    Range as LspRange,
    SignatureHelp,
    SignatureInformation,
    ParameterInformation,
    WorkspaceEdit,
    TextEdit,
    InlayHint,
    InlayHintKind,
    DocumentHighlight,
    DocumentHighlightKind,
    CodeLens,
    Command,
    TextDocumentEdit,
    FoldingRange,
    FoldingRangeKind,
    SelectionRange,
    SemanticTokens,
    SemanticTokensBuilder,
    Color,
    ColorInformation,
    ColorPresentation,
    CodeAction,
    CodeActionKind
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
import { findReferencesAcrossWorkspace, workspaceSymbols, invalidateIndex } from './workspace';

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

/* Semantic-token legend. Keep in sync with the providers below. */
const SEMANTIC_TOKEN_TYPES = [
    'namespace', 'class', 'function', 'method', 'parameter',
    'variable', 'property', 'enumMember', 'keyword', 'string',
    'number', 'operator', 'macro', 'type'
];
const SEMANTIC_TOKEN_MODIFIERS = [
    'declaration', 'readonly', 'static', 'deprecated',
    'modification', 'documentation', 'defaultLibrary'
];
function semanticTokenType(name: string): number {
    return SEMANTIC_TOKEN_TYPES.indexOf(name);
}
function semanticTokenModifier(...names: string[]): number {
    let bits = 0;
    for (const n of names) {
        const i = SEMANTIC_TOKEN_MODIFIERS.indexOf(n);
        if (i >= 0) bits |= 1 << i;
    }
    return bits;
}

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
            workspaceSymbolProvider: true,
            referencesProvider: true,
            renameProvider: { prepareProvider: true },
            documentHighlightProvider: true,
            codeLensProvider: { resolveProvider: false },
            documentFormattingProvider: true,
            inlayHintProvider: { resolveProvider: false },
            foldingRangeProvider: true,
            selectionRangeProvider: true,
            colorProvider: true,
            codeActionProvider: { codeActionKinds: [CodeActionKind.QuickFix, CodeActionKind.Refactor] },
            semanticTokensProvider: {
                legend: {
                    tokenTypes: SEMANTIC_TOKEN_TYPES,
                    tokenModifiers: SEMANTIC_TOKEN_MODIFIERS
                },
                full: true,
                range: false
            },
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

documents.onDidChangeContent(e => {
    /* The on-disk version of this file is now stale -- the workspace
     * indexer should re-pick it up next time someone asks for references. */
    const fsPath = uriToFsPath(e.document.uri);
    if (fsPath) invalidateIndex(fsPath);
    refresh(e.document);
});
documents.onDidClose(e => {
    const fsPath = uriToFsPath(e.document.uri);
    if (fsPath) invalidateIndex(fsPath);
    clearDocument(e.document.uri);
});

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
    /* Objects and manifest types are callable iff they expose a __call
     * metamethod (or come from a manifest that may have one we can't see). */
    if (t.kind === 'object') {
        return enumerateMembers(t).has('__call');
    }
    if (t.kind === 'manifest-exports' || t.kind === 'manifest-type') return true;
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
            const docMd = b.doc ?? (b.kind === 'function'
                ? `Defined at line ${b.declRange.start.line + 1}.`
                : undefined);
            items.push({
                label: name,
                kind: bindingKindToCompletion(b),
                detail: renderType(b.type),
                documentation: docMd ? { kind: MarkupKind.Markdown, value: docMd } : undefined,
                /* Push function and class declarations to the top of the
                 * list; locals beat globals in a tie. */
                sortText: completionSortKey(b)
            });
        }
    }
    return items;
});

function completionSortKey(b: Binding): string {
    const tier = (b.kind === 'param' || b.kind === 'self' || b.kind === 'forvar' || b.kind === 'pipe' || b.kind === 'catch') ? '1'
               : (b.kind === 'var' || b.kind === 'const') ? '2'
               : (b.kind === 'function' || b.kind === 'class') ? '3'
               : '4';
    return tier + b.name;
}

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
                if (b.doc) lines.push('', b.doc);
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

/* ----------------------------------------------------------------------- */
/* Find references                                                         */
/* ----------------------------------------------------------------------- */

connection.onReferences(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return [];
    const binding = bindingAtNode(a, node);
    if (!binding) return [];
    const hits = findReferencesAcrossWorkspace(binding, params.textDocument.uri, workspaceRoots);
    const out: Location[] = hits.map(h => ({ uri: h.uri, range: toLsp(h.range) }));
    if (params.context && !params.context.includeDeclaration) {
        return out.filter(l => !(l.uri === params.textDocument.uri
            && rangesEqual(l.range, toLsp(binding.declRange))));
    }
    return out;
});

function rangesEqual(a: LspRange, b: LspRange): boolean {
    return a.start.line === b.start.line && a.start.character === b.start.character
        && a.end.line === b.end.line && a.end.character === b.end.character;
}

function bindingAtNode(a: AnalyzedDocument, node: Node): Binding | null {
    if (node.kind === 'Ident') {
        const scope = a.resolved.scopeOf.get(node);
        return scope?.lookup(node.name) ?? null;
    }
    if (node.kind === 'FunctionDecl' || node.kind === 'ClassDecl') {
        return a.resolved.fileScope.bindings.get(node.name) ?? null;
    }
    return null;
}

/* ----------------------------------------------------------------------- */
/* Rename                                                                  */
/* ----------------------------------------------------------------------- */

connection.onPrepareRename(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return null;
    const b = bindingAtNode(a, node);
    if (!b) return null;
    /* Don't allow rename of namespace / builtin idents. */
    if (b.kind === 'self' || b.kind === 'pipe') return null;
    return {
        range: toLsp(node.kind === 'Ident' ? node.range : b.nameRange),
        placeholder: b.name
    };
});

connection.onRenameRequest((params): WorkspaceEdit | null => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return null;
    const b = bindingAtNode(a, node);
    if (!b) return null;

    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(params.newName)) return null;

    const hits = findReferencesAcrossWorkspace(b, params.textDocument.uri, workspaceRoots);
    const byUri = new Map<string, TextEdit[]>();
    for (const h of hits) {
        const list = byUri.get(h.uri) ?? [];
        list.push({ range: toLsp(h.range), newText: params.newName });
        byUri.set(h.uri, list);
    }
    const changes: { [uri: string]: TextEdit[] } = {};
    for (const [uri, edits] of byUri) changes[uri] = edits;
    return { changes };
});

/* ----------------------------------------------------------------------- */
/* Workspace symbols                                                       */
/* ----------------------------------------------------------------------- */

connection.onWorkspaceSymbol(params => {
    const q = (params.query ?? '').toLowerCase();
    const hits = workspaceSymbols(workspaceRoots);
    const out: SymbolInformation[] = [];
    for (const h of hits) {
        if (q && !h.name.toLowerCase().includes(q)) continue;
        out.push({
            name: h.name,
            kind: bindingKindToSymbolKind(h.kind),
            location: { uri: h.uri, range: toLsp(h.range) },
            containerName: h.detail
        });
        if (out.length >= 500) break;
    }
    return out;
});

function bindingKindToSymbolKind(k: Binding['kind']): LspSymbolKind {
    switch (k) {
        case 'function': return LspSymbolKind.Function;
        case 'class':    return LspSymbolKind.Class;
        case 'const':    return LspSymbolKind.Constant;
        case 'param':    return LspSymbolKind.Variable;
        case 'self':     return LspSymbolKind.Variable;
        case 'pipe':     return LspSymbolKind.Variable;
        case 'catch':    return LspSymbolKind.Variable;
        case 'forvar':   return LspSymbolKind.Variable;
        case 'global':   return LspSymbolKind.Variable;
        default:         return LspSymbolKind.Variable;
    }
}

/* ----------------------------------------------------------------------- */
/* Inlay hints                                                             */
/* ----------------------------------------------------------------------- */

connection.languages.inlayHint.on(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const out: InlayHint[] = [];

    /* Show inferred types on the right-hand side of every VAR/CONST/GLOBAL
     * binding that doesn't have an obvious literal on the right. */
    astChildrenWalk(a.program, (n) => {
        if (n.kind === 'VarDecl') {
            for (let i = 0; i < n.targets.length; i++) {
                const t = n.targets[i];
                /* Skip when the user already typed a type-revealing literal. */
                const init = n.init[i] ?? n.init[n.init.length - 1];
                if (!init) continue;
                if (init.kind === 'NumberLit' || init.kind === 'StringLit' ||
                    init.kind === 'BoolLit'  || init.kind === 'NullLit' ||
                    init.kind === 'ArrayLit' || init.kind === 'ObjectLit') continue;
                const binding = a.resolved.fileScope.bindings.get(t.name)
                    ?? scopeContaining(a, t.range.start.line, t.range.start.character).lookup(t.name);
                if (!binding) continue;
                const ty = a.inferred.bindingTypes.get(binding) ?? binding.type;
                if (ty.kind === 'prim' && (ty.name === 'unknown' || ty.name === 'any')) continue;
                out.push({
                    position: t.range.end,
                    label: ': ' + renderType(ty),
                    kind: InlayHintKind.Type,
                    paddingLeft: false,
                    paddingRight: true
                });
            }
        }
        /* Show parameter names at call sites with positional args. */
        if (n.kind === 'Call') {
            const calleeT = a.inferred.nodeTypes.get(n.callee);
            if (!calleeT || calleeT.kind !== 'function') return;
            for (let i = 0; i < n.args.length && i < calleeT.params.length; i++) {
                const arg = n.args[i];
                const p = calleeT.params[i];
                if (arg.spread || p.rest) continue;
                /* Skip when arg already shows the param name (callee+ident
                 * identical) -- common idiom in CanDo where args carry the
                 * same name as params. */
                if (arg.expr.kind === 'Ident' && arg.expr.name === p.name) continue;
                /* Skip trivial literals to reduce noise -- numbers/strings
                 * get the hint; idents and complex exprs don't. */
                if (arg.expr.kind !== 'NumberLit' &&
                    arg.expr.kind !== 'StringLit' &&
                    arg.expr.kind !== 'BoolLit' &&
                    arg.expr.kind !== 'NullLit') continue;
                out.push({
                    position: arg.expr.range.start,
                    label: p.name + ':',
                    kind: InlayHintKind.Parameter,
                    paddingLeft: false,
                    paddingRight: true
                });
            }
        }
    });
    /* Filter to the requested range. */
    return out.filter(h =>
        h.position.line >= params.range.start.line
        && h.position.line <= params.range.end.line);
});

function astChildrenWalk(n: Node, visit: (n: Node) => void): void {
    visit(n);
    for (const c of astChildren(n)) astChildrenWalk(c, visit);
}

/* ----------------------------------------------------------------------- */
/* Folding ranges                                                          */
/* ----------------------------------------------------------------------- */

connection.onFoldingRanges(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const out: FoldingRange[] = [];
    /* Every `{ ... }` block, every multi-line comment, and every multi-line
     * IF chain becomes a folding range. */
    astChildrenWalk(a.program, (n) => {
        const isFoldable =
            n.kind === 'BlockStmt' ||
            n.kind === 'FunctionDecl' ||
            n.kind === 'ClassDecl' ||
            n.kind === 'IfStmt' ||
            n.kind === 'WhileStmt' ||
            n.kind === 'ForRange' || n.kind === 'ForKeys' ||
            n.kind === 'ForValues' || n.kind === 'ForOver' ||
            n.kind === 'TryStmt' ||
            n.kind === 'ObjectLit' ||
            n.kind === 'ArrayLit';
        if (!isFoldable) return;
        if (n.range.end.line <= n.range.start.line) return;
        out.push({
            startLine: n.range.start.line,
            endLine: n.range.end.line - 1,
            kind: FoldingRangeKind.Region
        });
    });
    /* Comment tokens spanning multiple lines (block comments). */
    for (const t of a.tokens) {
        if (t.kind === 'comment' && t.range.end.line > t.range.start.line) {
            out.push({
                startLine: t.range.start.line,
                endLine: t.range.end.line,
                kind: FoldingRangeKind.Comment
            });
        }
    }
    return out;
});

/* ----------------------------------------------------------------------- */
/* Selection ranges                                                        */
/* ----------------------------------------------------------------------- */

connection.onSelectionRanges(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    return params.positions.map(pos => buildSelectionRange(a, pos.line, pos.character));
});

function buildSelectionRange(a: AnalyzedDocument, line: number, character: number): SelectionRange {
    /* Walk from the deepest containing AST node up to the program root,
     * building a stack of ranges that the editor cycles through. */
    const ranges: LspRange[] = [];
    const visit = (n: Node): boolean => {
        if (!rangeContains(n.range, line, character)) return false;
        let descended = false;
        for (const c of astChildren(n)) {
            if (visit(c)) { descended = true; break; }
        }
        if (!descended) ranges.push(toLsp(n.range));
        else ranges.push(toLsp(n.range));
        return true;
    };
    visit(a.program);
    /* Build chain from innermost to outermost. */
    let chain: SelectionRange | undefined;
    for (const r of ranges) chain = { range: r, parent: chain };
    return chain ?? { range: { start: { line, character }, end: { line, character } } };
}

/* ----------------------------------------------------------------------- */
/* Semantic tokens                                                         */
/* ----------------------------------------------------------------------- */

connection.languages.semanticTokens.on(params => {
    const a = getAnalyzed(params.textDocument.uri);
    const builder = new SemanticTokensBuilder();
    if (!a) return builder.build();
    const all: { line: number; ch: number; len: number; type: number; mods: number }[] = [];

    astChildrenWalk(a.program, (n) => {
        if (n.kind === 'Ident') {
            const scope = a.resolved.scopeOf.get(n);
            const b = scope?.lookup(n.name);
            if (b) {
                push(all, n.range, semanticTokenType(bindingTokenType(b)), bindingTokenModifiers(b));
            } else if (namespaceByName(n.name)) {
                push(all, n.range, semanticTokenType('namespace'), semanticTokenModifier('defaultLibrary'));
            } else if (builtinByName(n.name)) {
                push(all, n.range, semanticTokenType('function'), semanticTokenModifier('defaultLibrary'));
            }
        } else if (n.kind === 'Member') {
            push(all, n.propertyRange, semanticTokenType('property'), 0);
        } else if (n.kind === 'FunctionDecl') {
            push(all, n.nameRange, semanticTokenType('function'), semanticTokenModifier('declaration'));
        } else if (n.kind === 'ClassDecl') {
            push(all, n.nameRange, semanticTokenType('class'), semanticTokenModifier('declaration'));
        } else if (n.kind === 'TemplateLit') {
            for (const p of n.parts) {
                if (p.kind === 'expr' && p.expr) {
                    /* Highlight `${` / `}` boundaries as macros. */
                    push(all, p.range, semanticTokenType('macro'), 0);
                }
            }
        }
    });

    /* Highlight the dollar-brace boundaries in template strings by token. */
    for (const t of a.tokens) {
        if (t.kind === 'number') push(all, t.range, semanticTokenType('number'), 0);
        else if (t.kind === 'string' || t.kind === 'template-string') push(all, t.range, semanticTokenType('string'), 0);
        else if (t.kind === 'keyword') push(all, t.range, semanticTokenType('keyword'), 0);
    }

    /* Sort by (line, character) and emit. */
    all.sort((x, y) => x.line - y.line || x.ch - y.ch);
    for (const t of all) builder.push(t.line, t.ch, t.len, t.type, t.mods);
    return builder.build();
});

function push(out: { line: number; ch: number; len: number; type: number; mods: number }[],
              r: LexRange, type: number, mods: number): void {
    if (type < 0) return;
    if (r.start.line !== r.end.line) return; // skip multi-line tokens
    out.push({
        line: r.start.line,
        ch: r.start.character,
        len: r.end.character - r.start.character,
        type, mods
    });
}

function bindingTokenType(b: Binding): string {
    switch (b.kind) {
        case 'function': return 'function';
        case 'class':    return 'class';
        case 'const':    return 'variable';
        case 'param':    return 'parameter';
        case 'self':     return 'parameter';
        case 'pipe':     return 'variable';
        case 'catch':    return 'variable';
        case 'forvar':   return 'variable';
        default:         return 'variable';
    }
}

function bindingTokenModifiers(b: Binding): number {
    let m = 0;
    if (b.kind === 'const') m |= semanticTokenModifier('readonly');
    if (b.captured) m |= semanticTokenModifier('modification');
    return m;
}

/* ----------------------------------------------------------------------- */
/* Color provider (forms.Color.* + #rrggbb / 0xrrggbb literals)            */
/* ----------------------------------------------------------------------- */

connection.onDocumentColor(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const out: ColorInformation[] = [];
    /* Numeric color literals: 0xRRGGBB hex constants used in forms.Color.*
     * and similar. We only flag the form `0x[0-9A-Fa-f]{6}` -- 8-digit
     * ARGB hex is also accepted. */
    for (const t of a.tokens) {
        if (t.kind !== 'number') continue;
        const c = parseColorLiteral(t.value);
        if (c) out.push({ range: toLsp(t.range), color: c });
    }
    return out;
});

connection.onColorPresentation(params => {
    const r = params.color.red, g = params.color.green, b = params.color.blue;
    const hex = '0x' +
        toHex(Math.round(r * 255)) +
        toHex(Math.round(g * 255)) +
        toHex(Math.round(b * 255));
    const out: ColorPresentation[] = [
        { label: hex },
        { label: `\`rgb(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)})\`` }
    ];
    return out;
});

function toHex(n: number): string {
    const v = Math.max(0, Math.min(255, n));
    const s = v.toString(16).toUpperCase();
    return s.length < 2 ? '0' + s : s;
}

function parseColorLiteral(raw: string): Color | null {
    if (!/^0[xX][0-9A-Fa-f]{6,8}$/.test(raw)) return null;
    const digits = raw.slice(2);
    let r: number, g: number, b: number, a: number;
    if (digits.length === 6) {
        r = parseInt(digits.slice(0, 2), 16);
        g = parseInt(digits.slice(2, 4), 16);
        b = parseInt(digits.slice(4, 6), 16);
        a = 255;
    } else {
        a = parseInt(digits.slice(0, 2), 16);
        r = parseInt(digits.slice(2, 4), 16);
        g = parseInt(digits.slice(4, 6), 16);
        b = parseInt(digits.slice(6, 8), 16);
    }
    return { red: r / 255, green: g / 255, blue: b / 255, alpha: a / 255 };
}

/* ----------------------------------------------------------------------- */
/* Code actions                                                            */
/* ----------------------------------------------------------------------- */

connection.onCodeAction(params => {
    const out: CodeAction[] = [];
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return out;
    for (const diag of params.context.diagnostics) {
        if (typeof diag.message !== 'string') continue;
        /* Quick fix: undeclared identifier -> introduce `VAR <name> = NULL;`
         * at the top of the surrounding function/file. */
        const undeclared = /'([A-Za-z_][A-Za-z0-9_]*)' is not declared/.exec(diag.message);
        if (undeclared) {
            const name = undeclared[1];
            const insertLine = Math.max(0, diag.range.start.line);
            const edit: WorkspaceEdit = {
                changes: {
                    [params.textDocument.uri]: [{
                        range: { start: { line: insertLine, character: 0 }, end: { line: insertLine, character: 0 } },
                        newText: `VAR ${name} = NULL;\n`
                    }]
                }
            };
            out.push({
                title: `Declare 'VAR ${name} = NULL;' above this line`,
                kind: CodeActionKind.QuickFix,
                diagnostics: [diag],
                edit,
                isPreferred: true
            });
        }
        /* Quick fix: assign-to-const -> change CONST to VAR. We do a simple
         * text-scan on the line that holds the const declaration. */
        const constAssign = /Cannot assign to CONST binding '([A-Za-z_][A-Za-z0-9_]*)'/.exec(diag.message);
        if (constAssign) {
            const name = constAssign[1];
            const a = getAnalyzed(params.textDocument.uri);
            const b = a?.resolved.fileScope.bindings.get(name);
            if (b && b.kind === 'const') {
                /* Find the leading CONST keyword token range on the decl line. */
                const text = doc.getText();
                const lineStart = doc.offsetAt({ line: b.declRange.start.line, character: 0 });
                const lineEnd = doc.offsetAt({ line: b.declRange.start.line + 1, character: 0 });
                const lineText = text.slice(lineStart, lineEnd);
                const kwIdx = /\bCONST\b|\bconst\b/.exec(lineText);
                if (kwIdx) {
                    const startCh = kwIdx.index;
                    const replaceRange: LspRange = {
                        start: { line: b.declRange.start.line, character: startCh },
                        end:   { line: b.declRange.start.line, character: startCh + kwIdx[0].length }
                    };
                    out.push({
                        title: `Change CONST '${name}' to VAR`,
                        kind: CodeActionKind.QuickFix,
                        diagnostics: [diag],
                        edit: {
                            changes: {
                                [params.textDocument.uri]: [{ range: replaceRange, newText: 'VAR' }]
                            }
                        }
                    });
                }
            }
        }
    }
    return out;
});

/* ----------------------------------------------------------------------- */
/* Document highlights                                                     */
/* ----------------------------------------------------------------------- */

connection.onDocumentHighlight(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return [];
    const binding = bindingAtNode(a, node);
    if (!binding) return [];
    const out: DocumentHighlight[] = [];
    for (const r of binding.references) {
        const kind = rangesEqual(toLsp(r), toLsp(binding.nameRange))
            ? DocumentHighlightKind.Write
            : DocumentHighlightKind.Read;
        out.push({ range: toLsp(r), kind });
    }
    return out;
});

/* ----------------------------------------------------------------------- */
/* CodeLens                                                                */
/* ----------------------------------------------------------------------- */

connection.onCodeLens(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return [];
    const out: CodeLens[] = [];
    for (const stmt of a.program.body) {
        if (stmt.kind !== 'FunctionDecl' && stmt.kind !== 'ClassDecl') continue;
        const b = a.resolved.fileScope.bindings.get(stmt.name);
        if (!b) continue;
        /* declaration counts as 1 in references[] -- subtract it. */
        const count = Math.max(0, b.references.length - 1);
        const command: Command = {
            title: count === 1 ? '1 reference' : `${count} references`,
            command: '',  // information-only; clicking is a no-op
            arguments: []
        };
        out.push({ range: toLsp(stmt.nameRange), command });
    }
    return out;
});

/* ----------------------------------------------------------------------- */
/* Document formatting                                                     */
/* ----------------------------------------------------------------------- */

connection.onDocumentFormatting(params => {
    const doc = documents.get(params.textDocument.uri);
    if (!doc) return [];
    const original = doc.getText();
    const formatted = formatSource(original);
    if (formatted === original) return [];
    return [{
        range: {
            start: { line: 0, character: 0 },
            end: doc.positionAt(original.length)
        },
        newText: formatted
    }];
});

/** Conservative, token-aware formatter. Goals:
 *   - Collapse runs of internal whitespace to a single space.
 *   - Keep one space after `,` and around binary operators.
 *   - No space inside `(`, `[`, `{` adjacent to their contents.
 *   - Preserve line breaks the user wrote (we don't reflow blocks). */
function formatSource(src: string): string {
    /* Single-pass character-driven formatter that avoids touching strings,
     * template strings, and comments. Anything outside those is normalized. */
    let out = '';
    let i = 0;
    let atLineStart = true;
    let indent = 0;
    const INDENT = '    ';

    while (i < src.length) {
        const c = src[i];

        /* Preserve comments verbatim. */
        if (c === '/' && src[i + 1] === '/') {
            const end = src.indexOf('\n', i);
            const stop = end < 0 ? src.length : end;
            out += src.slice(i, stop);
            i = stop;
            continue;
        }
        if (c === '/' && src[i + 1] === '*') {
            const end = src.indexOf('*/', i + 2);
            const stop = end < 0 ? src.length : end + 2;
            out += src.slice(i, stop);
            i = stop;
            continue;
        }

        /* Preserve string and template literals. */
        if (c === '"' || c === '\'') {
            const stop = scanQuoted(src, i, c);
            out += src.slice(i, stop);
            i = stop;
            atLineStart = false;
            continue;
        }
        if (c === '`') {
            const stop = scanTemplate(src, i);
            out += src.slice(i, stop);
            i = stop;
            atLineStart = false;
            continue;
        }

        if (c === '\n') {
            /* Strip trailing whitespace before the newline. */
            out = out.replace(/[ \t]+$/, '');
            out += '\n';
            atLineStart = true;
            i++;
            continue;
        }
        if (c === ' ' || c === '\t' || c === '\r') {
            if (atLineStart) {
                /* Collapse leading whitespace -- re-emit it as `INDENT * depth`. */
                while (i < src.length && (src[i] === ' ' || src[i] === '\t' || src[i] === '\r')) i++;
                /* Re-look at the next character to decide whether to dedent. */
                if (src[i] === '}' || src[i] === ']' || src[i] === ')') {
                    out += INDENT.repeat(Math.max(0, indent - 1));
                } else {
                    out += INDENT.repeat(indent);
                }
                atLineStart = false;
                continue;
            }
            /* Collapse to a single space. */
            while (i < src.length && (src[i] === ' ' || src[i] === '\t' || src[i] === '\r')) i++;
            /* Suppress trailing internal spaces immediately before close
             * brackets / commas / semicolons. */
            const next = src[i];
            if (next !== ')' && next !== ']' && next !== '}' && next !== ',' && next !== ';' && next !== '\n') {
                out += ' ';
            }
            continue;
        }

        if (c === '{') { out += '{'; indent++; i++; atLineStart = false; continue; }
        if (c === '}') { indent = Math.max(0, indent - 1); out += '}'; i++; atLineStart = false; continue; }

        /* Insert a space after commas if missing. */
        if (c === ',') {
            out += ',';
            i++;
            if (i < src.length && src[i] !== ' ' && src[i] !== '\n' && src[i] !== '\t') {
                out += ' ';
            }
            atLineStart = false;
            continue;
        }

        out += c;
        i++;
        atLineStart = false;
    }
    /* Ensure a trailing newline. */
    if (!out.endsWith('\n')) out += '\n';
    return out;
}

function scanQuoted(src: string, start: number, quote: string): number {
    let i = start + 1;
    while (i < src.length) {
        if (src[i] === '\\' && i + 1 < src.length) { i += 2; continue; }
        if (src[i] === quote) return i + 1;
        if (src[i] === '\n' && quote === '"') return i;
        i++;
    }
    return i;
}

function scanTemplate(src: string, start: number): number {
    let i = start + 1;
    while (i < src.length) {
        if (src[i] === '\\' && i + 1 < src.length) { i += 2; continue; }
        if (src[i] === '`') return i + 1;
        if (src[i] === '$' && src[i + 1] === '{') {
            i += 2;
            let depth = 1;
            while (i < src.length && depth > 0) {
                if (src[i] === '{') depth++;
                else if (src[i] === '}') depth--;
                i++;
            }
            continue;
        }
        i++;
    }
    return i;
}

/* Reference imports retained for the public API surface. */
void path; void namespaceMemberDetail; void SemanticTokens; void TextDocumentEdit;

documents.listen(connection);
connection.listen();
