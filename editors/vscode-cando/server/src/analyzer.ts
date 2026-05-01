/*
 * Walk a token stream from the lexer and produce a lightweight symbol table:
 *   - top-level VAR / CONST / GLOBAL bindings (and bindings inside FUNCTION
 *     bodies for parameter / locals capture)
 *   - FUNCTION declarations (with parameter names)
 *   - CLASS declarations with their methods and fields
 *   - Object literals bound to top-level VAR/CONST/GLOBAL (for `name.member`
 *     completion)
 *   - `include("path")` bindings (for cross-file member completion)
 *   - Module exports via a top-level `RETURN { ... }` literal
 *
 * This is intentionally shallow -- enough to drive completion, document
 * symbols and go-to-definition, not enough to be a real semantic analyzer.
 */

import { Token, Range } from './lexer';

export type SymbolKind = 'function' | 'class' | 'variable' | 'constant' | 'parameter' | 'method' | 'field';

export interface Symbol {
    name: string;
    kind: SymbolKind;
    range: Range;          /* the identifier itself */
    selectionRange: Range; /* same as range; kept separate to mirror LSP */
    detail?: string;
    children?: Symbol[];
    parameters?: string[]; /* for functions / methods */
}

export interface IncludeBinding {
    /* The path string with quotes already stripped. */
    path: string;
    /* The string token's range, useful for hover / goto. */
    range: Range;
}

export interface AnalysisResult {
    symbols: Symbol[];
    /* Flat name -> first occurrence map for goto-def. */
    byName: Map<string, Symbol>;
    /* `VAR/CONST/GLOBAL name = include("path")` bindings. */
    includes: Map<string, IncludeBinding>;
    /* `VAR/CONST/GLOBAL name = { a: ..., b: ... }` -- captured top-level keys. */
    objectLiterals: Map<string, string[]>;
    /* Object literal keys from the last top-level `RETURN { ... }` statement,
     * used to surface a module's exports to its callers. */
    moduleExports: string[];
}

export function analyze(tokens: Token[]): AnalysisResult {
    const symbols: Symbol[] = [];
    const byName = new Map<string, Symbol>();
    const includes = new Map<string, IncludeBinding>();
    const objectLiterals = new Map<string, string[]>();
    let moduleExports: string[] = [];

    /* Filter out comments and newlines so the index-arithmetic below is
     * straightforward. Strings, numbers and template-strings are kept so
     * we can read RHS values. */
    const t: Token[] = tokens.filter(x => x.kind !== 'comment' && x.kind !== 'newline');

    const isKw = (tok: Token | undefined, ...names: string[]): boolean => {
        if (!tok || tok.kind !== 'keyword') return false;
        const v = tok.value;
        return names.some(n => v === n || v === n.toLowerCase());
    };
    const isPunct = (tok: Token | undefined, v: string): boolean =>
        !!tok && tok.kind === 'punct' && tok.value === v;
    const isOp = (tok: Token | undefined, v: string): boolean =>
        !!tok && tok.kind === 'op' && tok.value === v;

    /* ---------------------------------------------------------------
     * Helpers
     * ------------------------------------------------------------- */

    /* Skip a balanced bracket starting at `from` (inclusive of opening token).
     * Returns the index just past the closing bracket, or t.length if EOF. */
    function skipBalanced(from: number, open: string, close: string): number {
        let depth = 0;
        let j = from;
        while (j < t.length) {
            if (isPunct(t[j], open)) depth++;
            else if (isPunct(t[j], close)) {
                depth--;
                if (depth === 0) return j + 1;
            }
            j++;
        }
        return j;
    }

    /* Strip surrounding quote characters from a string token's raw value. */
    function unquote(raw: string): string {
        if (raw.length >= 2) {
            const a = raw[0];
            const b = raw[raw.length - 1];
            if ((a === '"' || a === '\'' || a === '`') && a === b) {
                return raw.slice(1, -1);
            }
        }
        return raw;
    }

    /* Collect identifier keys from an object literal whose opening `{` is at
     * index `start`. Returns the keys and the index just past `}`. */
    function collectObjectKeys(start: number): { keys: string[]; end: number } {
        const keys: string[] = [];
        if (!isPunct(t[start], '{')) return { keys, end: start };
        let j = start + 1;
        let depth = 1;
        while (j < t.length && depth > 0) {
            const tok = t[j];
            if (isPunct(tok, '{')) { depth++; j++; continue; }
            if (isPunct(tok, '}')) { depth--; j++; continue; }
            if (isPunct(tok, '[')) { j = skipBalanced(j, '[', ']'); continue; }
            if (isPunct(tok, '(')) { j = skipBalanced(j, '(', ')'); continue; }
            /* Only collect identifiers at the literal's top level. */
            if (depth === 1 && (tok.kind === 'ident' || tok.kind === 'keyword')) {
                const next = t[j + 1];
                if (isOp(next, ':')) {
                    keys.push(tok.value);
                }
            }
            /* Quoted string keys: `"foo": ...` */
            if (depth === 1 && tok.kind === 'string') {
                const next = t[j + 1];
                if (isOp(next, ':')) {
                    const k = unquote(tok.value);
                    if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) keys.push(k);
                }
            }
            j++;
        }
        return { keys, end: j };
    }

    /* Collect parameters between `(` at idx and matching `)`. Returns names
     * and the index of the closing paren (or last index if unbalanced). */
    function collectParams(openIdx: number): { params: Symbol[]; end: number } {
        const params: Symbol[] = [];
        if (!isPunct(t[openIdx], '(')) return { params, end: openIdx };
        let j = openIdx + 1;
        let depth = 1;
        while (j < t.length && depth > 0) {
            const tok = t[j];
            if (isPunct(tok, '(')) depth++;
            else if (isPunct(tok, ')')) {
                depth--;
                if (depth === 0) break;
            }
            else if (depth === 1 && tok.kind === 'ident') {
                /* Only the first identifier in each comma-separated slot,
                 * to skip default-value expressions. */
                const prev = t[j - 1];
                if (isPunct(prev, '(') || isPunct(prev, ',')) {
                    params.push({
                        name: tok.value,
                        kind: 'parameter',
                        range: tok.range,
                        selectionRange: tok.range
                    });
                }
            }
            j++;
        }
        return { params, end: j };
    }

    /* ---------------------------------------------------------------
     * Walk tokens
     * ------------------------------------------------------------- */

    let depth = 0;

    for (let i = 0; i < t.length; i++) {
        const tok = t[i];

        if (isPunct(tok, '{')) { depth++; continue; }
        if (isPunct(tok, '}')) { depth = Math.max(0, depth - 1); continue; }

        /* CLASS body capture happens regardless of depth (handled inline). */
        if (depth === 0 && isKw(tok, 'CLASS')) {
            const nameTok = t[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                let detail = `CLASS ${nameTok.value}`;
                let scan = i + 2;
                if (isKw(t[i + 2], 'EXTENDS') && t[i + 3]?.kind === 'ident') {
                    detail += ` EXTENDS ${t[i + 3].value}`;
                    scan = i + 4;
                }
                const members = collectClassMembers(scan);
                const sym: Symbol = {
                    name: nameTok.value,
                    kind: 'class',
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail,
                    children: members
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);
                /* Skip past the class body so its inner `{` doesn't confuse depth. */
                if (isPunct(t[scan], '{')) {
                    i = skipBalanced(scan, '{', '}') - 1;
                }
                continue;
            }
        }

        /* Top-level FUNCTION declarations only -- methods are caught above. */
        if (depth === 0 && isKw(tok, 'FUNCTION')) {
            const nameTok = t[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                const { params, end } = collectParams(i + 2);
                const sym: Symbol = {
                    name: nameTok.value,
                    kind: 'function',
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail: `FUNCTION ${nameTok.value}(${params.map(p => p.name).join(', ')})`,
                    children: params,
                    parameters: params.map(p => p.name)
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);
                i = end;
                continue;
            }
        }

        /* VAR / CONST / GLOBAL name [= rhs] */
        if (depth === 0 && isKw(tok, 'VAR', 'CONST', 'GLOBAL')) {
            const nameTok = t[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                const kind: SymbolKind = isKw(tok, 'CONST') ? 'constant' : 'variable';
                const sym: Symbol = {
                    name: nameTok.value,
                    kind,
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail: `${tok.value.toUpperCase()} ${nameTok.value}`
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);

                /* Inspect the RHS for include() / object-literal bindings. */
                if (isOp(t[i + 2], '=')) {
                    const rhs = i + 3;
                    /* include("path") */
                    if (
                        t[rhs]?.kind === 'ident' &&
                        t[rhs]?.value === 'include' &&
                        isPunct(t[rhs + 1], '(') &&
                        t[rhs + 2]?.kind === 'string'
                    ) {
                        const strTok = t[rhs + 2]!;
                        includes.set(nameTok.value, {
                            path: unquote(strTok.value),
                            range: strTok.range
                        });
                    }
                    /* { a: 1, b: 2 } */
                    if (isPunct(t[rhs], '{')) {
                        const { keys } = collectObjectKeys(rhs);
                        if (keys.length) objectLiterals.set(nameTok.value, keys);
                    }
                }
                continue;
            }
        }

        /* Top-level `RETURN { ... }` -- capture as module exports. */
        if (depth === 0 && isKw(tok, 'RETURN')) {
            if (isPunct(t[i + 1], '{')) {
                const { keys, end } = collectObjectKeys(i + 1);
                if (keys.length) moduleExports = keys;
                i = end - 1;
                continue;
            }
        }
    }

    /* CLASS body member collection: starts at the `{` after the class header. */
    function collectClassMembers(headerEnd: number): Symbol[] {
        const members: Symbol[] = [];
        if (!isPunct(t[headerEnd], '{')) return members;

        let j = headerEnd + 1;
        let d = 1;
        while (j < t.length && d > 0) {
            const tok = t[j];
            if (isPunct(tok, '{')) { d++; j++; continue; }
            if (isPunct(tok, '}')) { d--; j++; continue; }

            if (d === 1) {
                /* Optional STATIC / PRIVATE / ASYNC modifier prefix. */
                let k = j;
                while (isKw(t[k], 'STATIC', 'PRIVATE', 'ASYNC')) k++;

                if (isKw(t[k], 'FUNCTION')) {
                    const nameTok = t[k + 1];
                    if (nameTok && nameTok.kind === 'ident') {
                        const { params, end } = collectParams(k + 2);
                        members.push({
                            name: nameTok.value,
                            kind: 'method',
                            range: nameTok.range,
                            selectionRange: nameTok.range,
                            detail: `FUNCTION ${nameTok.value}(${params.map(p => p.name).join(', ')})`,
                            children: params,
                            parameters: params.map(p => p.name)
                        });
                        /* Skip the method body. */
                        const bodyOpen = end + 1;
                        if (isPunct(t[bodyOpen], '{')) {
                            j = skipBalanced(bodyOpen, '{', '}');
                            continue;
                        }
                        j = end + 1;
                        continue;
                    }
                }
                if (isKw(t[k], 'VAR', 'CONST', 'GLOBAL')) {
                    const nameTok = t[k + 1];
                    if (nameTok && nameTok.kind === 'ident') {
                        members.push({
                            name: nameTok.value,
                            kind: 'field',
                            range: nameTok.range,
                            selectionRange: nameTok.range,
                            detail: `${t[k].value.toUpperCase()} ${nameTok.value}`
                        });
                        /* Advance past the declaration so we don't re-enter
                         * on the same VAR after a STATIC/PRIVATE prefix. */
                        j = k + 2;
                        continue;
                    }
                }
            }
            j++;
        }
        return members;
    }

    return { symbols, byName, includes, objectLiterals, moduleExports };
}
