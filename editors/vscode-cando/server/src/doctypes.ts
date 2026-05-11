/*
 * Recursive-descent parser for the doc-comment type mini-language.
 *
 * Grammar:
 *   type    := union
 *   union   := suffix ('|' suffix)*
 *   suffix  := atom ('[]' | '?')*           // postfix array / optional
 *   atom    := prim | object | fn | named | '(' type ')'
 *   prim    := 'null' | 'bool' | 'boolean' | 'number' | 'string'
 *            | 'thread' | 'any' | 'unknown' | 'value'
 *   object  := '{' ( field (',' field)* ','? )? '}'
 *   field   := ident ('?')? ':' type
 *   fn      := '(' params? ')' '->' returns
 *   params  := param (',' param)*
 *   param   := '...' ident (':' type)? | ident (':' type)?
 *   returns := type (',' type)*           // multi-return
 *   named   := IDENT                       // @shape / @callback alias
 *
 * Errors return null so callers fall back to UNKNOWN. We also produce
 * an `errors` list for advisory diagnostics.
 */

import {
    TypeRef, FunctionParam, MemberType,
    ANY, UNKNOWN, NULL_T, BOOL_T, NUM_T, STR_T, THREAD_T,
    arrayOf, optionalOf, unionOf, emptyObject
} from './typesys';

export interface DocTypeContext {
    /** Lookup table for `@shape Name` / `@callback Name` aliases visible
     *  in the file. */
    aliases: Map<string, TypeRef>;
}

export interface DocTypeResult {
    type: TypeRef | null;
    errors: string[];
}

export function parseDocType(text: string, ctx: DocTypeContext): DocTypeResult {
    const p = new TypeParser(text, ctx);
    const t = p.tryType();
    if (t === null) {
        p.errors.push(p.errors.length ? p.errors[0] : `Invalid type: "${text}"`);
        return { type: null, errors: p.errors };
    }
    p.skipWs();
    if (p.pos < p.src.length) {
        p.errors.push(`Unexpected "${p.src.slice(p.pos)}" at end of type`);
    }
    return { type: t, errors: p.errors };
}

/* ----------------------------------------------------------------------- */
/* Parser                                                                  */
/* ----------------------------------------------------------------------- */

class TypeParser {
    public pos = 0;
    public errors: string[] = [];
    constructor(public src: string, private ctx: DocTypeContext) {}

    /* ---- low-level scanning ---- */

    skipWs(): void {
        while (this.pos < this.src.length && /\s/.test(this.src[this.pos])) this.pos++;
    }

    peek(): string { return this.src[this.pos] ?? ''; }

    match(s: string): boolean {
        this.skipWs();
        if (this.src.startsWith(s, this.pos)) {
            this.pos += s.length;
            return true;
        }
        return false;
    }

    matchIdent(): string | null {
        this.skipWs();
        const m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(this.src.slice(this.pos));
        if (!m) return null;
        this.pos += m[0].length;
        return m[0];
    }

    /* ---- grammar productions ---- */

    tryType(): TypeRef | null {
        return this.parseUnion();
    }

    private parseUnion(): TypeRef | null {
        const first = this.parseSuffix();
        if (first === null) return null;
        const parts: TypeRef[] = [first];
        while (this.match('|')) {
            const next = this.parseSuffix();
            if (next === null) {
                this.errors.push('Expected type after "|"');
                return unionOf(parts);
            }
            parts.push(next);
        }
        return parts.length === 1 ? parts[0] : unionOf(parts);
    }

    private parseSuffix(): TypeRef | null {
        let t = this.parseAtom();
        if (t === null) return null;
        for (;;) {
            this.skipWs();
            if (this.match('[]')) { t = arrayOf(t); continue; }
            if (this.match('?'))  { t = optionalOf(t); continue; }
            break;
        }
        return t;
    }

    private parseAtom(): TypeRef | null {
        this.skipWs();
        const c = this.peek();
        if (c === '{') return this.parseObject();
        if (c === '(') {
            /* Could be a parenthesised type or a function `( ... ) -> ret`.
             * Snapshot, try function form first, fall back. */
            const save = this.pos;
            const fn = this.tryFunction();
            if (fn) return fn;
            this.pos = save;
            this.match('(');
            const inner = this.tryType();
            if (inner === null) { this.errors.push('Expected type inside parentheses'); return null; }
            if (!this.match(')')) { this.errors.push('Expected ")"'); return null; }
            return inner;
        }
        const id = this.matchIdent();
        if (!id) return null;
        const prim = primFromName(id);
        if (prim) return prim;
        if (id === 'Array' && this.match('<')) {
            const inner = this.tryType();
            if (inner === null) return null;
            if (!this.match('>')) { this.errors.push('Expected ">"'); return null; }
            return arrayOf(inner);
        }
        const alias = this.ctx.aliases.get(id);
        if (alias) return alias;
        /* Unknown named type — keep going but mark as ANY so member
         * completion is permissive rather than wrong. */
        this.errors.push(`Unknown type "${id}"`);
        return ANY;
    }

    private parseObject(): TypeRef | null {
        if (!this.match('{')) return null;
        const members = new Map<string, MemberType>();
        this.skipWs();
        if (this.match('}')) return { kind: 'object', members };
        while (true) {
            this.skipWs();
            const name = this.matchIdent();
            if (!name) { this.errors.push('Expected field name in object type'); return null; }
            const optional = this.match('?');
            if (!this.match(':')) { this.errors.push(`Expected ":" after field "${name}"`); return null; }
            const t = this.tryType();
            if (t === null) return null;
            const memberType = optional ? optionalOf(t) : t;
            members.set(name, { type: memberType });
            this.skipWs();
            if (this.match(',')) continue;
            if (this.match('}')) break;
            this.errors.push('Expected "," or "}" in object type');
            return null;
        }
        return { kind: 'object', members };
    }

    /** Attempts to parse a function literal starting at `(`. Returns null
     *  (without consuming) if it doesn't look like one. The caller must
     *  reset `pos` on null. */
    private tryFunction(): TypeRef | null {
        const start = this.pos;
        if (!this.match('(')) return null;
        const params: FunctionParam[] = [];
        this.skipWs();
        if (!this.match(')')) {
            while (true) {
                this.skipWs();
                const rest = this.match('...');
                const name = this.matchIdent();
                if (!name) { this.pos = start; return null; }
                let t: TypeRef = ANY;
                if (this.match(':')) {
                    const pt = this.tryType();
                    if (pt === null) { this.pos = start; return null; }
                    t = pt;
                }
                const p: FunctionParam = { name, type: t };
                if (rest) p.rest = true;
                params.push(p);
                if (this.match(',')) continue;
                if (this.match(')')) break;
                this.pos = start; return null;
            }
        }
        this.skipWs();
        if (!this.match('->')) { this.pos = start; return null; }
        const returns: TypeRef[] = [];
        const r0 = this.tryType();
        if (r0 === null) { this.pos = start; return null; }
        returns.push(r0);
        while (this.match(',')) {
            const rn = this.tryType();
            if (rn === null) { this.pos = start; return null; }
            returns.push(rn);
        }
        return { kind: 'function', params, returns };
    }
}

function primFromName(s: string): TypeRef | null {
    switch (s) {
        case 'null':    return NULL_T;
        case 'bool':    return BOOL_T;
        case 'boolean': return BOOL_T;
        case 'number':  return NUM_T;
        case 'string':  return STR_T;
        case 'thread':  return THREAD_T;
        case 'any':     return ANY;
        case 'value':   return ANY;
        case 'unknown': return UNKNOWN;
        case 'void':    return NULL_T;
    }
    return null;
}

/* ----------------------------------------------------------------------- */
/* Public helpers                                                          */
/* ----------------------------------------------------------------------- */

export function emptyDocTypeContext(): DocTypeContext {
    return { aliases: new Map() };
}
