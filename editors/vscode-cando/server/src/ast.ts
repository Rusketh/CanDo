/*
 * AST node definitions for the CanDo language.
 *
 * The parser (parser.ts) produces a Program node; every node carries a
 * source Range so the LSP layer can map cursor positions back to the
 * smallest containing node. Node shapes mirror the spec in
 * /home/user/CanDo/docs/language/{statements,expressions,functions,classes,modules}.md.
 */

import { Range } from './lexer';

export type Node = Program | Stmt | Expr | IfBranch;

export interface Base { range: Range; }

/* -- Top level ----------------------------------------------------------- */

export interface Program extends Base {
    kind: 'Program';
    body: Stmt[];
}

/* -- Statements ---------------------------------------------------------- */

export type Stmt =
    | VarDecl
    | AssignStmt
    | ExprStmt
    | BlockStmt
    | IfStmt
    | WhileStmt
    | ForRange
    | ForKeys
    | ForValues
    | ForOver
    | BreakStmt
    | ContinueStmt
    | SettleStmt
    | ReturnStmt
    | ThrowStmt
    | TryStmt
    | FunctionDecl
    | ClassDecl
    | EmptyStmt;

export interface VarDecl extends Base {
    kind: 'VarDecl';
    keyword: 'VAR' | 'CONST' | 'GLOBAL';
    targets: Pattern[];
    /** May be empty when the declaration has no initializer (rare). */
    init: Expr[];
}

export type AssignTarget = Ident | Member | Index;

export interface AssignStmt extends Base {
    kind: 'AssignStmt';
    targets: AssignTarget[];
    op: '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '^=';
    rhs: Expr[];
}

export interface ExprStmt extends Base {
    kind: 'ExprStmt';
    expr: Expr;
}

export interface BlockStmt extends Base {
    kind: 'BlockStmt';
    body: Stmt[];
}

export interface IfBranch extends Base {
    /** IF | ELSE IF | ELSE | ALSO IF | ALSO */
    kind: 'IfBranch';
    branchKind: 'IF' | 'ELSE_IF' | 'ELSE' | 'ALSO_IF' | 'ALSO';
    cond?: Expr;
    body: BlockStmt;
}

export interface IfStmt extends Base {
    kind: 'IfStmt';
    chain: IfBranch[];
}

export interface WhileStmt extends Base {
    kind: 'WhileStmt';
    cond: Expr;
    body: BlockStmt;
}

export interface ForRange extends Base {
    kind: 'ForRange';
    ident: string;
    identRange: Range;
    dir: 'ASC' | 'DESC';
    from: Expr;
    to: Expr;
    body: BlockStmt;
}

export interface ForKeys extends Base {
    kind: 'ForKeys';
    ident: string;
    identRange: Range;
    src: Expr;
    body: BlockStmt;
}

export interface ForValues extends Base {
    kind: 'ForValues';
    ident: string;
    identRange: Range;
    src: Expr;
    body: BlockStmt;
}

export interface ForOver extends Base {
    kind: 'ForOver';
    idents: string[];
    identRanges: Range[];
    iter: Expr;
    body: BlockStmt;
}

export interface BreakStmt extends Base { kind: 'BreakStmt'; depth: number; }
export interface ContinueStmt extends Base { kind: 'ContinueStmt'; depth: number; }
export interface SettleStmt extends Base { kind: 'SettleStmt'; depth: number; }

export interface ReturnStmt extends Base {
    kind: 'ReturnStmt';
    values: Expr[];
}

export interface ThrowStmt extends Base {
    kind: 'ThrowStmt';
    values: Expr[];
}

export interface TryStmt extends Base {
    kind: 'TryStmt';
    tryBlock: BlockStmt;
    catch?: { params: Pattern[]; paramRanges: Range[]; body: BlockStmt; range: Range };
    finally?: BlockStmt;
}

export interface FunctionDecl extends Base {
    kind: 'FunctionDecl';
    name: string;
    nameRange: Range;
    params: Param[];
    body: BlockStmt;
}

export interface ClassDecl extends Base {
    kind: 'ClassDecl';
    name: string;
    nameRange: Range;
    extendsName?: string;
    extendsRange?: Range;
    ctorParams: Param[];
    body: BlockStmt;
}

export interface EmptyStmt extends Base { kind: 'EmptyStmt'; }

/* -- Expressions --------------------------------------------------------- */

export type Expr =
    | NumberLit
    | StringLit
    | TemplateLit
    | BoolLit
    | NullLit
    | Ident
    | ArrayLit
    | ObjectLit
    | Member
    | Index
    | Call
    | Unary
    | Postfix
    | Binary
    | MultiCompare
    | Ternary
    | FunctionExpr
    | ClassExpr
    | Mask
    | Spread
    | Pipe
    | Paren
    | RangeExpr
    | ThreadExpr
    | AwaitExpr
    | ErrorExpr;

export interface NumberLit extends Base { kind: 'NumberLit'; value: number; raw: string; }
export interface StringLit extends Base { kind: 'StringLit'; value: string; quote: '"' | "'"; }
export interface BoolLit   extends Base { kind: 'BoolLit'; value: boolean; }
export interface NullLit   extends Base { kind: 'NullLit'; }
export interface Ident     extends Base { kind: 'Ident'; name: string; }

export interface TemplatePart {
    kind: 'str' | 'expr';
    /** For 'str' parts. */
    value?: string;
    /** For 'expr' parts. The expression inside ${ ... }. */
    expr?: Expr;
    range: Range;
}
export interface TemplateLit extends Base {
    kind: 'TemplateLit';
    parts: TemplatePart[];
}

export interface ArrayLit extends Base {
    kind: 'ArrayLit';
    elements: Expr[];
}

export interface ObjectKey {
    /** ident: { foo: 1 } ; string: { "name": 1 } ; computed: { [expr]: 1 } */
    kind: 'ident' | 'string' | 'computed';
    name?: string;
    expr?: Expr;
    range: Range;
}
export interface ObjectProp { key: ObjectKey; value: Expr; range: Range; }
export interface ObjectLit extends Base {
    kind: 'ObjectLit';
    properties: ObjectProp[];
}

export interface Member extends Base {
    kind: 'Member';
    object: Expr;
    property: string;
    propertyRange: Range;
    safe: boolean;        // ?.
    /** ':' = method call dispatch (later wrapped in Call); '.' = normal. */
    accessor: '.' | ':' | '::';
}

export interface Index extends Base {
    kind: 'Index';
    object: Expr;
    index: Expr;
    safe: boolean;        // ?[
}

export interface Arg { expr: Expr; spread: boolean; range: Range; }

export interface Call extends Base {
    kind: 'Call';
    /** Function being called; for method/fluent calls callee is a Member with
     *  accessor ':' or '::'. */
    callee: Expr;
    args: Arg[];
    style: 'paren' | 'method' | 'fluent';
}

export interface Unary extends Base {
    kind: 'Unary';
    op: '-' | '!' | '#' | '~';
    argument: Expr;
}

export interface Postfix extends Base {
    kind: 'Postfix';
    op: '++' | '--';
    argument: Expr;
}

export interface Binary extends Base {
    kind: 'Binary';
    op: string;     // '+', '-', '*', '/', '%', '^', '&&', '||', '|', '&', '|&',
                    // '<<', '>>', '<', '>', '<=', '>=', '==', '!='
    left: Expr;
    right: Expr;
}

export interface MultiCompare extends Base {
    kind: 'MultiCompare';
    op: '==' | '!=' | '<' | '<=' | '>' | '>=';
    left: Expr;
    rights: Expr[];
}

export interface Ternary extends Base {
    kind: 'Ternary';
    cond: Expr;
    cons: Expr;
    alt: Expr;
}

export interface Param {
    name: string;
    rest: boolean;
    range: Range;
}

export interface FunctionExpr extends Base {
    kind: 'FunctionExpr';
    /** Named expression form: VAR f = FUNCTION foo(...) {...} */
    name?: string;
    nameRange?: Range;
    params: Param[];
    body: BlockStmt;
    arrow: boolean;
}

export interface ClassExpr extends Base {
    kind: 'ClassExpr';
    name?: string;
    nameRange?: Range;
    extendsName?: string;
    ctorParams: Param[];
    body: BlockStmt;
}

export interface Mask extends Base {
    kind: 'Mask';
    /** keep = '~', skip = '.' positions. */
    mask: ('keep' | 'skip')[];
    expr: Expr;
}

export interface Spread extends Base {
    kind: 'Spread';
    argument: Expr;
}

export interface Pipe extends Base {
    kind: 'Pipe';
    source: Expr;
    op: '~>' | '~!>' | '~&>';
    /** Either a single expression or a block (`{ ... RETURN x; }`). */
    body: Expr | BlockStmt;
}

export interface Paren extends Base {
    kind: 'Paren';
    expression: Expr;
}

export interface RangeExpr extends Base {
    kind: 'RangeExpr';
    from: Expr;
    to: Expr;
    dir: 'ASC' | 'DESC';
}

/** Used during error recovery so traversal never breaks. */
export interface ErrorExpr extends Base { kind: 'ErrorExpr'; }

/** `thread { ... }` (block form) or `thread expr` (call form). Spawns
 *  an OS thread that runs the body; evaluates to a thread handle. */
export interface ThreadExpr extends Base {
    kind: 'ThreadExpr';
    /** Either a BlockStmt for the block form or an Expr for the call
     *  form. We don't normalise -- inference walks both shapes. */
    body: BlockStmt | Expr;
}

/** `await expr` -- blocks until the thread completes and yields its
 *  return value(s). */
export interface AwaitExpr extends Base {
    kind: 'AwaitExpr';
    argument: Expr;
}

/* -- Patterns ------------------------------------------------------------ */

export type Pattern = IdentPattern | RestPattern;

export interface IdentPattern { kind: 'IdentPattern'; name: string; range: Range; }
export interface RestPattern  { kind: 'RestPattern';  name: string; range: Range; }

/* -- Visitor ------------------------------------------------------------- */

/**
 * Pre-order visit every AST node reachable from `root`. The visitor's return
 * value is ignored; mutating fields is not supported here -- this is a
 * read-only walker for collecting symbols, finding cursor positions, etc.
 */
export function walk(root: Node, visit: (n: Node, parent: Node | null) => void, parent: Node | null = null): void {
    visit(root, parent);
    for (const child of children(root)) walk(child, visit, root);
}

/** Yield direct children of `n` -- used by walk() and the cursor finder. */
export function* children(n: Node): IterableIterator<Node> {
    switch (n.kind) {
        case 'Program':
            for (const s of n.body) yield s;
            return;

        case 'VarDecl':
            for (const e of n.init) yield e;
            return;
        case 'AssignStmt':
            for (const t of n.targets) {
                /* Targets are AssignTargets (Ident | Member | Index) -- all
                 * Expr kinds, so safe to yield. */
                yield t;
            }
            for (const e of n.rhs) yield e;
            return;
        case 'ExprStmt': yield n.expr; return;
        case 'BlockStmt': for (const s of n.body) yield s; return;
        case 'IfStmt':
            for (const b of n.chain) yield b;
            return;
        case 'IfBranch':
            if (n.cond) yield n.cond;
            yield n.body;
            return;
        case 'WhileStmt': yield n.cond; yield n.body; return;
        case 'ForRange': yield n.from; yield n.to; yield n.body; return;
        case 'ForKeys': yield n.src; yield n.body; return;
        case 'ForValues': yield n.src; yield n.body; return;
        case 'ForOver': yield n.iter; yield n.body; return;
        case 'ReturnStmt': for (const v of n.values) yield v; return;
        case 'ThrowStmt': for (const v of n.values) yield v; return;
        case 'TryStmt':
            yield n.tryBlock;
            if (n.catch) yield n.catch.body;
            if (n.finally) yield n.finally;
            return;
        case 'FunctionDecl': yield n.body; return;
        case 'ClassDecl': yield n.body; return;

        case 'TemplateLit':
            for (const p of n.parts) if (p.kind === 'expr' && p.expr) yield p.expr;
            return;
        case 'ArrayLit': for (const e of n.elements) yield e; return;
        case 'ObjectLit':
            for (const p of n.properties) {
                if (p.key.kind === 'computed' && p.key.expr) yield p.key.expr;
                yield p.value;
            }
            return;
        case 'Member': yield n.object; return;
        case 'Index':  yield n.object; yield n.index; return;
        case 'Call':
            yield n.callee;
            for (const a of n.args) yield a.expr;
            return;
        case 'Unary': yield n.argument; return;
        case 'Postfix': yield n.argument; return;
        case 'Binary': yield n.left; yield n.right; return;
        case 'MultiCompare': yield n.left; for (const r of n.rights) yield r; return;
        case 'Ternary': yield n.cond; yield n.cons; yield n.alt; return;
        case 'FunctionExpr': yield n.body; return;
        case 'ClassExpr': yield n.body; return;
        case 'Mask': yield n.expr; return;
        case 'Spread': yield n.argument; return;
        case 'Pipe':
            yield n.source;
            yield n.body;
            return;
        case 'Paren': yield n.expression; return;
        case 'RangeExpr': yield n.from; yield n.to; return;
        case 'ThreadExpr': yield n.body; return;
        case 'AwaitExpr': yield n.argument; return;

        default:
            return;
    }
}

/* -- Range helpers ------------------------------------------------------- */

export function rangeContains(r: Range, line: number, character: number): boolean {
    const start = r.start, end = r.end;
    if (line < start.line || line > end.line) return false;
    if (line === start.line && character < start.character) return false;
    if (line === end.line && character > end.character) return false;
    return true;
}

/** Find the deepest AST node whose range contains the cursor position. */
export function nodeAt(root: Node, line: number, character: number): Node | null {
    if (!rangeContains(root.range, line, character)) return null;
    for (const c of children(root)) {
        const hit = nodeAt(c, line, character);
        if (hit) return hit;
    }
    return root;
}

/** Find the smallest expression node whose range contains the cursor. */
export function exprAt(root: Node, line: number, character: number): Expr | null {
    const n = nodeAt(root, line, character);
    if (!n) return null;
    if (isExpr(n)) return n;
    return null;
}

function isExpr(n: Node): n is Expr {
    switch (n.kind) {
        case 'NumberLit': case 'StringLit': case 'TemplateLit': case 'BoolLit':
        case 'NullLit': case 'Ident': case 'ArrayLit': case 'ObjectLit':
        case 'Member': case 'Index': case 'Call': case 'Unary': case 'Postfix':
        case 'Binary': case 'MultiCompare': case 'Ternary': case 'FunctionExpr':
        case 'ClassExpr': case 'Mask': case 'Spread': case 'Pipe': case 'Paren':
        case 'RangeExpr': case 'ErrorExpr':
            return true;
    }
    return false;
}
