/*
 * Doc-comment parser.
 *
 * Takes the raw text of a leading comment block (already stripped of
 * `//` / `///` / `/* *\/` markers and common indentation) and produces
 * a structured DocBlock describing the summary text and any recognised
 * `@tag` annotations.
 *
 * The tag vocabulary mirrors JSDoc/LuaLS conventions, restricted to what
 * the CanDo type system can represent. Unknown tags are preserved in
 * `unknownTags` so the LSP can surface advisory diagnostics without
 * losing the user's intent.
 *
 * Type-text after `{...}` is captured verbatim; parsing the type itself
 * is done lazily by doctypes.ts when the inferer needs a TypeRef.
 */

export interface DocBlock {
    /** Description text (everything that isn't a tag). Already trimmed. */
    description: string;
    /** Parsed tags, in source order. */
    tags: DocTag[];
    /** Tags whose name we didn't recognise. */
    unknownTags: RawTag[];
    /** Convenience: true iff a `@deprecated` tag is present. */
    deprecated?: string;
}

export type DocTag =
    | ParamTag
    | ReturnsTag
    | TypeTag
    | FieldTag
    | ShapeTag
    | CallbackTag
    | ClassTag
    | ThrowsTag
    | ThreadSafetyTag
    | DeprecatedTag
    | SeeTag
    | ExampleTag
    | RawTag;

export interface ParamTag {
    kind: 'param';
    name: string;
    typeText?: string;
    description: string;
}
export interface ReturnsTag {
    kind: 'returns';
    typeText?: string;
    description: string;
}
export interface TypeTag {
    kind: 'type';
    typeText: string;
    description: string;
}
export interface FieldTag {
    kind: 'field';
    name: string;
    typeText?: string;
    description: string;
}
export interface ShapeTag {
    kind: 'shape';
    name: string;
    typeText: string;
    description: string;
}
export interface CallbackTag {
    kind: 'callback';
    name: string;
    typeText: string;
    description: string;
}
export interface ClassTag {
    kind: 'class';
    name?: string;
    description: string;
}
export interface ThrowsTag {
    kind: 'throws';
    typeText?: string;
    description: string;
}
export interface ThreadSafetyTag {
    kind: 'thread-safe' | 'not-thread-safe';
    description: string;
}
export interface DeprecatedTag {
    kind: 'deprecated';
    description: string;
}
export interface SeeTag {
    kind: 'see';
    description: string;
}
export interface ExampleTag {
    kind: 'example';
    description: string;
}
export interface RawTag {
    kind: 'unknown';
    name: string;
    description: string;
}

/* ----------------------------------------------------------------------- */
/* Entry point                                                             */
/* ----------------------------------------------------------------------- */

export function parseDocBlock(raw: string): DocBlock {
    const lines = raw.split('\n');
    const summaryLines: string[] = [];
    const tags: DocTag[] = [];
    const unknown: RawTag[] = [];

    let i = 0;
    while (i < lines.length) {
        const line = lines[i];
        const tagMatch = /^\s*@([A-Za-z][A-Za-z0-9-_]*)\s*(.*)$/.exec(line);
        if (!tagMatch) {
            summaryLines.push(line);
            i++;
            continue;
        }
        const tagName = tagMatch[1].toLowerCase();
        /* Multi-line tag bodies: subsequent lines that don't start a new
         * tag get appended to this tag's body. */
        const body = [tagMatch[2]];
        let j = i + 1;
        while (j < lines.length && !/^\s*@[A-Za-z]/.test(lines[j])) {
            body.push(lines[j]);
            j++;
        }
        const tag = parseTag(tagName, joinBody(body));
        if (tag) {
            tags.push(tag);
            if (tag.kind === 'unknown') unknown.push(tag);
        }
        i = j;
    }

    let deprecated: string | undefined;
    for (const t of tags) {
        if (t.kind === 'deprecated') {
            deprecated = t.description || 'This binding is deprecated.';
        }
    }

    return {
        description: summaryLines.join('\n').trim(),
        tags,
        unknownTags: unknown,
        deprecated
    };
}

/* ----------------------------------------------------------------------- */
/* Per-tag dispatch                                                        */
/* ----------------------------------------------------------------------- */

function parseTag(name: string, body: string): DocTag | null {
    switch (name) {
        case 'param':    return parseParam(body);
        case 'arg':      return parseParam(body); // alias
        case 'return':   return parseReturns(body);
        case 'returns':  return parseReturns(body);
        case 'type':     return parseType(body);
        case 'field':    return parseField(body);
        case 'property': return parseField(body); // alias
        case 'shape':    return parseShape(body);
        case 'callback': return parseCallback(body);
        case 'class':    return parseClass(body);
        case 'throws':   return parseThrows(body);
        case 'throw':    return parseThrows(body); // alias
        case 'thread-safe':     return { kind: 'thread-safe', description: body.trim() };
        case 'threadsafe':      return { kind: 'thread-safe', description: body.trim() };
        case 'not-thread-safe': return { kind: 'not-thread-safe', description: body.trim() };
        case 'notthreadsafe':   return { kind: 'not-thread-safe', description: body.trim() };
        case 'deprecated': return { kind: 'deprecated', description: body.trim() };
        case 'see':      return { kind: 'see', description: body.trim() };
        case 'example':  return { kind: 'example', description: body.replace(/^\n+/, '').replace(/\n+$/, '') };
        default:         return { kind: 'unknown', name, description: body.trim() };
    }
}

/** Try to extract a brace-balanced `{type}` group starting at `body[i]`.
 *  Returns the inner type text and the index just past the closing `}`,
 *  or null if `body[i]` isn't `{` or the braces are unbalanced. */
function takeBraceGroup(body: string, i: number): { inner: string; next: number } | null {
    if (body[i] !== '{') return null;
    let depth = 0;
    for (let j = i; j < body.length; j++) {
        const c = body[j];
        if (c === '{') depth++;
        else if (c === '}') {
            depth--;
            if (depth === 0) return { inner: body.slice(i + 1, j).trim(), next: j + 1 };
        }
    }
    return null;
}

function skipSp(body: string, i: number): number {
    while (i < body.length && (body[i] === ' ' || body[i] === '\t')) i++;
    return i;
}

function takeIdent(body: string, i: number): { name: string; next: number } | null {
    const m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(body.slice(i));
    if (!m) return null;
    return { name: m[0], next: i + m[0].length };
}

function takeDashOrColon(body: string, i: number): number {
    if (body[i] === '-' || body[i] === ':') return skipSp(body, i + 1);
    return i;
}

/* @param name {type} description
 * @param name description           (no type)
 * @param {type} name description    (jsdoc order — also accepted)
 */
function parseParam(body: string): ParamTag | null {
    body = body.trim();
    let i = 0;
    /* @param {type} name desc */
    if (body[0] === '{') {
        const g = takeBraceGroup(body, 0);
        if (!g) return null;
        i = skipSp(body, g.next);
        const id = takeIdent(body, i);
        if (!id) return null;
        i = skipSp(body, id.next);
        i = takeDashOrColon(body, i);
        return { kind: 'param', name: id.name, typeText: g.inner, description: body.slice(i).trim() };
    }
    /* @param name {type}? desc */
    const id = takeIdent(body, 0);
    if (!id) return null;
    i = skipSp(body, id.next);
    let typeText: string | undefined;
    if (body[i] === '{') {
        const g = takeBraceGroup(body, i);
        if (g) {
            typeText = g.inner;
            i = skipSp(body, g.next);
        }
    }
    i = takeDashOrColon(body, i);
    const tag: ParamTag = { kind: 'param', name: id.name, description: body.slice(i).trim() };
    if (typeText !== undefined) tag.typeText = typeText;
    return tag;
}

function parseReturns(body: string): ReturnsTag {
    body = body.trim();
    if (body[0] === '{') {
        const g = takeBraceGroup(body, 0);
        if (g) {
            let i = skipSp(body, g.next);
            i = takeDashOrColon(body, i);
            return { kind: 'returns', typeText: g.inner, description: body.slice(i).trim() };
        }
    }
    return { kind: 'returns', description: body };
}

function parseType(body: string): TypeTag | null {
    body = body.trim();
    if (body[0] === '{') {
        const g = takeBraceGroup(body, 0);
        if (g) {
            const desc = body.slice(g.next).trim();
            return { kind: 'type', typeText: g.inner, description: desc };
        }
    }
    if (body.length) return { kind: 'type', typeText: body, description: '' };
    return null;
}

function parseField(body: string): FieldTag | null {
    body = body.trim();
    let i = 0;
    if (body[0] === '{') {
        const g = takeBraceGroup(body, 0);
        if (!g) return null;
        i = skipSp(body, g.next);
        const id = takeIdent(body, i);
        if (!id) return null;
        i = skipSp(body, id.next);
        i = takeDashOrColon(body, i);
        return { kind: 'field', name: id.name, typeText: g.inner, description: body.slice(i).trim() };
    }
    const id = takeIdent(body, 0);
    if (!id) return null;
    i = skipSp(body, id.next);
    let typeText: string | undefined;
    if (body[i] === '{') {
        const g = takeBraceGroup(body, i);
        if (g) {
            typeText = g.inner;
            i = skipSp(body, g.next);
        }
    }
    i = takeDashOrColon(body, i);
    const tag: FieldTag = { kind: 'field', name: id.name, description: body.slice(i).trim() };
    if (typeText !== undefined) tag.typeText = typeText;
    return tag;
}

/* @shape Name { k: T, k2: T2 }  description */
function parseShape(body: string): ShapeTag | null {
    body = body.trim();
    /* Match Name followed by a brace-balanced object type. */
    const nameMatch = /^([A-Za-z_][A-Za-z0-9_]*)\s*([\s\S]*)$/.exec(body);
    if (!nameMatch) return null;
    const rest = nameMatch[2].trimStart();
    if (!rest.startsWith('{')) return null;
    const end = findBalanced(rest, '{', '}');
    if (end < 0) return null;
    const typeText = rest.slice(0, end + 1);
    const description = rest.slice(end + 1).trim();
    return { kind: 'shape', name: nameMatch[1], typeText, description };
}

/* @callback Name (a: T, b: U) -> R   description */
function parseCallback(body: string): CallbackTag | null {
    body = body.trim();
    const nameMatch = /^([A-Za-z_][A-Za-z0-9_]*)\s*([\s\S]*)$/.exec(body);
    if (!nameMatch) return null;
    const rest = nameMatch[2].trimStart();
    if (!rest.startsWith('(')) return null;
    const close = findBalanced(rest, '(', ')');
    if (close < 0) return null;
    /* Look for `-> ReturnType` after the param list. */
    const afterParens = rest.slice(close + 1).trimStart();
    let typeText: string;
    let description: string;
    if (afterParens.startsWith('->')) {
        /* Greedy: take the return type up to the first 2+ whitespace
         * gap or newline that introduces description. */
        const retBody = afterParens.slice(2).trimStart();
        const nlIdx = retBody.indexOf('\n');
        const ret = nlIdx >= 0 ? retBody.slice(0, nlIdx).trim() : retBody.trim();
        typeText = rest.slice(0, close + 1) + ' -> ' + ret;
        description = nlIdx >= 0 ? retBody.slice(nlIdx + 1).trim() : '';
    } else {
        typeText = rest.slice(0, close + 1) + ' -> null';
        description = afterParens.trim();
    }
    return { kind: 'callback', name: nameMatch[1], typeText, description };
}

function parseClass(body: string): ClassTag {
    body = body.trim();
    const m = /^([A-Za-z_][A-Za-z0-9_]*)\s*([\s\S]*)$/.exec(body);
    if (m) return { kind: 'class', name: m[1], description: m[2].trim() };
    return { kind: 'class', description: body };
}

function parseThrows(body: string): ThrowsTag {
    body = body.trim();
    if (body[0] === '{') {
        const g = takeBraceGroup(body, 0);
        if (g) {
            let i = skipSp(body, g.next);
            i = takeDashOrColon(body, i);
            return { kind: 'throws', typeText: g.inner, description: body.slice(i).trim() };
        }
    }
    return { kind: 'throws', description: body };
}

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */

function joinBody(parts: string[]): string {
    /* The first element already lacks the @tag prefix. Trim each
     * subsequent line of its leading whitespace, preserve blank lines. */
    if (parts.length === 0) return '';
    const head = parts[0];
    if (parts.length === 1) return head;
    const tail = parts.slice(1).join('\n');
    return head + '\n' + tail;
}

/** Index of the matching close paren/brace; -1 if unbalanced.
 *  `s[0]` must equal `open`. */
function findBalanced(s: string, open: string, close: string): number {
    if (s[0] !== open) return -1;
    let depth = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        if (c === open) depth++;
        else if (c === close) {
            depth--;
            if (depth === 0) return i;
        }
    }
    return -1;
}
