/*
 * Type inference for CanDo.
 *
 * Walks the AST in source order with a current Scope and an in-flight
 * FlowEnv overlay (binding -> narrowed type). Each expression visit
 * returns a TypeRef and registers it in `nodeTypes` so the LSP layer can
 * look up the type at any cursor position.
 *
 * Inference is intentionally permissive: we never reject a program; the
 * worst case is a TypeRef of UNKNOWN, which suppresses completion that
 * would otherwise be a guess.
 */

import { Range } from './lexer';
import {
    Program, Stmt, Expr, FunctionDecl, ClassDecl, BlockStmt,
    VarDecl, AssignStmt, Ident, Member, Call, FunctionExpr, ClassExpr,
    ArrayLit, ObjectLit, Pipe, Node, walk as walkAst
} from './ast';
import { Scope, Binding, ResolveResult } from './scope';
import {
    TypeRef, FunctionType, FunctionParam, FunctionSummary, ObjectType, ClassType,
    ArrayType, NamespaceInfo, MemberType,
    ANY, UNKNOWN, NULL_T, BOOL_T, NUM_T, STR_T,
    arrayOf, tupleOf, emptyObject, optionalOf, unionOf,
    firstOf, enumerateMembers, narrowTruthy, narrowFalsy,
    setMember
} from './typesys';
import { findManifestFor } from './manifest';
import { NAMESPACES, GLOBAL_BUILTINS, NamespaceInfo as RawNamespaceInfo } from './builtins';
import { resolveIncludePath } from './paths';
import { parseDocType, DocTypeContext, emptyDocTypeContext } from './doctypes';
import { DocBlock, DocTag } from './docparse';

export interface InferOptions {
    documentUri: string;
    workspaceRoots: string[];
    /** Called to type-check an included .cdo file. Should return the type
     *  of the module's exported value, or null. The host passes a function
     *  here so analyze.ts can implement caching + cycle guard. */
    resolveIncludeType?: (absPath: string) => TypeRef | null;
}

export interface InferResult {
    /** Type assigned to each expression/statement node, keyed by the node
     *  reference itself. */
    nodeTypes: Map<Node, TypeRef>;
    /** Final type for every binding (post all narrowing). */
    bindingTypes: Map<Binding, TypeRef>;
    /** Top-level RETURN value of the program (drives module exports). */
    moduleType: TypeRef;
    /** Advisory errors produced while parsing `{type}` doc annotations.
     *  Surfaced as `doc-bad-type` diagnostics. */
    docTypeErrors: Array<{ range: Range; message: string }>;
}

/** Lightweight immutable overlay of binding-id -> TypeRef. */
type FlowEnv = Map<Binding, TypeRef>;

function flowGet(flow: FlowEnv, b: Binding): TypeRef {
    return flow.get(b) ?? b.type;
}
function flowSet(flow: FlowEnv, b: Binding, t: TypeRef): FlowEnv {
    const out = new Map(flow);
    out.set(b, t);
    return out;
}
function flowMerge(a: FlowEnv, b: FlowEnv): FlowEnv {
    const out = new Map(a);
    for (const [k, v] of b) {
        const existing = out.get(k);
        out.set(k, existing ? unionOf([existing, v]) : v);
    }
    /* Bindings only in `a` are already present; widen them with their
     * declared type as the "didn't reach" alternative. */
    for (const [k, v] of a) {
        if (!b.has(k)) out.set(k, unionOf([v, k.type]));
    }
    return out;
}

/* ----------------------------------------------------------------------- */
/* Builtin namespace registry                                              */
/* ----------------------------------------------------------------------- */

function buildBuiltinNamespaces(): Map<string, NamespaceInfo> {
    const out = new Map<string, NamespaceInfo>();
    for (const ns of NAMESPACES) {
        out.set(ns.name, namespaceFromRaw(ns));
    }
    return out;
}

function namespaceFromRaw(ns: RawNamespaceInfo): NamespaceInfo {
    const members = new Map<string, MemberType>();
    for (const m of ns.members) {
        const detail = ns.memberDetails?.[m];
        const t = detail ? functionFromDetail(detail) : { kind: 'function' as const, params: [], returns: [UNKNOWN], name: m };
        members.set(m, { type: t, doc: detail ? undefined : `${ns.name}.${m}` });
    }
    return { name: ns.name, doc: ns.doc, members };
}

/** Parse a hand-written "name(p1, p2) -> R" detail into a FunctionType.
 *  This is best-effort; the goal is to give signature help and hover
 *  something usable without full parsing. */
function functionFromDetail(detail: string): FunctionType {
    const open = detail.indexOf('(');
    const close = detail.lastIndexOf(')');
    if (open < 0 || close <= open) {
        return { kind: 'function', params: [], returns: [UNKNOWN], name: detail };
    }
    const inner = detail.slice(open + 1, close).trim();
    const params: FunctionParam[] = [];
    if (inner.length) {
        for (const part of inner.split(',')) {
            const p = part.trim();
            const rest = p.startsWith('...');
            const name = rest ? p.slice(3) : p;
            params.push({ name, type: ANY, rest });
        }
    }
    const arrow = detail.indexOf('->', close);
    const ret = arrow > 0 ? detail.slice(arrow + 2).trim() : '';
    return {
        kind: 'function',
        params,
        returns: [primFromName(ret) ?? UNKNOWN],
        name: detail
    };
}

function primFromName(s: string): TypeRef | null {
    switch (s.toLowerCase()) {
        case 'string': return STR_T;
        case 'number': return NUM_T;
        case 'bool':
        case 'boolean': return BOOL_T;
        case 'null': return NULL_T;
        case 'any': return ANY;
        case 'value': return ANY;
        case 'array': return arrayOf(ANY);
    }
    return null;
}

const ARRAY_NAMESPACE = (() => {
    const ns = NAMESPACES.find(n => n.name === 'array')!;
    return namespaceFromRaw(ns);
})();
const STRING_NAMESPACE = (() => {
    const ns = NAMESPACES.find(n => n.name === 'string')!;
    return namespaceFromRaw(ns);
})();
const OBJECT_NAMESPACE = (() => {
    const ns = NAMESPACES.find(n => n.name === 'object')!;
    return namespaceFromRaw(ns);
})();
const BUILTIN_NS_CACHE = buildBuiltinNamespaces();

function findNamespace(name: string): NamespaceInfo | null {
    return BUILTIN_NS_CACHE.get(name) ?? null;
}

/** Global builtins (print, include, type, ...) with their signatures. */
function globalBuiltinType(name: string): FunctionType | null {
    const b = GLOBAL_BUILTINS.find(g => g.name === name);
    if (!b) return null;
    if (b.name === 'include') {
        return {
            kind: 'function',
            params: [{ name: 'path', type: STR_T }],
            returns: [UNKNOWN],
            doc: b.doc,
            name: b.detail
        };
    }
    if (b.name === 'type') {
        return {
            kind: 'function',
            params: [{ name: 'value', type: ANY }],
            returns: [STR_T],
            doc: b.doc,
            name: b.detail
        };
    }
    if (b.name === 'toString') {
        return {
            kind: 'function',
            params: [{ name: 'value', type: ANY }],
            returns: [STR_T],
            doc: b.doc,
            name: b.detail
        };
    }
    if (b.name === 'inspect') {
        return {
            kind: 'function',
            params: [{ name: 'value', type: ANY }],
            returns: [STR_T],
            doc: b.doc,
            name: b.detail
        };
    }
    if (b.name === 'print') {
        return {
            kind: 'function',
            params: [{ name: 'args', type: ANY, rest: true }],
            returns: [NULL_T],
            doc: b.doc,
            name: b.detail
        };
    }
    return { kind: 'function', params: [], returns: [UNKNOWN], doc: b.doc, name: b.detail };
}

/* ----------------------------------------------------------------------- */
/* Inferer                                                                 */
/* ----------------------------------------------------------------------- */

export function infer(program: Program, resolved: ResolveResult, opts: InferOptions): InferResult {
    const inf = new Inferer(resolved, opts);
    inf.run(program);
    return {
        nodeTypes: inf.nodeTypes,
        bindingTypes: inf.bindingTypes,
        moduleType: inf.moduleType,
        docTypeErrors: inf.docTypeErrors
    };
}

class Inferer {
    public nodeTypes = new Map<Node, TypeRef>();
    public bindingTypes = new Map<Binding, TypeRef>();
    public moduleType: TypeRef = UNKNOWN;

    /** Stack of in-progress functions (for collecting RETURN tuples). */
    private fnStack: { returns: TypeRef[][] }[] = [];

    /** Per-file alias table populated from `@shape` / `@callback` tags
     *  on any binding visible at file scope. */
    private docCtx: DocTypeContext = emptyDocTypeContext();

    /** Errors produced while parsing `{type}` annotations in doc
     *  comments. Surfaced as advisory diagnostics. */
    public docTypeErrors: Array<{ range: Range; message: string }> = [];

    constructor(
        private readonly resolved: ResolveResult,
        private readonly opts: InferOptions
    ) {}

    run(program: Program): void {
        const file = this.resolved.fileScope;
        let flow: FlowEnv = new Map();

        /* Build the alias registry first so `@param x {MyShape}` can
         * resolve a shape declared anywhere in the file. */
        this.buildDocAliasRegistry();

        /* Two passes for forward refs to FunctionDecl / ClassDecl. First a
         * shallow declaration pass; then the full inference walk. */
        for (const s of program.body) this.predeclare(s, file);
        const moduleReturns: TypeRef[][] = [];
        this.fnStack.push({ returns: moduleReturns });
        for (const s of program.body) {
            flow = this.stmt(s, file, flow);
        }
        this.fnStack.pop();
        /* Module exports: the top-level RETURN, if any; otherwise the file
         * itself as an object with each top-level binding as a member. */
        if (moduleReturns.length > 0) {
            this.moduleType = this.combineReturns(moduleReturns);
            /* For a single-value module-return, expose the value directly. */
            if (this.moduleType.kind === 'tuple' && this.moduleType.items.length === 1) {
                this.moduleType = this.moduleType.items[0];
            }
        } else {
            const obj: ObjectType = emptyObject();
            for (const b of file.bindings.values()) {
                obj.members.set(b.name, {
                    type: this.bindingTypes.get(b) ?? b.type,
                    doc: undefined,
                    defRange: b.nameRange
                });
            }
            this.moduleType = obj;
        }

        /* Finalize binding types -- store the latest inferred type. */
        for (const b of this.resolved.allBindings) {
            if (!this.bindingTypes.has(b)) this.bindingTypes.set(b, b.type);
        }
    }

    /** Install a preliminary type for hoistable declarations so forward
     *  refs inside other top-level decls can see them. */
    private predeclare(s: Stmt, scope: Scope): void {
        if (s.kind === 'FunctionDecl') {
            const b = scope.bindings.get(s.name);
            if (b) {
                const ft = this.functionTypeFromDecl(s);
                b.type = ft;
                this.bindingTypes.set(b, ft);
            }
        } else if (s.kind === 'ClassDecl') {
            const b = scope.bindings.get(s.name);
            if (b) {
                const ct = this.classTypeFromDecl(s);
                b.type = ct;
                this.bindingTypes.set(b, ct);
            }
        }
    }

    private functionTypeFromDecl(s: FunctionDecl): FunctionType {
        const params: FunctionParam[] = s.params.map(p => ({
            name: p.name,
            type: p.rest ? arrayOf(ANY) : ANY,
            rest: p.rest
        }));
        return {
            kind: 'function',
            params,
            returns: [UNKNOWN],
            name: s.name,
            defRange: s.nameRange
        };
    }

    private classTypeFromDecl(s: ClassDecl): ClassType {
        const ctorParams: FunctionParam[] = s.ctorParams
            .filter(p => p.name !== 'self')
            .map(p => ({ name: p.name, type: ANY, rest: p.rest }));
        const instance: ObjectType = {
            kind: 'object',
            members: new Map(),
            className: s.name
        };
        return {
            kind: 'class',
            name: s.name,
            instance,
            ctorParams,
            defRange: s.nameRange
        };
    }

    /* ------------------------------------------------------------------- */
    /* Statement walker                                                    */
    /* ------------------------------------------------------------------- */

    private stmt(s: Stmt, scope: Scope, flow: FlowEnv): FlowEnv {
        switch (s.kind) {
            case 'VarDecl': return this.stmtVarDecl(s, scope, flow);
            case 'AssignStmt': return this.stmtAssign(s, scope, flow);
            case 'ExprStmt': this.expr(s.expr, scope, flow); return flow;
            case 'BlockStmt': {
                const inner = this.resolved.scopeOf.get(s) ?? scope;
                let f = flow;
                for (const sub of s.body) f = this.stmt(sub, inner, f);
                return flow;  // block-scoped narrowing doesn't escape
            }
            case 'IfStmt': return this.stmtIf(s, scope, flow);
            case 'WhileStmt': {
                this.expr(s.cond, scope, flow);
                const inner = this.resolved.scopeOf.get(s.body) ?? scope;
                let f = flow;
                for (const sub of s.body.body) f = this.stmt(sub, inner, f);
                return flow;
            }
            case 'ForRange': {
                this.expr(s.from, scope, flow);
                this.expr(s.to, scope, flow);
                const inner = this.resolved.scopeOf.get(s.body) ?? scope;
                const b = inner.bindings.get(s.ident);
                if (b) { b.type = NUM_T; this.bindingTypes.set(b, NUM_T); }
                let f = flow;
                for (const sub of s.body.body) f = this.stmt(sub, inner, f);
                return flow;
            }
            case 'ForKeys': {
                this.expr(s.src, scope, flow);
                const inner = this.resolved.scopeOf.get(s.body) ?? scope;
                const b = inner.bindings.get(s.ident);
                if (b) { b.type = STR_T; this.bindingTypes.set(b, STR_T); }
                let f = flow;
                for (const sub of s.body.body) f = this.stmt(sub, inner, f);
                return flow;
            }
            case 'ForValues': {
                const src = this.expr(s.src, scope, flow);
                const inner = this.resolved.scopeOf.get(s.body) ?? scope;
                const b = inner.bindings.get(s.ident);
                const elT = src.kind === 'array' ? src.element : ANY;
                if (b) { b.type = elT; this.bindingTypes.set(b, elT); }
                let f = flow;
                for (const sub of s.body.body) f = this.stmt(sub, inner, f);
                return flow;
            }
            case 'ForOver': {
                this.expr(s.iter, scope, flow);
                const inner = this.resolved.scopeOf.get(s.body) ?? scope;
                /* The iterator protocol yields (control, ...values). The
                 * runtime binds the loop vars to the values returned by the
                 * iter function past the control. For typing we treat them
                 * as ANY -- it would take return-type inference of the
                 * iterator function to do better. */
                for (const name of s.idents) {
                    const b = inner.bindings.get(name);
                    if (b) { b.type = ANY; this.bindingTypes.set(b, ANY); }
                }
                let f = flow;
                for (const sub of s.body.body) f = this.stmt(sub, inner, f);
                return flow;
            }
            case 'ReturnStmt': {
                const values = this.evalValueList(s.values, scope, flow);
                if (this.fnStack.length > 0) {
                    this.fnStack[this.fnStack.length - 1].returns.push(values);
                }
                return flow;
            }
            case 'ThrowStmt':
                this.evalValueList(s.values, scope, flow);
                return flow;
            case 'TryStmt': {
                const tryScope = this.resolved.scopeOf.get(s.tryBlock) ?? scope;
                let f = flow;
                for (const sub of s.tryBlock.body) f = this.stmt(sub, tryScope, f);
                if (s.catch) {
                    const cScope = this.resolved.scopeOf.get(s.catch.body) ?? scope;
                    /* CATCH params (kind, code, detail) are typed as ANY -- the
                     * runtime delivers raw THROW arguments. */
                    for (const p of s.catch.params) {
                        const b = cScope.bindings.get(p.kind === 'IdentPattern' ? p.name : p.name);
                        if (b) { b.type = ANY; this.bindingTypes.set(b, ANY); }
                    }
                    let cf = flow;
                    for (const sub of s.catch.body.body) cf = this.stmt(sub, cScope, cf);
                }
                if (s.finally) {
                    const fScope = this.resolved.scopeOf.get(s.finally) ?? scope;
                    let ff = flow;
                    for (const sub of s.finally.body) ff = this.stmt(sub, fScope, ff);
                }
                return flow;
            }
            case 'FunctionDecl': return this.stmtFunctionDecl(s, scope, flow);
            case 'ClassDecl': return this.stmtClassDecl(s, scope, flow);
            case 'BreakStmt':
            case 'ContinueStmt':
            case 'SettleStmt':
            case 'EmptyStmt':
                return flow;
        }
    }

    private stmtFunctionDecl(s: FunctionDecl, scope: Scope, flow: FlowEnv): FlowEnv {
        const b = scope.bindings.get(s.name);
        const fnScope = this.resolved.scopeOf.get(s) ?? scope;
        const bodyScope = this.resolved.scopeOf.get(s.body) ?? fnScope;

        /* Initialise param bindings inside fnScope. Preserve any
         * contextual type that an outer pass already installed (see the
         * method-shape inference in stmtAssign). */
        for (const p of s.params) {
            const pb = fnScope.bindings.get(p.name);
            if (!pb) continue;
            if (pb.type.kind === 'prim' && pb.type.name === 'unknown') {
                pb.type = p.rest ? arrayOf(ANY) : ANY;
                this.bindingTypes.set(pb, pb.type);
            }
        }

        /* Doc-driven params/returns override the defaults. Done before
         * the body so `o.field` reads inside the body see the declared
         * shape during inference. */
        const docReturns = this.applyDocToFunction(s, fnScope, b);

        const returns: TypeRef[][] = [];
        this.fnStack.push({ returns });
        let inner = flow;
        for (const sub of s.body.body) inner = this.stmt(sub, bodyScope, inner);
        this.fnStack.pop();

        const fnType = this.makeFunctionType(s.name, s.params, returns, s.nameRange);
        /* Layer doc info onto the synthesised function type. */
        this.applyDocSignatureOverlay(fnType, b, fnScope, docReturns);
        /* Collect the cross-call summary so callers can predict the
         * post-call shape of their arguments. */
        fnType.summary = this.collectFunctionSummary(s.body, s.params.map(p => p.name));
        if (b) {
            b.type = fnType;
            this.bindingTypes.set(b, fnType);
        }
        this.nodeTypes.set(s, fnType);
        return flow;
    }

    /** After body-walk produces an inferred FunctionType, fold in any
     *  `@param` types (already set on param bindings) and override
     *  return types from `@returns`. Also copies the description into
     *  `doc` for hover. */
    private applyDocSignatureOverlay(
        fnType: FunctionType,
        b: Binding | undefined,
        fnScope: Scope,
        docReturns: TypeRef[] | null
    ): void {
        if (!b || !b.docBlock) return;
        for (let i = 0; i < fnType.params.length; i++) {
            const fp = fnType.params[i];
            const pb = fnScope.bindings.get(fp.name);
            if (pb) fp.type = pb.type;
            const tag = b.docBlock.tags.find(t => t.kind === 'param' && t.name === fp.name);
            if (tag && tag.kind === 'param' && tag.description) fp.doc = tag.description;
        }
        if (docReturns && docReturns.length > 0) {
            fnType.returns = docReturns;
        }
        if (b.docBlock.description) fnType.doc = b.docBlock.description;
    }

    private stmtClassDecl(s: ClassDecl, scope: Scope, flow: FlowEnv): FlowEnv {
        const b = scope.bindings.get(s.name);
        const classType = (b?.type.kind === 'class') ? b.type : this.classTypeFromDecl(s);
        const instance = classType.instance;
        if (s.extendsName) {
            const parent = scope.lookup(s.extendsName);
            if (parent && parent.type.kind === 'class') {
                instance.prototype = parent.type.instance;
            }
        }
        /* Seed instance shape from `@field` annotations *before* walking
         * the constructor body so reads like `self.foo` resolve. */
        this.applyDocFieldsToClass(b, instance);
        if (b) {
            b.type = classType;
            this.bindingTypes.set(b, classType);
        }

        /* Walk constructor body so `self.x = …` assignments populate the
         * instance shape. */
        const fnScope = this.resolved.scopeOf.get(s) ?? scope;
        const bodyScope = this.resolved.scopeOf.get(s.body) ?? fnScope;
        const selfBind = fnScope.bindings.get('self');
        if (selfBind) {
            selfBind.type = instance;
            this.bindingTypes.set(selfBind, instance);
        }
        for (const p of s.ctorParams) {
            if (p.name === 'self') continue;
            const pb = fnScope.bindings.get(p.name);
            if (pb) {
                pb.type = ANY;
                this.bindingTypes.set(pb, ANY);
            }
        }
        let inner = flow;
        for (const sub of s.body.body) inner = this.stmt(sub, bodyScope, inner);
        this.nodeTypes.set(s, classType);
        return flow;
    }

    private makeFunctionType(name: string, params: FunctionDecl['params'], returns: TypeRef[][], defRange: Range): FunctionType {
        const fp: FunctionParam[] = params.map(p => ({
            name: p.name,
            type: p.rest ? arrayOf(ANY) : ANY,
            rest: p.rest
        }));
        const ret = this.combineReturns(returns);
        const items = ret.kind === 'tuple' ? ret.items : [ret];
        return {
            kind: 'function',
            params: fp,
            returns: items.length === 0 ? [NULL_T] : items,
            name,
            defRange
        };
    }

    private combineReturns(returns: TypeRef[][]): TypeRef {
        if (returns.length === 0) return NULL_T;
        const maxLen = Math.max(...returns.map(r => r.length));
        if (maxLen === 0) return NULL_T;
        if (maxLen === 1) {
            return unionOf(returns.map(r => r[0] ?? NULL_T));
        }
        const items: TypeRef[] = [];
        for (let i = 0; i < maxLen; i++) {
            items.push(unionOf(returns.map(r => r[i] ?? NULL_T)));
        }
        return tupleOf(items);
    }

    private stmtVarDecl(s: VarDecl, scope: Scope, flow: FlowEnv): FlowEnv {
        const values = this.evalRHSList(s.init, scope, flow);
        const targetScope = s.keyword === 'GLOBAL' ? this.fileOf(scope) : scope;
        for (let i = 0; i < s.targets.length; i++) {
            const t = s.targets[i];
            const v = values[i] ?? NULL_T;
            const b = targetScope.bindings.get(t.name);
            if (b) {
                /* `@type` on the declaration wins over inference. */
                const docT = this.applyDocToVar(b);
                b.type = docT ?? v;
                this.bindingTypes.set(b, b.type);
            }
        }
        this.nodeTypes.set(s, NULL_T);
        return flow;
    }

    private stmtAssign(s: AssignStmt, scope: Scope, flow: FlowEnv): FlowEnv {
        /* Method-shape inference: `ClassOrObj.method = FUNCTION(self, ...) {...}`
         * binds `self` to the owner's instance type so member access inside
         * the body resolves to the class's fields. Same trick for
         * `obj.method = FUNCTION(self, ...) {...}` where obj is a record. */
        if (s.targets.length === 1 && s.rhs.length === 1) {
            const t0 = s.targets[0];
            const r0 = s.rhs[0];
            if (t0.kind === 'Member' && t0.object.kind === 'Ident' && r0.kind === 'FunctionExpr'
                && r0.params.length > 0) {
                const owner = scope.lookup(t0.object.name);
                if (owner) {
                    let selfType: TypeRef | null = null;
                    if (owner.type.kind === 'class') selfType = owner.type.instance;
                    else if (owner.type.kind === 'object') selfType = owner.type;
                    if (selfType) {
                        const fnScope = this.resolved.scopeOf.get(r0);
                        const firstParamBinding = fnScope?.bindings.get(r0.params[0].name);
                        if (firstParamBinding) {
                            firstParamBinding.type = selfType;
                            this.bindingTypes.set(firstParamBinding, selfType);
                        }
                    }
                }
            }
        }

        const values = this.evalRHSList(s.rhs, scope, flow);
        for (let i = 0; i < s.targets.length; i++) {
            const t = s.targets[i];
            const v = values[i] ?? NULL_T;
            if (t.kind === 'Ident') {
                const b = scope.lookup(t.name);
                if (b) {
                    b.type = b.reassigned ? unionOf([b.type, v]) : v;
                    this.bindingTypes.set(b, b.type);
                } else {
                    /* Resolver already created an implicit global. */
                    const fb = this.fileOf(scope).bindings.get(t.name);
                    if (fb) { fb.type = v; this.bindingTypes.set(fb, v); }
                }
            } else if (t.kind === 'Member') {
                const objT = this.expr(t.object, scope, flow);
                this.attachMemberWrite(objT, t.property, v, t.propertyRange);
            } else if (t.kind === 'Index') {
                const objT = this.expr(t.object, scope, flow);
                this.expr(t.index, scope, flow);
                if (objT.kind === 'array') {
                    objT.element = unionOf([objT.element, v]);
                } else if (objT.kind === 'object') {
                    objT.indexValue = objT.indexValue
                        ? unionOf([objT.indexValue, v])
                        : v;
                }
            }
        }
        this.nodeTypes.set(s, NULL_T);
        return flow;
    }

    private attachMemberWrite(obj: TypeRef, name: string, value: TypeRef, defRange: Range): void {
        if (obj.kind === 'object') {
            setMember(obj, name, { type: value, defRange });
        } else if (obj.kind === 'class') {
            setMember(obj.instance, name, { type: value, defRange });
        }
        /* manifest types are sealed; ignore writes onto them. */
    }

    private stmtIf(s: import('./ast').IfStmt, scope: Scope, flow: FlowEnv): FlowEnv {
        let cumulative: FlowEnv | null = null;
        let elseFlow = flow;

        for (const br of s.chain) {
            const branchScope = this.resolved.scopeOf.get(br.body) ?? scope;
            let branchFlow = elseFlow;
            if (br.cond) {
                this.expr(br.cond, scope, branchFlow);
                branchFlow = this.applyCondNarrowing(br.cond, branchFlow, true);
            }
            let f = branchFlow;
            for (const sub of br.body.body) f = this.stmt(sub, branchScope, f);
            cumulative = cumulative ? flowMerge(cumulative, f) : f;
            if (br.cond) elseFlow = this.applyCondNarrowing(br.cond, elseFlow, false);
        }
        return cumulative ?? flow;
    }

    /** Apply truthy/falsy narrowing for `IF cond { ... }` style branches. */
    private applyCondNarrowing(cond: Expr, flow: FlowEnv, truthy: boolean): FlowEnv {
        if (cond.kind === 'Ident') {
            const b = this.lookupAt(cond);
            if (!b || b.reassigned) return flow;
            const cur = flowGet(flow, b);
            return flowSet(flow, b, truthy ? narrowTruthy(cur) : narrowFalsy(cur));
        }
        /* type(x) == "string"  or  "string" == type(x)
         * x.__type == "Foo"    or  "Foo" == x.__type
         * These pin a binding to a specific runtime tag. */
        if (cond.kind === 'Binary' && (cond.op === '==' || cond.op === '!=')) {
            const want = truthy ? cond.op === '==' : cond.op === '!=';
            const narrowed = this.tryDiscriminantNarrow(cond.left, cond.right, flow, want)
                          ?? this.tryDiscriminantNarrow(cond.right, cond.left, flow, want);
            if (narrowed) return narrowed;
        }
        return flow;
    }

    private tryDiscriminantNarrow(probe: Expr, value: Expr, flow: FlowEnv, equal: boolean): FlowEnv | null {
        if (value.kind !== 'StringLit') return null;
        const tagName = value.value;
        let target: Binding | null = null;
        if (probe.kind === 'Call'
            && probe.callee.kind === 'Ident' && probe.callee.name === 'type'
            && probe.args.length === 1
            && probe.args[0].expr.kind === 'Ident') {
            target = this.lookupOfIdent(probe.args[0].expr);
        } else if (probe.kind === 'Member' && probe.property === '__type'
            && probe.object.kind === 'Ident') {
            target = this.lookupOfIdent(probe.object);
        }
        if (!target || target.reassigned) return null;
        const cur = flowGet(flow, target);
        const narrowed = this.narrowToTag(cur, tagName, equal);
        if (narrowed === cur) return null;
        return flowSet(flow, target, narrowed);
    }

    private lookupOfIdent(e: Ident): Binding | null {
        const scope = this.resolved.scopeOf.get(e);
        return scope?.lookup(e.name) ?? null;
    }

    private narrowToTag(t: TypeRef, tag: string, equal: boolean): TypeRef {
        const matchesTag = (variant: TypeRef): boolean => {
            switch (variant.kind) {
                case 'prim': return variant.name === tag;
                case 'array': return tag === 'array';
                case 'object': return tag === 'object' || (!!variant.className && variant.className === tag);
                case 'class':  return variant.name === tag;
                case 'function': return tag === 'function';
                case 'manifest-type': return variant.typeName === tag;
                default: return false;
            }
        };
        const filter = (variants: TypeRef[]): TypeRef[] =>
            variants.filter(v => equal ? matchesTag(v) : !matchesTag(v));
        if (t.kind === 'union') {
            const out = filter(t.variants);
            if (out.length === 0) return t;
            if (out.length === 1) return out[0];
            return { kind: 'union', variants: out };
        }
        if (t.kind === 'optional') {
            /* optional<T> = T | null. If tag matches null, narrow to null;
             * otherwise narrow to T. */
            if (tag === 'null') return equal ? NULL_T : t.inner;
            return equal ? t.inner : NULL_T;
        }
        /* `any` and `unknown` get refined to the discriminant. */
        if (t.kind === 'prim' && (t.name === 'any' || t.name === 'unknown') && equal) {
            switch (tag) {
                case 'string': return STR_T;
                case 'number': return NUM_T;
                case 'bool':   return BOOL_T;
                case 'null':   return NULL_T;
                case 'array':  return arrayOf(ANY);
                case 'function': return { kind: 'function', params: [], returns: [ANY] };
                case 'object': return emptyObject();
            }
        }
        return t;
    }

    private lookupAt(e: Ident): Binding | null {
        const scope = this.resolved.scopeOf.get(e);
        if (!scope) return null;
        return scope.lookup(e.name);
    }

    private fileOf(scope: Scope): Scope {
        for (let s: Scope | null = scope; s; s = s.parent) {
            if (s.kind === 'file') return s;
        }
        return scope;
    }

    /* ------------------------------------------------------------------- */
    /* RHS list inference (multi-return distribution)                      */
    /* ------------------------------------------------------------------- */

    /** Evaluate `init` honoring the runtime rule: a trailing multi-return
     *  spreads; non-trailing values collapse to first. */
    private evalRHSList(init: Expr[], scope: Scope, flow: FlowEnv): TypeRef[] {
        const out: TypeRef[] = [];
        for (let i = 0; i < init.length; i++) {
            const t = this.expr(init[i], scope, flow);
            const isLast = i === init.length - 1;
            const value = this.applyMask(init[i], t);
            if (isLast && value.kind === 'tuple') {
                for (const item of value.items) out.push(item);
            } else {
                out.push(firstOf(value));
            }
        }
        return out;
    }

    private evalValueList(values: Expr[], scope: Scope, flow: FlowEnv): TypeRef[] {
        return this.evalRHSList(values, scope, flow);
    }

    private applyMask(expr: Expr, t: TypeRef): TypeRef {
        if (expr.kind !== 'Mask') return t;
        const inner = t.kind === 'tuple' ? t.items.slice() : [t];
        /* Distribute the mask across positions: keep `~`, drop `.`. */
        const result: TypeRef[] = [];
        for (let i = 0; i < expr.mask.length; i++) {
            if (expr.mask[i] === 'keep') {
                result.push(inner[i] ?? NULL_T);
            }
        }
        return tupleOf(result);
    }

    /* ------------------------------------------------------------------- */
    /* Expression walker                                                   */
    /* ------------------------------------------------------------------- */

    expr(e: Expr, scope: Scope, flow: FlowEnv): TypeRef {
        const t = this.exprImpl(e, scope, flow);
        this.nodeTypes.set(e, t);
        return t;
    }

    private exprImpl(e: Expr, scope: Scope, flow: FlowEnv): TypeRef {
        switch (e.kind) {
            case 'NumberLit': return NUM_T;
            case 'StringLit': return STR_T;
            case 'TemplateLit':
                for (const p of e.parts) if (p.kind === 'expr' && p.expr) this.expr(p.expr, scope, flow);
                return STR_T;
            case 'BoolLit': return BOOL_T;
            case 'NullLit': return NULL_T;
            case 'Ident': return this.inferIdent(e, scope, flow);
            case 'ArrayLit': return this.inferArrayLit(e, scope, flow);
            case 'ObjectLit': return this.inferObjectLit(e, scope, flow);
            case 'Member': return this.inferMember(e, scope, flow);
            case 'Index': return this.inferIndex(e, scope, flow);
            case 'Call': return this.inferCall(e, scope, flow);
            case 'Unary': return this.inferUnary(e, scope, flow);
            case 'Postfix': { this.expr(e.argument, scope, flow); return NUM_T; }
            case 'Binary': return this.inferBinary(e, scope, flow);
            case 'MultiCompare':
                this.expr(e.left, scope, flow);
                for (const r of e.rights) this.expr(r, scope, flow);
                return BOOL_T;
            case 'Ternary': {
                this.expr(e.cond, scope, flow);
                const a = this.expr(e.cons, scope, flow);
                const b = this.expr(e.alt, scope, flow);
                return unionOf([a, b]);
            }
            case 'FunctionExpr': return this.inferFunctionExpr(e, scope, flow);
            case 'ClassExpr': return this.inferClassExpr(e, scope, flow);
            case 'Mask': return this.expr(e.expr, scope, flow);
            case 'Spread': { this.expr(e.argument, scope, flow); return UNKNOWN; }
            case 'Pipe': return this.inferPipe(e, scope, flow);
            case 'Paren': {
                const t = this.expr(e.expression, scope, flow);
                return firstOf(t);
            }
            case 'RangeExpr':
                this.expr(e.from, scope, flow);
                this.expr(e.to, scope, flow);
                return arrayOf(NUM_T);
            case 'ErrorExpr': return UNKNOWN;
        }
    }

    private inferIdent(e: Ident, scope: Scope, flow: FlowEnv): TypeRef {
        /* Built-in global names: namespaces (array, string, ...) and global
         * builtins (print, type, ...). */
        const ns = findNamespace(e.name);
        if (ns) return { kind: 'namespace', info: ns };
        const gb = globalBuiltinType(e.name);
        if (gb) return gb;

        const b = scope.lookup(e.name);
        if (b) {
            return flowGet(flow, b);
        }
        return UNKNOWN;
    }

    private inferArrayLit(e: ArrayLit, scope: Scope, flow: FlowEnv): TypeRef {
        if (e.elements.length === 0) return arrayOf(ANY);
        const ts = e.elements.map(el => firstOf(this.expr(el, scope, flow)));
        return arrayOf(unionOf(ts));
    }

    private inferObjectLit(e: ObjectLit, scope: Scope, flow: FlowEnv): TypeRef {
        const obj: ObjectType = emptyObject();
        for (const p of e.properties) {
            if (p.key.kind === 'computed' && p.key.expr) {
                this.expr(p.key.expr, scope, flow);
                /* Computed keys: we can't know the name at static time. */
                continue;
            }
            const name = p.key.name ?? '';
            if (!name) continue;
            const v = firstOf(this.expr(p.value, scope, flow));
            if (name === '__index') {
                obj.prototype = v;
            } else {
                obj.members.set(name, { type: v, defRange: p.key.range });
            }
        }
        return obj;
    }

    private inferMember(e: Member, scope: Scope, flow: FlowEnv): TypeRef {
        const objT = this.expr(e.object, scope, flow);
        const lookupT = e.safe && objT.kind === 'optional' ? objT.inner : objT;
        const members = enumerateMembers(this.namespaceMembersFor(lookupT, e.object));
        const m = members.get(e.property);
        let result: TypeRef;
        if (m) {
            result = m.type;
        } else if (lookupT.kind === 'object' && lookupT.indexValue) {
            result = lookupT.indexValue;
        } else if (lookupT.kind === 'array' && e.property === 'length') {
            result = NUM_T;
        } else if (lookupT.kind === 'prim' && (lookupT.name === 'any' || lookupT.name === 'unknown')) {
            /* `any.foo` is `any`, not `unknown` -- we don't *know* the
             * member is missing, only that we can't enumerate it. Mirrors
             * the convention in TypeScript / mypy / Sorbet. */
            result = lookupT.name === 'any' ? ANY : UNKNOWN;
        } else {
            result = UNKNOWN;
        }
        if (e.safe) result = optionalOf(result);
        return result;
    }

    /** Pick the appropriate member source for `t`. For arrays, the std
     *  `array` namespace is used (so `arr:push(...)` resolves to the array
     *  helper). For strings, the `string` namespace. */
    private namespaceMembersFor(t: TypeRef, _receiver: Expr): TypeRef {
        if (t.kind === 'array') return { kind: 'object', members: this.arrayMethodMembers(t) };
        if (t.kind === 'prim' && t.name === 'string') return { kind: 'object', members: this.stringMethodMembers() };
        return t;
    }

    private arrayMethodMembers(arr: ArrayType): Map<string, MemberType> {
        const out = new Map<string, MemberType>();
        for (const [name, m] of ARRAY_NAMESPACE.members) out.set(name, m);
        /* Specialise return types where they depend on the element. */
        out.set('map', {
            type: {
                kind: 'function',
                params: [{ name: 'fn', type: { kind: 'function', params: [{ name: 'x', type: arr.element }], returns: [ANY] } }],
                returns: [arrayOf(ANY)],
                name: 'array.map'
            },
            doc: 'Map each element through `fn`.'
        });
        out.set('filter', {
            type: {
                kind: 'function',
                params: [{ name: 'fn', type: { kind: 'function', params: [{ name: 'x', type: arr.element }], returns: [BOOL_T] } }],
                returns: [arrayOf(arr.element)],
                name: 'array.filter'
            },
            doc: 'Keep elements where `fn` returns truthy.'
        });
        out.set('pop', {
            type: { kind: 'function', params: [], returns: [arr.element], name: 'array.pop' },
            doc: 'Remove and return the last element.'
        });
        out.set('push', {
            type: { kind: 'function', params: [{ name: 'value', type: arr.element }], returns: [NULL_T], name: 'array.push' },
            doc: 'Append a value to the array.'
        });
        out.set('length', { type: NUM_T, doc: 'Element count.' });
        return out;
    }

    private stringMethodMembers(): Map<string, MemberType> {
        return new Map(STRING_NAMESPACE.members);
    }

    private inferIndex(e: import('./ast').Index, scope: Scope, flow: FlowEnv): TypeRef {
        const objT = this.expr(e.object, scope, flow);
        this.expr(e.index, scope, flow);
        const lookupT = e.safe && objT.kind === 'optional' ? objT.inner : objT;
        let result: TypeRef;
        if (lookupT.kind === 'array') result = lookupT.element;
        else if (lookupT.kind === 'object') result = lookupT.indexValue ?? UNKNOWN;
        else if (lookupT.kind === 'prim' && lookupT.name === 'string') result = STR_T;
        else result = UNKNOWN;
        if (e.safe) result = optionalOf(result);
        return result;
    }

    private inferCall(e: Call, scope: Scope, flow: FlowEnv): TypeRef {
        /* Special-case include(...) before any general callee typing so we
         * don't return UNKNOWN from the generic global-builtin handler. */
        if (e.callee.kind === 'Ident' && e.callee.name === 'include' && e.args.length >= 1) {
            this.expr(e.callee, scope, flow);   // record callee node type
            for (const a of e.args) this.expr(a.expr, scope, flow);
            const a0 = e.args[0].expr;
            if (a0.kind === 'StringLit') {
                const t = this.resolveIncludeType(a0.value);
                if (t) return t;
            }
            return UNKNOWN;
        }

        const calleeT = this.expr(e.callee, scope, flow);
        for (const a of e.args) this.expr(a.expr, scope, flow);

        if (e.style === 'fluent') {
            /* `obj::m(...)` returns the receiver itself. */
            if (e.callee.kind === 'Member') return this.expr(e.callee.object, scope, flow);
            return calleeT;
        }
        if (calleeT.kind === 'class') {
            /* Constructor call: returns an instance. */
            return calleeT.instance;
        }
        if (calleeT.kind === 'function') {
            /* Replay the callee's per-param write summary onto the
             * arguments. Lets `init(a)` add the keys `init` writes to
             * its first param onto `a`'s shape -- exactly the
             * cross-function shape prediction we set out to build. */
            this.replaySummary(calleeT, e.args, scope);
            if (calleeT.returnsSelf && e.callee.kind === 'Member') {
                return this.expr(e.callee.object, scope, flow);
            }
            if (calleeT.returns.length === 1) return calleeT.returns[0];
            return tupleOf(calleeT.returns);
        }
        /* __call metamethod: objects whose class defines __call are
         * callable. Look it up on the callee's type and use its return. */
        const metaCall = this.metaCall(calleeT, '__call', []);
        if (metaCall) return metaCall;
        return UNKNOWN;
    }

    private resolveIncludeType(rawPath: string): TypeRef | null {
        const resolved = resolveIncludePath(rawPath, this.opts.documentUri, this.opts.workspaceRoots);
        if (resolved) {
            if (this.opts.resolveIncludeType) {
                const t = this.opts.resolveIncludeType(resolved);
                if (t) return t;
            }
            const manifest = findManifestFor(resolved);
            if (manifest) return { kind: 'manifest-exports', manifest };
            return null;
        }
        /* Binary modules (`.so`, `.dll`, `.dylib`) are commonly absent on
         * the developer's host -- but the sibling `cando.api.json` is
         * always present. Construct the speculative absolute path and
         * fish the manifest out directly. */
        const speculative = speculativeAbsPath(rawPath, this.opts.documentUri, this.opts.workspaceRoots);
        if (speculative) {
            const manifest = findManifestFor(speculative);
            if (manifest) return { kind: 'manifest-exports', manifest };
        }
        return null;
    }

    private inferUnary(e: import('./ast').Unary, scope: Scope, flow: FlowEnv): TypeRef {
        this.expr(e.argument, scope, flow);
        switch (e.op) {
            case '-':
            case '#': return NUM_T;
            case '!': return BOOL_T;
            case '~': return NUM_T;
        }
    }

    private inferBinary(e: import('./ast').Binary, scope: Scope, flow: FlowEnv): TypeRef {
        const a = this.expr(e.left, scope, flow);
        const b = this.expr(e.right, scope, flow);
        const meta = METHOD_FOR_OP[e.op];
        if (meta) {
            const t = this.metaCall(a, meta, [a, b]) ?? this.metaCall(b, meta, [a, b]);
            if (t) return t;
        }
        switch (e.op) {
            case '+': {
                /* String concat if either side is string. */
                if (isString(a) || isString(b)) return STR_T;
                return NUM_T;
            }
            case '-':
            case '*':
            case '/':
            case '%':
            case '^':
            case '<<':
            case '>>':
            case '&':
            case '|':
            case '|&':
                return NUM_T;
            case '==':
            case '!=':
            case '<':
            case '<=':
            case '>':
            case '>=':
                return BOOL_T;
            case '&&':
                /* `a && b` returns `a` if `a` is falsy, else `b`. */
                return unionOf([narrowFalsy(a), b]);
            case '||':
                /* `a || b` returns `a` if truthy, else `b`. */
                return unionOf([narrowTruthy(a), b]);
        }
        return UNKNOWN;
    }

    /** Look up a metamethod (e.g. `__add`, `__call`) on `t` and return the
     *  type of invoking it with `args`. Returns null when the metamethod is
     *  absent or non-function-typed. */
    private metaCall(t: TypeRef, name: string, _args: TypeRef[]): TypeRef | null {
        const members = enumerateMembers(t);
        const m = members.get(name);
        if (!m || m.type.kind !== 'function') return null;
        const fn = m.type;
        if (fn.returnsSelf) return t;
        if (fn.returns.length === 1) return fn.returns[0];
        return tupleOf(fn.returns);
    }

    private inferFunctionExpr(e: FunctionExpr, scope: Scope, flow: FlowEnv): TypeRef {
        const fnScope = this.resolved.scopeOf.get(e) ?? scope;
        const bodyScope = this.resolved.scopeOf.get(e.body) ?? fnScope;
        for (const p of e.params) {
            const pb = fnScope.bindings.get(p.name);
            if (!pb) continue;
            /* Preserve any contextually-installed type (e.g. `self` set up by
             * stmtAssign when assigning to ClassOrObj.method). Otherwise
             * default to ANY. */
            if (pb.type.kind === 'prim' && pb.type.name === 'unknown') {
                pb.type = p.rest ? arrayOf(ANY) : ANY;
                this.bindingTypes.set(pb, pb.type);
            }
        }
        const returns: TypeRef[][] = [];
        this.fnStack.push({ returns });
        let inner = flow;
        for (const sub of e.body.body) inner = this.stmt(sub, bodyScope, inner);
        this.fnStack.pop();
        const fp: FunctionParam[] = e.params.map(p => ({
            name: p.name,
            type: p.rest ? arrayOf(ANY) : ANY,
            rest: p.rest
        }));
        const ret = this.combineReturns(returns);
        const items = ret.kind === 'tuple' ? ret.items : [ret];
        return {
            kind: 'function',
            params: fp,
            returns: items.length === 0 ? [NULL_T] : items,
            name: e.name,
            defRange: e.nameRange ?? e.range,
            summary: this.collectFunctionSummary(e.body, e.params.map(p => p.name))
        };
    }

    private inferClassExpr(e: ClassExpr, scope: Scope, flow: FlowEnv): TypeRef {
        const fnScope = this.resolved.scopeOf.get(e) ?? scope;
        const bodyScope = this.resolved.scopeOf.get(e.body) ?? fnScope;
        const instance: ObjectType = {
            kind: 'object',
            members: new Map(),
            className: e.name
        };
        const ct: ClassType = {
            kind: 'class',
            name: e.name ?? '<anonymous>',
            instance,
            ctorParams: e.ctorParams.filter(p => p.name !== 'self').map(p => ({ name: p.name, type: ANY, rest: p.rest }))
        };
        if (e.extendsName) {
            const parent = scope.lookup(e.extendsName);
            if (parent && parent.type.kind === 'class') instance.prototype = parent.type.instance;
        }
        const sb = fnScope.bindings.get('self');
        if (sb) { sb.type = instance; this.bindingTypes.set(sb, instance); }
        let inner = flow;
        for (const sub of e.body.body) inner = this.stmt(sub, bodyScope, inner);
        return ct;
    }

    /* ------------------------------------------------------------------- */
    /* Doc-comment integration                                             */
    /* ------------------------------------------------------------------- */

    /** Walk every binding once and register `@shape` / `@callback`
     *  aliases. Two passes: aliases that reference other aliases work
     *  because the second pass parses with the populated table. */
    private buildDocAliasRegistry(): void {
        const blocks: DocBlock[] = [];
        for (const b of this.resolved.allBindings) {
            if (b.docBlock) blocks.push(b.docBlock);
        }
        blocks.push(...this.resolved.orphanDocBlocks);

        /* Two passes so aliases that reference other aliases resolve in
         * the second pass once the first has populated the table. */
        for (let pass = 0; pass < 2; pass++) {
            for (const block of blocks) {
                for (const tag of block.tags) {
                    if (tag.kind === 'shape' || tag.kind === 'callback') {
                        const r = parseDocType(tag.typeText, this.docCtx);
                        if (r.type) this.docCtx.aliases.set(tag.name, r.type);
                    }
                }
            }
        }
    }

    /** Look up a single doc-tag entry on a binding by tag kind. */
    private docTagsOf<K extends DocTag['kind']>(b: Binding | undefined, kind: K): Array<Extract<DocTag, { kind: K }>> {
        if (!b || !b.docBlock) return [];
        return b.docBlock.tags.filter(t => t.kind === kind) as Array<Extract<DocTag, { kind: K }>>;
    }

    /** Resolve `{type}` text to a TypeRef. Errors get pushed to
     *  `docTypeErrors` keyed by the binding's declaration range so the
     *  diagnostic surface can attach them. */
    private resolveDocType(text: string | undefined, attachAt: Range): TypeRef | null {
        if (!text) return null;
        const r = parseDocType(text, this.docCtx);
        for (const msg of r.errors) {
            this.docTypeErrors.push({ range: attachAt, message: msg });
        }
        return r.type;
    }

    /** Apply `@param` / `@returns` to a function declaration *before*
     *  the body is walked. Returns the function's declared return-type
     *  vector (or null if `@returns` was absent), so the caller can use
     *  it as a fallback when inference yields UNKNOWN. */
    private applyDocToFunction(decl: FunctionDecl, fnScope: Scope, declBinding?: Binding): TypeRef[] | null {
        const block = declBinding?.docBlock;
        if (!block) return null;

        for (const tag of block.tags) {
            if (tag.kind === 'param') {
                const pb = fnScope.bindings.get(tag.name);
                if (!pb) continue;
                const t = this.resolveDocType(tag.typeText, declBinding!.nameRange);
                if (t) {
                    pb.type = t;
                    this.bindingTypes.set(pb, t);
                }
            }
        }
        const returnsTags = block.tags.filter(t => t.kind === 'returns');
        if (returnsTags.length === 0) return null;
        const rets: TypeRef[] = [];
        for (const rt of returnsTags) {
            const t = this.resolveDocType(rt.typeText, declBinding!.nameRange);
            rets.push(t ?? UNKNOWN);
        }
        return rets;
    }

    /** Apply `@type` from a VarDecl's binding to its declared type. */
    private applyDocToVar(b: Binding | undefined): TypeRef | null {
        if (!b || !b.docBlock) return null;
        const typeTag = b.docBlock.tags.find(t => t.kind === 'type');
        if (!typeTag) return null;
        return this.resolveDocType(typeTag.typeText, b.nameRange);
    }

    /** Apply `@field` tags to a class instance's member table. */
    private applyDocFieldsToClass(b: Binding | undefined, instance: ObjectType): void {
        if (!b || !b.docBlock) return;
        for (const tag of b.docBlock.tags) {
            if (tag.kind !== 'field') continue;
            const t = this.resolveDocType(tag.typeText, b.nameRange) ?? ANY;
            instance.members.set(tag.name, {
                type: t,
                doc: tag.description || undefined,
                defRange: b.nameRange
            });
        }
    }

    /* ------------------------------------------------------------------- */
    /* Function summaries: cross-call shape effects                        */
    /* ------------------------------------------------------------------- */

    /** Walk a function body and record every direct `paramName.key = value`
     *  write. Reading is enough for now -- the spike that gets us most of
     *  the value is just the writes. Nested patterns (`p.a.b = v`),
     *  conditional writes, and writes through aliased locals are ignored;
     *  they fall back to today's behavior (no cross-call effect). */
    private collectFunctionSummary(body: BlockStmt, paramNames: string[]): FunctionSummary {
        const paramWrites = new Map<number, Map<string, TypeRef>>();
        if (paramNames.length === 0) return { paramWrites };
        const nameToIdx = new Map<string, number>();
        for (let i = 0; i < paramNames.length; i++) nameToIdx.set(paramNames[i], i);

        walkAst(body, (n) => {
            if (n.kind !== 'AssignStmt') return;
            const targets = n.targets;
            const rhs = n.rhs;
            for (let i = 0; i < targets.length; i++) {
                const t = targets[i];
                if (t.kind !== 'Member') continue;
                if (t.object.kind !== 'Ident') continue;
                const idx = nameToIdx.get(t.object.name);
                if (idx === undefined) continue;
                /* RHS type: positional match, fall back to ANY. */
                const r = rhs.length === targets.length ? rhs[i] : (rhs[0] ?? null);
                const valueT = r ? (this.nodeTypes.get(r) ?? ANY) : ANY;
                let m = paramWrites.get(idx);
                if (!m) { m = new Map(); paramWrites.set(idx, m); }
                const existing = m.get(t.property);
                m.set(t.property, existing ? unionOf([existing, valueT]) : valueT);
            }
        });
        return { paramWrites };
    }

    /** Replay a callee's summary onto the arguments at a call site, so
     *  the caller's local variables gain the keys the callee wrote.
     *  Conservative: only mutates argument types that are already
     *  object-shaped (so we don't promote scalars or break unions). */
    private replaySummary(fnType: FunctionType, args: Call['args'], scope: Scope): void {
        const sum = fnType.summary;
        if (!sum) return;
        for (const [paramIdx, writes] of sum.paramWrites) {
            const arg = args[paramIdx];
            if (!arg || arg.spread) continue;
            if (arg.expr.kind !== 'Ident') continue;
            const b = scope.lookup(arg.expr.name);
            if (!b) continue;
            const target = this.objectTargetFor(b);
            if (!target) continue;
            for (const [key, valueT] of writes) {
                setMember(target, key, { type: valueT });
            }
            /* If we promoted an empty/unknown var to a shape, refresh
             * the cached binding type so completion sees it. */
            this.bindingTypes.set(b, b.type);
        }
    }

    /** Find or create the ObjectType we should record writes on for a
     *  binding. Returns null when the binding's type isn't shape-like
     *  and we don't want to coerce it (e.g. it's a primitive, a class
     *  instance from a manifest, or a union). */
    private objectTargetFor(b: Binding): ObjectType | null {
        const t = b.type;
        if (t.kind === 'object') return t;
        if (t.kind === 'class') return t.instance;
        /* Promote `unknown` / freshly-empty-shape locals so the first
         * call into a writer gives them a body. We *don't* promote
         * `any`: that's a deliberate user-or-doc "anything goes" and
         * we shouldn't override it. */
        if (t.kind === 'prim' && t.name === 'unknown') {
            const fresh: ObjectType = emptyObject();
            b.type = fresh;
            return fresh;
        }
        return null;
    }

    private inferPipe(e: Pipe, scope: Scope, flow: FlowEnv): TypeRef {
        const srcT = this.expr(e.source, scope, flow);
        const inner = this.resolved.scopeOf.get(e) ?? scope;
        const pipeBind = inner.bindings.get('pipe');
        const element = srcT.kind === 'array' ? srcT.element : ANY;
        if (pipeBind) { pipeBind.type = element; this.bindingTypes.set(pipeBind, element); }
        const bodyT = this.expr(e.body, inner, flow);
        if (e.op === '~>') return arrayOf(firstOf(bodyT));
        /* ~!> and ~&> are filters: result is array of source-element. */
        return arrayOf(element);
    }
}

/** Build an absolute path candidate for a raw include argument, even when
 *  the file doesn't exist on disk. Used to locate `cando.api.json` for
 *  binary modules that haven't been compiled yet. */
function speculativeAbsPath(raw: string, documentUri: string, workspaceRoots: string[]): string | null {
    const docPath = uriToFsPath(documentUri);
    if (!docPath) return null;
    const path = require('path') as typeof import('path');
    if (path.isAbsolute(raw)) return raw;
    return path.resolve(path.dirname(docPath), raw);
    void workspaceRoots;
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

/** Operator -> metamethod name. Mirrors CanDo's runtime dispatch table. */
const METHOD_FOR_OP: Record<string, string> = {
    '+':  '__add',
    '-':  '__sub',
    '*':  '__mul',
    '/':  '__div',
    '%':  '__mod',
    '^':  '__pow',
    '==': '__eq',
    '<':  '__lt',
    '<=': '__le'
};

function isString(t: TypeRef): boolean {
    if (t.kind === 'prim') return t.name === 'string';
    if (t.kind === 'union') return t.variants.every(isString);
    if (t.kind === 'optional') return isString(t.inner);
    return false;
}

