/*
 * modules/cffi/cffi_types.h -- Pure-C type-parser and layout helpers
 * for the CanDo cffi binary module.
 *
 * Kept header-only with `static inline` definitions so the unit tests
 * can include this file directly and exercise each helper without
 * linking against the rest of the module (or against libcando).
 *
 * The type system here is intentionally small: enough primitive kinds
 * to cover every libc / Win32 / vendor-DLL prototype real users want to
 * bind, plus a generic `void*` opaque pointer.  Structs, unions, and
 * arrays are deferred to a later milestone; this file documents the
 * grammar that is in scope.
 *
 * Grammar (informal):
 *
 *     signature   := type '(' arglist? ')'
 *     arglist     := 'void' | type (',' type)*
 *     type        := qualifier* base pointer*
 *     qualifier   := 'const' | 'volatile' | 'restrict'
 *     base        := 'void' | 'bool' | '_Bool'
 *                  | 'char' | 'signed char' | 'unsigned char'
 *                  | 'short' | 'short int' | 'signed short' | …
 *                  | 'int' | 'signed int' | 'unsigned int' | 'unsigned'
 *                  | 'long' | 'signed long' | 'unsigned long'
 *                  | 'long long' | 'signed long long' | 'unsigned long long'
 *                  | 'float' | 'double'
 *                  | 'size_t' | 'ssize_t' | 'ptrdiff_t'
 *                  | 'intptr_t' | 'uintptr_t'
 *                  | 'int8_t' | … | 'int64_t' | 'uint8_t' | … | 'uint64_t'
 *     pointer     := '*' qualifier*
 *
 * `char*` and `const char*` parse to CFFI_CSTR (string-friendly
 * marshalling).  Every other pointer parses to CFFI_PTR (opaque).
 *
 * Tokens are case-sensitive (C names) and whitespace-insensitive.
 * Trailing whitespace is allowed everywhere.
 */

#ifndef CANDO_CFFI_TYPES_H
#define CANDO_CFFI_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

/* =========================================================================
 * Kinds
 * ======================================================================= */

typedef enum CffiKind {
    CFFI_VOID = 0,
    CFFI_BOOL,
    CFFI_I8,  CFFI_U8,
    CFFI_I16, CFFI_U16,
    CFFI_I32, CFFI_U32,
    CFFI_I64, CFFI_U64,
    CFFI_F32, CFFI_F64,
    CFFI_PTR,    /* opaque void*, T* (non-char) */
    CFFI_CSTR,   /* char*, const char* -- gets string-friendly marshalling */
    CFFI_KIND_COUNT
} CffiKind;

/* Max function arity supported by lib:call / lib:bind.  Bumping this
 * up requires no other changes; the value is also asserted at parse
 * time so the user gets a clean error rather than a stack smash. */
#ifndef CFFI_MAX_ARGS
#define CFFI_MAX_ARGS 16
#endif

typedef struct CffiSig {
    CffiKind ret;
    int      nargs;
    CffiKind args[CFFI_MAX_ARGS];
} CffiSig;

/* =========================================================================
 * Width / alignment of primitive kinds
 *
 * Indexed by CffiKind.  Pointer width is the host's `sizeof(void*)`,
 * so a build for ILP32 would naturally get 4; we only ever build for
 * 64-bit hosts, but the table doesn't assume.
 * ======================================================================= */

static inline size_t cffi_kind_size(CffiKind k)
{
    switch (k) {
        case CFFI_VOID: return 0;
        case CFFI_BOOL: return 1;
        case CFFI_I8:   case CFFI_U8:  return 1;
        case CFFI_I16:  case CFFI_U16: return 2;
        case CFFI_I32:  case CFFI_U32: return 4;
        case CFFI_I64:  case CFFI_U64: return 8;
        case CFFI_F32:  return 4;
        case CFFI_F64:  return 8;
        case CFFI_PTR:  case CFFI_CSTR: return sizeof(void*);
        default: return 0;
    }
}

static inline size_t cffi_kind_align(CffiKind k)
{
    /* Every scalar libffi cares about has alignment == size on the
     * platforms we target (x86-64 SysV, x86-64 Win64, AArch64 SysV).
     * Track them separately anyway so adding e.g. long-double-with-
     * smaller-align doesn't silently misalign other types. */
    return cffi_kind_size(k);
}

static inline bool cffi_kind_is_integer(CffiKind k)
{
    switch (k) {
        case CFFI_BOOL:
        case CFFI_I8:  case CFFI_U8:
        case CFFI_I16: case CFFI_U16:
        case CFFI_I32: case CFFI_U32:
        case CFFI_I64: case CFFI_U64:
            return true;
        default:
            return false;
    }
}

static inline bool cffi_kind_is_signed(CffiKind k)
{
    switch (k) {
        case CFFI_I8: case CFFI_I16: case CFFI_I32: case CFFI_I64:
            return true;
        default:
            return false;
    }
}

static inline bool cffi_kind_is_float(CffiKind k)
{
    return k == CFFI_F32 || k == CFFI_F64;
}

static inline bool cffi_kind_is_pointer(CffiKind k)
{
    return k == CFFI_PTR || k == CFFI_CSTR;
}

/* Human-readable spelling.  Used in error messages. */
static inline const char *cffi_kind_name(CffiKind k)
{
    switch (k) {
        case CFFI_VOID: return "void";
        case CFFI_BOOL: return "bool";
        case CFFI_I8:   return "int8";
        case CFFI_U8:   return "uint8";
        case CFFI_I16:  return "int16";
        case CFFI_U16:  return "uint16";
        case CFFI_I32:  return "int32";
        case CFFI_U32:  return "uint32";
        case CFFI_I64:  return "int64";
        case CFFI_U64:  return "uint64";
        case CFFI_F32:  return "float";
        case CFFI_F64:  return "double";
        case CFFI_PTR:  return "pointer";
        case CFFI_CSTR: return "cstring";
        default:        return "?";
    }
}

/* =========================================================================
 * Platform-dependent C type widths
 *
 * `long` is 64-bit on LP64 (Linux, macOS) and 32-bit on LLP64
 * (Windows MSVC, MinGW).  Bake the choice in at compile time so the
 * mapping reflects whatever ABI this module ends up linking against.
 *
 * `size_t` is treated as unsigned-int-of-pointer-width; same for
 * `ptrdiff_t` (signed).  `intptr_t` / `uintptr_t` are explicit
 * pointer-width integers per the C standard.
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
#  define CFFI_LONG_KIND   CFFI_I32
#  define CFFI_ULONG_KIND  CFFI_U32
#else
#  define CFFI_LONG_KIND   (sizeof(long) == 8 ? CFFI_I64 : CFFI_I32)
#  define CFFI_ULONG_KIND  (sizeof(long) == 8 ? CFFI_U64 : CFFI_U32)
#endif

#define CFFI_LLONG_KIND   CFFI_I64
#define CFFI_ULLONG_KIND  CFFI_U64

/* size_t / ssize_t / ptrdiff_t / intptr_t / uintptr_t -- all
 * pointer-width on every platform we target. */
#define CFFI_SIZE_KIND     (sizeof(size_t) == 8 ? CFFI_U64 : CFFI_U32)
#define CFFI_SSIZE_KIND    (sizeof(size_t) == 8 ? CFFI_I64 : CFFI_I32)
#define CFFI_PTRDIFF_KIND  CFFI_SSIZE_KIND
#define CFFI_INTPTR_KIND   CFFI_SSIZE_KIND
#define CFFI_UINTPTR_KIND  CFFI_SIZE_KIND

/* =========================================================================
 * Parser
 * ======================================================================= */

typedef struct CffiParseError {
    char        message[160];   /* fixed-size buffer -- no allocation */
    size_t      offset;         /* byte offset into the input where the error was detected */
} CffiParseError;

static inline void cffi_parse_error_clear(CffiParseError *e)
{
    if (!e) return;
    e->message[0] = '\0';
    e->offset     = 0;
}

static inline void cffi_set_error(CffiParseError *e, size_t offset,
                                  const char *msg)
{
    if (!e) return;
    e->offset = offset;
    size_t n = strlen(msg);
    if (n >= sizeof(e->message)) n = sizeof(e->message) - 1;
    memcpy(e->message, msg, n);
    e->message[n] = '\0';
}

/* --- Lexer state ------------------------------------------------------- */

typedef struct CffiLex {
    const char *src;
    size_t      len;
    size_t      pos;
} CffiLex;

static inline void cffi_lex_skip_ws(CffiLex *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            lx->pos++;
        else
            break;
    }
}

static inline bool cffi_is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static inline bool cffi_consume_char(CffiLex *lx, char c)
{
    cffi_lex_skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == c) {
        lx->pos++;
        return true;
    }
    return false;
}

/* cffi_consume_word -- match `word` as a whole identifier (delimited
 * by non-word characters or end-of-input).  Does not consume on
 * mismatch. */
static inline bool cffi_consume_word(CffiLex *lx, const char *word)
{
    cffi_lex_skip_ws(lx);
    size_t n = strlen(word);
    if (lx->pos + n > lx->len) return false;
    if (memcmp(lx->src + lx->pos, word, n) != 0) return false;
    /* Boundary check: next char must not extend the identifier. */
    if (lx->pos + n < lx->len && cffi_is_word_char(lx->src[lx->pos + n]))
        return false;
    lx->pos += n;
    return true;
}

/* Peek at the next identifier without consuming it.  Returns the
 * length of the identifier (0 if no identifier at the cursor). */
static inline size_t cffi_peek_word(CffiLex *lx, const char **out_start)
{
    cffi_lex_skip_ws(lx);
    size_t s = lx->pos;
    if (s >= lx->len) { if (out_start) *out_start = NULL; return 0; }
    char c = lx->src[s];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        { if (out_start) *out_start = NULL; return 0; }
    size_t e = s + 1;
    while (e < lx->len && cffi_is_word_char(lx->src[e])) e++;
    if (out_start) *out_start = lx->src + s;
    return e - s;
}

/* Consume + discard zero or more `const` / `volatile` / `restrict`
 * qualifiers at the cursor. */
static inline void cffi_consume_qualifiers(CffiLex *lx)
{
    for (;;) {
        size_t save = lx->pos;
        if (cffi_consume_word(lx, "const"))    continue;
        if (cffi_consume_word(lx, "volatile")) continue;
        if (cffi_consume_word(lx, "restrict")) continue;
        if (cffi_consume_word(lx, "__restrict")) continue;
        lx->pos = save;
        break;
    }
}

/* --- Base-type matcher ------------------------------------------------- */
/*
 * Order matters: longer forms must be tried before shorter ones, since
 * `cffi_consume_word` is greedy on a single identifier but a multi-word
 * base type ("long long") needs each piece consumed in turn.  The base
 * matcher tries each candidate; on miss it rewinds to `save`.
 */
static inline bool cffi_match_base(CffiLex *lx, CffiKind *out_kind)
{
    size_t save = lx->pos;

    /* Two-token (sign + type) and three-token (unsigned long long).
     * Try the longest combinations first, rewinding on miss. */
    if (cffi_consume_word(lx, "unsigned")) {
        size_t s2 = lx->pos;
        if (cffi_consume_word(lx, "long")) {
            if (cffi_consume_word(lx, "long")) {
                (void)cffi_consume_word(lx, "int");
                *out_kind = CFFI_ULLONG_KIND; return true;
            }
            (void)cffi_consume_word(lx, "int");
            *out_kind = CFFI_ULONG_KIND; return true;
        }
        lx->pos = s2;
        if (cffi_consume_word(lx, "short")) {
            (void)cffi_consume_word(lx, "int");
            *out_kind = CFFI_U16; return true;
        }
        lx->pos = s2;
        if (cffi_consume_word(lx, "char"))  { *out_kind = CFFI_U8;  return true; }
        if (cffi_consume_word(lx, "int"))   { *out_kind = CFFI_U32; return true; }
        /* Bare `unsigned` is `unsigned int`. */
        *out_kind = CFFI_U32; return true;
    }
    if (cffi_consume_word(lx, "signed")) {
        size_t s2 = lx->pos;
        if (cffi_consume_word(lx, "long")) {
            if (cffi_consume_word(lx, "long")) {
                (void)cffi_consume_word(lx, "int");
                *out_kind = CFFI_LLONG_KIND; return true;
            }
            (void)cffi_consume_word(lx, "int");
            *out_kind = CFFI_LONG_KIND; return true;
        }
        lx->pos = s2;
        if (cffi_consume_word(lx, "short")) {
            (void)cffi_consume_word(lx, "int");
            *out_kind = CFFI_I16; return true;
        }
        lx->pos = s2;
        if (cffi_consume_word(lx, "char"))  { *out_kind = CFFI_I8;  return true; }
        if (cffi_consume_word(lx, "int"))   { *out_kind = CFFI_I32; return true; }
        /* Bare `signed` is `signed int`. */
        *out_kind = CFFI_I32; return true;
    }
    if (cffi_consume_word(lx, "long")) {
        if (cffi_consume_word(lx, "long")) {
            (void)cffi_consume_word(lx, "int");
            *out_kind = CFFI_LLONG_KIND; return true;
        }
        if (cffi_consume_word(lx, "double")) {
            /* `long double` is rare in C ABIs and varies across
             * platforms (80-bit on x86 SysV, 64-bit on Windows,
             * 128-bit on AArch64 Apple).  Reject for now -- bindings
             * needing it can pass the bytes through a buffer. */
            lx->pos = save;
            return false;
        }
        (void)cffi_consume_word(lx, "int");
        *out_kind = CFFI_LONG_KIND; return true;
    }
    if (cffi_consume_word(lx, "short")) {
        (void)cffi_consume_word(lx, "int");
        *out_kind = CFFI_I16; return true;
    }

    /* Fixed-width and stdint aliases.  Try longest names first. */
    if (cffi_consume_word(lx, "uintptr_t")) { *out_kind = CFFI_UINTPTR_KIND;  return true; }
    if (cffi_consume_word(lx, "intptr_t"))  { *out_kind = CFFI_INTPTR_KIND;   return true; }
    if (cffi_consume_word(lx, "ptrdiff_t")) { *out_kind = CFFI_PTRDIFF_KIND;  return true; }
    if (cffi_consume_word(lx, "ssize_t"))   { *out_kind = CFFI_SSIZE_KIND;    return true; }
    if (cffi_consume_word(lx, "size_t"))    { *out_kind = CFFI_SIZE_KIND;     return true; }

    if (cffi_consume_word(lx, "uint64_t"))  { *out_kind = CFFI_U64; return true; }
    if (cffi_consume_word(lx, "uint32_t"))  { *out_kind = CFFI_U32; return true; }
    if (cffi_consume_word(lx, "uint16_t"))  { *out_kind = CFFI_U16; return true; }
    if (cffi_consume_word(lx, "uint8_t"))   { *out_kind = CFFI_U8;  return true; }
    if (cffi_consume_word(lx, "int64_t"))   { *out_kind = CFFI_I64; return true; }
    if (cffi_consume_word(lx, "int32_t"))   { *out_kind = CFFI_I32; return true; }
    if (cffi_consume_word(lx, "int16_t"))   { *out_kind = CFFI_I16; return true; }
    if (cffi_consume_word(lx, "int8_t"))    { *out_kind = CFFI_I8;  return true; }

    if (cffi_consume_word(lx, "double"))    { *out_kind = CFFI_F64;  return true; }
    if (cffi_consume_word(lx, "float"))     { *out_kind = CFFI_F32;  return true; }

    if (cffi_consume_word(lx, "char"))      { *out_kind = CFFI_I8;   return true; }
    if (cffi_consume_word(lx, "int"))       { *out_kind = CFFI_I32;  return true; }
    if (cffi_consume_word(lx, "bool"))      { *out_kind = CFFI_BOOL; return true; }
    if (cffi_consume_word(lx, "_Bool"))     { *out_kind = CFFI_BOOL; return true; }
    if (cffi_consume_word(lx, "void"))      { *out_kind = CFFI_VOID; return true; }

    lx->pos = save;
    return false;
}

/* --- Single type ------------------------------------------------------- */

static inline bool cffi_parse_type(CffiLex *lx, CffiKind *out_kind,
                                    CffiParseError *err)
{
    cffi_consume_qualifiers(lx);
    CffiKind base = CFFI_VOID;
    size_t base_at = lx->pos;
    if (!cffi_match_base(lx, &base)) {
        cffi_set_error(err, lx->pos, "expected a type name");
        return false;
    }
    cffi_consume_qualifiers(lx);

    /* Pointer stars.  Each `*` upgrades the kind to PTR, except that
     * the first `*` on `char` / `signed char` (which we track as I8)
     * becomes CFFI_CSTR for string-friendly marshalling.
     *
     * Subsequent stars (e.g. `char**`) collapse back to opaque PTR.
     * This matches user intent: `char**` is rarely "an array of C
     * strings to copy" -- it's a pointer to a buffer of pointers. */
    bool first_star = true;
    while (cffi_consume_char(lx, '*')) {
        if (first_star && (base == CFFI_I8) && base_at == lx->pos - 1 /* trivial */) {
            /* unused: just to keep base_at referenced */
        }
        if (first_star && base == CFFI_I8) {
            base = CFFI_CSTR;
        } else {
            base = CFFI_PTR;
        }
        cffi_consume_qualifiers(lx);
        first_star = false;
    }
    (void)base_at;

    *out_kind = base;
    return true;
}

/* --- Full signature ---------------------------------------------------- */

static inline bool cffi_parse_signature(const char *text, CffiSig *out_sig,
                                         CffiParseError *err)
{
    cffi_parse_error_clear(err);
    if (!text) {
        cffi_set_error(err, 0, "signature is null");
        return false;
    }

    CffiLex lx = { text, strlen(text), 0 };

    /* Return type. */
    if (!cffi_parse_type(&lx, &out_sig->ret, err)) return false;

    /* '(' */
    if (!cffi_consume_char(&lx, '(')) {
        cffi_set_error(err, lx.pos, "expected '(' after return type");
        return false;
    }

    out_sig->nargs = 0;

    /* Empty arglist or explicit 'void'. */
    cffi_lex_skip_ws(&lx);
    if (cffi_consume_char(&lx, ')')) {
        goto check_tail;
    }
    {
        size_t save = lx.pos;
        if (cffi_consume_word(&lx, "void")) {
            cffi_lex_skip_ws(&lx);
            if (cffi_consume_char(&lx, ')')) {
                goto check_tail;
            }
            /* Wasn't a bare `void)` -- could be `void*` etc.  Rewind. */
            lx.pos = save;
        }
    }

    for (;;) {
        if (out_sig->nargs >= CFFI_MAX_ARGS) {
            cffi_set_error(err, lx.pos,
                "too many parameters (compiled-in limit is CFFI_MAX_ARGS)");
            return false;
        }
        CffiKind a;
        if (!cffi_parse_type(&lx, &a, err)) return false;
        if (a == CFFI_VOID) {
            cffi_set_error(err, lx.pos,
                "parameter has type 'void'; use 'void*' for an opaque pointer "
                "or '(void)' for a no-arg function");
            return false;
        }
        out_sig->args[out_sig->nargs++] = a;

        if (cffi_consume_char(&lx, ',')) continue;
        if (cffi_consume_char(&lx, ')')) break;
        cffi_set_error(err, lx.pos, "expected ',' or ')'");
        return false;
    }

check_tail:
    /* Reject trailing garbage. */
    cffi_lex_skip_ws(&lx);
    if (lx.pos != lx.len) {
        cffi_set_error(err, lx.pos, "trailing characters after ')'");
        return false;
    }
    return true;
}

/* =========================================================================
 * Standalone type parser (used by ffi.sizeof / ffi.alignof / ffi.alloc)
 * ======================================================================= */

static inline bool cffi_parse_single_type(const char *text, CffiKind *out_kind,
                                          CffiParseError *err)
{
    cffi_parse_error_clear(err);
    if (!text) {
        cffi_set_error(err, 0, "type is null");
        return false;
    }
    CffiLex lx = { text, strlen(text), 0 };
    if (!cffi_parse_type(&lx, out_kind, err)) return false;
    cffi_lex_skip_ws(&lx);
    if (lx.pos != lx.len) {
        cffi_set_error(err, lx.pos, "trailing characters after type");
        return false;
    }
    return true;
}

#endif /* CANDO_CFFI_TYPES_H */
