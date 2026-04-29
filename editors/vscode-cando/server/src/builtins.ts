/*
 * Static metadata about CanDo keywords, global builtins and the standard
 * library namespaces. Names mirror the registrations in source/natives.c
 * and source/lib/*.c so completions stay accurate as the runtime grows.
 */

export interface KeywordInfo {
    name: string;
    detail: string;
    doc: string;
}

export interface BuiltinInfo {
    name: string;
    detail: string;
    doc: string;
}

export interface NamespaceInfo {
    name: string;
    doc: string;
    members: string[];
}

export const KEYWORDS_UPPER: KeywordInfo[] = [
    { name: 'IF',       detail: 'IF cond { ... }',                    doc: 'Conditional branch. Supports multi-comparison: `IF x == 1, 2, 3 { ... }`.' },
    { name: 'ELSE',     detail: 'ELSE { ... }',                       doc: 'Alternative branch of an `IF`.' },
    { name: 'WHILE',    detail: 'WHILE cond { ... }',                 doc: 'Loop while condition is truthy.' },
    { name: 'FOR',      detail: 'FOR i OF a -> b { ... }',            doc: 'Iteration. Forms: `FOR i OF a -> b`, `FOR i OF b <- a`, `FOR x IN coll`, `FOR k, v OVER obj`.' },
    { name: 'FUNCTION', detail: 'FUNCTION name(args) { ... }',        doc: 'Declare a named function.' },
    { name: 'CLASS',    detail: 'CLASS Name [EXTENDS Parent] { ... }', doc: 'Declare a class with prototype-based inheritance.' },
    { name: 'EXTENDS',  detail: 'EXTENDS Parent',                     doc: 'Inherit from another class.' },
    { name: 'RETURN',   detail: 'RETURN [expr[, expr, ...]];',        doc: 'Return one or more values from a function.' },
    { name: 'THROW',    detail: 'THROW expr;',                        doc: 'Raise an error to the nearest CATCH.' },
    { name: 'TRY',      detail: 'TRY { } CATCH e { } FINALY { }',     doc: 'Begin a protected block.' },
    { name: 'CATCH',    detail: 'CATCH name { ... }',                 doc: 'Handle an error thrown in a TRY block.' },
    { name: 'FINALY',   detail: 'FINALY { ... }',                     doc: 'Always runs after TRY/CATCH. Note: spelt with one L.' },
    { name: 'CONST',    detail: 'CONST name = value;',                doc: 'Declare an immutable binding.' },
    { name: 'VAR',      detail: 'VAR name = value;',                  doc: 'Declare a mutable, block-scoped binding.' },
    { name: 'GLOBAL',   detail: 'GLOBAL name = value;',               doc: 'Declare a global variable.' },
    { name: 'STATIC',   detail: 'STATIC ...',                         doc: 'Class-level (non-instance) member.' },
    { name: 'PRIVATE',  detail: 'PRIVATE ...',                        doc: 'Class member not visible outside the class.' },
    { name: 'ASYNC',    detail: 'ASYNC FUNCTION name() { ... }',      doc: 'Mark a function as asynchronous.' },
    { name: 'AWAIT',    detail: 'AWAIT expr',                         doc: 'Await an asynchronous result.' },
    { name: 'THREAD',   detail: 'THREAD expr;',                       doc: 'Spawn an OS thread to run the expression.' },
    { name: 'NULL',     detail: 'NULL',                               doc: 'The null value.' },
    { name: 'TRUE',     detail: 'TRUE',                               doc: 'Boolean true.' },
    { name: 'FALSE',    detail: 'FALSE',                              doc: 'Boolean false.' },
    { name: 'IN',       detail: 'FOR x IN coll',                      doc: 'Iteration form: each element of an iterable.' },
    { name: 'OF',       detail: 'FOR i OF a -> b',                    doc: 'Iteration form: range generator.' },
    { name: 'OVER',     detail: 'FOR k, v OVER obj',                  doc: 'Iteration form: keys and values of an object.' },
    { name: 'CONTINUE', detail: 'CONTINUE;',                          doc: 'Skip to the next iteration of the enclosing loop.' },
    { name: 'BREAK',    detail: 'BREAK;',                             doc: 'Exit the enclosing loop.' }
];

export const PIPE_KEYWORD: KeywordInfo = {
    name: 'pipe',
    detail: 'pipe',
    doc: 'Implicit iteration variable inside `~>`, `~!>`, `~&>` pipe blocks.'
};

export const GLOBAL_BUILTINS: BuiltinInfo[] = [
    { name: 'print',    detail: 'print(...args)',     doc: 'Write space-separated args to stdout, followed by a newline. Arrays are expanded element-by-element.' },
    { name: 'type',     detail: 'type(value)',        doc: 'Return the type name of `value` as a string. Honours an object\'s `__type` metafield.' },
    { name: 'toString', detail: 'toString(value)',    doc: 'Return the string form of `value`.' },
    { name: 'inspect',  detail: 'inspect(value)',     doc: 'Return a debug representation of `value`, including object internals.' }
];

export const NAMESPACES: NamespaceInfo[] = [
    {
        name: 'array', doc: 'Array helpers (mostly chainable via the `:` method syntax).',
        members: ['copy', 'filter', 'length', 'map', 'pop', 'push', 'reduce', 'remove', 'splice']
    },
    {
        name: 'string', doc: 'String helpers.',
        members: ['char', 'chars', 'find', 'format', 'left', 'length', 'match', 'repeat', 'replace', 'right', 'split', 'sub', 'trim']
    },
    {
        name: 'math', doc: 'Math functions and constants.',
        members: [
            'abs', 'acos', 'asin', 'atan', 'ceil', 'clamp', 'cos', 'cosh', 'deg', 'exp',
            'floor', 'log', 'max', 'min', 'pow', 'rad', 'random', 'round', 'sign', 'sin',
            'sinh', 'sqrt', 'tan',
            'pi', 'tau', 'e', 'huge'
        ]
    },
    {
        name: 'json', doc: 'JSON encoder / decoder.',
        members: ['parse', 'stringify']
    },
    {
        name: 'csv', doc: 'CSV encoder / decoder.',
        members: ['parse', 'stringify']
    },
    {
        name: 'file', doc: 'File-system access.',
        members: ['append', 'copy', 'delete', 'exists', 'lines', 'list', 'mkdir', 'move', 'read', 'size', 'write']
    },
    {
        name: 'os', doc: 'Operating-system helpers.',
        members: ['clock', 'execute', 'exit', 'getenv', 'setenv', 'time']
    },
    {
        name: 'datetime', doc: 'Date and time utilities.',
        members: ['format', 'now', 'parse']
    },
    {
        name: 'crypto', doc: 'Cryptographic primitives.',
        members: []
    },
    {
        name: 'http', doc: 'Plain-text HTTP client.',
        members: ['get', 'request']
    },
    {
        name: 'https', doc: 'TLS-secured HTTP client.',
        members: ['get', 'request']
    },
    {
        name: 'socket', doc: 'TCP socket primitives.',
        members: ['connect', 'resolve', 'tcp']
    },
    {
        name: 'secure_socket', doc: 'TLS-wrapped socket primitives.',
        members: ['connect', 'tcp']
    },
    {
        name: 'net', doc: 'Network helpers.',
        members: ['lookup']
    },
    {
        name: 'thread', doc: 'Thread / promise helpers.',
        members: ['cancel', 'catch', 'current', 'done', 'error', 'id', 'join', 'sleep', 'state', 'then']
    },
    {
        name: 'process', doc: 'Process information.',
        members: ['pid', 'ppid']
    },
    {
        name: 'object', doc: 'Object reflection helpers.',
        members: ['apply', 'assign', 'copy', 'get', 'keys', 'lock', 'locked', 'set', 'unlock', 'values']
    },
    {
        name: 'app', doc: 'Application / runtime helpers.',
        members: []
    }
];

export function namespaceByName(name: string): NamespaceInfo | undefined {
    return NAMESPACES.find(ns => ns.name === name);
}

export function isKeywordUpper(name: string): boolean {
    return KEYWORDS_UPPER.some(k => k.name === name);
}

export function isKeywordAnyCase(name: string): boolean {
    if (name === 'pipe') return true;
    if (name === name.toUpperCase() && isKeywordUpper(name)) return true;
    if (name === name.toLowerCase() && isKeywordUpper(name.toUpperCase())) return true;
    return false;
}
