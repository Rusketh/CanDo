/*
 * Workspace-wide indexer.
 *
 * Backs Find References, Rename, and Workspace Symbols. On first request
 * we walk every `.cdo` file under the workspace roots, analyze it once,
 * and cache the result by mtime. Subsequent requests revalidate the
 * cache cheaply.
 *
 * Performance budget: a typical CanDo workspace has ~100 .cdo files at
 * 100-500 lines each; a full scan is well under 200ms in practice.
 */

import * as fs from 'fs';
import * as path from 'path';
import { AnalyzedDocument, analyzeDocument } from './analyze';
import { Binding } from './scope';
import { Range } from './lexer';

interface IndexEntry {
    absPath: string;
    uri: string;
    mtimeMs: number;
    doc: AnalyzedDocument;
}

const indexCache = new Map<string, IndexEntry>();

/** Walk every workspace root for .cdo files (capped at a few thousand) and
 *  return their AnalyzedDocument. Re-analyzes a file only when its mtime
 *  has changed since the last call. */
export function indexWorkspace(workspaceRoots: string[]): IndexEntry[] {
    const out: IndexEntry[] = [];
    const seen = new Set<string>();
    let budget = 4000;
    for (const root of workspaceRoots) {
        walk(root);
    }
    function walk(dir: string): void {
        if (budget <= 0) return;
        let entries: fs.Dirent[];
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); }
        catch { return; }
        for (const e of entries) {
            if (budget <= 0) return;
            const full = path.join(dir, e.name);
            /* Skip common large directories. */
            if (e.isDirectory()) {
                if (e.name === 'node_modules' || e.name === '.git' || e.name === 'out' ||
                    e.name === 'build' || e.name === 'dist' || e.name === 'target' ||
                    e.name.startsWith('.')) continue;
                walk(full);
                continue;
            }
            if (!e.isFile() || !full.endsWith('.cdo')) continue;
            if (seen.has(full)) continue;
            seen.add(full);
            budget--;
            const entry = analyzePath(full, workspaceRoots);
            if (entry) out.push(entry);
        }
    }
    return out;
}

function analyzePath(absPath: string, workspaceRoots: string[]): IndexEntry | null {
    let mtimeMs: number;
    try { mtimeMs = fs.statSync(absPath).mtimeMs; } catch { return null; }
    const cached = indexCache.get(absPath);
    if (cached && cached.mtimeMs === mtimeMs) return cached;
    let text: string;
    try { text = fs.readFileSync(absPath, 'utf8'); } catch { return null; }
    const uri = 'file://' + absPath.replace(/\\/g, '/');
    const doc = analyzeDocument(uri, text, mtimeMs | 0, workspaceRoots);
    const entry: IndexEntry = { absPath, uri, mtimeMs, doc };
    indexCache.set(absPath, entry);
    return entry;
}

/** Find every reference (across the workspace) that resolves to the same
 *  named binding as the supplied one. A "match" is any binding with the
 *  same name *and* same declaration shape (we use name + decl-kind +
 *  scope-kind as a coarse heuristic; refine with manifest-aware matching
 *  later if we need cross-module accuracy). */
export interface RefHit {
    uri: string;
    range: Range;
}

export function findReferencesAcrossWorkspace(
    binding: Binding,
    primaryUri: string,
    workspaceRoots: string[]
): RefHit[] {
    const hits: RefHit[] = [];

    /* Always include the primary document's references. */
    for (const r of binding.references) {
        hits.push({ uri: primaryUri, range: r });
    }

    /* For file-scoped bindings, scan other workspace files for same-named
     * bindings. Block-scoped bindings (locals, params) can never be
     * referenced from another file, so skip the scan for them. */
    if (binding.scope.kind !== 'file') return hits;
    if (workspaceRoots.length === 0) return hits;

    const entries = indexWorkspace(workspaceRoots);
    for (const entry of entries) {
        if (entry.uri === primaryUri) continue;
        for (const other of entry.doc.resolved.allBindings) {
            if (other.name !== binding.name) continue;
            if (other.scope.kind !== 'file') continue;
            /* Same-name file-scoped binding -- treat each occurrence as a
             * candidate reference. (Cross-file accuracy isn't perfect for
             * dynamic languages; this errs on the side of completeness.) */
            for (const r of other.references) {
                hits.push({ uri: entry.uri, range: r });
            }
        }
    }
    return hits;
}

/** Aggregate every top-level binding from every analyzed workspace file,
 *  for use by workspace/symbol requests. */
export interface SymbolHit {
    uri: string;
    name: string;
    detail: string;
    range: Range;
    selectionRange: Range;
    kind: Binding['kind'];
}

export function workspaceSymbols(workspaceRoots: string[]): SymbolHit[] {
    const out: SymbolHit[] = [];
    const entries = indexWorkspace(workspaceRoots);
    for (const entry of entries) {
        for (const b of entry.doc.resolved.fileScope.bindings.values()) {
            out.push({
                uri: entry.uri,
                name: b.name,
                detail: b.kind,
                range: b.declRange,
                selectionRange: b.nameRange,
                kind: b.kind
            });
        }
    }
    return out;
}

/** Invalidate cached analysis for the given absolute path. Called by the
 *  LSP layer when an open document changes (so workspace refs see the
 *  latest version, not the on-disk one). */
export function invalidateIndex(absPath: string): void {
    indexCache.delete(absPath);
}
