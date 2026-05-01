/*
 * Module manifests (`cando.api.json`).
 *
 * A manifest sits next to a binary or .cdo module and describes the value
 * the module returns from `include(...)`. The language server reads it to
 * power completion, hover and signature help on values that the runtime
 * itself can't be introspected (binary `.so`/`.dll` modules) or to
 * supplement what's visible in source (`.cdo` modules with native-style
 * sub-objects).
 *
 * Schema -- see `docs/cando-api-manifest.md` for the long form.
 *
 *   {
 *     "name":    "forms",
 *     "version": "1.0.0",
 *     "doc":     "Native UI toolkit.",
 *     "exports": {                       // the value `include(...)` returns
 *       "VERSION":       { "kind": "value", "type": "string" },
 *       "Button":        { "kind": "function",
 *                          "params": [{ "name": "parent", "type": "Control" }],
 *                          "returns": "Button" }
 *     },
 *     "types": {
 *       "Control": {                     // base class for every widget
 *         "doc":     "Common UI control surface.",
 *         "members": { "setText": { ... }, ... }
 *       },
 *       "Button": {
 *         "extends": "Control",          // semantic inheritance
 *         "indexes": ["Control"],        // mirrors the runtime's __index chain;
 *                                        // accepts an array for multiple parents
 *         "members": { "onClick": { "kind": "event", "signature": "function(self)" } }
 *       }
 *     }
 *   }
 *
 * `extends` and `indexes` are equivalent for completion / hover purposes;
 * they exist as separate fields so a manifest can mirror the runtime's
 * actual prototype-chain wiring (CanDo objects inherit through a `__index`
 * field) without losing the conceptual `extends` relation between types.
 */

import * as fs from 'fs';
import * as path from 'path';

export interface Param {
    name: string;
    type?: string;
    optional?: boolean;
    rest?: boolean;
    doc?: string;
}

export type MemberKind = 'function' | 'value' | 'event' | 'constant';

export interface MemberSpec {
    kind?: MemberKind;
    /* Type of the value (for non-function members) or member-of relationship. */
    type?: string;
    params?: Param[];
    /* Function/method return type; defaults to `void` if absent on a
     * function. Special value `"self"` (or `"this"`) means the method
     * returns its receiver's type, which is how fluent chaining like
     * `f:setText("hi"):center()` keeps its type. */
    returns?: string;
    doc?: string;
    /* Aliases that resolve to the same spec (`SetText` -> `setText` etc.). */
    aliases?: string[];
    /* Hand-written event signature, e.g. "function(self, x, y)". */
    signature?: string;
    /* When true the member is a method called via `:` syntax; the first
     * param is implicit `self`. Inferred from the manifest when omitted. */
    self?: boolean;
}

export interface TypeDef {
    doc?: string;
    /* A single parent type. The whole chain is included in completion. */
    extends?: string;
    /* Prototype-chain parents (`__index` value at runtime). Either a single
     * type name or an array of them. Equivalent to `extends` for typing
     * purposes, kept distinct so manifests can mirror runtime layout. */
    indexes?: string | string[];
    members: Record<string, MemberSpec>;
}

export interface Manifest {
    /* Logical module name (typically the directory name). */
    name: string;
    version?: string;
    doc?: string;
    /* Shape of the value `include("…")` returns for this module. */
    exports: Record<string, MemberSpec>;
    types?: Record<string, TypeDef>;
}

/* -------------------------------------------------------------------------
 * Loading + caching
 * ----------------------------------------------------------------------- */

interface CacheEntry {
    manifest: Manifest | null;
    mtimeMs: number;
}

const fileCache = new Map<string, CacheEntry>();

/**
 * Read and parse the manifest at `filePath`. Cached by mtime; a corrupt
 * file produces a one-time console warning and is treated as missing.
 */
export function loadManifestFile(filePath: string): Manifest | null {
    let mtimeMs = 0;
    try {
        mtimeMs = fs.statSync(filePath).mtimeMs;
    } catch {
        fileCache.delete(filePath);
        return null;
    }
    const cached = fileCache.get(filePath);
    if (cached && cached.mtimeMs === mtimeMs) return cached.manifest;

    let manifest: Manifest | null = null;
    try {
        const raw = fs.readFileSync(filePath, 'utf8');
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === 'object' && parsed.exports && typeof parsed.exports === 'object') {
            manifest = parsed as Manifest;
        }
    } catch (e) {
        /* Don't let a malformed manifest break completion for the rest of
         * the workspace. We swallow the error silently rather than
         * console.warn to avoid noise in the language-server output --
         * the editor's diagnostics layer is the right place to surface
         * authoring problems if we ever want to. */
        manifest = null;
    }

    fileCache.set(filePath, { manifest, mtimeMs });
    return manifest;
}

/**
 * Walk the directory of `modulePath` and find a `cando.api.json` (or a
 * sibling `<basename>.api.json`) describing it. Returns null if none is
 * present.
 *
 * The runtime's include() always returns a single value, so a per-module
 * manifest is the natural unit. We probe in this order:
 *   - `<dir>/<basename>.api.json` (lets one folder host several modules)
 *   - `<dir>/cando.api.json`      (the recommended default)
 */
export function findManifestFor(modulePath: string): Manifest | null {
    const dir = path.dirname(modulePath);
    const base = path.basename(modulePath, path.extname(modulePath));

    const candidates = [
        path.join(dir, base + '.api.json'),
        path.join(dir, 'cando.api.json')
    ];
    for (const c of candidates) {
        const m = loadManifestFile(c);
        if (m) return m;
    }
    return null;
}

/* -------------------------------------------------------------------------
 * Type-walk helpers
 * ----------------------------------------------------------------------- */

/**
 * Resolve every member visible on `typeName` within `manifest`, including
 * those inherited via `extends` and `indexes`. Closer definitions win;
 * cycles short-circuit.
 */
export function resolveTypeMembers(
    manifest: Manifest,
    typeName: string
): Map<string, MemberSpec> {
    const out = new Map<string, MemberSpec>();
    const seen = new Set<string>();
    const visit = (name: string): void => {
        if (seen.has(name)) return;
        seen.add(name);
        const def = manifest.types?.[name];
        if (!def) return;
        /* Walk parents first so closer (more-specific) definitions overwrite. */
        if (def.extends) visit(def.extends);
        if (def.indexes) {
            const list = Array.isArray(def.indexes) ? def.indexes : [def.indexes];
            for (const parent of list) visit(parent);
        }
        for (const [memberName, spec] of Object.entries(def.members ?? {})) {
            out.set(memberName, spec);
            for (const alias of spec.aliases ?? []) out.set(alias, spec);
        }
    };
    visit(typeName);
    return out;
}

/**
 * Same as resolveTypeMembers but for the manifest's top-level exports.
 * (Exports don't inherit from anything, but we still expand aliases.)
 */
export function resolveExportMembers(manifest: Manifest): Map<string, MemberSpec> {
    const out = new Map<string, MemberSpec>();
    for (const [name, spec] of Object.entries(manifest.exports ?? {})) {
        out.set(name, spec);
        for (const alias of spec.aliases ?? []) out.set(alias, spec);
    }
    return out;
}
