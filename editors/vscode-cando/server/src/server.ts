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
            callHierarchyProvider: true,
            workspace: {
                fileOperations: {
                    willRename: { filters: [{ pattern: { glob: '**/*.cdo' } }] }
                }
            },
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
                    /* Argument-type check: every positional arg whose param
                     * has a concrete declared type must be assignable. */
                    for (let i = 0; i < n.args.length && i < calleeT.params.length; i++) {
                        const arg = n.args[i];
                        const p = calleeT.params[i];
                        if (arg.spread || p.rest) continue;
                        const argT = a.inferred.nodeTypes.get(arg.expr);
                        if (!argT) continue;
                        if (!isAssignable(argT, p.type)) {
                            out.push({
                                severity: DiagnosticSeverity.Warning,
                                range: toLsp(arg.expr.range),
                                message: `Argument type '${renderType(argT)}' is not assignable to parameter '${p.name}: ${renderType(p.type)}'`,
                                source: 'cando',
                                code: 'arg-type'
                            });
                        }
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
                /* Did-you-mean suggestion: scan visible bindings for the
                 * closest match by edit distance. Only emit when there's a
                 * reasonably close hit (distance <= 2 or <= 1/3 of length). */
                const suggestion = closestVisibleName(scope, n.name);
                const detail = suggestion ? ` -- did you mean '${suggestion}'?` : '';
                out.push({
                    severity: DiagnosticSeverity.Hint,
                    range: toLsp(n.range),
                    message: `'${n.name}' is not declared in this file (will be looked up as a global)${detail}`,
                    source: 'cando',
                    code: suggestion ? `did-you-mean:${suggestion}` : 'undefined-ident',
                    data: suggestion ? { suggestion } : undefined
                });
            }
        }
    });

    /* Unused binding detection. Skip names starting with `_`, function /
     * class declarations (they're API surface that may be exported via
     * the file's RETURN), `self`, `pipe`, CATCH params, and globals. */
    for (const b of a.resolved.allBindings) {
        if (b.name.startsWith('_')) continue;
        if (b.name === 'self' || b.name === 'pipe') continue;
        if (b.kind === 'self' || b.kind === 'pipe' || b.kind === 'catch') continue;
        if (b.kind === 'function' || b.kind === 'class' || b.kind === 'global') continue;
        if (b.references.length > 1) continue;  // declaration counts as 1
        const tag = b.kind === 'param' ? 'parameter' : 'variable';
        out.push({
            severity: DiagnosticSeverity.Hint,
            range: toLsp(b.nameRange),
            message: `Unused ${tag} '${b.name}' (prefix with '_' to silence)`,
            source: 'cando',
            code: 'unused',
            tags: [1]  // DiagnosticTag.Unnecessary -- editor renders faded
        });
    }

    /* Doc-comment diagnostics:
     *   - doc-bad-type: `{type}` annotation failed to parse cleanly.
     *   - doc-unknown-tag: `@foo` tag we don't recognise.
     *   - doc-deprecated-use: identifier resolves to a `@deprecated`
     *     binding (informational, surfaced as faded text via tag). */
    for (const d of a.inferred.docTypeErrors ?? []) {
        out.push({
            severity: DiagnosticSeverity.Information,
            range: toLsp(d.range),
            message: d.message,
            source: 'cando',
            code: 'doc-bad-type'
        });
    }
    for (const b of a.resolved.allBindings) {
        if (!b.docBlock) continue;
        for (const tag of b.docBlock.unknownTags) {
            out.push({
                severity: DiagnosticSeverity.Hint,
                range: toLsp(b.nameRange),
                message: `Unknown doc tag '@${tag.name}' on '${b.name}'`,
                source: 'cando',
                code: 'doc-unknown-tag'
            });
        }
    }
    walkAst(a.program, (n) => {
        if (n.kind !== 'Ident') return;
        const scope = a.resolved.scopeOf.get(n);
        const b = scope?.lookup(n.name);
        if (!b || !b.deprecated) return;
        if (b.references.length > 0 && b.references[0] === n.range) return; // skip the decl itself
        out.push({
            severity: DiagnosticSeverity.Hint,
            range: toLsp(n.range),
            message: `'${n.name}' is deprecated: ${b.deprecated}`,
            source: 'cando',
            code: 'doc-deprecated-use',
            tags: [2]  // DiagnosticTag.Deprecated -- editor renders struck-through
        });
    });

    /* Dead code detection: any statement after a RETURN / THROW / BREAK /
     * CONTINUE / SETTLE in the same block is unreachable. */
    walkAst(a.program, (n) => {
        if (n.kind !== 'BlockStmt') return;
        let killed = false;
        let killer: typeof n.body[number] | null = null;
        for (const s of n.body) {
            if (killed) {
                out.push({
                    severity: DiagnosticSeverity.Hint,
                    range: toLsp(s.range),
                    message: `Unreachable: code after ${killer?.kind === 'ReturnStmt' ? 'RETURN' : killer?.kind === 'ThrowStmt' ? 'THROW' : 'control-flow exit'}`,
                    source: 'cando',
                    code: 'unreachable',
                    tags: [1]
                });
            }
            if (s.kind === 'ReturnStmt' || s.kind === 'ThrowStmt' ||
                s.kind === 'BreakStmt' || s.kind === 'ContinueStmt' ||
                s.kind === 'SettleStmt') {
                killed = true;
                killer = s;
            }
        }
    });
    return out;
}

/** Conservative assignability: is `from` an acceptable value for a slot
 *  that wants `to`? Falls back to `true` whenever either side is `any` /
 *  `unknown` so we don't flood diagnostics on under-typed code. */
function isAssignable(from: TypeRef, to: TypeRef): boolean {
    if (to.kind === 'prim' && (to.name === 'any' || to.name === 'unknown')) return true;
    if (from.kind === 'prim' && (from.name === 'any' || from.name === 'unknown')) return true;
    if (renderType(from) === renderType(to)) return true;
    if (to.kind === 'union') return to.variants.some(v => isAssignable(from, v));
    if (from.kind === 'union') return from.variants.every(v => isAssignable(v, to));
    if (to.kind === 'optional') {
        if (from.kind === 'prim' && from.name === 'null') return true;
        return isAssignable(from, to.inner);
    }
    if (from.kind === 'optional') return false;  // can't pass a maybe-null where required
    if (to.kind === 'array' && from.kind === 'array') return isAssignable(from.element, to.element);
    if (to.kind === 'object' && from.kind === 'object') return true;  // structural; permissive
    if (to.kind === 'manifest-type' && from.kind === 'manifest-type') return from.typeName === to.typeName;
    /* Class instance is acceptable where the instance type is wanted. */
    if (to.kind === 'object' && from.kind === 'class') return true;
    return false;
}

function closestVisibleName(scope: Scope | undefined, name: string): string | null {
    if (!scope) return null;
    const candidates: string[] = [];
    for (let s: Scope | null = scope; s; s = s.parent) {
        for (const k of s.bindings.keys()) candidates.push(k);
    }
    for (const ns of NAMESPACES) candidates.push(ns.name);
    for (const b of GLOBAL_BUILTINS) candidates.push(b.name);
    let best: { name: string; dist: number } | null = null;
    for (const c of candidates) {
        const d = editDistance(name, c);
        if (d > 2 && d > Math.floor(name.length / 3)) continue;
        if (!best || d < best.dist) best = { name: c, dist: d };
    }
    return best?.name ?? null;
}

function editDistance(a: string, b: string): number {
    /* Standard Damerau-Levenshtein bounded to 8 for performance. */
    if (a === b) return 0;
    const al = a.length, bl = b.length;
    if (Math.abs(al - bl) > 8) return Infinity;
    const prev = new Array(bl + 1).fill(0);
    const cur = new Array(bl + 1).fill(0);
    for (let j = 0; j <= bl; j++) prev[j] = j;
    for (let i = 1; i <= al; i++) {
        cur[0] = i;
        for (let j = 1; j <= bl; j++) {
            const cost = a[i - 1] === b[j - 1] ? 0 : 1;
            cur[j] = Math.min(
                prev[j] + 1,
                cur[j - 1] + 1,
                prev[j - 1] + cost
            );
        }
        for (let j = 0; j <= bl; j++) prev[j] = cur[j];
    }
    return prev[bl];
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
            const item: CompletionItem = {
                label: name,
                kind: bindingKindToCompletion(b),
                detail: renderType(b.type),
                documentation: docMd ? { kind: MarkupKind.Markdown, value: docMd } : undefined,
                /* Push function and class declarations to the top of the
                 * list; locals beat globals in a tie. */
                sortText: completionSortKey(b)
            };
            if (b.deprecated) {
                item.tags = [1]; // CompletionItemTag.Deprecated -- editor renders struck-through
            }
            items.push(item);
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
                edit
            });
            /* Auto-include: if `<name>` matches a known native module under
             * the workspace's modules/, suggest `VAR <name> = include(...)`
             * at the top of the file. */
            const candidate = findIncludableModule(name, params.textDocument.uri);
            if (candidate) {
                out.push({
                    title: `Add: VAR ${name} = include("${candidate.relative}");`,
                    kind: CodeActionKind.QuickFix,
                    diagnostics: [diag],
                    edit: {
                        changes: {
                            [params.textDocument.uri]: [{
                                range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } },
                                newText: `VAR ${name} = include("${candidate.relative}");\n`
                            }]
                        }
                    },
                    isPreferred: true
                });
            }
        }
        /* Did-you-mean from the suggestion baked into the diagnostic code. */
        const dym = typeof diag.code === 'string' && /^did-you-mean:(.+)$/.exec(diag.code);
        if (dym) {
            const suggestion = dym[1];
            out.push({
                title: `Change to '${suggestion}'`,
                kind: CodeActionKind.QuickFix,
                diagnostics: [diag],
                edit: {
                    changes: {
                        [params.textDocument.uri]: [{ range: diag.range, newText: suggestion }]
                    }
                },
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

/* ----------------------------------------------------------------------- */
/* Auto-include candidate finder                                           */
/* ----------------------------------------------------------------------- */

import * as fs from 'fs';
import { findManifestFor } from './manifest';

/** Scan workspace `modules/<name>/cando.api.json` files and return the
 *  best relative-path candidate to `include()` for the binding name the
 *  user typed. Currently matches by manifest `name` field exactly, then
 *  by directory name, then by path containment. */
function findIncludableModule(bindingName: string, documentUri: string):
    { relative: string; absolute: string } | null {
    const docPath = uriToFsPath(documentUri);
    if (!docPath) return null;
    for (const root of workspaceRoots) {
        const modulesDir = path.join(root, 'modules');
        if (!safeIsDir(modulesDir)) continue;
        let entries: string[];
        try { entries = fs.readdirSync(modulesDir); } catch { continue; }
        for (const e of entries) {
            const dir = path.join(modulesDir, e);
            if (!safeIsDir(dir)) continue;
            /* Look for the manifest. */
            let absoluteCandidate = path.join(dir, e + '.so');
            let manifest = findManifestFor(absoluteCandidate);
            if (!manifest) {
                /* Try a .cdo entry point. */
                absoluteCandidate = path.join(dir, 'init.cdo');
                if (!safeIsFile(absoluteCandidate)) absoluteCandidate = path.join(dir, e + '.cdo');
                if (!safeIsFile(absoluteCandidate)) continue;
            }
            const matches = (manifest && manifest.name === bindingName) ||
                            e === bindingName ||
                            e === bindingName.toLowerCase();
            if (!matches) continue;
            const relative = path.relative(path.dirname(docPath), absoluteCandidate)
                .replace(/\\/g, '/');
            return {
                relative: relative.startsWith('.') ? relative : './' + relative,
                absolute: absoluteCandidate
            };
        }
    }
    return null;
}

function safeIsDir(p: string): boolean {
    try { return fs.statSync(p).isDirectory(); } catch { return false; }
}
function safeIsFile(p: string): boolean {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

/* ----------------------------------------------------------------------- */
/* Workspace file watching                                                 */
/* ----------------------------------------------------------------------- */

connection.onDidChangeWatchedFiles(params => {
    for (const change of params.changes) {
        const fsPath = uriToFsPath(change.uri);
        if (fsPath) invalidateIndex(fsPath);
    }
    /* Refresh open documents so their cross-file references reflect the
     * just-changed file. */
    for (const doc of documents.all()) refresh(doc);
});

connection.workspace.onWillRenameFiles(params => {
    /* When a `.cdo` file is renamed we update every workspace include()
     * call that referenced its old name. We compute textDocument edits
     * only for files we have analyzed; the user can run a re-analyse if
     * they have files we haven't opened. */
    const edits: WorkspaceEdit = { changes: {} };
    const renames = params.files.map(f => ({
        oldPath: uriToFsPath(f.oldUri),
        newPath: uriToFsPath(f.newUri)
    })).filter(r => r.oldPath && r.newPath) as { oldPath: string; newPath: string }[];
    if (renames.length === 0) return null;
    for (const doc of documents.all()) {
        const a = getAnalyzed(doc.uri);
        if (!a) continue;
        const docFsPath = uriToFsPath(doc.uri);
        if (!docFsPath) continue;
        const docDir = path.dirname(docFsPath);
        const fileEdits: TextEdit[] = [];
        walkAst(a.program, (n) => {
            if (n.kind === 'Call' && n.callee.kind === 'Ident' && n.callee.name === 'include'
                && n.args.length >= 1 && n.args[0].expr.kind === 'StringLit') {
                const arg = n.args[0].expr;
                const rawPath = arg.value;
                const resolved = resolveIncludePath(rawPath, doc.uri, workspaceRoots);
                if (!resolved) return;
                for (const r of renames) {
                    if (resolved !== r.oldPath) continue;
                    const newRel = path.relative(docDir, r.newPath).replace(/\\/g, '/');
                    const newPath = newRel.startsWith('.') ? newRel : './' + newRel;
                    /* Replace just the string contents (preserve quotes). */
                    const startCh = arg.range.start.character + 1;
                    const endCh = arg.range.end.character - 1;
                    fileEdits.push({
                        range: {
                            start: { line: arg.range.start.line, character: startCh },
                            end: { line: arg.range.end.line, character: endCh }
                        },
                        newText: newPath
                    });
                }
            }
        });
        if (fileEdits.length) edits.changes![doc.uri] = fileEdits;
    }
    return Object.keys(edits.changes ?? {}).length ? edits : null;
});

/* ----------------------------------------------------------------------- */
/* Call hierarchy                                                          */
/* ----------------------------------------------------------------------- */

connection.languages.callHierarchy.onPrepare(params => {
    const a = getAnalyzed(params.textDocument.uri);
    if (!a) return null;
    const node = nodeAt(a.program, params.position.line, params.position.character);
    if (!node) return null;
    const b = bindingAtNode(a, node);
    if (!b || (b.kind !== 'function' && b.kind !== 'class')) return null;
    return [{
        name: b.name,
        kind: b.kind === 'class' ? LspSymbolKind.Class : LspSymbolKind.Function,
        uri: params.textDocument.uri,
        range: toLsp(b.declRange),
        selectionRange: toLsp(b.nameRange),
        detail: renderType(b.type),
        data: { name: b.name, kind: b.kind, uri: params.textDocument.uri }
    }];
});

connection.languages.callHierarchy.onIncomingCalls(params => {
    const item = params.item;
    const targetName = item.name;
    const out: { from: typeof item; fromRanges: LspRange[] }[] = [];
    /* Find every reference to this binding across the workspace and group
     * them by the enclosing function in the calling document. */
    const wsHits = findReferencesAcrossWorkspace(
        // build a synthetic binding to drive the search
        { name: targetName, kind: 'function', references: [], scope: { kind: 'file' } } as unknown as Binding,
        item.uri,
        workspaceRoots
    );
    /* Group hits by (uri, enclosing function name). */
    const grouped = new Map<string, { uri: string; fnName: string; fnRange: LspRange; ranges: LspRange[] }>();
    for (const h of wsHits) {
        const a = getAnalyzed(h.uri);
        if (!a) continue;
        const enclosing = enclosingFunctionAt(a, h.range.start.line, h.range.start.character);
        if (!enclosing) continue;
        const key = h.uri + '|' + enclosing.name;
        let entry = grouped.get(key);
        if (!entry) {
            entry = { uri: h.uri, fnName: enclosing.name, fnRange: toLsp(enclosing.range), ranges: [] };
            grouped.set(key, entry);
        }
        entry.ranges.push(toLsp(h.range));
    }
    for (const e of grouped.values()) {
        out.push({
            from: {
                name: e.fnName,
                kind: LspSymbolKind.Function,
                uri: e.uri,
                range: e.fnRange,
                selectionRange: e.fnRange
            },
            fromRanges: e.ranges
        });
    }
    return out;
});

connection.languages.callHierarchy.onOutgoingCalls(params => {
    const item = params.item;
    const a = getAnalyzed(item.uri);
    if (!a) return [];
    /* Find the function decl whose name matches the item; walk its body
     * collecting Call expressions and grouping by callee binding. */
    const fnNode = findFunctionDecl(a, item.name);
    if (!fnNode) return [];
    const grouped = new Map<string, { name: string; range: LspRange; ranges: LspRange[] }>();
    walkAst(fnNode, (n) => {
        if (n.kind !== 'Call') return;
        let calleeName = '';
        if (n.callee.kind === 'Ident') calleeName = n.callee.name;
        else if (n.callee.kind === 'Member') calleeName = n.callee.property;
        if (!calleeName) return;
        const b = a.resolved.fileScope.bindings.get(calleeName);
        const range = b ? toLsp(b.declRange) : toLsp(n.callee.range);
        const entry = grouped.get(calleeName) ?? { name: calleeName, range, ranges: [] };
        entry.ranges.push(toLsp(n.callee.range));
        grouped.set(calleeName, entry);
    });
    return [...grouped.values()].map(e => ({
        to: {
            name: e.name,
            kind: LspSymbolKind.Function,
            uri: item.uri,
            range: e.range,
            selectionRange: e.range
        },
        fromRanges: e.ranges
    }));
});

function enclosingFunctionAt(a: AnalyzedDocument, line: number, character: number): { name: string; range: LexRange } | null {
    let best: { name: string; range: LexRange } | null = null;
    walkAst(a.program, (n) => {
        if (n.kind === 'FunctionDecl' && rangeContains(n.range, line, character)) {
            best = { name: n.name, range: n.range };
        } else if (n.kind === 'ClassDecl' && rangeContains(n.range, line, character)) {
            best = { name: n.name, range: n.range };
        }
    });
    return best;
}

function findFunctionDecl(a: AnalyzedDocument, name: string): Node | null {
    for (const s of a.program.body) {
        if ((s.kind === 'FunctionDecl' || s.kind === 'ClassDecl') && s.name === name) return s;
    }
    return null;
}

/* Reference imports retained for the public API surface. */
void path; void namespaceMemberDetail; void SemanticTokens; void TextDocumentEdit;

documents.listen(connection);
connection.listen();
