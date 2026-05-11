/*
 * Type representation, factories, rendering, member enumeration and flow
 * narrowing helpers for the CanDo type system.
 *
 * The runtime is dynamically typed; this module captures a *best-effort*
 * structural type for each binding so completion/hover can describe what
 * the editor expects to be valid at any given point. Where we can't make
 * a confident inference we fall back to UNKNOWN (which suppresses
 * possibly-wrong completions) or ANY (which means "anything goes").
 */

import { Range } from './lexer';
import { Manifest, MemberSpec, Param as ManifestParam, TypeDef, resolveTypeMembers, resolveExportMembers } from './manifest';

/* ----------------------------------------------------------------------- */
/* TypeRef                                                                 */
/* ----------------------------------------------------------------------- */

export type TypeRef =
    | PrimType
    | ArrayType
    | ObjectType
    | FunctionType
    | ClassType
    | UnionType
    | OptionalType
    | TupleType
    | ManifestExportsType
    | ManifestNamedType
    | NamespaceType;

export interface PrimType { kind: 'prim'; name: 'any' | 'unknown' | 'null' | 'bool' | 'number' | 'string' | 'thread'; }
export interface ArrayType { kind: 'array'; element: TypeRef; }
export interface ObjectType {
    kind: 'object';
    members: Map<string, MemberType>;
    /** Type of values held under arbitrary keys (set when we observe
     *  `obj[k] = v` assignments). */
    indexValue?: TypeRef;
    /** Prototype-chain parent (mirrors runtime `__index`). */
    prototype?: TypeRef;
    /** Optional class name for nicer rendering. */
    className?: string;
}
export interface FunctionType {
    kind: 'function';
    params: FunctionParam[];
    /** Return values, positionally. A multi-return function has length > 1. */
    returns: TypeRef[];
    /** When the function is the body of a method declared via `:` syntax
     *  the receiver type goes here so signature help can describe it. */
    self?: TypeRef;
    /** When set, the function returns its receiver type unchanged
     *  (mirrors manifest `returns: "self"`). */
    returnsSelf?: boolean;
    /** When set, the function takes `...rest` of this element type. */
    rest?: TypeRef;
    /** Optional human-readable name for hover. */
    name?: string;
    /** Optional doc text. */
    doc?: string;
    /** Optional source location of the declaration. */
    defRange?: Range;
}
export interface FunctionParam {
    name: string;
    type: TypeRef;
    optional?: boolean;
    rest?: boolean;
    doc?: string;
}
export interface ClassType {
    kind: 'class';
    name: string;
    /** Type of an instance produced by calling the class. */
    instance: ObjectType;
    /** Constructor params. */
    ctorParams: FunctionParam[];
    /** Reference to the same in-file class symbol, if any. */
    defRange?: Range;
}
export interface UnionType { kind: 'union'; variants: TypeRef[]; }
export interface OptionalType { kind: 'optional'; inner: TypeRef; }
export interface TupleType { kind: 'tuple'; items: TypeRef[]; }
export interface ManifestExportsType { kind: 'manifest-exports'; manifest: Manifest; }
export interface ManifestNamedType { kind: 'manifest-type'; manifest: Manifest; typeName: string; }

export interface NamespaceInfo {
    name: string;
    doc?: string;
    members: Map<string, MemberType>;
}
export interface NamespaceType { kind: 'namespace'; info: NamespaceInfo; }

export interface MemberType {
    type: TypeRef;
    doc?: string;
    /** Source location of the declaration (when we saw one). */
    defRange?: Range;
    /** Manifest spec backing this member, if any. Used by hover to keep
     *  formatting parity with the legacy 'kind/params/returns' display. */
    manifestSpec?: MemberSpec;
    /** Marked when the manifest registered this as an event. */
    isEvent?: boolean;
    /** Marked when this member is declared CONST. */
    readOnly?: boolean;
}

/* ----------------------------------------------------------------------- */
/* Constants                                                               */
/* ----------------------------------------------------------------------- */

export const ANY: TypeRef     = { kind: 'prim', name: 'any' };
export const UNKNOWN: TypeRef = { kind: 'prim', name: 'unknown' };
export const NULL_T: TypeRef  = { kind: 'prim', name: 'null' };
export const BOOL_T: TypeRef  = { kind: 'prim', name: 'bool' };
export const NUM_T: TypeRef   = { kind: 'prim', name: 'number' };
export const STR_T: TypeRef   = { kind: 'prim', name: 'string' };
export const THREAD_T: TypeRef = { kind: 'prim', name: 'thread' };

/* ----------------------------------------------------------------------- */
/* Factories                                                               */
/* ----------------------------------------------------------------------- */

export function arrayOf(element: TypeRef): ArrayType {
    return { kind: 'array', element };
}

export function tupleOf(items: TypeRef[]): TupleType {
    return { kind: 'tuple', items };
}

export function emptyObject(): ObjectType {
    return { kind: 'object', members: new Map() };
}

export function optionalOf(inner: TypeRef): TypeRef {
    if (inner.kind === 'optional') return inner;
    if (inner.kind === 'prim' && inner.name === 'null') return inner;
    return { kind: 'optional', inner };
}

export function unionOf(variants: TypeRef[]): TypeRef {
    const flat: TypeRef[] = [];
    for (const v of variants) {
        if (v.kind === 'union') flat.push(...v.variants);
        else flat.push(v);
    }
    /* Dedupe by structural identity (rendered form). */
    const seen = new Set<string>();
    const out: TypeRef[] = [];
    for (const v of flat) {
        const k = renderType(v);
        if (!seen.has(k)) { seen.add(k); out.push(v); }
    }
    if (out.length === 0) return UNKNOWN;
    if (out.length === 1) return out[0];
    return { kind: 'union', variants: out };
}

/* ----------------------------------------------------------------------- */
/* Multi-return helpers                                                    */
/* ----------------------------------------------------------------------- */

/** Get the value type at multi-value position `i`, with the rest treated
 *  as NULL (matches runtime). */
export function tupleAt(t: TypeRef, i: number): TypeRef {
    if (t.kind === 'tuple') return t.items[i] ?? NULL_T;
    return i === 0 ? t : NULL_T;
}

/** Collapse a multi-return tuple to its first value (single-value
 *  contexts: paren-wrapped call, expression in non-trailing position). */
export function firstOf(t: TypeRef): TypeRef {
    return t.kind === 'tuple' ? (t.items[0] ?? NULL_T) : t;
}

/* ----------------------------------------------------------------------- */
/* Member enumeration                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Enumerate all member names visible on `t`. Walks `prototype` chains for
 * object types and `extends`/`indexes` chains for manifest types.
 *
 * Returns a fresh map; callers may mutate it freely.
 */
export function enumerateMembers(t: TypeRef): Map<string, MemberType> {
    const out = new Map<string, MemberType>();
    collectMembers(t, out, new Set());
    return out;
}

function collectMembers(t: TypeRef, out: Map<string, MemberType>, seen: Set<TypeRef>): void {
    if (seen.has(t)) return;
    seen.add(t);
    switch (t.kind) {
        case 'object': {
            /* Walk prototype first so closer members override. */
            if (t.prototype) collectMembers(t.prototype, out, seen);
            for (const [name, m] of t.members) out.set(name, m);
            return;
        }
        case 'class': {
            collectMembers(t.instance, out, seen);
            return;
        }
        case 'manifest-exports': {
            const exp = resolveExportMembers(t.manifest);
            for (const [name, spec] of exp) {
                out.set(name, memberFromManifestSpec(t.manifest, spec));
            }
            return;
        }
        case 'manifest-type': {
            const members = resolveTypeMembers(t.manifest, t.typeName);
            for (const [name, spec] of members) {
                out.set(name, memberFromManifestSpec(t.manifest, spec));
            }
            return;
        }
        case 'namespace': {
            for (const [name, m] of t.info.members) out.set(name, m);
            return;
        }
        case 'optional': {
            collectMembers(t.inner, out, seen);
            return;
        }
        case 'union': {
            /* Use the union (not intersection) of variant members. The user
             * is asking "what's available here?"; if a variant is `null` we
             * want to suggest the non-null variant's members instead of
             * nothing. The first non-null variant wins on conflicts so the
             * "real" type drives the rendering. */
            for (let i = t.variants.length - 1; i >= 0; i--) {
                const v = t.variants[i];
                if (v.kind === 'prim' && v.name === 'null') continue;
                const tmp = new Map<string, MemberType>();
                collectMembers(v, tmp, new Set());
                for (const [name, m] of tmp) out.set(name, m);
            }
            return;
        }
        case 'array': {
            /* Array members come from the std-library `array` namespace; the
             * inference engine injects those when the cursor is on an array
             * receiver. We expose `length` here as a placeholder so the user
             * always sees something. */
            out.set('length', { type: NUM_T, doc: 'Number of elements.' });
            out.set('push',   { type: methodTypeFromSig('push(value)'),   doc: 'Append a value to the array.' });
            out.set('pop',    { type: methodTypeFromSig('pop() -> value'), doc: 'Remove and return the last element.' });
            out.set('map',    { type: methodTypeFromSig('map(fn) -> array'), doc: 'Map each element through `fn`.' });
            out.set('filter', { type: methodTypeFromSig('filter(fn) -> array'), doc: 'Keep elements where `fn` returns truthy.' });
            return;
        }
        case 'prim':
        case 'tuple':
            return;
    }
}

/** Build a synthetic FunctionType from a short signature string -- used to
 *  give the user something useful when the manifest doesn't provide one. */
function methodTypeFromSig(sig: string): FunctionType {
    return {
        kind: 'function',
        params: [],
        returns: [UNKNOWN],
        name: sig
    };
}

function memberFromManifestSpec(manifest: Manifest, spec: MemberSpec): MemberType {
    return {
        type: typeFromManifestSpec(manifest, spec),
        doc: spec.doc,
        manifestSpec: spec,
        isEvent: spec.kind === 'event',
        readOnly: spec.kind === 'constant'
    };
}

/* ----------------------------------------------------------------------- */
/* Manifest -> TypeRef bridge                                              */
/* ----------------------------------------------------------------------- */

export function typeFromManifestSpec(manifest: Manifest, spec: MemberSpec): TypeRef {
    /* Function/method members. `event` and `signature`-only entries are
     * treated as functions too. */
    if (spec.kind === 'function' || spec.kind === 'event' || spec.params || spec.signature) {
        const params: FunctionParam[] = (spec.params ?? []).map(p => ({
            name: p.name,
            type: typeFromManifestRef(manifest, p.type),
            optional: !!p.optional,
            rest: !!p.rest,
            doc: p.doc
        }));
        const ret = (spec.returns === 'self' || spec.returns === 'this')
            ? UNKNOWN
            : typeFromManifestRef(manifest, spec.returns);
        return {
            kind: 'function',
            params,
            returns: [ret],
            returnsSelf: spec.returns === 'self' || spec.returns === 'this',
            doc: spec.doc
        };
    }
    /* value / constant members. */
    return typeFromManifestRef(manifest, spec.type);
}

export function typeFromManifestRef(manifest: Manifest, ref: string | undefined): TypeRef {
    if (!ref) return ANY;
    switch (ref.toLowerCase()) {
        case 'string':    return STR_T;
        case 'number':    return NUM_T;
        case 'bool':
        case 'boolean':   return BOOL_T;
        case 'null':      return NULL_T;
        case 'void':      return NULL_T;
        case 'any':       return ANY;
        case 'array':     return arrayOf(ANY);
        case 'function':  return { kind: 'function', params: [], returns: [ANY] };
        case 'thread':    return THREAD_T;
    }
    /* Generic `array<T>` shorthand. */
    const arrayMatch = /^array<(.+)>$/.exec(ref.trim());
    if (arrayMatch) return arrayOf(typeFromManifestRef(manifest, arrayMatch[1]));
    /* Named manifest type. */
    if (manifest.types && manifest.types[ref]) {
        return { kind: 'manifest-type', manifest, typeName: ref };
    }
    /* Unknown ref name -- treat as ANY rather than UNKNOWN so the editor
     * still shows members, since manifests may forward-declare types. */
    return ANY;
}

/* ----------------------------------------------------------------------- */
/* Rendering                                                               */
/* ----------------------------------------------------------------------- */

/** Render a TypeRef for hover / completion details. Designed to be short
 *  and consistent: `string`, `array<Foo>`, `(p1: T, p2: T) -> R`, etc. */
export function renderType(t: TypeRef): string {
    return renderTypeImpl(t, new Set());
}

function renderTypeImpl(t: TypeRef, seen: Set<TypeRef>): string {
    if (seen.has(t)) return '...';
    switch (t.kind) {
        case 'prim': return t.name;
        case 'array':
            return `array<${renderTypeImpl(t.element, withSeen(seen, t))}>`;
        case 'object': {
            if (t.className) return t.className;
            const keys = [...t.members.keys()];
            if (keys.length === 0) return 'object';
            const head = keys.slice(0, 3).join(', ');
            const more = keys.length > 3 ? `, …` : '';
            return `{ ${head}${more} }`;
        }
        case 'function': {
            const ps = t.params.map(p => {
                const t2 = renderTypeImpl(p.type, withSeen(seen, t));
                const sigil = p.rest ? '...' : '';
                const opt = p.optional ? '?' : '';
                return `${sigil}${p.name}${opt}: ${t2}`;
            }).join(', ');
            const ret = t.returnsSelf
                ? 'self'
                : t.returns.length === 1
                    ? renderTypeImpl(t.returns[0], withSeen(seen, t))
                    : t.returns.map(r => renderTypeImpl(r, withSeen(seen, t))).join(', ');
            return `(${ps}) -> ${ret}`;
        }
        case 'class':
            return `class ${t.name}`;
        case 'union':
            return t.variants.map(v => renderTypeImpl(v, withSeen(seen, t))).join(' | ');
        case 'optional':
            return `${renderTypeImpl(t.inner, withSeen(seen, t))}?`;
        case 'tuple':
            return t.items.map(i => renderTypeImpl(i, withSeen(seen, t))).join(', ');
        case 'manifest-exports':
            return `module ${t.manifest.name}`;
        case 'manifest-type':
            return t.typeName;
        case 'namespace':
            return `${t.info.name} (std)`;
    }
}

function withSeen(seen: Set<TypeRef>, t: TypeRef): Set<TypeRef> {
    const next = new Set(seen);
    next.add(t);
    return next;
}

/* ----------------------------------------------------------------------- */
/* Flow narrowing                                                          */
/* ----------------------------------------------------------------------- */

/** Strip `null` and `false`-only types from a union, yielding the "truthy"
 *  side. Used by IF/ELSE IF narrowing. */
export function narrowTruthy(t: TypeRef): TypeRef {
    if (t.kind === 'optional') return t.inner;
    if (t.kind === 'union') {
        const out = t.variants.filter(v => !isAlwaysFalsy(v));
        if (out.length === 0) return UNKNOWN;
        if (out.length === 1) return out[0];
        return { kind: 'union', variants: out };
    }
    if (t.kind === 'prim' && t.name === 'null') return UNKNOWN;
    return t;
}

/** Narrow to only the `null`/`false` variants (else branch). */
export function narrowFalsy(t: TypeRef): TypeRef {
    if (t.kind === 'optional') return NULL_T;
    if (t.kind === 'union') {
        const out = t.variants.filter(v => isAlwaysFalsy(v));
        if (out.length === 0) return NULL_T;
        if (out.length === 1) return out[0];
        return { kind: 'union', variants: out };
    }
    return t;
}

function isAlwaysFalsy(t: TypeRef): boolean {
    if (t.kind === 'prim' && t.name === 'null') return true;
    if (t.kind === 'prim' && t.name === 'bool') return false;
    return false;
}

/* ----------------------------------------------------------------------- */
/* Object literal member writes                                            */
/* ----------------------------------------------------------------------- */

/** Merge `additional` member into `obj.members`, widening if a same-named
 *  member exists. */
export function setMember(obj: ObjectType, name: string, m: MemberType): void {
    const existing = obj.members.get(name);
    if (!existing) { obj.members.set(name, m); return; }
    obj.members.set(name, {
        type: unionOf([existing.type, m.type]),
        doc: m.doc ?? existing.doc,
        defRange: m.defRange ?? existing.defRange,
        manifestSpec: m.manifestSpec ?? existing.manifestSpec,
        isEvent: m.isEvent ?? existing.isEvent,
        readOnly: m.readOnly ?? existing.readOnly
    });
}

/** Equality on TypeRef structure -- coarse but good enough for "same type"
 *  checks during inference. */
export function typeEquals(a: TypeRef, b: TypeRef): boolean {
    return renderType(a) === renderType(b);
}

/* ----------------------------------------------------------------------- */
/* Manifest helpers re-exported (used by infer.ts and the LSP layer)       */
/* ----------------------------------------------------------------------- */

export function ctorParamsFromManifest(manifest: Manifest, def: TypeDef | undefined): FunctionParam[] {
    /* Manifests describe types not constructors; if a constructor-like
     * `function` export with `returns: <typeName>` exists we could mine it,
     * but most modules expose factories per-type. Return empty here -- the
     * inference engine bridges named-type construction separately. */
    void manifest; void def;
    return [];
}

/* Make sure ManifestParam is preserved in the API surface. */
export type { ManifestParam };
