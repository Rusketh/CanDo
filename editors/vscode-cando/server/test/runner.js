#!/usr/bin/env node
/*
 * Headless test runner for the CanDo language server.
 *
 * Compile with `npx tsc -b` first, then run from the extension root:
 *   node server/test/runner.js
 *
 * Each `.cdo` file under server/test/cases/ describes one expectation. The
 * runner places the cursor at `|`, strips it, asks the inferer for the
 * type at that position, and compares against inline directives:
 *
 *   // EXPECT: <renderedType>
 *      The receiver's type must render exactly to <renderedType>.
 *
 *   // MEMBERS: a, b, c
 *      Each name must appear in the receiver's member set.
 *
 *   // NOMEMBERS: x, y
 *      None of those names may appear (useful for guarding narrowing).
 *
 *   // PARSE_ERRORS: <n>
 *      Maximum parse errors permitted (default: 0).
 */

const fs = require('fs');
const path = require('path');

const OUT = path.resolve(__dirname, '..', 'out');
const { analyzeDocument } = require(path.join(OUT, 'analyze.js'));
const { renderType, enumerateMembers, firstOf } = require(path.join(OUT, 'typesys.js'));
const { nodeAt } = require(path.join(OUT, 'ast.js'));

const CASES_DIR = path.resolve(__dirname, 'cases');
const WORKSPACE = path.resolve(__dirname, '..', '..', '..', '..');

let pass = 0;
let fail = 0;
const failures = [];

function fileToUri(p) {
    return 'file://' + p.replace(/\\/g, '/');
}

function findCursor(text) {
    const idx = text.indexOf('|');
    if (idx < 0) return null;
    const before = text.slice(0, idx);
    const lines = before.split('\n');
    const line = lines.length - 1;
    const character = lines[lines.length - 1].length;
    const stripped = text.slice(0, idx) + text.slice(idx + 1);
    return { line, character, stripped, offset: idx };
}

function parseDirectives(text) {
    const out = { expect: null, members: [], nomembers: [], parseErrors: 0 };
    const re = /\/\/\s*(EXPECT|MEMBERS|NOMEMBERS|PARSE_ERRORS):\s*(.+)$/gm;
    let m;
    while ((m = re.exec(text))) {
        const tag = m[1].trim();
        const val = m[2].trim();
        if (tag === 'EXPECT') out.expect = val;
        else if (tag === 'MEMBERS') out.members.push(...val.split(',').map(s => s.trim()).filter(Boolean));
        else if (tag === 'NOMEMBERS') out.nomembers.push(...val.split(',').map(s => s.trim()).filter(Boolean));
        else if (tag === 'PARSE_ERRORS') out.parseErrors = parseInt(val, 10) | 0;
    }
    return out;
}

function targetTypeAt(a, line, character, text) {
    /* If the cursor follows a `.` or `:` we want the member receiver's
     * type. Otherwise look at the AST node directly under the cursor. */
    const offset = positionToOffset(text, line, character);
    let i = offset;
    while (i > 0 && /[A-Za-z0-9_]/.test(text[i - 1])) i--;
    let j = i;
    while (j > 0 && (text[j - 1] === ' ' || text[j - 1] === '\t')) j--;
    if (j > 0 && (text[j - 1] === '.' || text[j - 1] === ':')) {
        /* Walk back through the receiver tokens. The simplest reliable way:
         * find the receiver expression by descending the AST to whatever
         * Member / Index / Call ends at position `j - 1` and return its
         * `.object` type. */
        const lineCh = offsetToPos(text, j - 1);
        const node = nodeAt(a.program, lineCh.line, Math.max(0, lineCh.character - 1));
        if (node && (node.kind === 'Member' || node.kind === 'Index')) {
            return a.inferred.nodeTypes.get(node.object);
        }
        /* `obj.|` -- the partial member name is empty so the cursor is right
         * after the `.`. Walk up from the cursor location to the enclosing
         * Member with property '' (which we synthesised during error
         * recovery) or, more reliably, to the nearest Ident before the dot. */
        if (node && node.kind === 'Ident') {
            return a.inferred.nodeTypes.get(node);
        }
    }
    const lineCh2 = { line, character };
    const node = nodeAt(a.program, lineCh2.line, Math.max(0, lineCh2.character - 1));
    if (!node) return null;
    return a.inferred.nodeTypes.get(node);
}

function positionToOffset(text, line, character) {
    let offset = 0, currentLine = 0;
    for (let i = 0; i < text.length; i++) {
        if (currentLine === line) {
            return offset + character;
        }
        if (text[i] === '\n') currentLine++;
        offset++;
    }
    return offset;
}

function offsetToPos(text, off) {
    let line = 0, ch = 0;
    for (let i = 0; i < off && i < text.length; i++) {
        if (text[i] === '\n') { line++; ch = 0; } else { ch++; }
    }
    return { line, character: ch };
}

function runCase(filePath) {
    const raw = fs.readFileSync(filePath, 'utf8');
    const name = path.basename(filePath);
    const cur = findCursor(raw);
    if (!cur) {
        record(name, 'no `|` cursor marker');
        return;
    }
    const directives = parseDirectives(raw);
    const src = cur.stripped;
    const uri = fileToUri(filePath);
    const a = analyzeDocument(uri, src, 1, [WORKSPACE]);

    /* Filter out the trailing "Expected member name after '.'" error that
     * always fires when the cursor is mid-edit at a member access (i.e.
     * `foo.|` becomes `foo.` after stripping). Real IDE usage shows the
     * error briefly and clears it once the user keeps typing -- it's not
     * the kind of failure these tests want to flag. */
    const realErrors = a.parseErrors.filter(e => !/^Expected member name after/.test(e.message));
    if (realErrors.length > directives.parseErrors) {
        record(name, `expected <=${directives.parseErrors} parse errors, got ${realErrors.length}: ${realErrors.map(e => e.message).join('; ')}`);
        return;
    }

    const t = targetTypeAt(a, cur.line, cur.character, src);
    if (!t) {
        record(name, 'no type inferred at cursor');
        return;
    }

    if (directives.expect !== null) {
        const got = renderType(firstOf ? firstOf(t) : t);
        if (got !== directives.expect) {
            record(name, `EXPECT mismatch: want "${directives.expect}", got "${got}"`);
            return;
        }
    }
    if (directives.members.length > 0 || directives.nomembers.length > 0) {
        const members = enumerateMembers(t);
        for (const want of directives.members) {
            if (!members.has(want)) {
                record(name, `MEMBERS missing "${want}" (have: ${[...members.keys()].slice(0, 20).join(', ')})`);
                return;
            }
        }
        for (const banned of directives.nomembers) {
            if (members.has(banned)) {
                record(name, `NOMEMBERS leaked "${banned}"`);
                return;
            }
        }
    }
    pass++;
}

function record(name, reason) {
    fail++;
    failures.push({ name, reason });
}

function main() {
    const files = fs.readdirSync(CASES_DIR).filter(f => f.endsWith('.cdo')).sort();
    if (files.length === 0) {
        console.error('No test cases in', CASES_DIR);
        process.exit(2);
    }
    for (const f of files) runCase(path.join(CASES_DIR, f));
    console.log(`Pass: ${pass}  Fail: ${fail}`);
    if (failures.length) {
        console.log('\nFailures:');
        for (const f of failures) console.log(`  - ${f.name}: ${f.reason}`);
        process.exit(1);
    }
}

main();
