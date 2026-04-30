/*
 * A pragmatic JS port of the CanDo lexer (source/parser/lexer.c).
 *
 * Goals:
 *   1. Recognise tokens well enough to drive completion, hover and basic
 *      diagnostics (unterminated strings/comments, stray characters).
 *   2. Match the runtime's keyword rule: only pure-uppercase or pure-lowercase
 *      identifiers are keywords; mixed case is a plain identifier.
 *
 * Non-goals:
 *   - Full numeric lexing fidelity (we accept the common forms).
 *   - Backtick interpolation parsing -- treated as a single token; the
 *     embedded `${...}` ranges are exposed via `interpolations` so the
 *     analyzer can still see identifiers inside them.
 */

import { isKeywordAnyCase } from './builtins';

export type TokenKind =
    | 'ident'
    | 'keyword'
    | 'number'
    | 'string'
    | 'template-string'
    | 'comment'
    | 'op'
    | 'punct'
    | 'newline'
    | 'eof'
    | 'error';

export interface Position { line: number; character: number; }
export interface Range { start: Position; end: Position; }

export interface Token {
    kind: TokenKind;
    value: string;
    range: Range;
    /* For `template-string` only: spans of `${...}` content (excluding the
     * delimiters), so the analyzer can recurse. */
    interpolations?: Range[];
    /* For `error` tokens, a human-readable message. */
    message?: string;
}

const SINGLE_OPS = new Set('+-*/%^&|<>=!~.#?:;,()[]{}'.split(''));

export class Lexer {
    private readonly src: string;
    private pos = 0;
    private line = 0;
    private col = 0;

    constructor(src: string) { this.src = src; }

    public tokenize(): Token[] {
        const out: Token[] = [];
        while (this.pos < this.src.length) {
            const c = this.src[this.pos];

            if (c === ' ' || c === '\t' || c === '\r') {
                this.advance();
                continue;
            }
            if (c === '\n') {
                this.advance();
                continue;
            }

            if (c === '/' && this.peek(1) === '/') {
                out.push(this.scanLineComment());
                continue;
            }
            if (c === '/' && this.peek(1) === '*') {
                out.push(this.scanBlockComment());
                continue;
            }

            if (c === '"' || c === '\'') {
                out.push(this.scanString(c));
                continue;
            }
            if (c === '`') {
                out.push(this.scanTemplateString());
                continue;
            }

            if (this.isDigit(c)) {
                out.push(this.scanNumber());
                continue;
            }

            if (this.isIdentStart(c)) {
                out.push(this.scanIdent());
                continue;
            }

            if (SINGLE_OPS.has(c)) {
                out.push(this.scanOperator());
                continue;
            }

            // Stray byte
            const start = this.markStart();
            this.advance();
            out.push({
                kind: 'error',
                value: c,
                range: this.makeRange(start),
                message: `Unexpected character '${c}'`
            });
        }

        const eofStart = this.markStart();
        out.push({ kind: 'eof', value: '', range: this.makeRange(eofStart) });
        return out;
    }

    private scanLineComment(): Token {
        const start = this.markStart();
        const begin = this.pos;
        while (this.pos < this.src.length && this.src[this.pos] !== '\n') {
            this.advance();
        }
        return {
            kind: 'comment',
            value: this.src.slice(begin, this.pos),
            range: this.makeRange(start)
        };
    }

    private scanBlockComment(): Token {
        const start = this.markStart();
        const begin = this.pos;
        this.advance(); this.advance(); // /*
        let closed = false;
        while (this.pos < this.src.length) {
            if (this.src[this.pos] === '*' && this.peek(1) === '/') {
                this.advance(); this.advance();
                closed = true;
                break;
            }
            this.advance();
        }
        const tok: Token = {
            kind: 'comment',
            value: this.src.slice(begin, this.pos),
            range: this.makeRange(start)
        };
        if (!closed) {
            tok.kind = 'error';
            tok.message = 'Unterminated block comment';
        }
        return tok;
    }

    private scanString(quote: string): Token {
        const start = this.markStart();
        const begin = this.pos;
        this.advance();
        let closed = false;
        while (this.pos < this.src.length) {
            const ch = this.src[this.pos];
            if (ch === '\\' && this.pos + 1 < this.src.length) {
                this.advance(); this.advance();
                continue;
            }
            if (ch === quote) {
                this.advance();
                closed = true;
                break;
            }
            // Double-quoted strings cannot span lines; single-quoted may.
            if (ch === '\n' && quote === '"') {
                break;
            }
            this.advance();
        }
        const tok: Token = {
            kind: 'string',
            value: this.src.slice(begin, this.pos),
            range: this.makeRange(start)
        };
        if (!closed) {
            tok.kind = 'error';
            tok.message = `Unterminated ${quote === '"' ? 'double' : 'single'}-quoted string`;
        }
        return tok;
    }

    private scanTemplateString(): Token {
        const start = this.markStart();
        const begin = this.pos;
        this.advance(); // `
        const interps: Range[] = [];
        let closed = false;
        while (this.pos < this.src.length) {
            const ch = this.src[this.pos];
            if (ch === '\\' && this.pos + 1 < this.src.length) {
                this.advance(); this.advance();
                continue;
            }
            if (ch === '`') {
                this.advance();
                closed = true;
                break;
            }
            if (ch === '$' && this.peek(1) === '{') {
                this.advance(); this.advance();
                const innerStart = this.markStart();
                let depth = 1;
                while (this.pos < this.src.length && depth > 0) {
                    const c2 = this.src[this.pos];
                    if (c2 === '{') depth++;
                    else if (c2 === '}') {
                        depth--;
                        if (depth === 0) break;
                    }
                    this.advance();
                }
                interps.push(this.makeRange(innerStart));
                if (this.pos < this.src.length && this.src[this.pos] === '}') {
                    this.advance();
                }
                continue;
            }
            this.advance();
        }
        const tok: Token = {
            kind: 'template-string',
            value: this.src.slice(begin, this.pos),
            range: this.makeRange(start),
            interpolations: interps
        };
        if (!closed) {
            tok.kind = 'error';
            tok.message = 'Unterminated template string';
        }
        return tok;
    }

    private scanNumber(): Token {
        const start = this.markStart();
        const begin = this.pos;
        if (this.src[this.pos] === '0' && (this.peek(1) === 'x' || this.peek(1) === 'X')) {
            this.advance(); this.advance();
            while (this.pos < this.src.length && /[0-9A-Fa-f]/.test(this.src[this.pos])) this.advance();
        } else {
            while (this.pos < this.src.length && this.isDigit(this.src[this.pos])) this.advance();
            if (this.src[this.pos] === '.' && this.isDigit(this.peek(1))) {
                this.advance();
                while (this.pos < this.src.length && this.isDigit(this.src[this.pos])) this.advance();
            }
            if (this.src[this.pos] === 'e' || this.src[this.pos] === 'E') {
                this.advance();
                if (this.src[this.pos] === '+' || this.src[this.pos] === '-') this.advance();
                while (this.pos < this.src.length && this.isDigit(this.src[this.pos])) this.advance();
            }
        }
        return {
            kind: 'number',
            value: this.src.slice(begin, this.pos),
            range: this.makeRange(start)
        };
    }

    private scanIdent(): Token {
        const start = this.markStart();
        const begin = this.pos;
        while (this.pos < this.src.length && this.isIdentPart(this.src[this.pos])) {
            this.advance();
        }
        const value = this.src.slice(begin, this.pos);
        const kind: TokenKind = isKeywordAnyCase(value) ? 'keyword' : 'ident';
        return { kind, value, range: this.makeRange(start) };
    }

    private scanOperator(): Token {
        const start = this.markStart();
        const begin = this.pos;
        const c = this.src[this.pos];
        const c1 = this.peek(1);
        const c2 = this.peek(2);

        const three = c + (c1 ?? '') + (c2 ?? '');
        const two = c + (c1 ?? '');

        // 3-char operators
        if (three === '...' || three === '~&>' || three === '~!>') {
            this.advance(); this.advance(); this.advance();
            return { kind: 'op', value: three, range: this.makeRange(start) };
        }

        // 2-char operators
        const TWO_CHAR = new Set([
            '==', '!=', '<=', '>=', '<<', '>>', '|&',
            '+=', '-=', '*=', '/=', '%=', '^=',
            '++', '--', '&&', '||', '=>', '?.', '?[',
            '->', '<-', '::', '~>'
        ]);
        if (TWO_CHAR.has(two)) {
            this.advance(); this.advance();
            return { kind: 'op', value: two, range: this.makeRange(start) };
        }

        this.advance();
        const kind: TokenKind = '(){}[],;'.includes(c) ? 'punct' : 'op';
        return { kind, value: this.src.slice(begin, this.pos), range: this.makeRange(start) };
    }

    private isDigit(c: string | undefined): boolean {
        return !!c && c >= '0' && c <= '9';
    }
    private isIdentStart(c: string): boolean {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c === '_';
    }
    private isIdentPart(c: string): boolean {
        return this.isIdentStart(c) || this.isDigit(c);
    }
    private peek(n: number): string | undefined {
        return this.src[this.pos + n];
    }
    private advance(): void {
        const c = this.src[this.pos];
        this.pos++;
        if (c === '\n') {
            this.line++;
            this.col = 0;
        } else {
            this.col++;
        }
    }
    private markStart(): Position {
        return { line: this.line, character: this.col };
    }
    private makeRange(start: Position): Range {
        return { start, end: { line: this.line, character: this.col } };
    }
}
