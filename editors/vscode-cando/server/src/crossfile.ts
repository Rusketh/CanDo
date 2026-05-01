/*
 * Resolve `include("...")` arguments and harvest the exported member names
 * of the included file so that `name.member` completion can offer them.
 *
 * Supported sources:
 *   - .cdo  : lexed and analyzed; the keys of the top-level `RETURN { ... }`
 *             statement are returned. If there is no such statement we fall
 *             back to top-level VAR / CONST / GLOBAL / FUNCTION names.
 *   - .json : parsed; top-level object keys are returned.
 *   - .yaml / .yml / .csv / .so / .dylib / .dll : skipped (no static
 *             introspection possible).
 *
 * Results are cached per absolute path with the file's mtime so an edit to
 * the included module invalidates the cache automatically.
 */

import * as fs from 'fs';
import * as path from 'path';

import { Lexer } from './lexer';
import { analyze } from './analyzer';
import { resolveIncludePath } from './paths';

interface CacheEntry {
    members: string[];
    mtimeMs: number;
}

const cache = new Map<string, CacheEntry>();

export interface IncludeExports {
    members: string[];
    /* Resolved absolute path; useful for hover / diagnostics. */
    path: string;
}

export function getIncludeExports(
    rawPath: string,
    documentUri: string,
    workspaceRoots: string[]
): IncludeExports | null {
    const abs = resolveIncludePath(rawPath, documentUri, workspaceRoots);
    if (!abs) return null;

    let mtimeMs = 0;
    try {
        mtimeMs = fs.statSync(abs).mtimeMs;
    } catch {
        return null;
    }

    const cached = cache.get(abs);
    if (cached && cached.mtimeMs === mtimeMs) {
        return { members: cached.members, path: abs };
    }

    const members = readMembers(abs);
    cache.set(abs, { members, mtimeMs });
    return { members, path: abs };
}

function readMembers(abs: string): string[] {
    const ext = path.extname(abs).toLowerCase();
    let text: string;
    try {
        text = fs.readFileSync(abs, 'utf8');
    } catch {
        return [];
    }

    if (ext === '.cdo') {
        const tokens = new Lexer(text).tokenize();
        const result = analyze(tokens);
        if (result.moduleExports.length) return dedupe(result.moduleExports);
        /* Fallback: surface the module's top-level declarations. */
        return dedupe(result.symbols.map(s => s.name));
    }

    if (ext === '.json') {
        try {
            const parsed = JSON.parse(text);
            if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
                return Object.keys(parsed);
            }
        } catch { /* fallthrough */ }
        return [];
    }

    return [];
}

function dedupe(xs: string[]): string[] {
    return [...new Set(xs)];
}

/** Test hook: drop the cache (used to keep memory bounded across very long
 * sessions). Currently unused. */
export function clearCrossFileCache(): void {
    cache.clear();
}
