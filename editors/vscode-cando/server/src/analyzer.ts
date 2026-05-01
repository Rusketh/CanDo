/*
 * Walk a token stream from the lexer and produce a lightweight symbol table:
 *   - top-level VAR / CONST / GLOBAL bindings (and bindings inside FUNCTION
 *     bodies for parameter / locals capture)
 *   - FUNCTION declarations (with parameter names)
 *   - CLASS declarations with their methods and fields. CanDo's class
 *     statement is `CLASS Name [EXTENDS Parent] = [(params)] { body }`;
 *     methods are typically attached afterwards via
 *     `Name.method = FUNCTION(self) { ... }`. Both shapes are captured.
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
                let scan = i + 2;
                let parent: string | undefined;
                let detail = `CLASS ${nameTok.value}`;

                if (isKw(t[scan], 'EXTENDS') && t[scan + 1]?.kind === 'ident') {
                    parent = t[scan + 1].value;
                    detail += ` EXTENDS ${parent}`;
                    scan += 2;
                }

                /* `=` is mandatory in the statement form. */
                if (!isOp(t[scan], '=')) {
                    /* Not a class declaration we recognise -- skip and keep
                     * walking. */
                    continue;
                }
                scan++;

                /* Optional `(params)` for the constructor. */
                let params: Symbol[] = [];
                if (isPunct(t[scan], '(')) {
                    const collected = collectParams(scan);
                    params = collected.params;
                    scan = collected.end + 1;
                }

                /* Mandatory `{ body }`. */
                let bodyEnd = scan;
                const fields: Symbol[] = [];
                if (isPunct(t[scan], '{')) {
                    fields.push(...collectSelfAssignments(scan));
                    bodyEnd = skipBalanced(scan, '{', '}');
                }

                if (params.length) {
                    detail += `(${params.map(p => p.name).join(', ')})`;
                }

                const sym: Symbol = {
                    name: nameTok.value,
                    kind: 'class',
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail,
                    children: [...params, ...fields],
                    parameters: params.map(p => p.name)
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);

                /* Skip past the class body so its inner braces don't perturb
                 * the depth tracker. */
                i = bodyEnd - 1;
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

    /* Collect `self.X = ...` patterns inside the constructor body to surface
     * fields (and inline method assignments) on the class. */
    function collectSelfAssignments(headerEnd: number): Symbol[] {
        const members: Symbol[] = [];
        if (!isPunct(t[headerEnd], '{')) return members;
        const seen = new Set<string>();

        let j = headerEnd + 1;
        let d = 1;
        while (j < t.length && d > 0) {
            const tok = t[j];
            if (isPunct(tok, '{')) { d++; j++; continue; }
            if (isPunct(tok, '}')) { d--; j++; continue; }

            /* Look for `self.name = ...` or `self:name = ...` at any nesting
             * level inside the constructor. Don't restrict by depth -- this
             * is safe because we only key off the literal token sequence. */
            if (
                tok.kind === 'ident' && tok.value === 'self' &&
                isOp(t[j + 1], '.') &&
                t[j + 2]?.kind === 'ident' &&
                isOp(t[j + 3], '=')
            ) {
                const fieldTok = t[j + 2];
                if (!seen.has(fieldTok.value)) {
                    seen.add(fieldTok.value);
                    const isFn = isKw(t[j + 4], 'FUNCTION');
                    members.push({
                        name: fieldTok.value,
                        kind: isFn ? 'method' : 'field',
                        range: fieldTok.range,
                        selectionRange: fieldTok.range,
                        detail: isFn ? `self.${fieldTok.value}(...)` : `self.${fieldTok.value}`
                    });
                }
                j += 4;
                continue;
            }
            j++;
        }
        return members;
    }

    /* Second pass: methods are usually attached to a class *after* its body
     * via `Name.method = FUNCTION(...) { ... }`. Walk the whole token stream
     * once more and add those to the class symbol's children. */
    const classByName = new Map<string, Symbol>();
    for (const s of symbols) if (s.kind === 'class') classByName.set(s.name, s);

    if (classByName.size > 0) {
        for (let i = 0; i < t.length; i++) {
            const head = t[i];
            if (head.kind !== 'ident') continue;
            const cls = classByName.get(head.value);
            if (!cls) continue;
            if (!isOp(t[i + 1], '.')) continue;
            const memberTok = t[i + 2];
            if (memberTok?.kind !== 'ident') continue;
            if (!isOp(t[i + 3], '=')) continue;

            if (cls.children?.some(c => c.name === memberTok.value)) continue;

            const isFn = isKw(t[i + 4], 'FUNCTION');
            let params: Symbol[] = [];
            let detail = isFn ? `${cls.name}.${memberTok.value}(...)` : `${cls.name}.${memberTok.value}`;
            if (isFn && isPunct(t[i + 5], '(')) {
                const collected = collectParams(i + 5);
                params = collected.params;
                detail = `${cls.name}.${memberTok.value}(${params.map(p => p.name).join(', ')})`;
            }
            const child: Symbol = {
                name: memberTok.value,
                kind: isFn ? 'method' : 'field',
                range: memberTok.range,
                selectionRange: memberTok.range,
                detail,
                children: params,
                parameters: isFn ? params.map(p => p.name) : undefined
            };
            (cls.children ??= []).push(child);
        }
    }

    return { symbols, byName, includes, objectLiterals, moduleExports };
}
