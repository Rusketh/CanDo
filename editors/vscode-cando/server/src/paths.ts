/*
 * Filesystem-aware completion helpers for `include(...)` strings.
 *
 * CanDo's include() resolver (source/lib/include.c) accepts:
 *   - .cdo, .json, .csv, .yaml, .yml  (data / source modules)
 *   - .so, .dylib, .dll                (binary modules)
 *   - bare paths with no extension     (probed in the order so → dylib → dll → cdo)
 *
 * Resolution starts in the calling script's directory and walks up to the
 * process cwd. We mirror that on the editor side: we look in the document's
 * directory first and then its ancestors, stopping at the workspace root
 * (or the filesystem root if no workspace is open).
 */

import * as fs from 'fs';
import * as path from 'path';
import { CompletionItem, CompletionItemKind, InsertReplaceEdit, Range as LspRange } from 'vscode-languageserver/node';

const INCLUDABLE_EXT = new Set([
    '.cdo', '.json', '.csv', '.yaml', '.yml',
    '.so', '.dylib', '.dll'
]);

export interface IncludeStringContext {
    /* Text the user has already typed inside the quotes, up to the cursor. */
    typed: string;
    /* The opening quote character. */
    quote: string;
    /* Range in the document covering `typed` (so completions can replace it
     * cleanly when the user is editing in the middle of a path). */
    range: LspRange;
}

/**
 * If `offset` falls inside the string argument of an `include(...)` call,
 * return a context describing what the user has typed so far. Otherwise
 * `null`.
 *
 * The detection is intentionally permissive: we walk backwards from the
 * cursor across plain string content, then expect `("` or `('` and the bare
 * word `include` immediately before that. This keeps the parser dependency
 * minimal and works inside half-typed expressions.
 */
export function detectIncludeString(text: string, offset: number): IncludeStringContext | null {
    /* Walk back to the opening quote, bailing on newline or another delimiter. */
    let i = offset - 1;
    while (i >= 0) {
        const c = text[i];
        if (c === '"' || c === '\'') break;
        if (c === '\n') return null;
        if (c === '`') return null;
        i--;
    }
    if (i < 0) return null;
    const quote = text[i];
    const stringStart = i + 1;

    /* Skip whitespace before the quote. */
    let j = i - 1;
    while (j >= 0 && (text[j] === ' ' || text[j] === '\t')) j--;
    if (text[j] !== '(') return null;

    /* Skip whitespace before the paren and check for the `include` identifier. */
    j--;
    while (j >= 0 && (text[j] === ' ' || text[j] === '\t')) j--;
    const wordEnd = j + 1;
    while (j >= 0 && /[A-Za-z0-9_]/.test(text[j])) j--;
    const word = text.slice(j + 1, wordEnd);
    if (word !== 'include') return null;

    const typed = text.slice(stringStart, offset);
    return {
        typed,
        quote,
        range: {
            start: offsetToPosition(text, stringStart),
            end: offsetToPosition(text, offset)
        }
    };
}

/**
 * Build completion items for the partial path `ctx.typed`, resolved relative
 * to the document at `documentUri`. `workspaceRoots` bounds the upward search
 * just like the runtime resolver bounds itself to cwd.
 */
export function completeIncludePath(
    ctx: IncludeStringContext,
    documentUri: string,
    workspaceRoots: string[]
): CompletionItem[] {
    const docPath = uriToFsPath(documentUri);
    if (!docPath) return [];

    /* Decide which directory the user's typed prefix is anchored to. */
    const docDir = path.dirname(docPath);
    const typed = ctx.typed;
    const isExplicitlyRelative = typed.startsWith('./') || typed.startsWith('../') || typed.startsWith('/');
    const lastSep = typed.lastIndexOf('/');
    const dirPart = lastSep >= 0 ? typed.slice(0, lastSep + 1) : '';
    const filePart = lastSep >= 0 ? typed.slice(lastSep + 1) : typed;

    const items = new Map<string, CompletionItem>();
    const seenDirs = new Set<string>();

    const searchRoots: string[] = [];
    if (path.isAbsolute(typed) || typed.startsWith('/')) {
        searchRoots.push(path.parse(docPath).root);
    } else {
        /* Walk from the document's directory up toward the workspace root,
         * mirroring the include() resolver. */
        let cur = docDir;
        const stop = workspaceRoots.length
            ? workspaceRoots.map(r => path.resolve(r))
            : [path.parse(docDir).root];
        while (true) {
            searchRoots.push(cur);
            if (stop.includes(path.resolve(cur))) break;
            const parent = path.dirname(cur);
            if (parent === cur) break;
            cur = parent;
            /* Cap the walk so a deeply nested file doesn't blow up. */
            if (searchRoots.length > 16) break;
        }
        /* For explicitly-relative prefixes we should not climb. */
        if (isExplicitlyRelative) searchRoots.length = 1;
    }

    for (const root of searchRoots) {
        const dir = path.resolve(root, dirPart || '.');
        if (seenDirs.has(dir)) continue;
        seenDirs.add(dir);

        let entries: fs.Dirent[];
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        } catch {
            continue;
        }

        for (const ent of entries) {
            if (ent.name.startsWith('.')) continue;
            if (ent.isDirectory()) {
                const insertText = dirPart + ent.name + '/';
                items.set(insertText, makeItem(ent.name + '/', insertText, ctx, CompletionItemKind.Folder, dir));
                continue;
            }
            if (!ent.isFile()) continue;
            const ext = path.extname(ent.name).toLowerCase();
            if (!INCLUDABLE_EXT.has(ext)) continue;
            const insertText = dirPart + ent.name;
            items.set(insertText, makeItem(ent.name, insertText, ctx, kindForExt(ext), dir));
        }
    }

    /* Lightly prefer entries whose basename matches the user's filePart prefix. */
    const lower = filePart.toLowerCase();
    for (const it of items.values()) {
        const base = (it.label as string).toLowerCase();
        it.sortText = (base.startsWith(lower) ? '0_' : '1_') + base;
    }

    return [...items.values()];
}

function makeItem(
    label: string,
    insertText: string,
    ctx: IncludeStringContext,
    kind: CompletionItemKind,
    dir: string
): CompletionItem {
    const edit: InsertReplaceEdit = {
        newText: insertText,
        insert: ctx.range,
        replace: ctx.range
    };
    return {
        label,
        kind,
        detail: dir,
        textEdit: edit,
        /* Trigger another completion round after a folder is inserted, so the
         * user sees its contents immediately. */
        command: insertText.endsWith('/')
            ? { command: 'editor.action.triggerSuggest', title: 'Re-trigger' }
            : undefined
    };
}

function kindForExt(ext: string): CompletionItemKind {
    switch (ext) {
        case '.cdo': return CompletionItemKind.Module;
        case '.json':
        case '.yaml':
        case '.yml':
        case '.csv': return CompletionItemKind.File;
        case '.so':
        case '.dylib':
        case '.dll': return CompletionItemKind.Module;
        default: return CompletionItemKind.File;
    }
}

/**
 * Resolve an `include("path")` argument to an absolute filesystem path,
 * mirroring source/lib/include.c. Tries each candidate extension when the
 * path has none. Returns `null` if nothing exists.
 */
export function resolveIncludePath(
    raw: string,
    documentUri: string,
    workspaceRoots: string[]
): string | null {
    const docPath = uriToFsPath(documentUri);
    if (!docPath) return null;
    const docDir = path.dirname(docPath);

    const candidates: string[] = [];
    const probe = (base: string) => {
        if (path.extname(base)) {
            candidates.push(base);
        } else {
            for (const ext of ['.cdo', '.json', '.yaml', '.yml', '.csv']) {
                candidates.push(base + ext);
            }
        }
    };

    if (path.isAbsolute(raw)) {
        probe(raw);
    } else {
        let cur = docDir;
        const stop = workspaceRoots.map(r => path.resolve(r));
        const tried = new Set<string>();
        while (!tried.has(cur)) {
            tried.add(cur);
            probe(path.resolve(cur, raw));
            if (stop.includes(path.resolve(cur))) break;
            const parent = path.dirname(cur);
            if (parent === cur) break;
            cur = parent;
            if (tried.size > 16) break;
        }
    }

    for (const c of candidates) {
        try {
            const st = fs.statSync(c);
            if (st.isFile()) return c;
        } catch { /* not present, keep trying */ }
    }
    return null;
}

/**
 * Lightweight `file://` URI -> filesystem path decoder. Avoids pulling in
 * `vscode-uri` since the language-server bundle only ever sees file URIs.
 */
function uriToFsPath(uri: string): string | null {
    if (!uri.startsWith('file://')) return null;
    let p = uri.slice('file://'.length);
    /* Strip optional authority component (`file://host/path` -> `/path`). */
    const slash = p.indexOf('/');
    if (slash > 0) p = p.slice(slash);
    try {
        p = decodeURIComponent(p);
    } catch {
        /* Leave unencoded characters as-is. */
    }
    /* Windows: `/C:/foo` -> `C:/foo`. */
    if (/^\/[A-Za-z]:/.test(p)) p = p.slice(1);
    return p;
}

function offsetToPosition(text: string, offset: number): { line: number; character: number } {
    let line = 0;
    let lineStart = 0;
    for (let i = 0; i < offset; i++) {
        if (text[i] === '\n') {
            line++;
            lineStart = i + 1;
        }
    }
    return { line, character: offset - lineStart };
}
