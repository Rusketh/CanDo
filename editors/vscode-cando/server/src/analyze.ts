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
    const { program, errors: parseErrors } = parse(tokens);
    const resolved = resolveScopes(program);
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
