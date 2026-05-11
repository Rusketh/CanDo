/*
 * Recursive-descent (statements) + Pratt (expressions) parser for CanDo.
 *
 * Input  : Token[] from lexer.ts.
 * Output : Program AST + ParseError[] (parse-mode-recovery diagnostics).
 *
 * Recovery: panic-mode to the next `;` or `}` -- the resulting AST may
 * contain ErrorExpr placeholders so intellisense keeps working on the
 * rest of the file even when the cursor is mid-edit.
 *
 * Precedence table is taken verbatim from
 *   /home/user/CanDo/docs/language/expressions.md
 * with the runtime parser at /home/user/CanDo/source/parser/parser.c as
 * the tiebreaker for edge cases.
 */

import { Token, Range, Position } from './lexer';
import {
    Program, Stmt, Expr, BlockStmt, VarDecl, AssignStmt, AssignTarget,
    ExprStmt, IfStmt, IfBranch, WhileStmt, ForRange, ForKeys, ForValues,
    ForOver, BreakStmt, ContinueStmt, SettleStmt, ReturnStmt, ThrowStmt,
    TryStmt, FunctionDecl, ClassDecl, EmptyStmt,
    NumberLit, StringLit, BoolLit, NullLit, Ident, ArrayLit, ObjectLit,
    ObjectProp, ObjectKey, Member, Index, Call, Arg, Unary, Postfix,
    Binary, MultiCompare, Ternary, FunctionExpr, ClassExpr, Mask, Spread,
    Pipe, Paren, RangeExpr, ErrorExpr, TemplateLit, TemplatePart,
    Param, Pattern, IdentPattern, RestPattern
} from './ast';

export interface ParseError {
    range: Range;
    message: string;
}

export interface ParseResult {
    program: Program;
    errors: ParseError[];
}

const ASSIGNMENT_OPS = new Set(['=', '+=', '-=', '*=', '/=', '%=', '^=']);
const COMPARE_EQ = new Set(['==', '!=']);
const COMPARE_ORD = new Set(['<', '<=', '>', '>=']);

const ZERO_POS: Position = { line: 0, character: 0 };
const ZERO_RANGE: Range = { start: ZERO_POS, end: ZERO_POS };

export function parse(tokens: Token[]): ParseResult {
    const p = new Parser(tokens);
    const program = p.parseProgram();
    return { program, errors: p.errors };
}

class Parser {
    private readonly toks: Token[];
    /** Index into `toks`. Comment / newline / error tokens are skipped on read. */
    private i = 0;
    public errors: ParseError[] = [];

    constructor(tokens: Token[]) {
        this.toks = tokens.filter(t => t.kind !== 'comment' && t.kind !== 'newline');
    }

    /* ----------------------------------------------------------------- */
    /* Token helpers                                                     */
    /* ----------------------------------------------------------------- */

    private peek(off = 0): Token {
        return this.toks[this.i + off] ?? this.eofTok();
    }
    private eofTok(): Token {
        const last = this.toks[this.toks.length - 1];
        if (last) return last;
        return { kind: 'eof', value: '', range: ZERO_RANGE };
    }
    private advance(): Token {
        const t = this.peek();
        if (this.i < this.toks.length) this.i++;
        return t;
    }
    private atEnd(): boolean {
        return this.peek().kind === 'eof';
    }
    private isKw(t: Token, ...names: string[]): boolean {
        if (t.kind !== 'keyword') return false;
        const up = t.value.toUpperCase();
        return names.includes(up);
    }
    private matchKw(...names: string[]): boolean {
        if (this.isKw(this.peek(), ...names)) { this.advance(); return true; }
        return false;
    }
    private matchOp(...ops: string[]): boolean {
        const t = this.peek();
        if ((t.kind === 'op' || t.kind === 'punct') && ops.includes(t.value)) {
            this.advance();
            return true;
        }
        return false;
    }
    private isOp(...ops: string[]): boolean {
        const t = this.peek();
        return (t.kind === 'op' || t.kind === 'punct') && ops.includes(t.value);
    }
    private expectOp(op: string, ctx: string): Token | null {
        const t = this.peek();
        if ((t.kind === 'op' || t.kind === 'punct') && t.value === op) return this.advance();
        this.error(t.range, `Expected '${op}' in ${ctx}`);
        return null;
    }
    private expectKw(name: string, ctx: string): Token | null {
        const t = this.peek();
        if (this.isKw(t, name)) return this.advance();
        this.error(t.range, `Expected '${name}' in ${ctx}`);
        return null;
    }
    private expectIdent(ctx: string): Token | null {
        const t = this.peek();
        if (t.kind === 'ident') return this.advance();
        this.error(t.range, `Expected identifier in ${ctx}`);
        return null;
    }

    private error(r: Range, msg: string): void {
        this.errors.push({ range: r, message: msg });
    }

    /* ----------------------------------------------------------------- */
    /* Program / statements                                              */
    /* ----------------------------------------------------------------- */

    public parseProgram(): Program {
        const start = this.peek().range.start;
        const body: Stmt[] = [];
        while (!this.atEnd()) {
            const before = this.i;
            const s = this.parseStmtSafe();
            if (s) body.push(s);
            if (this.i === before) this.advance();  // forward-progress guard
        }
        const end = this.peek().range.end;
        return { kind: 'Program', body, range: { start, end } };
    }

    /** parseStatement with panic-mode recovery. */
    private parseStmtSafe(): Stmt | null {
        try {
            return this.parseStmt();
        } catch (e) {
            this.synchronize();
            return null;
        }
    }

    private synchronize(): void {
        /* Skip ahead until we hit a `;`, a closing `}` we're inside of, or
         * the start of a statement-introducing keyword. Never consume the
         * synchronization token itself, so the outer loop can handle it. */
        while (!this.atEnd()) {
            const t = this.peek();
            if ((t.kind === 'op' || t.kind === 'punct') && t.value === ';') {
                this.advance(); return;
            }
            if ((t.kind === 'op' || t.kind === 'punct') && t.value === '}') return;
            if (t.kind === 'keyword') {
                const up = t.value.toUpperCase();
                if (['IF', 'WHILE', 'FOR', 'FUNCTION', 'CLASS', 'RETURN',
                     'THROW', 'TRY', 'CATCH', 'FINALY', 'BREAK', 'CONTINUE',
                     'SETTLE', 'VAR', 'CONST', 'GLOBAL', 'THREAD'].includes(up)) return;
            }
            this.advance();
        }
    }

    private parseStmt(): Stmt | null {
        const t = this.peek();
        if (t.kind === 'keyword') {
            const up = t.value.toUpperCase();
            switch (up) {
                case 'VAR':
                case 'CONST':
                case 'GLOBAL':
                    return this.parseVarDecl(up as 'VAR' | 'CONST' | 'GLOBAL');
                case 'IF':       return this.parseIf();
                case 'WHILE':    return this.parseWhile();
                case 'FOR':      return this.parseFor();
                case 'FUNCTION': return this.parseFunctionDecl();
                case 'CLASS':    return this.parseClassDecl();
                case 'RETURN':   return this.parseReturn();
                case 'THROW':    return this.parseThrow();
                case 'TRY':      return this.parseTry();
                case 'BREAK':    return this.parseBreakLike('BreakStmt');
                case 'CONTINUE': return this.parseBreakLike('ContinueStmt');
                case 'SETTLE':   return this.parseBreakLike('SettleStmt');
                case 'THREAD': {
                    /* THREAD expr; — desugars to a thread-spawn; for analysis we
                     * just treat it as an expression statement so the body is
                     * still typed. */
                    const tStart = this.advance().range.start;
                    const expr = this.parseExpression();
                    this.matchOp(';');
                    return { kind: 'ExprStmt', expr, range: { start: tStart, end: expr.range.end } };
                }
            }
        }
        if (this.isOp('{')) return this.parseBlock();
        if (this.isOp(';')) {
            const semi = this.advance();
            return { kind: 'EmptyStmt', range: semi.range };
        }
        return this.parseExpressionStatement();
    }

    private parseBlock(): BlockStmt {
        const open = this.expectOp('{', 'block') ?? this.peek();
        const body: Stmt[] = [];
        while (!this.atEnd() && !this.isOp('}')) {
            const before = this.i;
            const s = this.parseStmtSafe();
            if (s) body.push(s);
            if (this.i === before) this.advance();
        }
        const close = this.peek();
        this.matchOp('}');
        return {
            kind: 'BlockStmt',
            body,
            range: { start: open.range.start, end: close.range.end }
        };
    }

    private parseVarDecl(keyword: 'VAR' | 'CONST' | 'GLOBAL'): VarDecl {
        const kwTok = this.advance();
        const targets: Pattern[] = [this.parsePattern()];
        while (this.matchOp(',')) targets.push(this.parsePattern());

        const init: Expr[] = [];
        if (this.matchOp('=')) {
            init.push(this.parseExpression());
            while (this.matchOp(',')) init.push(this.parseExpression());
        }

        const semi = this.peek();
        this.matchOp(';');
        return {
            kind: 'VarDecl',
            keyword,
            targets,
            init,
            range: { start: kwTok.range.start, end: semi.range.end }
        };
    }

    private parsePattern(): Pattern {
        if (this.matchOp('...')) {
            const id = this.expectIdent('rest pattern');
            const name = id?.value ?? '';
            const range = id?.range ?? this.peek().range;
            return { kind: 'RestPattern', name, range };
        }
        const id = this.expectIdent('pattern');
        const name = id?.value ?? '';
        const range = id?.range ?? this.peek().range;
        return { kind: 'IdentPattern', name, range };
    }

    private parseIf(): IfStmt {
        const start = this.peek().range.start;
        const chain: IfBranch[] = [];
        /* First branch: IF cond { body } */
        const ifKw = this.advance();
        const cond1 = this.parseConditionExpr();
        const body1 = this.parseBlock();
        chain.push({
            kind: 'IfBranch',
            branchKind: 'IF',
            cond: cond1,
            body: body1,
            range: { start: ifKw.range.start, end: body1.range.end }
        });

        while (true) {
            if (this.isKw(this.peek(), 'ELSE')) {
                const elseKw = this.advance();
                if (this.isKw(this.peek(), 'IF')) {
                    this.advance();
                    const c = this.parseConditionExpr();
                    const b = this.parseBlock();
                    chain.push({
                        kind: 'IfBranch',
                        branchKind: 'ELSE_IF',
                        cond: c, body: b,
                        range: { start: elseKw.range.start, end: b.range.end }
                    });
                } else {
                    const b = this.parseBlock();
                    chain.push({
                        kind: 'IfBranch',
                        branchKind: 'ELSE',
                        body: b,
                        range: { start: elseKw.range.start, end: b.range.end }
                    });
                }
            } else if (this.isKw(this.peek(), 'ALSO')) {
                const alsoKw = this.advance();
                if (this.isKw(this.peek(), 'IF')) {
                    this.advance();
                    const c = this.parseConditionExpr();
                    const b = this.parseBlock();
                    chain.push({
                        kind: 'IfBranch',
                        branchKind: 'ALSO_IF',
                        cond: c, body: b,
                        range: { start: alsoKw.range.start, end: b.range.end }
                    });
                } else {
                    const b = this.parseBlock();
                    chain.push({
                        kind: 'IfBranch',
                        branchKind: 'ALSO',
                        body: b,
                        range: { start: alsoKw.range.start, end: b.range.end }
                    });
                }
            } else break;
        }
        return {
            kind: 'IfStmt',
            chain,
            range: { start, end: chain[chain.length - 1].range.end }
        };
    }

    /**
     * Parse an expression that may include trailing `, expr` continuations
     * extending a top-level comparison into a multi-compare. Used by IF /
     * ELSE IF / ALSO IF / WHILE / ternary `?` conditions.
     */
    private parseConditionExpr(): Expr {
        const e = this.parseExpression();
        if (e.kind !== 'Binary' && e.kind !== 'MultiCompare') return e;
        if (e.kind === 'Binary' && !COMPARE_EQ.has(e.op) && !COMPARE_ORD.has(e.op)) return e;
        if (!this.isOp(',')) return e;

        const rights: Expr[] = [];
        let op: '==' | '!=' | '<' | '<=' | '>' | '>=';
        let left: Expr;
        if (e.kind === 'MultiCompare') {
            op = e.op;
            left = e.left;
            for (const r of e.rights) rights.push(r);
        } else {
            op = e.op as MultiCompare['op'];
            left = e.left;
            rights.push(e.right);
        }
        while (this.matchOp(',')) {
            /* Each alternative is parsed above comparison precedence so
             * additional `<` / `>` don't chain into the multi-compare. */
            rights.push(this.parseBinaryAt(10));
        }
        return {
            kind: 'MultiCompare',
            op, left, rights,
            range: { start: e.range.start, end: rights[rights.length - 1].range.end }
        };
    }

    private parseWhile(): WhileStmt {
        const kw = this.advance();
        const cond = this.parseConditionExpr();
        const body = this.parseBlock();
        return {
            kind: 'WhileStmt',
            cond, body,
            range: { start: kw.range.start, end: body.range.end }
        };
    }

    private parseFor(): Stmt {
        const kw = this.advance();
        /* FOR <ident> [, <ident>...] (IN|OF|OVER) <expr> { body } */
        const idents: { name: string; range: Range }[] = [];
        const first = this.expectIdent('for-loop variable');
        if (first) idents.push({ name: first.value, range: first.range });
        while (this.matchOp(',')) {
            const next = this.expectIdent('for-loop variable');
            if (next) idents.push({ name: next.value, range: next.range });
        }

        const kindTok = this.peek();
        let loopKind: 'IN' | 'OF' | 'OVER' = 'IN';
        if (this.isKw(kindTok, 'IN')) { loopKind = 'IN'; this.advance(); }
        else if (this.isKw(kindTok, 'OF')) { loopKind = 'OF'; this.advance(); }
        else if (this.isKw(kindTok, 'OVER')) { loopKind = 'OVER'; this.advance(); }
        else this.error(kindTok.range, "Expected IN, OF, or OVER in FOR loop");

        const expr = this.parseExpression();
        const body = this.parseBlock();
        const range = { start: kw.range.start, end: body.range.end };

        const ident0 = idents[0] ?? { name: '', range: kw.range };

        if (loopKind === 'IN') {
            if (expr.kind === 'RangeExpr') {
                return {
                    kind: 'ForRange',
                    ident: ident0.name,
                    identRange: ident0.range,
                    dir: expr.dir,
                    from: expr.from,
                    to: expr.to,
                    body, range
                };
            }
            return {
                kind: 'ForKeys',
                ident: ident0.name,
                identRange: ident0.range,
                src: expr,
                body, range
            };
        }
        if (loopKind === 'OF') {
            return {
                kind: 'ForValues',
                ident: ident0.name,
                identRange: ident0.range,
                src: expr,
                body, range
            };
        }
        /* OVER -- multi-target supported. */
        return {
            kind: 'ForOver',
            idents: idents.map(x => x.name),
            identRanges: idents.map(x => x.range),
            iter: expr,
            body, range
        };
    }

    private parseFunctionDecl(): Stmt {
        const kw = this.advance();
        /* Statement form: FUNCTION name(params) { body }
         * Expression form: FUNCTION(params) { body } ... if next is `(` we
         * treat the whole thing as an expression statement. */
        if (this.peek().kind === 'ident') {
            const nameTok = this.advance();
            const params = this.parseParamList();
            const body = this.parseBlock();
            return {
                kind: 'FunctionDecl',
                name: nameTok.value,
                nameRange: nameTok.range,
                params, body,
                range: { start: kw.range.start, end: body.range.end }
            };
        }
        /* Anonymous function expression -- rewind so parseExpressionStatement
         * picks it up. */
        this.i--;
        return this.parseExpressionStatement();
    }

    private parseParamList(): Param[] {
        const params: Param[] = [];
        this.expectOp('(', 'parameter list');
        if (!this.isOp(')')) {
            do {
                if (this.matchOp('...')) {
                    const id = this.expectIdent('rest parameter');
                    if (id) params.push({ name: id.value, rest: true, range: id.range });
                } else {
                    const id = this.expectIdent('parameter');
                    if (id) params.push({ name: id.value, rest: false, range: id.range });
                }
            } while (this.matchOp(','));
        }
        this.matchOp(')');
        return params;
    }

    private parseClassDecl(): Stmt {
        const kw = this.advance();
        /* CLASS Name [EXTENDS Parent] = (self, ...) { body }
         * Anonymous expression form (CLASS (...) { ... }) is delegated to
         * the expression parser. */
        if (this.peek().kind !== 'ident') {
            this.i--;
            return this.parseExpressionStatement();
        }
        const nameTok = this.advance();
        let extendsName: string | undefined;
        let extendsRange: Range | undefined;
        if (this.matchKw('EXTENDS')) {
            const p = this.expectIdent('EXTENDS target');
            if (p) { extendsName = p.value; extendsRange = p.range; }
        }
        let ctorParams: Param[] = [];
        if (this.matchOp('=')) {
            if (this.isOp('(')) ctorParams = this.parseParamList();
        }
        const body = this.parseBlock();
        return {
            kind: 'ClassDecl',
            name: nameTok.value,
            nameRange: nameTok.range,
            extendsName, extendsRange,
            ctorParams, body,
            range: { start: kw.range.start, end: body.range.end }
        };
    }

    private parseReturn(): ReturnStmt {
        const kw = this.advance();
        const values: Expr[] = [];
        if (!this.isOp(';') && !this.isOp('}') && !this.atEnd()) {
            values.push(this.parseExpression());
            while (this.matchOp(',')) values.push(this.parseExpression());
        }
        const semi = this.peek();
        this.matchOp(';');
        return {
            kind: 'ReturnStmt',
            values,
            range: { start: kw.range.start, end: semi.range.end }
        };
    }

    private parseThrow(): ThrowStmt {
        const kw = this.advance();
        const values: Expr[] = [];
        if (!this.isOp(';') && !this.isOp('}') && !this.atEnd()) {
            values.push(this.parseExpression());
            while (this.matchOp(',')) values.push(this.parseExpression());
        }
        const semi = this.peek();
        this.matchOp(';');
        return {
            kind: 'ThrowStmt',
            values,
            range: { start: kw.range.start, end: semi.range.end }
        };
    }

    private parseTry(): TryStmt {
        const kw = this.advance();
        const tryBlock = this.parseBlock();
        let catchClause: TryStmt['catch'] = undefined;
        let finallyBlock: BlockStmt | undefined;
        if (this.matchKw('CATCH')) {
            const catchStart = this.peek().range.start;
            const params: Pattern[] = [];
            const paramRanges: Range[] = [];
            if (this.matchOp('(')) {
                if (!this.isOp(')')) {
                    do {
                        const p = this.parsePattern();
                        params.push(p);
                        paramRanges.push(p.range);
                    } while (this.matchOp(','));
                }
                this.matchOp(')');
            } else if (this.peek().kind === 'ident') {
                /* CATCH name { } — single bare param without parens. */
                const id = this.advance();
                params.push({ kind: 'IdentPattern', name: id.value, range: id.range });
                paramRanges.push(id.range);
            }
            const body = this.parseBlock();
            catchClause = {
                params, paramRanges, body,
                range: { start: catchStart, end: body.range.end }
            };
        }
        if (this.matchKw('FINALY') || this.matchKw('FINALLY')) {
            finallyBlock = this.parseBlock();
        }
        const endRange = finallyBlock?.range.end
            ?? catchClause?.range.end
            ?? tryBlock.range.end;
        return {
            kind: 'TryStmt',
            tryBlock,
            catch: catchClause,
            finally: finallyBlock,
            range: { start: kw.range.start, end: endRange }
        };
    }

    private parseBreakLike(kind: 'BreakStmt' | 'ContinueStmt' | 'SettleStmt'): Stmt {
        const kw = this.advance();
        let depth = 0;
        if (this.peek().kind === 'number') {
            depth = parseInt(this.peek().value, 10) | 0;
            this.advance();
        }
        const semi = this.peek();
        this.matchOp(';');
        const range = { start: kw.range.start, end: semi.range.end };
        if (kind === 'BreakStmt')   return { kind: 'BreakStmt',   depth, range };
        if (kind === 'ContinueStmt') return { kind: 'ContinueStmt', depth, range };
        return { kind: 'SettleStmt', depth, range };
    }

    private parseExpressionStatement(): Stmt {
        const start = this.peek().range.start;
        const first = this.parseExpression();

        /* Multi-target assignment: `a, b = x, y;` */
        if (this.isOp(',') && this.isAssignTarget(first)) {
            const targets: AssignTarget[] = [first as AssignTarget];
            while (this.matchOp(',')) {
                const next = this.parseExpression();
                if (!this.isAssignTarget(next)) {
                    this.error(next.range, 'Invalid assignment target');
                    targets.push({ kind: 'Ident', name: '', range: next.range });
                } else {
                    targets.push(next as AssignTarget);
                }
            }
            if (this.peek().kind === 'op' && ASSIGNMENT_OPS.has(this.peek().value)) {
                const opTok = this.advance();
                const rhs: Expr[] = [this.parseExpression()];
                while (this.matchOp(',')) rhs.push(this.parseExpression());
                const semi = this.peek();
                this.matchOp(';');
                return {
                    kind: 'AssignStmt',
                    targets,
                    op: opTok.value as AssignStmt['op'],
                    rhs,
                    range: { start, end: semi.range.end }
                };
            }
            this.error(this.peek().range, "Expected '=' after multi-target");
            const semi = this.peek();
            this.matchOp(';');
            return {
                kind: 'ExprStmt',
                expr: first,
                range: { start, end: semi.range.end }
            };
        }

        /* Single-target assignment / compound assignment. */
        if (this.peek().kind === 'op' && ASSIGNMENT_OPS.has(this.peek().value)
            && this.isAssignTarget(first)) {
            const opTok = this.advance();
            const rhs: Expr[] = [this.parseExpression()];
            while (this.matchOp(',')) rhs.push(this.parseExpression());
            const semi = this.peek();
            this.matchOp(';');
            return {
                kind: 'AssignStmt',
                targets: [first as AssignTarget],
                op: opTok.value as AssignStmt['op'],
                rhs,
                range: { start, end: semi.range.end }
            };
        }

        const semi = this.peek();
        this.matchOp(';');
        return {
            kind: 'ExprStmt',
            expr: first,
            range: { start, end: semi.range.end }
        };
    }

    private isAssignTarget(e: Expr): boolean {
        return e.kind === 'Ident' || e.kind === 'Member' || e.kind === 'Index';
    }

    /* ----------------------------------------------------------------- */
    /* Expressions (Pratt)                                               */
    /* ----------------------------------------------------------------- */

    /*
     * Precedence (low → high):
     *   1  assignment      = += -= ...  (right-assoc) -- handled at stmt level
     *   2  ternary         ? :          (right-assoc)
     *   3  pipe            ~> ~!> ~&>
     *   4  logical OR      ||
     *   5  logical AND     &&
     *   6  bitwise OR/XOR  |  |&
     *   7  bitwise AND     &
     *   8  equality        == !=        (collected into MultiCompare if cond)
     *   9  comparison      < <= > >=
     *  10  shift           << >>
     *  11  range           -> <-        (produces RangeExpr)
     *  12  additive        + -
     *  13  multiplicative  * / %
     *  14  power           ^            (right-assoc)
     *  15  unary prefix    - ! # ~
     *  16  postfix (call, member, index, :, ::, ?., ?[, ++, --)
     */

    public parseExpression(): Expr {
        return this.parseTernary();
    }

    private parseTernary(): Expr {
        const cond = this.parseBinaryAt(3);
        if (this.matchOp('?')) {
            const cons = this.parseExpression();
            this.expectOp(':', 'ternary');
            const alt = this.parseExpression();
            return {
                kind: 'Ternary',
                cond, cons, alt,
                range: { start: cond.range.start, end: alt.range.end }
            };
        }
        return cond;
    }

    /**
     * Pratt parser at precedence level `minBp`. Each operator has a
     * binding-power; right-assoc operators add 0.5 to make the recursion
     * grab the right.
     */
    private parseBinaryAt(minBp: number): Expr {
        let left = this.parsePrefix();
        while (true) {
            const t = this.peek();
            if (t.kind !== 'op' && t.kind !== 'punct') break;
            const op = t.value;
            const info = INFIX_INFO[op];
            if (!info) break;
            if (info.bp < minBp) break;
            this.advance();
            const nextMin = info.rightAssoc ? info.bp : info.bp + 1;
            const right = this.parseBinaryAt(nextMin);
            if (op === '->' || op === '<-') {
                left = {
                    kind: 'RangeExpr',
                    from: op === '->' ? left : right,
                    to:   op === '->' ? right : left,
                    dir: op === '->' ? 'ASC' : 'DESC',
                    range: { start: left.range.start, end: right.range.end }
                };
            } else if (op === '~>' || op === '~!>' || op === '~&>') {
                left = {
                    kind: 'Pipe',
                    source: left,
                    op: op as Pipe['op'],
                    body: right,
                    range: { start: left.range.start, end: right.range.end }
                };
            } else {
                left = {
                    kind: 'Binary',
                    op, left, right,
                    range: { start: left.range.start, end: right.range.end }
                };
            }
        }
        return left;
    }

    /** Unary prefix and "atom" production, followed by postfix loop. */
    private parsePrefix(): Expr {
        const t = this.peek();
        if (t.kind === 'op' && (t.value === '-' || t.value === '!' || t.value === '#' || t.value === '~')) {
            const opTok = this.advance();
            const arg = this.parsePrefix();
            return {
                kind: 'Unary',
                op: opTok.value as Unary['op'],
                argument: arg,
                range: { start: opTok.range.start, end: arg.range.end }
            };
        }
        if (t.kind === 'op' && t.value === '...') {
            const opTok = this.advance();
            const arg = this.parsePrefix();
            return {
                kind: 'Spread',
                argument: arg,
                range: { start: opTok.range.start, end: arg.range.end }
            };
        }
        const atom = this.parseAtom();
        return this.parsePostfix(atom);
    }

    private parsePostfix(expr: Expr): Expr {
        while (true) {
            const t = this.peek();
            if (t.kind === 'punct' && t.value === '(') {
                /* Direct call: f(args) */
                const callArgs = this.parseCallArgs();
                expr = {
                    kind: 'Call',
                    callee: expr,
                    args: callArgs.args,
                    style: this.callStyleFor(expr),
                    range: { start: expr.range.start, end: callArgs.endRange.end }
                };
                continue;
            }
            if (t.kind === 'punct' && t.value === '[') {
                this.advance();
                const idx = this.parseExpression();
                const close = this.peek();
                this.expectOp(']', 'index');
                expr = {
                    kind: 'Index',
                    object: expr,
                    index: idx,
                    safe: false,
                    range: { start: expr.range.start, end: close.range.end }
                };
                continue;
            }
            if (t.kind === 'op' && t.value === '?[') {
                this.advance();
                const idx = this.parseExpression();
                const close = this.peek();
                this.expectOp(']', 'safe index');
                expr = {
                    kind: 'Index',
                    object: expr,
                    index: idx,
                    safe: true,
                    range: { start: expr.range.start, end: close.range.end }
                };
                continue;
            }
            if (t.kind === 'op' && (t.value === '.' || t.value === ':' || t.value === '::')) {
                const dotTok = this.advance();
                const propTok = this.peek();
                if (propTok.kind !== 'ident' && propTok.kind !== 'keyword') {
                    this.error(propTok.range, `Expected member name after '${dotTok.value}'`);
                    expr = {
                        kind: 'Member',
                        object: expr,
                        property: '',
                        propertyRange: dotTok.range,
                        safe: false,
                        accessor: dotTok.value as Member['accessor'],
                        range: { start: expr.range.start, end: dotTok.range.end }
                    };
                    continue;
                }
                this.advance();
                expr = {
                    kind: 'Member',
                    object: expr,
                    property: propTok.value,
                    propertyRange: propTok.range,
                    safe: false,
                    accessor: dotTok.value as Member['accessor'],
                    range: { start: expr.range.start, end: propTok.range.end }
                };
                continue;
            }
            if (t.kind === 'op' && t.value === '?.') {
                this.advance();
                const propTok = this.peek();
                if (propTok.kind !== 'ident' && propTok.kind !== 'keyword') {
                    this.error(propTok.range, "Expected member name after '?.'");
                    expr = {
                        kind: 'Member',
                        object: expr,
                        property: '',
                        propertyRange: t.range,
                        safe: true,
                        accessor: '.',
                        range: { start: expr.range.start, end: t.range.end }
                    };
                    continue;
                }
                this.advance();
                expr = {
                    kind: 'Member',
                    object: expr,
                    property: propTok.value,
                    propertyRange: propTok.range,
                    safe: true,
                    accessor: '.',
                    range: { start: expr.range.start, end: propTok.range.end }
                };
                continue;
            }
            if (t.kind === 'op' && (t.value === '++' || t.value === '--')) {
                const opTok = this.advance();
                expr = {
                    kind: 'Postfix',
                    op: opTok.value as Postfix['op'],
                    argument: expr,
                    range: { start: expr.range.start, end: opTok.range.end }
                };
                continue;
            }
            break;
        }
        return expr;
    }

    private callStyleFor(callee: Expr): Call['style'] {
        if (callee.kind === 'Member') {
            if (callee.accessor === ':')  return 'method';
            if (callee.accessor === '::') return 'fluent';
        }
        return 'paren';
    }

    private parseCallArgs(): { args: Arg[]; endRange: Range } {
        const open = this.expectOp('(', 'call arguments');
        const args: Arg[] = [];
        if (!this.isOp(')')) {
            do {
                if (this.isOp(')')) break;
                if (this.isOp('...')) {
                    const dots = this.advance();
                    const e = this.parseExpression();
                    args.push({
                        expr: e, spread: true,
                        range: { start: dots.range.start, end: e.range.end }
                    });
                } else {
                    const e = this.parseExpression();
                    args.push({ expr: e, spread: false, range: e.range });
                }
            } while (this.matchOp(','));
        }
        const close = this.peek();
        this.matchOp(')');
        return { args, endRange: close.range };
    }

    private parseAtom(): Expr {
        const t = this.peek();
        switch (t.kind) {
            case 'number': {
                this.advance();
                const v = parseNumberLiteral(t.value);
                return { kind: 'NumberLit', value: v, raw: t.value, range: t.range };
            }
            case 'string': {
                this.advance();
                const quote = t.value[0] as '"' | "'";
                const inner = stripStringDelims(t.value);
                return { kind: 'StringLit', value: inner, quote, range: t.range };
            }
            case 'template-string':
                return this.parseTemplateString(this.advance());
            case 'ident': {
                this.advance();
                return { kind: 'Ident', name: t.value, range: t.range };
            }
            case 'keyword': {
                const up = t.value.toUpperCase();
                if (up === 'TRUE')  { this.advance(); return { kind: 'BoolLit', value: true,  range: t.range }; }
                if (up === 'FALSE') { this.advance(); return { kind: 'BoolLit', value: false, range: t.range }; }
                if (up === 'NULL')  { this.advance(); return { kind: 'NullLit',                range: t.range }; }
                if (up === 'FUNCTION') return this.parseFunctionExpr();
                if (up === 'CLASS')    return this.parseClassExpr();
                /* `pipe` keyword is the loop variable inside ~> bodies. */
                if (t.value === 'pipe' || up === 'PIPE') {
                    this.advance();
                    return { kind: 'Ident', name: 'pipe', range: t.range };
                }
                this.advance();
                this.error(t.range, `Unexpected keyword '${t.value}' in expression`);
                return { kind: 'ErrorExpr', range: t.range };
            }
            case 'op':
            case 'punct': {
                if (t.value === '(') return this.parseGroupingOrLambdaOrMask();
                if (t.value === '[') return this.parseArrayLit();
                if (t.value === '{') return this.parseObjectLit();
                this.advance();
                this.error(t.range, `Unexpected '${t.value}' in expression`);
                return { kind: 'ErrorExpr', range: t.range };
            }
        }
        this.advance();
        return { kind: 'ErrorExpr', range: t.range };
    }

    /**
     * `(` can start three things:
     *   - a parenthesized expression
     *   - a mask `(~.~) expr`
     *   - an arrow lambda `(a, b) => expr`
     */
    private parseGroupingOrLambdaOrMask(): Expr {
        const open = this.peek();
        /* Mask: `(` followed by `~` / `.` / `~.` characters, then `)`. */
        if (this.peek(1).kind === 'op' && (this.peek(1).value === '~' || this.peek(1).value === '.')) {
            const save = this.i;
            this.advance();  // (
            const masks: ('keep' | 'skip')[] = [];
            let ok = true;
            while (true) {
                const tt = this.peek();
                if (tt.kind === 'op' && tt.value === '~') { masks.push('keep'); this.advance(); continue; }
                if (tt.kind === 'op' && tt.value === '.') { masks.push('skip'); this.advance(); continue; }
                break;
            }
            if (masks.length > 0 && this.isOp(')')) {
                this.advance();
                const e = this.parseExpression();
                return {
                    kind: 'Mask', mask: masks, expr: e,
                    range: { start: open.range.start, end: e.range.end }
                };
            }
            ok = false;
            void ok;
            this.i = save;
        }

        /* Arrow lambda: `(` <params> `)` `=>` ... Look ahead naively. */
        if (this.looksLikeArrow()) return this.parseArrowLambda();

        this.advance(); // (
        const inner = this.parseExpression();
        const close = this.peek();
        this.expectOp(')', 'parenthesized expression');
        return {
            kind: 'Paren', expression: inner,
            range: { start: open.range.start, end: close.range.end }
        };
    }

    private looksLikeArrow(): boolean {
        /* Scan ahead matching parens. If after the matching `)` we find `=>`,
         * it's an arrow lambda. Bounded scan to keep this cheap. */
        let depth = 0;
        for (let j = this.i; j < this.toks.length && j < this.i + 64; j++) {
            const tt = this.toks[j];
            if ((tt.kind === 'op' || tt.kind === 'punct') && tt.value === '(') depth++;
            else if ((tt.kind === 'op' || tt.kind === 'punct') && tt.value === ')') {
                depth--;
                if (depth === 0) {
                    const next = this.toks[j + 1];
                    return !!(next && next.kind === 'op' && next.value === '=>');
                }
            } else if ((tt.kind === 'op' || tt.kind === 'punct') && (tt.value === ';' || tt.value === '{' || tt.value === '}')) {
                return false;
            }
        }
        return false;
    }

    private parseArrowLambda(): FunctionExpr {
        const open = this.peek();
        const params = this.parseParamList();
        this.expectOp('=>', 'arrow lambda');
        /* Body can be a block or an expression. */
        let body: BlockStmt;
        if (this.isOp('{')) {
            body = this.parseBlock();
        } else {
            const e = this.parseExpression();
            const ret: ReturnStmt = {
                kind: 'ReturnStmt',
                values: [e],
                range: e.range
            };
            body = {
                kind: 'BlockStmt',
                body: [ret],
                range: e.range
            };
        }
        return {
            kind: 'FunctionExpr',
            params, body, arrow: true,
            range: { start: open.range.start, end: body.range.end }
        };
    }

    private parseFunctionExpr(): FunctionExpr {
        const kw = this.advance();
        let name: string | undefined;
        let nameRange: Range | undefined;
        if (this.peek().kind === 'ident') {
            const id = this.advance();
            name = id.value;
            nameRange = id.range;
        }
        const params = this.parseParamList();
        const body = this.parseBlock();
        return {
            kind: 'FunctionExpr',
            name, nameRange,
            params, body, arrow: false,
            range: { start: kw.range.start, end: body.range.end }
        };
    }

    private parseClassExpr(): ClassExpr {
        const kw = this.advance();
        let name: string | undefined;
        let nameRange: Range | undefined;
        if (this.peek().kind === 'ident') {
            const id = this.advance();
            name = id.value;
            nameRange = id.range;
        }
        let extendsName: string | undefined;
        if (this.matchKw('EXTENDS')) {
            const p = this.expectIdent('EXTENDS target');
            if (p) extendsName = p.value;
        }
        let ctorParams: Param[] = [];
        if (this.isOp('(')) ctorParams = this.parseParamList();
        const body = this.parseBlock();
        return {
            kind: 'ClassExpr',
            name, nameRange,
            extendsName,
            ctorParams, body,
            range: { start: kw.range.start, end: body.range.end }
        };
    }

    private parseArrayLit(): ArrayLit {
        const open = this.advance(); // [
        const elements: Expr[] = [];
        if (!this.isOp(']')) {
            do {
                if (this.isOp(']')) break;
                elements.push(this.parseExpression());
            } while (this.matchOp(','));
        }
        const close = this.peek();
        this.expectOp(']', 'array literal');
        return {
            kind: 'ArrayLit', elements,
            range: { start: open.range.start, end: close.range.end }
        };
    }

    private parseObjectLit(): ObjectLit {
        const open = this.advance(); // {
        const properties: ObjectProp[] = [];
        if (!this.isOp('}')) {
            do {
                if (this.isOp('}')) break;
                const key = this.parseObjectKey();
                this.expectOp(':', 'object literal property');
                const value = this.parseExpression();
                properties.push({
                    key, value,
                    range: { start: key.range.start, end: value.range.end }
                });
            } while (this.matchOp(','));
        }
        const close = this.peek();
        this.expectOp('}', 'object literal');
        return {
            kind: 'ObjectLit', properties,
            range: { start: open.range.start, end: close.range.end }
        };
    }

    private parseObjectKey(): ObjectKey {
        const t = this.peek();
        if (t.kind === 'ident') {
            this.advance();
            return { kind: 'ident', name: t.value, range: t.range };
        }
        if (t.kind === 'keyword') {
            /* `__index`, `__type`, and similar metafields parse as keyword
             * idents because the lexer has no notion of "any-case word".
             * Allow keywords as object literal keys -- the runtime does. */
            this.advance();
            return { kind: 'ident', name: t.value, range: t.range };
        }
        if (t.kind === 'string') {
            this.advance();
            return { kind: 'string', name: stripStringDelims(t.value), range: t.range };
        }
        if (t.kind === 'punct' && t.value === '[') {
            const open = this.advance();
            const e = this.parseExpression();
            const close = this.peek();
            this.expectOp(']', 'computed key');
            return {
                kind: 'computed',
                expr: e,
                range: { start: open.range.start, end: close.range.end }
            };
        }
        this.error(t.range, 'Expected object key');
        this.advance();
        return { kind: 'ident', name: '', range: t.range };
    }

    private parseTemplateString(tok: Token): TemplateLit {
        /* The lexer captured the whole backtick string as one token plus a
         * list of interpolation ranges. For analysis we treat each `${...}`
         * as an opaque 'expr' part whose result is unknown; the containing
         * template is `string`-typed regardless of part types. The cursor
         * finder still descends into the parts so member completion inside
         * an interpolation works via fallback identifier lookup. */
        const parts: TemplatePart[] = [];
        const interps = tok.interpolations ?? [];
        if (interps.length === 0) {
            parts.push({ kind: 'str', value: tok.value, range: tok.range });
            return { kind: 'TemplateLit', parts, range: tok.range };
        }
        for (const r of interps) {
            parts.push({
                kind: 'expr',
                expr: { kind: 'ErrorExpr', range: r },
                range: r
            });
        }
        return { kind: 'TemplateLit', parts, range: tok.range };
    }
}

/* ----------------------------------------------------------------------- */
/* Operator info table                                                     */
/* ----------------------------------------------------------------------- */

interface InfixInfo {
    bp: number;
    rightAssoc?: boolean;
}

const INFIX_INFO: Record<string, InfixInfo> = {
    /* pipes (3) */
    '~>':  { bp: 3 },
    '~!>': { bp: 3 },
    '~&>': { bp: 3 },
    /* logical (4-5) */
    '||':  { bp: 4 },
    '&&':  { bp: 5 },
    /* bitwise (6-7) */
    '|':   { bp: 6 },
    '|&':  { bp: 6 },
    '&':   { bp: 7 },
    /* equality (8) */
    '==':  { bp: 8 },
    '!=':  { bp: 8 },
    /* comparison (9) */
    '<':   { bp: 9 },
    '<=':  { bp: 9 },
    '>':   { bp: 9 },
    '>=':  { bp: 9 },
    /* shift (10) */
    '<<':  { bp: 10 },
    '>>':  { bp: 10 },
    /* range (11) */
    '->':  { bp: 11 },
    '<-':  { bp: 11 },
    /* additive (12) */
    '+':   { bp: 12 },
    '-':   { bp: 12 },
    /* multiplicative (13) */
    '*':   { bp: 13 },
    '/':   { bp: 13 },
    '%':   { bp: 13 },
    /* power (14, right) */
    '^':   { bp: 14, rightAssoc: true }
};

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */

function parseNumberLiteral(raw: string): number {
    if (raw.startsWith('0x') || raw.startsWith('0X')) return parseInt(raw, 16);
    if (raw.startsWith('0b') || raw.startsWith('0B')) return parseInt(raw.slice(2), 2);
    const n = Number(raw);
    return Number.isFinite(n) ? n : 0;
}

function stripStringDelims(raw: string): string {
    if (raw.length < 2) return '';
    const first = raw[0];
    if (first !== '"' && first !== "'") return raw;
    const last = raw[raw.length - 1];
    const end = last === first ? raw.length - 1 : raw.length;
    return raw.slice(1, end);
}

