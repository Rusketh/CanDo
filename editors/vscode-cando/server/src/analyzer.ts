/*
 * Walk a token stream from the lexer and produce a lightweight symbol table:
 *   - top-level VAR / CONST / GLOBAL bindings
 *   - FUNCTION declarations (with parameter names)
 *   - CLASS declarations
 *
 * This is intentionally shallow -- enough to drive completion, document
 * symbols and go-to-definition, not enough to be a real semantic analyzer.
 */

import { Token, Range } from './lexer';

export type SymbolKind = 'function' | 'class' | 'variable' | 'constant' | 'parameter';

export interface Symbol {
    name: string;
    kind: SymbolKind;
    range: Range;          /* the identifier itself */
    selectionRange: Range; /* same as range; kept separate to mirror LSP */
    detail?: string;
    children?: Symbol[];
}

export interface AnalysisResult {
    symbols: Symbol[];
    /* Flat name -> first occurrence map for goto-def. */
    byName: Map<string, Symbol>;
}

export function analyze(tokens: Token[]): AnalysisResult {
    const symbols: Symbol[] = [];
    const byName = new Map<string, Symbol>();
    const idents: Token[] = tokens.filter(t => t.kind !== 'comment');

    const isKw = (t: Token | undefined, ...names: string[]): boolean => {
        if (!t || t.kind !== 'keyword') return false;
        const v = t.value;
        return names.some(n => v === n || v === n.toLowerCase());
    };

    let depth = 0;

    for (let i = 0; i < idents.length; i++) {
        const t = idents[i];

        if (t.kind === 'punct' && t.value === '{') depth++;
        else if (t.kind === 'punct' && t.value === '}') depth = Math.max(0, depth - 1);

        if (depth !== 0) continue;

        // FUNCTION name(args)
        if (isKw(t, 'FUNCTION')) {
            const nameTok = idents[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                const params: Symbol[] = [];
                // collect parameters between ( and )
                let j = i + 2;
                if (idents[j]?.value === '(') {
                    j++;
                    while (j < idents.length && idents[j].value !== ')') {
                        const p = idents[j];
                        if (p.kind === 'ident') {
                            params.push({
                                name: p.value,
                                kind: 'parameter',
                                range: p.range,
                                selectionRange: p.range
                            });
                        }
                        j++;
                    }
                }
                const sym: Symbol = {
                    name: nameTok.value,
                    kind: 'function',
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail: `FUNCTION ${nameTok.value}(${params.map(p => p.name).join(', ')})`,
                    children: params
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);
                continue;
            }
        }

        // CLASS Name [EXTENDS Parent]
        if (isKw(t, 'CLASS')) {
            const nameTok = idents[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                let detail = `CLASS ${nameTok.value}`;
                if (isKw(idents[i + 2], 'EXTENDS') && idents[i + 3]?.kind === 'ident') {
                    detail += ` EXTENDS ${idents[i + 3].value}`;
                }
                const sym: Symbol = {
                    name: nameTok.value,
                    kind: 'class',
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);
                continue;
            }
        }

        // VAR / CONST / GLOBAL name
        if (isKw(t, 'VAR', 'CONST', 'GLOBAL')) {
            const nameTok = idents[i + 1];
            if (nameTok && nameTok.kind === 'ident') {
                const kind: SymbolKind = isKw(t, 'CONST') ? 'constant' : 'variable';
                const sym: Symbol = {
                    name: nameTok.value,
                    kind,
                    range: nameTok.range,
                    selectionRange: nameTok.range,
                    detail: `${t.value.toUpperCase()} ${nameTok.value}`
                };
                symbols.push(sym);
                if (!byName.has(sym.name)) byName.set(sym.name, sym);
                continue;
            }
        }
    }

    return { symbols, byName };
}
