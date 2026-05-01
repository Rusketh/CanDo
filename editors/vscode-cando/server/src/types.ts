/*
 * Lightweight type tracker.
 *
 * Walks a token stream once and builds a `TypeEnv` that maps every name
 * the script binds (`VAR`/`CONST`/`GLOBAL` declarations, plus the lhs of
 * top-level assignments) to a best-effort `TypeRef`. The tracker uses
 * three sources of truth:
 *
 *   1. In-file information from `analyze()` -- classes, object literals,
 *      `include(...)` bindings.
 *   2. Module manifests (`cando.api.json`) discovered next to a binary or
 *      .cdo target -- they describe the value `include(...)` returns and
 *      the named types reachable from it.
 *   3. The std-library namespace tables in `builtins.ts`.
 *
 * The tracker is deliberately conservative: when it can't make a confident
 * inference it returns `unknown`, and the completion code surfaces no
 * suggestions rather than misleading ones. It does NOT do flow-sensitive
 * narrowing, generics, or closure return-type inference.
 *
 * Inheritance:
 *   - `extends` on a manifest type behaves like classical single-inheritance.
 *   - `indexes` (single string or array) mirrors CanDo's runtime
 *     prototype-chain mechanism (`__index`) and walks the same way.
 *   - In user code, an object literal with an `__index: parent` field is
 *     recognised as a record that inherits from `parent`'s type.
 *   - `CLASS Foo EXTENDS Bar` walks the in-file class chain.
 */

import * as path from 'path';

import { Token } from './lexer';
import { Symbol as CandoSymbol } from './analyzer';
import {
    Manifest,
    MemberSpec,
    findManifestFor,
    resolveExportMembers,
    resolveTypeMembers
} from './manifest';
import { resolveIncludePath } from './paths';
import { NAMESPACES } from './builtins';

/* -------------------------------------------------------------------------
 * TypeRef
 * ----------------------------------------------------------------------- */

export type TypeRef =
    | { kind: 'primitive'; name: 'string' | 'number' | 'bool' | 'null' | 'array' | 'function' | 'void' | 'any' }
    | { kind: 'unknown' }
    | { kind: 'manifest-exports'; manifest: Manifest }
    | { kind: 'manifest-type'; manifest: Manifest; typeName: string }
    /** Anonymous record built from an object literal in the user's code.
     *  `indexParent` is set when the literal contained `__index: someExpr`. */
    | { kind: 'record'; members: Map<string, MemberSpec>; indexParent?: TypeRef }
    /** Reference to a `CLASS Foo { ... }` declared in the same file. */
    | { kind: 'in-file-class'; className: string };

const ANY: TypeRef = { kind: 'primitive', name: 'any' };
const UNKNOWN: TypeRef = { kind: 'unknown' };

/* -------------------------------------------------------------------------
 * Type environment
 * ----------------------------------------------------------------------- */

export interface TypeEnv {
    /* Variable / constant / parameter -> type. */
    bindings: Map<string, TypeRef>;
    /* In-file CLASS declarations, including parent (`EXTENDS`) chains. */
    classes: Map<string, CandoSymbol>;
    /* Manifests by their absolute file path; useful for hover detail. */
    manifests: Map<string, Manifest>;
    /* The document URI we built this env for; used to resolve include paths. */
    documentUri: string;
}

export function buildTypeEnv(
    tokens: Token[],
    inFileSymbols: CandoSymbol[],
    documentUri: string,
    workspaceRoots: string[]
): TypeEnv {
    const env: TypeEnv = {
        bindings: new Map(),
        classes: new Map(),
        manifests: new Map(),
        documentUri
    };

    /* Index in-file classes so EXTENDS chains can be walked later. */
    for (const s of inFileSymbols) {
        if (s.kind === 'class') env.classes.set(s.name, s);
    }

    /* Filter to the tokens the inference helpers care about. We keep
     * everything except whitespace-shaped tokens. */
    const t = tokens.filter(x => x.kind !== 'comment' && x.kind !== 'newline');

    /* First pass: capture every binding shape we recognise. */
    walkBindings(t, env, workspaceRoots);

    /* Second pass: pick up runtime member additions of the form
     *   name.member  = rhs;
     *   name:member  = rhs;
     * When `name` resolves to a record, augment its member set so the
     * captured field is offered on `name.` completion. This is the
     * canonical CanDo pattern for attaching methods after a literal:
     *
     *     VAR t = { };
     *     t.meth = FUNCTION(self) { ... };
     */
    augmentRecordsWithRuntimeMembers(t, env);

    return env;
}

function walkBindings(t: Token[], env: TypeEnv, workspaceRoots: string[]): void {
    for (let i = 0; i < t.length; i++) {
        const tok = t[i];

        /* CLASS declarations are recognised by the analyzer and registered in
         * env.classes. Skip past their `(params) { body }` here so the
         * type-tracker doesn't mistake `CLASS Foo = { }` for a bare
         * assignment binding `Foo` to an empty record. */
        if (tok.kind === 'keyword' && isOneOf(tok.value, 'CLASS')) {
            let j = i + 1;
            if (t[j]?.kind === 'ident') j++;
            if (t[j]?.kind === 'keyword' && isOneOf(t[j].value, 'EXTENDS')) {
                j++;
                if (t[j]?.kind === 'ident') j++;
            }
            if (t[j]?.kind === 'op' && t[j].value === '=') j++;
            if (t[j]?.kind === 'punct' && t[j].value === '(') {
                j = skipBalanced(t, j, '(', ')');
            }
            if (t[j]?.kind === 'punct' && t[j].value === '{') {
                j = skipBalanced(t, j, '{', '}');
            }
            i = j - 1;
            continue;
        }

        /* `VAR|CONST|GLOBAL name = rhs` */
        if (tok.kind === 'keyword' && isOneOf(tok.value, 'VAR', 'CONST', 'GLOBAL')) {
            const nameTok = t[i + 1];
            if (nameTok?.kind === 'ident') {
                if (t[i + 2]?.kind === 'op' && t[i + 2].value === '=') {
                    const { ref, next } = inferExpression(t, i + 3, env, workspaceRoots);
                    if (!env.bindings.has(nameTok.value)) {
                        env.bindings.set(nameTok.value, ref);
                    }
                    i = next - 1;
                    continue;
                }
                /* `VAR name;` -- declared but unbound. */
                env.bindings.set(nameTok.value, UNKNOWN);
            }
            continue;
        }

        /* Bare assignment: `name = rhs`. Only honoured when `name` isn't
         * already bound (so we don't clobber a stronger inference) AND isn't
         * the name of a class (class identifiers are a stronger hint than a
         * stray `Cls = ...` line). */
        if (tok.kind === 'ident') {
            const eq = t[i + 1];
            if (
                eq?.kind === 'op' && eq.value === '='
                && !env.bindings.has(tok.value)
                && !env.classes.has(tok.value)
            ) {
                const { ref, next } = inferExpression(t, i + 2, env, workspaceRoots);
                env.bindings.set(tok.value, ref);
                i = next - 1;
                continue;
            }
        }
    }
}

function augmentRecordsWithRuntimeMembers(t: Token[], env: TypeEnv): void {
    for (let i = 0; i < t.length; i++) {
        const head = t[i];
        if (head.kind !== 'ident') continue;
        const dot = t[i + 1];
        if (dot?.kind !== 'op' || (dot.value !== '.' && dot.value !== ':')) continue;
        const memberTok = t[i + 2];
        if (memberTok?.kind !== 'ident') continue;
        const eq = t[i + 3];
        if (eq?.kind !== 'op' || eq.value !== '=') continue;

        const ref = env.bindings.get(head.value);
        if (!ref || ref.kind !== 'record') continue;
        if (ref.members.has(memberTok.value)) continue;

        const isFn = t[i + 4]?.kind === 'keyword'
            && (t[i + 4].value === 'FUNCTION' || t[i + 4].value === 'function');
        ref.members.set(memberTok.value, isFn ? { kind: 'function' } : { kind: 'value' });
    }
}

/* -------------------------------------------------------------------------
 * Expression inference
 * ----------------------------------------------------------------------- */

/**
 * Read a primary expression starting at `tokens[i]`, then peel off any
 * trailing `.x` / `:x` / `(...)` / `[...]` postfixes. Returns the inferred
 * type and the index just past the consumed expression.
 *
 * `endExclusive`, if provided, caps how far postfix consumption may reach.
 * Used by `inferReceiverAt` to stop right before the dot we're completing
 * on rather than walking past it.
 */
export function inferExpression(
    tokens: Token[],
    i: number,
    env: TypeEnv,
    workspaceRoots: string[],
    endExclusive: number = tokens.length
): { ref: TypeRef; next: number } {
    const primary = inferPrimary(tokens, i, env, workspaceRoots);
    return inferPostfix(primary.ref, tokens, primary.next, env, workspaceRoots, endExclusive);
}

function inferPrimary(
    tokens: Token[],
    i: number,
    env: TypeEnv,
    workspaceRoots: string[]
): { ref: TypeRef; next: number } {
    const tok = tokens[i];
    if (!tok) return { ref: UNKNOWN, next: i };

    if (tok.kind === 'string' || tok.kind === 'template-string') {
        return { ref: { kind: 'primitive', name: 'string' }, next: i + 1 };
    }
    if (tok.kind === 'number') {
        return { ref: { kind: 'primitive', name: 'number' }, next: i + 1 };
    }
    if (tok.kind === 'keyword') {
        if (matches(tok.value, 'TRUE', 'FALSE')) return { ref: { kind: 'primitive', name: 'bool' }, next: i + 1 };
        if (matches(tok.value, 'NULL'))           return { ref: { kind: 'primitive', name: 'null' }, next: i + 1 };
        if (matches(tok.value, 'FUNCTION')) {
            /* Anonymous function literal -- skip its body and treat the value
             * as a generic function. */
            let j = i + 1;
            if (tokens[j]?.kind === 'punct' && tokens[j].value === '(') {
                j = skipBalanced(tokens, j, '(', ')');
            }
            if (tokens[j]?.kind === 'punct' && tokens[j].value === '{') {
                j = skipBalanced(tokens, j, '{', '}');
            }
            return { ref: { kind: 'primitive', name: 'function' }, next: j };
        }
    }

    if (tok.kind === 'punct' && tok.value === '[') {
        const end = skipBalanced(tokens, i, '[', ']');
        return { ref: { kind: 'primitive', name: 'array' }, next: end };
    }

    if (tok.kind === 'punct' && tok.value === '{') {
        const lit = readObjectLiteral(tokens, i, env, workspaceRoots);
        return { ref: lit.ref, next: lit.next };
    }

    if (tok.kind === 'punct' && tok.value === '(') {
        /* Parenthesised expression -- infer the inner expression then return
         * past the `)`. */
        const inner = inferExpression(tokens, i + 1, env, workspaceRoots);
        const end = skipBalanced(tokens, i, '(', ')');
        return { ref: inner.ref, next: end };
    }

    if (tok.kind === 'ident') {
        /* `include("path")` -- handled specially because the rhs determines
         * the entire module-export type. */
        if (tok.value === 'include' && tokens[i + 1]?.kind === 'punct' && tokens[i + 1].value === '(') {
            const arg = tokens[i + 2];
            const end = skipBalanced(tokens, i + 1, '(', ')');
            if (arg?.kind === 'string') {
                const ref = resolveIncludeType(unquote(arg.value), env, workspaceRoots);
                return { ref, next: end };
            }
            return { ref: UNKNOWN, next: end };
        }

        /* Standard-library namespace: `string`, `math`, ... become a
         * synthetic exports record so postfix `.method` works. */
        const ns = NAMESPACES.find(n => n.name === tok.value);
        if (ns) {
            return { ref: namespaceAsRecord(ns), next: i + 1 };
        }

        /* User-defined name. */
        const bound = env.bindings.get(tok.value);
        if (bound) return { ref: bound, next: i + 1 };

        /* Class names resolve as a function value -- `Cls(...)` constructs an
         * instance. */
        if (env.classes.has(tok.value)) {
            return { ref: { kind: 'in-file-class', className: tok.value }, next: i + 1 };
        }

        return { ref: UNKNOWN, next: i + 1 };
    }

    return { ref: UNKNOWN, next: i + 1 };
}

function inferPostfix(
    start: TypeRef,
    tokens: Token[],
    i: number,
    env: TypeEnv,
    workspaceRoots: string[],
    endExclusive: number = tokens.length
): { ref: TypeRef; next: number } {
    let ref = start;
    let j = i;

    while (j < endExclusive) {
        const tok = tokens[j];

        /* Member access:
         *   `.`  -- field / method lookup; if called, the result has the
         *           method's declared return type.
         *   `:`  -- method call; same lookup as `.`, but the runtime passes
         *           the receiver as the implicit first argument. From the
         *           type tracker's perspective `:` and `.` resolve the
         *           same way -- the difference is how the call is dispatched.
         *   `::` -- fluent chain. `t::method(...)` ALWAYS returns `t` (the
         *           receiver), regardless of what `method` actually returns.
         *           This is how scripts daisy-chain calls on objects whose
         *           methods don't explicitly `return self`.
         */
        if (
            tok.kind === 'op' &&
            (tok.value === '.' || tok.value === ':' || tok.value === '::')
        ) {
            const isChain = tok.value === '::';
            const nameTok = tokens[j + 1];
            if (nameTok?.kind !== 'ident') break;
            const member = lookupMemberSpec(ref, nameTok.value, env);
            if (!member) {
                /* `::` still preserves the receiver even if we can't look
                 * the member up -- you might be chaining through a runtime-
                 * defined helper. For `.` and `:`, an unknown member kills
                 * inference. */
                if (!isChain) ref = UNKNOWN;
                j += 2;
                continue;
            }
            const next = tokens[j + 2];
            const isCall = next?.kind === 'punct' && next.value === '(';
            if (isCall) {
                const callEnd = skipBalanced(tokens, j + 2, '(', ')');
                if (isChain || isSelfReturn(member.returns)) {
                    /* ref is unchanged: `::` is a fluent chain, and a
                     * `"returns": "self"` declaration explicitly says the
                     * method preserves its receiver's type. */
                } else {
                    ref = resolveTypeReference(member.returns ?? 'any', env);
                }
                j = callEnd;
                continue;
            }
            /* Bare member reference (no call). */
            if (isChain) {
                /* `t::method` without `()` is a syntax error in CanDo, but
                 * be defensive -- preserve the receiver. */
                j += 2;
                continue;
            }
            if (member.kind === 'function') {
                ref = { kind: 'primitive', name: 'function' };
            } else {
                ref = resolveTypeReference(member.type ?? 'any', env);
            }
            j += 2;
            continue;
        }

        /* Direct call `name(args)`. */
        if (tok.kind === 'punct' && tok.value === '(') {
            const end = skipBalanced(tokens, j, '(', ')');
            /* Calling a class returns an instance of it. CanDo classes are
             * callable directly (`Animal("Rex")`), no `NEW` keyword. */
            if (ref.kind !== 'in-file-class') {
                ref = UNKNOWN;
            }
            j = end;
            continue;
        }

        /* Index `name[expr]` -- element type unknown. */
        if (tok.kind === 'punct' && tok.value === '[') {
            j = skipBalanced(tokens, j, '[', ']');
            ref = UNKNOWN;
            continue;
        }

        break;
    }

    return { ref, next: j };
}

function readObjectLiteral(
    tokens: Token[],
    i: number,
    env: TypeEnv,
    workspaceRoots: string[]
): { ref: TypeRef; next: number } {
    /* tokens[i] is `{`. */
    const members = new Map<string, MemberSpec>();
    let indexParent: TypeRef | undefined;

    let j = i + 1;
    let depth = 1;
    while (j < tokens.length && depth > 0) {
        const tok = tokens[j];
        if (tok.kind === 'punct') {
            if (tok.value === '{') { depth++; j++; continue; }
            if (tok.value === '}') { depth--; j++; continue; }
            if (tok.value === '[') { j = skipBalanced(tokens, j, '[', ']'); continue; }
            if (tok.value === '(') { j = skipBalanced(tokens, j, '(', ')'); continue; }
        }
        if (depth === 1 && (tok.kind === 'ident' || tok.kind === 'keyword' || tok.kind === 'string')) {
            const colon = tokens[j + 1];
            if (colon?.kind === 'op' && colon.value === ':') {
                const key = tok.kind === 'string' ? unquote(tok.value) : tok.value;
                if (key === '__index') {
                    /* The value of `__index` becomes the prototype parent. */
                    const inner = inferExpression(tokens, j + 2, env, workspaceRoots);
                    indexParent = inner.ref;
                    j = inner.next;
                    continue;
                }
                if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) {
                    /* Best-effort: peek at the value to decide if the slot is
                     * a function or a plain value. */
                    const value = tokens[j + 2];
                    const isFn = value?.kind === 'keyword' && matches(value.value, 'FUNCTION');
                    members.set(key, isFn ? { kind: 'function' } : { kind: 'value' });
                }
            }
        }
        j++;
    }

    return { ref: { kind: 'record', members, indexParent }, next: j };
}

/* -------------------------------------------------------------------------
 * Member lookup (the engine behind `name.` completion)
 * ----------------------------------------------------------------------- */

/**
 * Return every member visible on `ref`, walking inheritance / __index
 * chains as appropriate. Closer definitions win.
 */
export function listMembers(ref: TypeRef, env: TypeEnv): Map<string, MemberSpec> {
    const out = new Map<string, MemberSpec>();
    const seen = new Set<string>();

    const visit = (current: TypeRef): void => {
        const key = describe(current);
        if (seen.has(key)) return;
        seen.add(key);

        switch (current.kind) {
            case 'manifest-exports': {
                const m = resolveExportMembers(current.manifest);
                m.forEach((spec, name) => { if (!out.has(name)) out.set(name, spec); });
                return;
            }
            case 'manifest-type': {
                const m = resolveTypeMembers(current.manifest, current.typeName);
                m.forEach((spec, name) => { if (!out.has(name)) out.set(name, spec); });
                return;
            }
            case 'record': {
                current.members.forEach((spec, name) => { if (!out.has(name)) out.set(name, spec); });
                if (current.indexParent) visit(current.indexParent);
                return;
            }
            case 'in-file-class': {
                const cls = env.classes.get(current.className);
                if (!cls) return;
                for (const child of cls.children ?? []) {
                    if (out.has(child.name)) continue;
                    out.set(child.name, {
                        kind: child.kind === 'method' ? 'function' : 'value',
                        doc: child.detail
                    });
                }
                /* CLASS Foo EXTENDS Bar -- follow the chain. */
                const parentName = cls.detail?.match(/EXTENDS\s+(\w+)/)?.[1];
                if (parentName) visit({ kind: 'in-file-class', className: parentName });
                return;
            }
            case 'primitive':
            case 'unknown':
                return;
        }
    };

    visit(ref);
    return out;
}

function lookupMemberSpec(ref: TypeRef, name: string, env: TypeEnv): MemberSpec | null {
    const all = listMembers(ref, env);
    return all.get(name) ?? null;
}

/* -------------------------------------------------------------------------
 * Type-string resolution (manifest "type" / "returns" -> TypeRef)
 * ----------------------------------------------------------------------- */

function isSelfReturn(s: string | undefined): boolean {
    return s === 'self' || s === 'this' || s === 'Self';
}

function resolveTypeReference(typeStr: string, env: TypeEnv): TypeRef {
    if (!typeStr) return ANY;
    const t = typeStr.trim();
    switch (t) {
        case 'string':         return { kind: 'primitive', name: 'string' };
        case 'number':         return { kind: 'primitive', name: 'number' };
        case 'bool':
        case 'boolean':        return { kind: 'primitive', name: 'bool' };
        case 'null':           return { kind: 'primitive', name: 'null' };
        case 'array':          return { kind: 'primitive', name: 'array' };
        case 'function':       return { kind: 'primitive', name: 'function' };
        case 'void':           return { kind: 'primitive', name: 'void' };
        case 'any':            return ANY;
        case 'object':         return { kind: 'record', members: new Map() };
    }

    /* Cross-manifest reference: `forms.TextBox`. Search every manifest
     * we've seen for the named type. */
    const dot = t.indexOf('.');
    if (dot > 0) {
        const ns = t.slice(0, dot);
        const ty = t.slice(dot + 1);
        for (const m of env.manifests.values()) {
            if (m.name === ns && m.types?.[ty]) {
                return { kind: 'manifest-type', manifest: m, typeName: ty };
            }
        }
    }

    /* Otherwise look it up in any manifest currently loaded for this env. */
    for (const m of env.manifests.values()) {
        if (m.types?.[t]) return { kind: 'manifest-type', manifest: m, typeName: t };
    }

    /* Fallback: in-file class. */
    if (env.classes.has(t)) return { kind: 'in-file-class', className: t };

    return UNKNOWN;
}

function resolveIncludeType(rawPath: string, env: TypeEnv, workspaceRoots: string[]): TypeRef {
    /* First try the runtime-equivalent resolution: file must exist on disk. */
    const abs = resolveIncludePath(rawPath, env.documentUri, workspaceRoots);
    if (abs) {
        const manifest = findManifestFor(abs);
        if (manifest) {
            env.manifests.set(abs, manifest);
            return { kind: 'manifest-exports', manifest };
        }
    }

    /* Fallback: a manifest can exist alongside a binary that hasn't been
     * built yet. Probe the directory the include path *would* resolve to
     * and look for the manifest there. */
    const speculative = speculativeManifestPath(rawPath, env.documentUri, workspaceRoots);
    if (speculative) {
        const manifest = findManifestFor(speculative);
        if (manifest) {
            env.manifests.set(speculative, manifest);
            return { kind: 'manifest-exports', manifest };
        }
    }

    /* No manifest -- defer to crossfile-derived members. The completion
     * layer falls back to `getIncludeExports` for this case. */
    return UNKNOWN;
}

/**
 * Compute the absolute path that `rawPath` would resolve to (relative to the
 * document) without requiring the file to exist. Used to find manifests
 * that ship alongside an unbuilt binary.
 */
function speculativeManifestPath(
    rawPath: string,
    documentUri: string,
    workspaceRoots: string[]
): string | null {
    const docPath = uriToFsPath(documentUri);
    if (!docPath) return null;
    const docDir = path.dirname(docPath);
    if (path.isAbsolute(rawPath)) return rawPath;
    /* Walk up from the document's directory, mirroring the runtime's
     * search order, and stop at the first ancestor that contains a
     * directory matching the relative include head. */
    let cur = docDir;
    const stop = workspaceRoots.map(r => path.resolve(r));
    for (let i = 0; i < 16; i++) {
        const candidate = path.resolve(cur, rawPath);
        if (findManifestFor(candidate)) return candidate;
        if (stop.includes(path.resolve(cur))) break;
        const parent = path.dirname(cur);
        if (parent === cur) break;
        cur = parent;
    }
    /* No ancestor turned up a manifest -- fall back to the document's own
     * directory so the caller can still ask for one. */
    return path.resolve(docDir, rawPath);
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

/* -------------------------------------------------------------------------
 * Reverse helpers (used by completion at a cursor position)
 * ----------------------------------------------------------------------- */

/**
 * Given a token array and a cursor position (the index of the token right
 * after the trailing `.` or `:`), walk backwards to find the start of the
 * receiver expression and return its inferred type. Used to make
 * `forms.createTextBox(parent).` complete `TextBox`'s members.
 */
export function inferReceiverAt(
    tokens: Token[],
    dotIndex: number,
    env: TypeEnv,
    workspaceRoots: string[]
): TypeRef {
    /* Walk back, skipping balanced `()` / `[]` groups, to find the start of
     * the chain. Anything other than ident / `.` / `:` / `::` / `(` / `[`
     * ends the receiver. */
    let start = dotIndex - 1;
    while (start >= 0) {
        const tok = tokens[start];
        if (tok.kind === 'punct' && (tok.value === ')' || tok.value === ']')) {
            const open = tok.value === ')' ? '(' : '[';
            const close = tok.value;
            start = skipBalancedBackwards(tokens, start, open, close) - 1;
            continue;
        }
        if (tok.kind === 'ident') { start--; continue; }
        if (tok.kind === 'op' && (tok.value === '.' || tok.value === ':' || tok.value === '::')) {
            start--; continue;
        }
        break;
    }
    start++;
    if (start >= dotIndex) return UNKNOWN;
    const { ref } = inferExpression(tokens, start, env, workspaceRoots, dotIndex);
    return ref;
}

/* -------------------------------------------------------------------------
 * Tiny utilities
 * ----------------------------------------------------------------------- */

function isOneOf(v: string, ...names: string[]): boolean {
    return names.some(n => v === n || v === n.toLowerCase());
}

function matches(v: string, ...names: string[]): boolean {
    return names.some(n => v === n || v === n.toLowerCase());
}

function unquote(raw: string): string {
    if (raw.length >= 2) {
        const a = raw[0], b = raw[raw.length - 1];
        if ((a === '"' || a === '\'' || a === '`') && a === b) return raw.slice(1, -1);
    }
    return raw;
}

function skipBalanced(tokens: Token[], from: number, open: string, close: string): number {
    let depth = 0;
    let j = from;
    while (j < tokens.length) {
        const tok = tokens[j];
        if (tok.kind === 'punct' && tok.value === open) depth++;
        else if (tok.kind === 'punct' && tok.value === close) {
            depth--;
            if (depth === 0) return j + 1;
        }
        j++;
    }
    return j;
}

function skipBalancedBackwards(tokens: Token[], from: number, open: string, close: string): number {
    let depth = 0;
    let j = from;
    while (j >= 0) {
        const tok = tokens[j];
        if (tok.kind === 'punct' && tok.value === close) depth++;
        else if (tok.kind === 'punct' && tok.value === open) {
            depth--;
            if (depth === 0) return j;
        }
        j--;
    }
    return j;
}

function namespaceAsRecord(ns: { name: string; members: string[]; memberDetails?: Record<string, string> }): TypeRef {
    const members = new Map<string, MemberSpec>();
    for (const m of ns.members) {
        members.set(m, {
            kind: 'function',
            doc: ns.memberDetails?.[m] ?? `${ns.name}.${m}`
        });
    }
    return { kind: 'record', members };
}

function describe(ref: TypeRef): string {
    switch (ref.kind) {
        case 'primitive':         return `prim:${ref.name}`;
        case 'unknown':           return 'unknown';
        case 'manifest-exports':  return `mfst:${ref.manifest.name}:exports`;
        case 'manifest-type':     return `mfst:${ref.manifest.name}:${ref.typeName}`;
        case 'in-file-class':     return `cls:${ref.className}`;
        case 'record': {
            const keys = [...ref.members.keys()].sort().join(',');
            return `rec:[${keys}]:${ref.indexParent ? describe(ref.indexParent) : ''}`;
        }
    }
}

/* -------------------------------------------------------------------------
 * Public hover / display helpers
 * ----------------------------------------------------------------------- */

export function describeTypeForHover(ref: TypeRef): string {
    switch (ref.kind) {
        case 'primitive':         return ref.name;
        case 'unknown':           return 'any';
        case 'manifest-exports':  return `${ref.manifest.name} (module)`;
        case 'manifest-type':     return `${ref.manifest.name}.${ref.typeName}`;
        case 'in-file-class':     return ref.className;
        case 'record': {
            const keys = [...ref.members.keys()].slice(0, 4).join(', ');
            const more = ref.members.size > 4 ? ', …' : '';
            return `{ ${keys}${more} }`;
        }
    }
}

export function formatMemberDetail(spec: MemberSpec, name: string): string {
    if (spec.kind === 'function') {
        const params = (spec.params ?? []).map(p => {
            const t = p.type ? `: ${p.type}` : '';
            const opt = p.optional ? '?' : '';
            const rest = p.rest ? '...' : '';
            return `${rest}${p.name}${opt}${t}`;
        }).join(', ');
        const ret = spec.returns ? ` -> ${spec.returns}` : '';
        return `${name}(${params})${ret}`;
    }
    if (spec.kind === 'event') {
        return `${name} ${spec.signature ?? '(event)'}`;
    }
    if (spec.type) return `${name}: ${spec.type}`;
    return name;
}

