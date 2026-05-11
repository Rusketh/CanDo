/*
 * Scope tree and name resolution for CanDo.
 *
 * Walks an AST and produces a tree of scopes (file > function > block),
 * each with named bindings. Bindings carry the AST node that introduced
 * them, the binding kind (var / const / param / function / class / pipe /
 * self / catch), and a placeholder type to be filled in by the inference
 * pass (infer.ts).
 *
 * The resolver does NOT type-check; it only answers "what binding does
 * this identifier resolve to?". The result is the substrate for hover,
 * go-to-definition, and the type-inference flow analysis.
 */

import { Range } from './lexer';
import {
    Program, Node, Stmt, Expr, walk, children
} from './ast';
import { TypeRef, UNKNOWN, ANY } from './typesys';

export type BindingKind =
    | 'var'
    | 'const'
    | 'global'
    | 'param'
    | 'function'
    | 'class'
    | 'pipe'
    | 'self'
    | 'catch'
    | 'forvar';

export interface Binding {
    name: string;
    kind: BindingKind;
    /** AST node that introduced this binding. */
    decl: Node;
    declRange: Range;
    /** Source range of the identifier that names the binding. */
    nameRange: Range;
    /** Scope that owns this binding. */
    scope: Scope;
    /** Currently inferred type (filled in by infer.ts). */
    type: TypeRef;
    /** Set by the resolver when an inner function references this binding. */
    captured: boolean;
    /** Set by the resolver when the binding is mutated after declaration. */
    reassigned: boolean;
    /** Every Ident occurrence that resolved to this binding, including the
     *  declaration's own name range. Drives Find References / Rename. */
    references: Range[];
    /** Documentation comment harvested from the lines immediately above the
     *  declaration (see analyze.ts). Markdown; rendered verbatim in hover. */
    doc?: string;
}

export type ScopeKind = 'file' | 'function' | 'block';

export class Scope {
    public readonly id: number;
    public readonly kind: ScopeKind;
    public readonly parent: Scope | null;
    public readonly node: Node | null;
    public readonly bindings: Map<string, Binding> = new Map();
    /** Function scopes only -- nested-function upvalue names. */
    public readonly upvalues: Set<string> = new Set();
    public readonly children: Scope[] = [];

    constructor(id: number, kind: ScopeKind, parent: Scope | null, node: Node | null) {
        this.id = id;
        this.kind = kind;
        this.parent = parent;
        this.node = node;
        if (parent) parent.children.push(this);
    }

    /** Find a binding by name, walking parent scopes. */
    lookup(name: string): Binding | null {
        for (let s: Scope | null = this; s; s = s.parent) {
            const b = s.bindings.get(name);
            if (b) return b;
        }
        return null;
    }

    enclosingFunction(): Scope | null {
        for (let s: Scope | null = this; s; s = s.parent) {
            if (s.kind === 'function' || s.kind === 'file') return s;
        }
        return null;
    }
}

export interface ResolveResult {
    fileScope: Scope;
    /** Maps every node's range-start to the deepest scope containing it.
     *  Used by lookupScopeAt(). */
    scopeOf: Map<Node, Scope>;
    /** Bindings flattened (handy for diagnostics and tests). */
    allBindings: Binding[];
}

export function resolve(program: Program): ResolveResult {
    const r = new Resolver();
    const file = new Scope(r.nextId(), 'file', null, program);
    r.scopeOf.set(program, file);
    /* Hoist top-level FunctionDecl + ClassDecl so forward references
     * resolve. VarDecl bindings are NOT hoisted (block-scoped). */
    for (const s of program.body) {
        if (s.kind === 'FunctionDecl') r.declare(file, s.name, 'function', s, s.nameRange, s.nameRange);
        else if (s.kind === 'ClassDecl') r.declare(file, s.name, 'class', s, s.nameRange, s.nameRange);
    }
    for (const s of program.body) r.stmt(s, file);

    return { fileScope: file, scopeOf: r.scopeOf, allBindings: r.allBindings };
}

class Resolver {
    private idCounter = 0;
    public scopeOf = new Map<Node, Scope>();
    public allBindings: Binding[] = [];

    nextId(): number { return ++this.idCounter; }

    declare(scope: Scope, name: string, kind: BindingKind, decl: Node, declRange: Range, nameRange: Range): Binding {
        const existing = scope.bindings.get(name);
        const b: Binding = {
            name, kind, decl, declRange, nameRange, scope,
            type: kind === 'self' ? ANY : UNKNOWN,
            captured: false,
            reassigned: false,
            references: [nameRange]
        };
        scope.bindings.set(name, b);
        if (!existing) this.allBindings.push(b);
        else {
            /* Re-declaration shadows earlier in same scope -- replace, keep
             * record. */
            const idx = this.allBindings.indexOf(existing);
            if (idx >= 0) this.allBindings[idx] = b;
            else this.allBindings.push(b);
        }
        return b;
    }

    stmt(s: Stmt, scope: Scope): void {
        this.scopeOf.set(s, scope);
        switch (s.kind) {
            case 'VarDecl': {
                for (let i = 0; i < s.init.length; i++) this.expr(s.init[i], scope);
                const kind: BindingKind = s.keyword === 'CONST' ? 'const'
                    : s.keyword === 'GLOBAL' ? 'global' : 'var';
                const targetScope = s.keyword === 'GLOBAL' ? this.fileOf(scope) : scope;
                for (const t of s.targets) {
                    this.declare(targetScope, t.name, kind, s, s.range, t.range);
                }
                return;
            }
            case 'AssignStmt': {
                for (const e of s.rhs) this.expr(e, scope);
                for (const t of s.targets) {
                    if (t.kind === 'Ident') {
                        const b = scope.lookup(t.name);
                        if (b) {
                            b.reassigned = true;
                            b.references.push(t.range);
                        } else {
                            /* Implicit global: matches the runtime rule that an
                             * assignment to an undeclared name creates a
                             * global. */
                            this.declare(this.fileOf(scope), t.name, 'global', s, s.range, t.range);
                        }
                    } else {
                        this.expr(t, scope);
                    }
                }
                return;
            }
            case 'ExprStmt': this.expr(s.expr, scope); return;
            case 'BlockStmt': {
                const inner = new Scope(this.nextId(), 'block', scope, s);
                this.scopeOf.set(s, inner);
                for (const sub of s.body) this.stmt(sub, inner);
                return;
            }
            case 'IfStmt': {
                for (const br of s.chain) {
                    this.scopeOf.set(br, scope);
                    if (br.cond) this.expr(br.cond, scope);
                    const inner = new Scope(this.nextId(), 'block', scope, br.body);
                    this.scopeOf.set(br.body, inner);
                    for (const sub of br.body.body) this.stmt(sub, inner);
                }
                return;
            }
            case 'WhileStmt': {
                this.expr(s.cond, scope);
                const inner = new Scope(this.nextId(), 'block', scope, s.body);
                this.scopeOf.set(s.body, inner);
                for (const sub of s.body.body) this.stmt(sub, inner);
                return;
            }
            case 'ForRange': {
                this.expr(s.from, scope);
                this.expr(s.to, scope);
                const inner = new Scope(this.nextId(), 'block', scope, s.body);
                this.scopeOf.set(s.body, inner);
                this.declare(inner, s.ident, 'forvar', s, s.identRange, s.identRange);
                for (const sub of s.body.body) this.stmt(sub, inner);
                return;
            }
            case 'ForKeys':
            case 'ForValues': {
                this.expr(s.src, scope);
                const inner = new Scope(this.nextId(), 'block', scope, s.body);
                this.scopeOf.set(s.body, inner);
                this.declare(inner, s.ident, 'forvar', s, s.identRange, s.identRange);
                for (const sub of s.body.body) this.stmt(sub, inner);
                return;
            }
            case 'ForOver': {
                this.expr(s.iter, scope);
                const inner = new Scope(this.nextId(), 'block', scope, s.body);
                this.scopeOf.set(s.body, inner);
                for (let i = 0; i < s.idents.length; i++) {
                    this.declare(inner, s.idents[i], 'forvar', s, s.identRanges[i], s.identRanges[i]);
                }
                for (const sub of s.body.body) this.stmt(sub, inner);
                return;
            }
            case 'BreakStmt':
            case 'ContinueStmt':
            case 'SettleStmt':
            case 'EmptyStmt':
                return;
            case 'ReturnStmt':
            case 'ThrowStmt':
                for (const v of s.values) this.expr(v, scope);
                return;
            case 'TryStmt': {
                const tryInner = new Scope(this.nextId(), 'block', scope, s.tryBlock);
                this.scopeOf.set(s.tryBlock, tryInner);
                for (const sub of s.tryBlock.body) this.stmt(sub, tryInner);
                if (s.catch) {
                    const catchInner = new Scope(this.nextId(), 'block', scope, s.catch.body);
                    this.scopeOf.set(s.catch.body, catchInner);
                    for (let i = 0; i < s.catch.params.length; i++) {
                        const p = s.catch.params[i];
                        this.declare(catchInner, p.name, 'catch', s, p.range, p.range);
                    }
                    for (const sub of s.catch.body.body) this.stmt(sub, catchInner);
                }
                if (s.finally) {
                    const finInner = new Scope(this.nextId(), 'block', scope, s.finally);
                    this.scopeOf.set(s.finally, finInner);
                    for (const sub of s.finally.body) this.stmt(sub, finInner);
                }
                return;
            }
            case 'FunctionDecl': {
                /* Already declared if hoisted at file scope. Otherwise declare
                 * now in the current scope. */
                if (scope.kind !== 'file' || !scope.bindings.has(s.name)) {
                    this.declare(scope, s.name, 'function', s, s.nameRange, s.nameRange);
                }
                const fn = new Scope(this.nextId(), 'function', scope, s);
                this.scopeOf.set(s, fn);
                for (const p of s.params) {
                    this.declare(fn, p.name, 'param', s, p.range, p.range);
                }
                /* Statement-form functions expose their own name inside the
                 * body (for self-recursion). */
                this.declare(fn, s.name, 'function', s, s.nameRange, s.nameRange);
                const bodyScope = new Scope(this.nextId(), 'block', fn, s.body);
                this.scopeOf.set(s.body, bodyScope);
                for (const sub of s.body.body) this.stmt(sub, bodyScope);
                return;
            }
            case 'ClassDecl': {
                if (scope.kind !== 'file' || !scope.bindings.has(s.name)) {
                    this.declare(scope, s.name, 'class', s, s.nameRange, s.nameRange);
                }
                const fn = new Scope(this.nextId(), 'function', scope, s);
                this.scopeOf.set(s, fn);
                /* Constructor sees `self` as the new instance + each param. */
                for (const p of s.ctorParams) {
                    const kind: BindingKind = p.name === 'self' ? 'self' : 'param';
                    this.declare(fn, p.name, kind, s, p.range, p.range);
                }
                if (!fn.bindings.has('self')) {
                    this.declare(fn, 'self', 'self', s, s.nameRange, s.nameRange);
                }
                const bodyScope = new Scope(this.nextId(), 'block', fn, s.body);
                this.scopeOf.set(s.body, bodyScope);
                for (const sub of s.body.body) this.stmt(sub, bodyScope);
                return;
            }
        }
    }

    expr(e: Expr, scope: Scope): void {
        this.scopeOf.set(e, scope);
        switch (e.kind) {
            case 'Ident': {
                const b = scope.lookup(e.name);
                if (b) {
                    this.markCaptureIfNeeded(b, scope);
                    b.references.push(e.range);
                }
                return;
            }
            case 'FunctionExpr': {
                const fn = new Scope(this.nextId(), 'function', scope, e);
                this.scopeOf.set(e, fn);
                if (e.name) {
                    this.declare(fn, e.name, 'function', e, e.nameRange ?? e.range, e.nameRange ?? e.range);
                }
                for (const p of e.params) {
                    this.declare(fn, p.name, 'param', e, p.range, p.range);
                }
                const body = new Scope(this.nextId(), 'block', fn, e.body);
                this.scopeOf.set(e.body, body);
                for (const sub of e.body.body) this.stmt(sub, body);
                return;
            }
            case 'ClassExpr': {
                const fn = new Scope(this.nextId(), 'function', scope, e);
                this.scopeOf.set(e, fn);
                for (const p of e.ctorParams) {
                    const kind: BindingKind = p.name === 'self' ? 'self' : 'param';
                    this.declare(fn, p.name, kind, e, p.range, p.range);
                }
                if (!fn.bindings.has('self')) {
                    this.declare(fn, 'self', 'self', e, e.range, e.range);
                }
                const body = new Scope(this.nextId(), 'block', fn, e.body);
                this.scopeOf.set(e.body, body);
                for (const sub of e.body.body) this.stmt(sub, body);
                return;
            }
            case 'Pipe': {
                this.expr(e.source, scope);
                const body = new Scope(this.nextId(), 'block', scope, e);
                this.scopeOf.set(e, body);
                this.declare(body, 'pipe', 'pipe', e, e.range, e.range);
                this.expr(e.body, body);
                return;
            }
            case 'Member': this.expr(e.object, scope); return;
            case 'Index': this.expr(e.object, scope); this.expr(e.index, scope); return;
            case 'Call':
                this.expr(e.callee, scope);
                for (const a of e.args) this.expr(a.expr, scope);
                return;
            case 'Unary': this.expr(e.argument, scope); return;
            case 'Postfix': this.expr(e.argument, scope); return;
            case 'Binary': this.expr(e.left, scope); this.expr(e.right, scope); return;
            case 'MultiCompare':
                this.expr(e.left, scope);
                for (const r of e.rights) this.expr(r, scope);
                return;
            case 'Ternary':
                this.expr(e.cond, scope);
                this.expr(e.cons, scope);
                this.expr(e.alt, scope);
                return;
            case 'ArrayLit':
                for (const el of e.elements) this.expr(el, scope);
                return;
            case 'ObjectLit':
                for (const p of e.properties) {
                    if (p.key.kind === 'computed' && p.key.expr) this.expr(p.key.expr, scope);
                    this.expr(p.value, scope);
                }
                return;
            case 'TemplateLit':
                for (const p of e.parts) {
                    if (p.kind === 'expr' && p.expr) this.expr(p.expr, scope);
                }
                return;
            case 'Mask': this.expr(e.expr, scope); return;
            case 'Spread': this.expr(e.argument, scope); return;
            case 'Paren': this.expr(e.expression, scope); return;
            case 'RangeExpr': this.expr(e.from, scope); this.expr(e.to, scope); return;

            case 'NumberLit':
            case 'StringLit':
            case 'BoolLit':
            case 'NullLit':
            case 'ErrorExpr':
                return;
        }
    }

    /** Mark a binding as captured if `useScope` is in a function nested
     *  inside the binding's owning function (i.e. the use crosses a
     *  function boundary). */
    private markCaptureIfNeeded(binding: Binding, useScope: Scope): void {
        const declFn = this.enclosingFunctionOf(binding.scope);
        const useFn  = this.enclosingFunctionOf(useScope);
        if (declFn && useFn && declFn !== useFn) {
            binding.captured = true;
            /* Mark every function scope along the chain as containing this
             * upvalue. Used by hover/render to indicate captured state. */
            for (let s: Scope | null = useFn; s && s !== declFn; s = s.parent) {
                if (s.kind === 'function') s.upvalues.add(binding.name);
            }
        }
    }

    private enclosingFunctionOf(scope: Scope): Scope | null {
        for (let s: Scope | null = scope; s; s = s.parent) {
            if (s.kind === 'function' || s.kind === 'file') return s;
        }
        return null;
    }

    private fileOf(scope: Scope): Scope {
        for (let s: Scope | null = scope; s; s = s.parent) {
            if (s.kind === 'file') return s;
        }
        return scope;
    }
}

/* ----------------------------------------------------------------------- */
/* Cursor scope lookup                                                     */
/* ----------------------------------------------------------------------- */

/**
 * Find the deepest scope whose node range contains the cursor. Walks the
 * scope tree from `fileScope` downward.
 */
export function scopeAt(fileScope: Scope, line: number, character: number): Scope {
    let best: Scope = fileScope;
    const visit = (s: Scope): void => {
        if (s.node && !rangeContainsPoint(s.node.range, line, character)) return;
        best = s;
        for (const c of s.children) visit(c);
    };
    visit(fileScope);
    return best;
}

function rangeContainsPoint(r: Range, line: number, character: number): boolean {
    if (line < r.start.line || line > r.end.line) return false;
    if (line === r.start.line && character < r.start.character) return false;
    if (line === r.end.line && character > r.end.character) return false;
    return true;
}

export { walk, children };
