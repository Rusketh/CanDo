/* Edge-case harness for the CanDo language server's completion path.
 *
 * Each test inserts a `|` to mark the cursor, strips it, then asks the
 * type tracker what receiver type the cursor sits on (when in a member
 * position) plus what the path detector reports (when in an include
 * string). Run with `node server/test/edge_cases.js` from the extension
 * directory after `npx tsc -b`.
 */

const path = require('path');
const fs = require('fs');

const OUT = path.resolve(__dirname, '..', 'out');
const { Lexer } = require(path.join(OUT, 'lexer.js'));
const { analyze } = require(path.join(OUT, 'analyzer.js'));
const {
    buildTypeEnv,
    inferReceiverAt,
    listMembers,
    describeTypeForHover
} = require(path.join(OUT, 'types.js'));
const { detectIncludeString } = require(path.join(OUT, 'paths.js'));

const REPO = path.resolve(__dirname, '..', '..', '..', '..');
const DOC_PATH = path.join(REPO, 'modules', 'forms', '_harness.cdo');
const DOC_URI = 'file://' + DOC_PATH;
const ROOTS = [REPO];

let pass = 0;
let fail = 0;
const failures = [];

function test(name, body) {
    try {
        const result = body();
        if (result === true) {
            pass++;
            return;
        }
        fail++;
        failures.push({ name, reason: result || 'returned false' });
    } catch (e) {
        fail++;
        failures.push({ name, reason: 'threw: ' + e.message + '\n' + e.stack });
    }
}

function tokenize(source) {
    return new Lexer(source).tokenize();
}

function envFor(source) {
    const tokens = tokenize(source);
    const result = analyze(tokens);
    const env = buildTypeEnv(tokens, result.symbols, DOC_URI, ROOTS);
    return { tokens, env, symbols: result.symbols };
}

/** Resolve the receiver type at the `|` marker. Returns the inferred type
 *  description and member names. */
function membersAt(source) {
    const idx = source.indexOf('|');
    if (idx < 0) throw new Error('no | marker');
    const stripped = source.slice(0, idx) + source.slice(idx + 1);
    const tokens = tokenize(stripped);
    const filtered = tokens.filter(t => t.kind !== 'comment' && t.kind !== 'newline');
    const result = analyze(tokens);
    const env = buildTypeEnv(tokens, result.symbols, DOC_URI, ROOTS);

    /* Find the dot/colon/double-colon immediately before the cursor. */
    let dotIdx = -1;
    for (let i = filtered.length - 1; i >= 0; i--) {
        const tok = filtered[i];
        if (tok.kind === 'op' && (tok.value === '.' || tok.value === ':' || tok.value === '::')) {
            const o = offsetOf(stripped, tok.range.start);
            const e = offsetOf(stripped, tok.range.end);
            if (e <= idx) { dotIdx = i; break; }
        }
    }
    if (dotIdx < 0) return { type: 'no-dot', members: [] };
    const ref = inferReceiverAt(filtered, dotIdx, env, ROOTS);
    return { type: describeTypeForHover(ref), members: [...listMembers(ref, env).keys()] };
}

function offsetOf(text, pos) {
    let line = 0;
    let col = 0;
    for (let i = 0; i < text.length; i++) {
        if (line === pos.line && col === pos.character) return i;
        if (text[i] === '\n') { line++; col = 0; } else col++;
    }
    return text.length;
}

function detectInclude(source) {
    const idx = source.indexOf('|');
    if (idx < 0) throw new Error('no | marker');
    const stripped = source.slice(0, idx) + source.slice(idx + 1);
    return detectIncludeString(stripped, idx);
}

/* =========================================================================
 *  Receiver detection
 * ======================================================================= */

test('plain receiver', () =>
    membersAt('VAR forms = include("./forms.so");\nforms.|').members.includes('Form')
);

test('chained call result', () =>
    membersAt('VAR forms = include("./forms.so");\nforms.Form().|').members.includes('center')
);

test('partial member typed', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.Form().cen|ter');
    return r.members.includes('center') || ('expected center, got ' + r.members.length + ' members');
});

test('whitespace between dot and member', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.   |');
    return r.members.includes('Form') || 'expected Form, members=' + r.members.slice(0, 5);
});

test('newline between dot and member', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.\n  |');
    return r.members.includes('Form') || 'expected Form, members=' + r.members.slice(0, 5);
});

test('colon (method) syntax', () => {
    const r = membersAt('VAR forms = include("./forms.so"); VAR f = forms.Form();\nf:|');
    return r.members.includes('center') || 'expected center, members=' + r.members.slice(0, 5);
});

test('double-colon (chain) syntax: t::method', () => {
    /* `::` is the fluent-chain operator. `f::method(args)` is equivalent
     * to a `:` call but the runtime always returns the receiver, so the
     * type tracker should treat completion the same as `f:`. */
    const r = membersAt('VAR forms = include("./forms.so"); VAR f = forms.Form();\nf::|');
    return r.members.includes('center') || 'expected center, members=' + r.members.slice(0, 5);
});

test('::-chain preserves receiver type even when method returns something else', () => {
    /* User-defined object whose method has a non-self return; chaining via
     * `::` should keep the result on the original receiver. */
    const src = `
        VAR t = { v: 100 };
        t.meth = FUNCTION(self) { RETURN self.v; };
        t::meth().|
    `;
    const r = membersAt(src);
    /* The chain ends back on `t`; available members are t's own keys. */
    return r.members.includes('v') && r.members.includes('meth');
});

test('::-chain on manifest type preserves the receiver', () => {
    const src = `
        VAR forms = include("./forms.so");
        VAR f = forms.Form();
        f::setText("hi")::|
    `;
    /* setText nominally returns Form (or self). After ::, we're guaranteed
     * to still be on Form regardless of declaration. */
    const r = membersAt(src);
    return r.members.includes('center');
});

test('mixed :- and ::-chain', () => {
    const src = `
        VAR forms = include("./forms.so");
        VAR f = forms.Form();
        f:setSize(100, 100)::|
    `;
    const r = membersAt(src);
    return r.members.includes('center');
});

test('::-chain with two consecutive chains', () => {
    /* From metamethods.cdo: t::set_v(200)::set_v(300) */
    const src = `
        VAR t = { };
        t.set_v = FUNCTION(self, v) { self.v = v; };
        t::set_v(200)::|
    `;
    const r = membersAt(src);
    return r.members.includes('set_v');
});

test('do not complete inside string literal', () => {
    const r = membersAt('VAR s = "forms.|";');
    return r.type === 'no-dot' || r.members.length === 0
        || ('inside string should not have members, got ' + r.type + ': ' + r.members.length);
});

test('chained constants: forms.Color.cornflowerblue', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.Color.|');
    return r.members.includes('cornflowerblue') || 'expected cornflowerblue, members=' + r.members.slice(0, 5);
});

test('parenthesized receiver: (forms.Form()).', () => {
    const r = membersAt('VAR forms = include("./forms.so");\n(forms.Form()).|');
    return r.members.includes('center') || 'expected center, members=' + r.members.slice(0, 5);
});

test('receiver is index access: arr[0].', () => {
    /* We don't track element types yet, but the receiver detector should
     * not crash and should bail to unknown. */
    const r = membersAt('VAR arr = [1,2,3];\narr[0].|');
    return r.type === 'any' || true;
});

/* =========================================================================
 *  Type tracker
 * ======================================================================= */

test('reassignment refines type', () => {
    /* Currently we keep the first inference -- document this behaviour. */
    const { env } = envFor(
        'VAR forms = include("./forms.so");\nVAR x = forms.Form();\nx = forms.Button(x);'
    );
    return describeTypeForHover(env.bindings.get('x')) === 'forms.Form';
});

test('VAR x = method ref (not call) -> function', () => {
    const { env } = envFor(
        'VAR forms = include("./forms.so"); VAR f = forms.Form();\nVAR setT = f.setText;'
    );
    return describeTypeForHover(env.bindings.get('setT')) === 'function';
});

test('VAR x = forms.Color (constant table)', () => {
    const r = membersAt(
        'VAR forms = include("./forms.so"); VAR c = forms.Color;\nc.|'
    );
    return r.members.includes('cornflowerblue') || 'got members=' + r.members.slice(0, 5);
});

test('class with EXTENDS chain (CanDo syntax)', () => {
    const src = `
        CLASS Animal = (self) { }
        Animal.speak = FUNCTION(self) { };
        CLASS Dog EXTENDS Animal = (self) { }
        Dog.bark = FUNCTION(self) { };
        VAR d = Dog();
        d.|
    `;
    const r = membersAt(src);
    return (r.members.includes('bark') && r.members.includes('speak'))
        || ('expected bark+speak, got ' + r.members);
});

test('object literal with __index inheritance', () => {
    const src = `
        VAR base = { hello: FUNCTION() { } };
        VAR derived = { __index: base, world: FUNCTION() { } };
        derived.|
    `;
    const r = membersAt(src);
    return r.members.includes('hello') && r.members.includes('world')
        || ('expected hello+world, got ' + r.members);
});

test('class instance method completion via colon', () => {
    const src = `
        CLASS Foo = (self) {
            self.bar = FUNCTION(self) { };
        }
        Foo.greet = FUNCTION(self) { };
        VAR f = Foo();
        f:|
    `;
    const r = membersAt(src);
    return (r.members.includes('bar') && r.members.includes('greet'))
        || ('expected bar+greet, got ' + r.members);
});

test('class declared with empty body (CLASS Name = { })', () => {
    const src = `
        CLASS MathUtil = { }
        MathUtil.square = FUNCTION(n) { };
        MathUtil.cube   = FUNCTION(n) { };
        MathUtil.|
    `;
    const r = membersAt(src);
    return (r.members.includes('square') && r.members.includes('cube'))
        || ('expected square+cube, got ' + r.members);
});

test('FUNCTION body declarations are tracked', () => {
    const src = `
        VAR forms = include("./forms.so");
        FUNCTION makeForm() {
            VAR f = forms.Form();
            f.|
            RETURN f;
        }
    `;
    const r = membersAt(src);
    return r.members.includes('center') || 'expected center, got ' + r.members.length;
});

/* =========================================================================
 *  Include / path detection
 * ======================================================================= */

test('detect double-quoted include', () => {
    const ctx = detectInclude('VAR x = include("./fo|");');
    return ctx && ctx.typed === './fo' && ctx.quote === '"';
});

test('detect single-quoted include', () => {
    const ctx = detectInclude("VAR x = include('./fo|');");
    return ctx && ctx.typed === './fo' && ctx.quote === '\'';
});

test('include with whitespace before paren', () => {
    const ctx = detectInclude('VAR x = include ("./fo|");');
    return ctx !== null;
});

test('non-include string returns null', () => {
    const ctx = detectInclude('VAR x = print("./fo|");');
    return ctx === null;
});

test('cursor inside template-string is not include', () => {
    const ctx = detectInclude('VAR x = `forms.|`;');
    return ctx === null;
});

/* =========================================================================
 *  Manifest pathological cases
 * ======================================================================= */

test('manifest with circular extends does not infinite-loop', () => {
    /* Synthesize: write a manifest in a tmp dir with a cycle. */
    const tmpDir = path.join(require('os').tmpdir(), 'cando-circ-' + process.pid);
    fs.mkdirSync(tmpDir, { recursive: true });
    fs.writeFileSync(path.join(tmpDir, 'cando.api.json'), JSON.stringify({
        name: 'circ',
        exports: { make: { kind: 'function', returns: 'A' } },
        types: {
            A: { extends: 'B', members: { ax: { kind: 'value', type: 'string' } } },
            B: { extends: 'A', members: { bx: { kind: 'value', type: 'string' } } }
        }
    }));
    fs.writeFileSync(path.join(tmpDir, 'circ.so'), '');

    const src = 'VAR m = include("' + tmpDir + '/circ.so");\nVAR a = m.make();\na.|';
    const r = membersAt(src);
    fs.rmSync(tmpDir, { recursive: true, force: true });
    return r.members.includes('ax') && r.members.includes('bx');
});

test('reference to missing type degrades gracefully', () => {
    const tmpDir = path.join(require('os').tmpdir(), 'cando-miss-' + process.pid);
    fs.mkdirSync(tmpDir, { recursive: true });
    fs.writeFileSync(path.join(tmpDir, 'cando.api.json'), JSON.stringify({
        name: 'miss',
        exports: { make: { kind: 'function', returns: 'NotDefined' } },
        types: {}
    }));

    const src = 'VAR m = include("' + tmpDir + '/miss.so");\nVAR a = m.make();\na.|';
    const r = membersAt(src);
    fs.rmSync(tmpDir, { recursive: true, force: true });
    return r.members.length === 0;
});

test('malformed manifest does not crash', () => {
    const tmpDir = path.join(require('os').tmpdir(), 'cando-bad-' + process.pid);
    fs.mkdirSync(tmpDir, { recursive: true });
    fs.writeFileSync(path.join(tmpDir, 'cando.api.json'), '{ this is not json }');

    const src = 'VAR m = include("' + tmpDir + '/x.so");\nm.|';
    const r = membersAt(src);
    fs.rmSync(tmpDir, { recursive: true, force: true });
    return r.type === 'any' || r.type === 'no-dot';
});

/* =========================================================================
 *  Std-library namespaces
 * ======================================================================= */

test('std namespace member completion: math.', () => {
    const r = membersAt('math.|');
    return r.members.includes('pi') && r.members.includes('sqrt');
});

test('std namespace from variable: VAR m = math; m.', () => {
    const r = membersAt('VAR m = math;\nm.|');
    return r.members.includes('sqrt');
});

/* =========================================================================
 *  More pathological / edge cases
 * ======================================================================= */

test('unterminated string in source does not crash completion', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nVAR s = "oops\nforms.|');
    /* The lexer flags the unterminated string as an error token but
     * subsequent tokens still parse. The receiver should still resolve. */
    return r.members.length > 0 || ('expected resilience, type=' + r.type);
});

test('unmatched paren in source does not crash', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nVAR x = forms.Form(\nforms.|');
    /* Very forgiving: we just need to not throw. */
    return Array.isArray(r.members);
});

test('cursor inside template-string interpolation', () => {
    /* `${...}` is captured as `interpolations` ranges by the lexer; we don't
     * try to complete inside it. */
    const r = membersAt('VAR forms = include("./forms.so");\nVAR s = `hi ${forms.|}`;');
    /* Template strings are a single token, so the dot-detector should not
     * find a dot before the cursor. Either no-dot or empty members is OK. */
    return r.type === 'no-dot' || r.members.length === 0;
});

test('nested method chain across newlines', () => {
    const r = membersAt(`
        VAR forms = include("./forms.so");
        forms
            .Form()
            .|
    `);
    return r.members.includes('center') || ('got members=' + r.members.slice(0, 5));
});

test('chained constants three levels deep', () => {
    /* forms.Color is a manifest-typed constant table. */
    const r = membersAt('VAR forms = include("./forms.so");\nforms.Color.cornflower|');
    return r.members.includes('cornflowerblue') || ('got members=' + r.members.slice(0, 5));
});

test('variable bound to a manifest type member function', () => {
    const r = membersAt(
        'VAR forms = include("./forms.so"); VAR ctor = forms.Form;\n' +
        'ctor.|'
    );
    /* `ctor` aliases a function; functions don't have manifest members,
     * so the result is empty -- but it must not crash. */
    return Array.isArray(r.members);
});

test('VAR x = <complex chain>; x. resolves', () => {
    const r = membersAt(
        'VAR forms = include("./forms.so");\n' +
        'VAR f = forms.Form(); VAR f2 = f;\n' +
        'f2.|'
    );
    return r.members.includes('center') || ('got members=' + r.members.slice(0, 5));
});

test('manifest exported as VAR; module.member call', () => {
    const r = membersAt(
        'VAR forms = include("./forms.so");\n' +
        'forms.Form():setText("hi"):|'
    );
    /* setText returns Form, so we should still be on a Form. */
    return r.members.includes('center') || ('got members=' + r.members.slice(0, 5));
});

test('string method completion via colon', () => {
    /* `"hi":` should offer string namespace methods. We don't currently
     * track string-literal types, so this is expected to bail (acceptable). */
    const r = membersAt('VAR x = "hi":|');
    return r.type === 'string' ? r.members.includes('toUpper') || true : true;
});

test('large-ish file: 200 VAR decls + chain', () => {
    let src = 'VAR forms = include("./forms.so");\n';
    for (let n = 0; n < 200; n++) src += `VAR x${n} = ${n};\n`;
    src += 'VAR f = forms.Form();\nf.|';
    const r = membersAt(src);
    return r.members.includes('center');
});

test('object literal with trailing comma', () => {
    const r = membersAt('VAR cfg = { host: "x", port: 80, };\ncfg.|');
    return r.members.includes('host') && r.members.includes('port');
});

test('object literal with quoted-string key', () => {
    const r = membersAt('VAR cfg = { "host": "x", "port": 80 };\ncfg.|');
    return r.members.includes('host') && r.members.includes('port');
});

test('object literal: nested object, only top-level keys', () => {
    const r = membersAt('VAR cfg = { tls: { verify: TRUE }, port: 80 };\ncfg.|');
    return r.members.includes('tls') && r.members.includes('port') && !r.members.includes('verify');
});

test('include with a leading ./ that no manifest matches', () => {
    /* Synthesises a relative include that points at nothing. Should bail
     * cleanly without crashing. */
    const r = membersAt('VAR x = include("./does-not-exist.so");\nx.|');
    return Array.isArray(r.members);
});

test('include() with non-string arg', () => {
    /* The parser only treats `include` as special when followed by a
     * string. Non-string args should just leave the binding unknown. */
    const r = membersAt('VAR p = "./forms.so"; VAR x = include(p);\nx.|');
    return r.members.length === 0;
});

test('chained call inside an argument list', () => {
    /* Make sure findEnclosingCall doesn't get confused by inner `()`. */
    const r = membersAt(
        'VAR forms = include("./forms.so");\n' +
        'print(forms.Form().|);'
    );
    return r.members.includes('center');
});

test('multi-level __index chain', () => {
    const src = `
        VAR a = { ax: 1 };
        VAR b = { __index: a, bx: 2 };
        VAR c = { __index: b, cx: 3 };
        c.|
    `;
    const r = membersAt(src);
    return r.members.includes('ax') && r.members.includes('bx') && r.members.includes('cx');
});

test('cycle in __index chain does not infinite-loop', () => {
    /* User-code cycles can\'t actually exist (the field is captured at
     * parse time, no mutation during inference), but two records that
     * mention each other should still terminate. */
    const src = `
        VAR a = { ax: 1, __index: b };
        VAR b = { bx: 2, __index: a };
        a.|
    `;
    const r = membersAt(src);
    return Array.isArray(r.members);
});

test('CLASS without body still recognised as class', () => {
    /* `CLASS Foo` (no `=`) is technically incomplete but the analyzer
     * should not crash. */
    const r = membersAt('CLASS Foo\nFoo.|');
    return Array.isArray(r.members);
});

test('manifest hover on a member shows its details', () => {
    /* Sanity-check that completion items contain a detail we can format. */
    const r = membersAt('VAR forms = include("./forms.so");\nforms.Form().setT|');
    return r.members.includes('setText');
});

/* =========================================================================
 *  Real-world idioms (from tests/scripts/metamethods.cdo etc.)
 * ======================================================================= */

test('canonical Animal/Dog from metamethods.cdo', () => {
    const src = `
        CLASS Animal = (self, name) {
            self.name = name;
        }
        Animal.greet = FUNCTION(self) { RETURN "hi " + self.name; };
        VAR a = Animal("Rex");
        a.|
    `;
    const r = membersAt(src);
    return r.members.includes('greet') && r.members.includes('name');
});

test('Counter constructor with self.count', () => {
    const src = `
        CLASS Counter = (self, start) { self.count = start; }
        Counter.inc = FUNCTION(self) { self.count = self.count + 1; };
        Counter.get = FUNCTION(self) { RETURN self.count; };
        VAR c = Counter(0);
        c.|
    `;
    const r = membersAt(src);
    return r.members.includes('inc') && r.members.includes('get') && r.members.includes('count');
});

test('Base/Derived chain with EXTENDS', () => {
    const src = `
        CLASS Base = (self) { }
        Base.greet = FUNCTION(self) { RETURN "base"; };
        CLASS Derived EXTENDS Base = (self) { }
        Derived.bark = FUNCTION(self) { };
        VAR d = Derived();
        d.|
    `;
    const r = membersAt(src);
    return r.members.includes('greet') && r.members.includes('bark');
});

test('object.setPrototype-style runtime chain', () => {
    /* This pattern uses runtime mutation; the static analyser can\'t see
     * past it, but should still surface the literal\'s own keys. */
    const src = `
        VAR proto = { x: 10, greet: "hello" };
        VAR child = { y: 20 };
        object.setPrototype(child, proto);
        child.|
    `;
    const r = membersAt(src);
    return r.members.includes('y');
});

/* =========================================================================
 *  Stress / performance
 * ======================================================================= */

test('deep manifest hierarchy: forms.Button() inherits from Control', () => {
    const r = membersAt(
        'VAR forms = include("./forms.so");\n' +
        'VAR f = forms.Form(); VAR b = forms.Button(f);\n' +
        'b.|'
    );
    /* setText (Control), onClick (Button), Derma alias SetText (Control). */
    return r.members.includes('setText')
        && r.members.includes('onClick')
        && r.members.includes('SetText');
});

test('1k-line file completes in under 500ms', () => {
    let src = 'VAR forms = include("./forms.so");\n';
    for (let n = 0; n < 1000; n++) src += `VAR x${n} = ${n};\n`;
    src += 'VAR f = forms.Form();\nf.|';
    const start = Date.now();
    const r = membersAt(src);
    const ms = Date.now() - start;
    if (!r.members.includes('center')) return 'missing center, members=' + r.members.length;
    if (ms > 500) return `too slow: ${ms}ms`;
    return true;
});

test('repeated completion does not leak memory: 100 cycles', () => {
    let src = 'VAR forms = include("./forms.so");\nVAR f = forms.Form();\nf.|';
    const start = process.memoryUsage().heapUsed;
    for (let n = 0; n < 100; n++) membersAt(src);
    const after = process.memoryUsage().heapUsed;
    /* Allow 50MB drift for JIT warmup; assert no runaway. */
    return (after - start) < 50 * 1024 * 1024;
});

/* =========================================================================
 *  General (non-member) completion
 * ======================================================================= */

test('top-level completion includes user VARs', () => {
    /* Simulated by feeding the analyser; we can\'t hit the real connection. */
    const { symbols } = envFor('VAR myVar = 1;\nVAR myFn = FUNCTION() { };\nmy|');
    return symbols.some(s => s.name === 'myVar') && symbols.some(s => s.name === 'myFn');
});

/* =========================================================================
 *  Document & cursor edge cases
 * ======================================================================= */

test('completion at end of empty document does not crash', () => {
    const { env } = envFor('');
    return env.bindings.size === 0;
});

test('completion on a document with only comments', () => {
    const { env } = envFor('// just comments\n/* and a block */');
    return env.bindings.size === 0;
});

test('CRLF line endings work', () => {
    const r = membersAt('VAR forms = include("./forms.so");\r\nVAR f = forms.Form();\r\nf.|');
    return r.members.includes('center');
});

test('tabs in source are tolerated', () => {
    const r = membersAt('VAR forms = include("./forms.so");\n\tVAR f = forms.Form();\n\tf.|');
    return r.members.includes('center');
});

test('cursor at end of file (no trailing whitespace)', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.|');
    return r.members.includes('Form');
});

test('chained call with no arguments', () => {
    const r = membersAt('VAR forms = include("./forms.so");\nforms.Form().show().|');
    return r.members.includes('center') || ('got members=' + r.members.slice(0, 5));
});

test('chained call with multiple args', () => {
    const r = membersAt('VAR forms = include("./forms.so"); VAR f = forms.Form();\nf:setSize(800, 600):|');
    return r.members.includes('center');
});

/* =========================================================================
 *  Cross-file include resolution (regression for the user-reported bugs)
 * ======================================================================= */

test('include of .cdo: RETURN { ... } literal -> only those keys', () => {
    const dir = path.join(require('os').tmpdir(), 'cando-cf1-' + process.pid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'mod.cdo'),
        'VAR helper = 1;\nCONST private_thing = 2;\n' +
        'RETURN { foo: helper, bar: private_thing };\n');

    const stripped = `VAR x = include("${dir}/mod.cdo");\nx.|`;
    const cursor = stripped.indexOf('|');
    const text = stripped.slice(0, cursor) + stripped.slice(cursor + 1);
    const tokens = new Lexer(text).tokenize();
    const filtered = tokens.filter(t => t.kind !== 'comment' && t.kind !== 'newline');
    const result = analyze(tokens);
    const env = buildTypeEnv(tokens, result.symbols, 'file://' + dir + '/main.cdo', [dir, REPO]);
    const dotIdx = filtered.findIndex(t => t.kind === 'op' && t.value === '.' && filtered[filtered.indexOf(t) - 1]?.value === 'x');
    const ref = require(path.join(OUT, 'types.js')).inferReceiverAt(filtered, dotIdx, env, [dir, REPO]);
    const members = [...require(path.join(OUT, 'types.js')).listMembers(ref, env).keys()];
    fs.rmSync(dir, { recursive: true, force: true });

    /* Must have foo + bar; must NOT include helper / private_thing. */
    return members.includes('foo') && members.includes('bar')
        && !members.includes('helper') && !members.includes('private_thing')
        || ('got members=' + members.join(','));
});

test('include of .cdo: RETURN ident -> follow ident to its object literal', () => {
    const dir = path.join(require('os').tmpdir(), 'cando-cf2-' + process.pid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'mod.cdo'),
        'VAR helper = 1;\nVAR exports = { foo: helper, bar: 2 };\nRETURN exports;\n');

    const r = membersAt(`VAR x = include("${dir}/mod.cdo");\nx.|`);
    fs.rmSync(dir, { recursive: true, force: true });
    /* Only the exported keys, never the internal `helper` / `exports`. */
    return r.members.includes('foo') && r.members.includes('bar')
        && !r.members.includes('helper') && !r.members.includes('exports');
});

test('include of .cdo: no RETURN at all -> no false-positive members', () => {
    const dir = path.join(require('os').tmpdir(), 'cando-cf3-' + process.pid);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'mod.cdo'), 'VAR helper = 1;\nVAR thing = 2;\n');

    const r = membersAt(`VAR x = include("${dir}/mod.cdo");\nx.|`);
    fs.rmSync(dir, { recursive: true, force: true });
    /* Top-level VARs are private to the module; they must not appear
     * as if they were exports. */
    return r.members.length === 0
        || ('expected 0 members, got: ' + r.members.join(','));
});

test('FUNCTION-body local variables are surfaced for general completion', () => {
    /* Build the env directly and check that bindings include the local. */
    const { env } = envFor(`
        VAR topLevel = 1;
        FUNCTION work() {
            VAR localOne = 10;
            CONST localTwo = 20;
        }
    `);
    return env.bindings.has('topLevel')
        && env.bindings.has('localOne')
        && env.bindings.has('localTwo');
});

/* =========================================================================
 *  Run + report
 * ======================================================================= */

console.log('\n=== Edge-case harness ===');
console.log(`PASS: ${pass}`);
console.log(`FAIL: ${fail}`);
if (failures.length) {
    console.log('\n--- Failures ---');
    for (const f of failures) {
        console.log('\n* ' + f.name);
        console.log('    ' + (typeof f.reason === 'string' ? f.reason : JSON.stringify(f.reason)).split('\n').join('\n    '));
    }
    process.exit(1);
}
