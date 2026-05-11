/*
 * Analyze pipeline: lex -> parse -> resolve -> infer.
 *
 * Owns two caches:
 *   - LSP-document cache, keyed by uri + version. Filled by analyzeDocument.
 *   - Filesystem cache, keyed by absolute path + mtime. Filled by
 *     analyzeFsFile, used when one document include()s another that the
 *     editor hasn't opened.
 *
 * Cross-file include resolution: when the inferer encounters
 * `include("./foo.cdo")`, it asks back via `resolveIncludeType`. We run
 * the same pipeline on that file, take the moduleType, and pass it back.
 * A per-call cycle guard avoids infinite recursion on circular includes.
 */

import * as fs from 'fs';
import { Lexer, Token, Range } from './lexer';
import { parse, ParseError } from './parser';
import { Program, Node } from './ast';
import { resolve as resolveScopes, ResolveResult, Scope, Binding } from './scope';
import { infer, InferResult } from './infer';
import { TypeRef, ANY } from './typesys';
import { findManifestFor } from './manifest';
import { resolveIncludePath } from './paths';
import { parseDocBlock, DocBlock } from './docparse';
import * as path from 'path';

export interface AnalyzedDocument {
    uri: string;
    version: number;
    text: string;
    tokens: Token[];
    program: Program;
    parseErrors: ParseError[];
    resolved: ResolveResult;
    inferred: InferResult;
}

interface DocCacheEntry { uri: string; version: number; doc: AnalyzedDocument; }
interface FsCacheEntry { mtimeMs: number; doc: AnalyzedDocument; }

const docCache = new Map<string, DocCacheEntry>();
const fsCache = new Map<string, FsCacheEntry>();

/* ----------------------------------------------------------------------- */
/* Public entry points                                                     */
/* ----------------------------------------------------------------------- */

export function analyzeDocument(uri: string, text: string, version: number, workspaceRoots: string[]): AnalyzedDocument {
    const existing = docCache.get(uri);
    if (existing && existing.version === version && existing.doc.text === text) return existing.doc;

    const doc = analyzeFresh(uri, text, version, workspaceRoots, new Set());
    docCache.set(uri, { uri, version, doc });
    return doc;
}

export function clearDocument(uri: string): void {
    docCache.delete(uri);
}

export function getCached(uri: string): AnalyzedDocument | null {
    return docCache.get(uri)?.doc ?? null;
}

/* ----------------------------------------------------------------------- */
/* Internals                                                               */
/* ----------------------------------------------------------------------- */

function analyzeFresh(uri: string, text: string, version: number, workspaceRoots: string[], includeStack: Set<string>): AnalyzedDocument {
    const tokens = new Lexer(text).tokenize();
    const { program, errors: parseErrors } = parse(tokens, text);
    const resolved = resolveScopes(program);
    attachDocComments(tokens, resolved);
    const inferred = infer(program, resolved, {
        documentUri: uri,
        workspaceRoots,
        resolveIncludeType: (absPath: string): TypeRef | null => {
            if (includeStack.has(absPath)) return null;  // cycle guard
            return resolveIncludeAt(absPath, workspaceRoots, includeStack);
        }
    });
    return { uri, version, text, tokens, program, parseErrors, resolved, inferred };
}

/** Walk every binding whose declaration starts on line N and look for
 *  consecutive `//` or `///` comments whose end line is N-1, N-2, ... .
 *  Concatenated comment text (joined by newlines) becomes binding.doc.
 *
 *  Doc blocks that don't attach to any binding (e.g. a top-of-file
 *  `@shape Foo { ... }` separated by a blank line from the next decl)
 *  are recorded on `resolved.orphanDocBlocks` so the alias registry
 *  can still pick up their `@shape` / `@callback` declarations. */
function attachDocComments(tokens: Token[], resolved: ResolveResult): void {
    /* Group comments by their end line for cheap lookup. */
    const byEndLine = new Map<number, Token>();
    for (const t of tokens) {
        if (t.kind !== 'comment') continue;
        byEndLine.set(t.range.end.line, t);
    }

    /* Track which comment tokens we attach to a binding so we can find
     * orphan blocks afterwards. */
    const consumed = new Set<Token>();

    for (const b of resolved.allBindings) {
        if (b.kind !== 'function' && b.kind !== 'class' &&
            b.kind !== 'const' && b.kind !== 'var' && b.kind !== 'global') continue;
        const lines: string[] = [];
        const usedHere: Token[] = [];
        let line = b.declRange.start.line - 1;
        while (line >= 0) {
            const c = byEndLine.get(line);
            if (!c) break;
            lines.unshift(stripCommentMarker(c.value));
            usedHere.push(c);
            line = c.range.start.line - 1;
        }
        if (lines.length) {
            const raw = lines.join('\n').trim();
            b.docBlock = parseDocBlock(raw);
            b.doc = renderDocComment(raw);
            if (b.docBlock.deprecated) b.deprecated = b.docBlock.deprecated;
            for (const c of usedHere) consumed.add(c);
        }
    }

    /* Find orphan blocks. Group consecutive comment tokens (no gaps)
     * and parse each unconsumed group. */
    const allComments = tokens
        .filter(t => t.kind === 'comment')
        .sort((a, b) => a.range.start.line - b.range.start.line);
    let i = 0;
    while (i < allComments.length) {
        const start = i;
        let j = i + 1;
        while (j < allComments.length &&
               allComments[j].range.start.line === allComments[j - 1].range.end.line + 1) {
            j++;
        }
        const group = allComments.slice(start, j);
        if (!group.some(c => consumed.has(c))) {
            const raw = group.map(c => stripCommentMarker(c.value)).join('\n').trim();
            if (raw.length > 0) {
                const block = parseDocBlock(raw);
                /* Only keep orphan blocks that carry useful tags --
                 * loose narrative comments shouldn't pollute the
                 * registry. */
                if (block.tags.length > 0) {
                    resolved.orphanDocBlocks.push(block);
                }
            }
        }
        i = j;
    }
}

/** Reformat a doc comment that uses JSDoc-style tags (`@param`,
 *  `@returns`, `@example`, `@deprecated`) into a Markdown block that
 *  renders nicely in hover. Unknown tags fall through unchanged. */
function renderDocComment(raw: string): string {
    const lines = raw.split('\n');
    const summary: string[] = [];
    const params: { name: string; rest: string }[] = [];
    let returns: string | null = null;
    let deprecated: string | null = null;
    const examples: string[] = [];

    let inExample = false;
    let exampleBuf = '';
    for (const line of lines) {
        const trimmed = line.trim();
        if (inExample) {
            if (trimmed.startsWith('@')) {
                examples.push(exampleBuf.trim());
                exampleBuf = '';
                inExample = false;
                /* fall through to tag handling below */
            } else {
                exampleBuf += line + '\n';
                continue;
            }
        }
        const tag = /^@(\w+)\s*(.*)$/.exec(trimmed);
        if (!tag) {
            summary.push(line);
            continue;
        }
        const name = tag[1].toLowerCase();
        const rest = tag[2];
        if (name === 'param') {
            const parts = /^([A-Za-z_][A-Za-z0-9_]*)\s*[-:]?\s*(.*)$/.exec(rest);
            if (parts) params.push({ name: parts[1], rest: parts[2] });
        } else if (name === 'return' || name === 'returns') {
            returns = rest;
        } else if (name === 'deprecated') {
            deprecated = rest || 'This binding is deprecated.';
        } else if (name === 'example') {
            inExample = true;
            exampleBuf = '';
        }
    }
    if (inExample) examples.push(exampleBuf.trim());

    const out: string[] = [];
    if (deprecated) out.push(`**Deprecated:** ${deprecated}`, '');
    if (summary.length) out.push(summary.join('\n').trim());
    if (params.length) {
        out.push('', '**Parameters:**');
        for (const p of params) out.push(`  - \`${p.name}\` -- ${p.rest}`);
    }
    if (returns) {
        out.push('', `**Returns:** ${returns}`);
    }
    for (const ex of examples) {
        if (!ex) continue;
        out.push('', '**Example:**', '```cdo', ex, '```');
    }
    return out.join('\n').trim();
}

function stripCommentMarker(raw: string): string {
    /* Line comment values start with `//` (or `///`). Block comments start
     * with `/*`. We strip the lead-in and any common indentation. */
    let v = raw;
    if (v.startsWith('///')) v = v.slice(3);
    else if (v.startsWith('//')) v = v.slice(2);
    else if (v.startsWith('/*')) {
        v = v.slice(2);
        if (v.endsWith('*/')) v = v.slice(0, -2);
        /* For block comments, strip leading `*` per line. */
        v = v.split('\n').map(line => line.replace(/^\s*\*\s?/, '')).join('\n');
    }
    return v.trim();
}

function resolveIncludeAt(absPath: string, workspaceRoots: string[], stack: Set<string>): TypeRef | null {
    const ext = path.extname(absPath).toLowerCase();

    /* Native / binary modules: only the manifest can speak for their shape. */
    if (ext === '.so' || ext === '.dylib' || ext === '.dll') {
        const manifest = findManifestFor(absPath);
        return manifest ? { kind: 'manifest-exports', manifest } : null;
    }

    /* Data files: pretend they're any-keyed objects. */
    if (ext === '.json' || ext === '.yaml' || ext === '.yml' || ext === '.csv') {
        return { kind: 'object', members: new Map(), indexValue: ANY };
    }

    if (ext === '.cdo') {
        let mtimeMs = 0;
        try { mtimeMs = fs.statSync(absPath).mtimeMs; } catch { return null; }
        const cached = fsCache.get(absPath);
        if (cached && cached.mtimeMs === mtimeMs) return cached.doc.inferred.moduleType;

        let text: string;
        try { text = fs.readFileSync(absPath, 'utf8'); } catch { return null; }
        const uri = 'file://' + absPath.replace(/\\/g, '/');
        const next = new Set(stack);
        next.add(absPath);
        const doc = analyzeFresh(uri, text, 0, workspaceRoots, next);
        fsCache.set(absPath, { mtimeMs, doc });
        return doc.inferred.moduleType;
    }

    /* Unknown / no extension: try the neighbouring manifest as a last resort. */
    const manifest = findManifestFor(absPath);
    if (manifest) return { kind: 'manifest-exports', manifest };
    return null;
}

/* ----------------------------------------------------------------------- */
/* Re-exports                                                              */
/* ----------------------------------------------------------------------- */

export { resolveIncludePath, findManifestFor };
export type { Token, Range, Program, Node, Scope, Binding, ResolveResult, InferResult, ParseError };
