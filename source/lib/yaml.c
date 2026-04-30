/*
 * lib/yaml.c -- YAML encode/decode standard library for Cando.
 *
 * Self-contained YAML 1.2 subset implementation.  See yaml.h for the
 * supported feature set; broadly:
 *   - block mappings, block sequences, flow mappings, flow sequences
 *   - plain / single-quoted / double-quoted scalars
 *   - literal (|) and folded (>) block scalars with default chomping
 *   - core-schema scalar typing (null/bool/int/float/string)
 *   - line comments (`# ...`)
 *   - optional leading document separator `---`
 *
 * Must compile with gcc -std=c11.
 */

#include "yaml.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* =========================================================================
 * Dynamic byte buffer -- shared by the parser (scalar accumulation) and the
 * writer (output accumulation).
 * ======================================================================= */

typedef struct {
    char  *data;
    usize  len;
    usize  cap;
    bool   oom;
} YBuf;

static void ybuf_grow(YBuf *b, usize need)
{
    if (b->oom) return;
    if (b->len + need <= b->cap) return;
    usize nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + need) nc *= 2;
    char *p = (char *)realloc(b->data, nc);
    if (!p) { b->oom = true; return; }
    b->data = p;
    b->cap  = nc;
}

static void ybuf_push(YBuf *b, const char *src, usize len)
{
    ybuf_grow(b, len);
    if (b->oom) return;
    memcpy(b->data + b->len, src, len);
    b->len += len;
}

static void ybuf_push_char(YBuf *b, char c) { ybuf_push(b, &c, 1); }
static void ybuf_push_cstr(YBuf *b, const char *s) { ybuf_push(b, s, strlen(s)); }

static void ybuf_free(YBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = b->cap = 0;
}

/* =========================================================================
 * Pre-tokenised line table.
 *
 * The parser is line-driven: every YAML construct's structure is determined
 * by indentation, so we precompute (indent, content_offset, content_len) for
 * every non-blank, non-comment line up front.  Blank/comment lines are kept
 * in the array but flagged so structural parsers can skip them cheaply.
 * ======================================================================= */

typedef struct {
    u32  start;      /* byte offset of first char of the line in src     */
    u32  end;        /* byte offset just past the last char (excl. \n)   */
    u32  indent;     /* count of leading spaces                          */
    u32  content;    /* offset of first non-space, non-comment char      */
    u32  content_end;/* offset just past last meaningful char            */
    bool blank;      /* line is empty / whitespace-only / comment-only   */
    bool has_tab_indent; /* tab found in indent — error for block ctx    */
} YLine;

typedef struct {
    const char *src;
    u32         len;
    YLine      *lines;
    u32         n_lines;
    u32         pos;        /* current line index                        */
    CandoVM    *vm;
    bool        has_error;
    char        error[256];
} YParser;

/* =========================================================================
 * Forward declarations
 * ======================================================================= */

static CandoValue yp_parse_node(YParser *p, u32 min_indent);
static CandoValue yp_parse_block_map(YParser *p, u32 indent);
static CandoValue yp_parse_block_seq(YParser *p, u32 indent);
static CandoValue yp_parse_flow_value(YParser *p, const char *s, u32 *i, u32 n);
static CandoValue yp_decode_plain_scalar(YParser *p,
                                          const char *s, u32 len);

/* =========================================================================
 * Error reporting
 * ======================================================================= */

static void yp_error_at(YParser *p, u32 line_idx, const char *msg)
{
    if (p->has_error) return;
    /* Report a 1-based line number for human readability.  Callers may pass
     * UINT32_MAX to indicate "unknown line"; we degrade gracefully. */
    if (line_idx < p->n_lines) {
        snprintf(p->error, sizeof(p->error), "%s (at line %u)",
                 msg, (unsigned)(line_idx + 1));
    } else {
        snprintf(p->error, sizeof(p->error), "%s", msg);
    }
    p->has_error = true;
}

static void yp_error(YParser *p, const char *msg)
{
    yp_error_at(p, p->pos, msg);
}

/* =========================================================================
 * Line pre-tokenisation
 *
 * Walks the raw source byte-by-byte.  For each line:
 *   - records [start, end) offsets (end excludes the line terminator)
 *   - counts leading spaces (tabs in indent set has_tab_indent)
 *   - locates the first non-whitespace character; if it is '#' the line is
 *     marked blank
 *   - locates content_end by scanning forward and stripping a trailing
 *     "  # comment" segment when found OUTSIDE any quoted string
 *
 * Trailing-comment stripping respects single-quoted (no escapes) and
 * double-quoted (\"-aware) string literals; that's enough for typical
 * YAML where the comment sentinel is "<space>#" not "#" alone.
 * ======================================================================= */

static u32 yl_find_comment_end(const char *src, u32 start, u32 end)
{
    /* Scan [start, end); return the offset of the comment sentinel
     * (`<sp>#`) when one is found OUTSIDE quoted strings, else `end`. */
    bool in_squote = false, in_dquote = false;
    for (u32 i = start; i < end; i++) {
        char c = src[i];
        if (in_squote) {
            if (c == '\'' && i + 1 < end && src[i + 1] == '\'') {
                i++;             /* '' inside single-quoted string */
            } else if (c == '\'') {
                in_squote = false;
            }
            continue;
        }
        if (in_dquote) {
            if (c == '\\' && i + 1 < end) { i++; continue; }
            if (c == '"') in_dquote = false;
            continue;
        }
        if (c == '\'') { in_squote = true; continue; }
        if (c == '"')  { in_dquote = true; continue; }
        if (c == '#' && (i == start || src[i - 1] == ' ' || src[i - 1] == '\t'))
            return i;
    }
    return end;
}

static void yl_tokenise(YParser *p)
{
    u32 cap = 64, n = 0;
    YLine *lines = (YLine *)malloc(cap * sizeof(YLine));
    if (!lines) { yp_error(p, "out of memory"); return; }

    u32 i = 0;
    while (i <= p->len) {
        u32 start = i;
        while (i < p->len && p->src[i] != '\n') i++;
        u32 end = i;                 /* exclusive of '\n' */
        if (end > start && p->src[end - 1] == '\r') end--;

        if (n == cap) {
            cap *= 2;
            YLine *np = (YLine *)realloc(lines, cap * sizeof(YLine));
            if (!np) { free(lines); yp_error(p, "out of memory"); return; }
            lines = np;
        }

        YLine *L = &lines[n++];
        L->start          = start;
        L->end            = end;
        L->has_tab_indent = false;

        /* Leading whitespace -> indent. */
        u32 j = start;
        u32 sp = 0;
        while (j < end) {
            char c = p->src[j];
            if (c == ' ') { sp++; j++; }
            else if (c == '\t') { L->has_tab_indent = true; j++; }
            else break;
        }
        L->indent = sp;

        if (j >= end || p->src[j] == '#') {
            /* Blank or comment-only line. */
            L->blank       = true;
            L->content     = j;
            L->content_end = j;
        } else {
            L->blank       = false;
            L->content     = j;
            u32 cend = yl_find_comment_end(p->src, j, end);
            /* Trim trailing whitespace before the comment / line end. */
            while (cend > j && (p->src[cend - 1] == ' ' ||
                                p->src[cend - 1] == '\t'))
                cend--;
            L->content_end = cend;
        }

        if (i < p->len && p->src[i] == '\n') i++;
        else if (i >= p->len) break;
    }

    p->lines   = lines;
    p->n_lines = n;
}

/* =========================================================================
 * cdo bridging
 *
 * Same trick the JSON parser uses: a CandoValue produced from the bridge
 * needs to be converted back into a CdoValue when storing it in a parent
 * container.  Strings get a fresh CdoString*; objects/arrays expose their
 * underlying CdoObject* via the live handle.
 * ======================================================================= */

static CdoValue yp_to_cdo(YParser *p, CandoValue v)
{
    switch ((TypeTag)v.tag) {
        case TYPE_NULL:   return cdo_null();
        case TYPE_BOOL:   return cdo_bool(v.as.boolean);
        case TYPE_NUMBER: return cdo_number(v.as.number);
        case TYPE_STRING: {
            CdoString *s = cdo_string_new(v.as.string->data,
                                           v.as.string->length);
            return cdo_string_value(s);
        }
        case TYPE_OBJECT: {
            CdoObject *obj = cando_bridge_resolve(p->vm, v.as.handle);
            return (obj->kind == OBJ_ARRAY)
                   ? cdo_array_value(obj)
                   : cdo_object_value(obj);
        }
    }
    return cdo_null();
}

/* =========================================================================
 * Scalar typing -- plain (unquoted) scalars in core schema
 * ======================================================================= */

static bool yp_str_eq_ci(const char *s, u32 n, const char *lit)
{
    u32 m = (u32)strlen(lit);
    if (m != n) return false;
    for (u32 i = 0; i < n; i++) {
        char a = s[i];
        char b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* Returns true iff [s, s+n) parses as a YAML 1.2 core-schema integer.
 * Sets *out to the parsed value when true.  Accepts decimal, 0x… hex,
 * and 0o… / 0… octal forms with an optional leading sign. */
static bool yp_try_parse_int(const char *s, u32 n, f64 *out)
{
    if (n == 0) return false;
    u32 i = 0;
    int sign = 1;
    if (s[i] == '+') i++;
    else if (s[i] == '-') { sign = -1; i++; }
    if (i >= n) return false;

    /* Hexadecimal / octal */
    if (s[i] == '0' && i + 1 < n &&
        (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        if (i >= n) return false;
        u64 acc = 0;
        for (; i < n; i++) {
            char c = s[i];
            u32 d;
            if      (c >= '0' && c <= '9') d = (u32)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
            else return false;
            acc = acc * 16 + d;
        }
        *out = (f64)((i64)(sign) * (i64)acc);
        return true;
    }
    if (s[i] == '0' && i + 1 < n &&
        (s[i + 1] == 'o' || s[i + 1] == 'O')) {
        i += 2;
        if (i >= n) return false;
        u64 acc = 0;
        for (; i < n; i++) {
            char c = s[i];
            if (c < '0' || c > '7') return false;
            acc = acc * 8 + (u32)(c - '0');
        }
        *out = (f64)((i64)(sign) * (i64)acc);
        return true;
    }

    /* Plain decimal */
    u32 start = i;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    if (i == start) return false;
    u64 acc = 0;
    for (u32 k = start; k < i; k++) acc = acc * 10 + (u32)(s[k] - '0');
    *out = (f64)((i64)sign * (i64)acc);
    return true;
}

/* Returns true iff [s, s+n) parses as a YAML 1.2 core-schema float. */
static bool yp_try_parse_float(const char *s, u32 n, f64 *out)
{
    if (n == 0) return false;
    /* .inf / -.inf / .nan */
    if (yp_str_eq_ci(s, n, ".inf"))  { *out = INFINITY;  return true; }
    if (yp_str_eq_ci(s, n, "+.inf")) { *out = INFINITY;  return true; }
    if (yp_str_eq_ci(s, n, "-.inf")) { *out = -INFINITY; return true; }
    if (yp_str_eq_ci(s, n, ".nan"))  { *out = NAN;       return true; }

    /* Reject anything not consisting solely of digits / sign / dot / e / E. */
    bool seen_digit = false, seen_dot = false, seen_exp = false;
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { seen_digit = true; continue; }
        if (c == '.') {
            if (seen_dot || seen_exp) return false;
            seen_dot = true; continue;
        }
        if (c == 'e' || c == 'E') {
            if (seen_exp || !seen_digit) return false;
            seen_exp = true; seen_digit = false; continue;
        }
        if (c == '+' || c == '-') {
            if (i == 0) continue;
            if (!seen_exp) return false;
            char prev = s[i - 1];
            if (prev != 'e' && prev != 'E') return false;
            continue;
        }
        return false;
    }
    if (!seen_digit) return false;

    char tmp[64];
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    char *endp;
    f64 v = strtod(tmp, &endp);
    if (endp != tmp + n) return false;
    *out = v;
    return true;
}

/* yp_decode_plain_scalar: convert an unquoted YAML scalar string into a
 * CandoValue using core-schema typing rules.  The caller has already
 * stripped surrounding whitespace and escapes are not applicable. */
static CandoValue yp_decode_plain_scalar(YParser *p,
                                          const char *s, u32 len)
{
    (void)p;
    if (len == 0) return cando_null();

    /* null / ~ */
    if (len == 1 && s[0] == '~') return cando_null();
    if (yp_str_eq_ci(s, len, "null")) return cando_null();
    if (yp_str_eq_ci(s, len, "Null")) return cando_null();
    if (yp_str_eq_ci(s, len, "NULL")) return cando_null();

    /* booleans (YAML 1.1 superset that's commonly expected) */
    if (yp_str_eq_ci(s, len, "true")  || yp_str_eq_ci(s, len, "yes") ||
        yp_str_eq_ci(s, len, "on"))   return cando_bool(true);
    if (yp_str_eq_ci(s, len, "false") || yp_str_eq_ci(s, len, "no")  ||
        yp_str_eq_ci(s, len, "off"))  return cando_bool(false);

    /* numbers */
    f64 num;
    if (yp_try_parse_int(s, len, &num))   return cando_number(num);
    if (yp_try_parse_float(s, len, &num)) return cando_number(num);

    /* fallthrough: string */
    CandoString *str = cando_string_new(s, len);
    return cando_string_value(str);
}

/* =========================================================================
 * Quoted scalar decoders
 *
 * Both helpers expect the cursor *i to point at the opening quote and on
 * success advance past the closing quote.  Single-quoted strings have only
 * one escape (`''` for a literal apostrophe).  Double-quoted strings honour
 * a JSON-compatible escape set plus `\xNN`, `\uNNNN`, `\UNNNNNNNN`.
 * Returns a freshly-allocated CandoString.  On error sets a parser error
 * and returns cando_null().
 * ======================================================================= */

static CandoValue yp_decode_squoted(YParser *p, const char *s, u32 *i, u32 n)
{
    YBuf buf = {0};
    u32 k = *i + 1;
    bool closed = false;
    while (k < n) {
        char c = s[k];
        if (c == '\'') {
            if (k + 1 < n && s[k + 1] == '\'') {
                ybuf_push_char(&buf, '\'');
                k += 2;
                continue;
            }
            k++;
            closed = true;
            break;
        }
        ybuf_push_char(&buf, c);
        k++;
    }
    if (!closed) {
        ybuf_free(&buf);
        yp_error(p, "unterminated single-quoted string");
        return cando_null();
    }
    if (buf.oom) {
        ybuf_free(&buf);
        yp_error(p, "out of memory in single-quoted string");
        return cando_null();
    }
    *i = k;
    CandoString *str = cando_string_new(buf.data ? buf.data : "", (u32)buf.len);
    ybuf_free(&buf);
    return cando_string_value(str);
}

/* Encode codepoint cp as UTF-8 into buf. */
static void yp_emit_utf8(YBuf *buf, u32 cp)
{
    if (cp < 0x80u) {
        ybuf_push_char(buf, (char)cp);
    } else if (cp < 0x800u) {
        ybuf_push_char(buf, (char)(0xC0u | (cp >> 6)));
        ybuf_push_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        ybuf_push_char(buf, (char)(0xE0u | (cp >> 12)));
        ybuf_push_char(buf, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        ybuf_push_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else {
        ybuf_push_char(buf, (char)(0xF0u | (cp >> 18)));
        ybuf_push_char(buf, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
        ybuf_push_char(buf, (char)(0x80u | ((cp >> 6)  & 0x3Fu)));
        ybuf_push_char(buf, (char)(0x80u | (cp          & 0x3Fu)));
    }
}

static bool yp_read_hex(const char *s, u32 i, u32 count, u32 max, u32 *out)
{
    if (i + count > max) return false;
    u32 acc = 0;
    for (u32 k = 0; k < count; k++) {
        char c = s[i + k];
        u32 d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return false;
        acc = (acc << 4) | d;
    }
    *out = acc;
    return true;
}

static CandoValue yp_decode_dquoted(YParser *p, const char *s, u32 *i, u32 n)
{
    YBuf buf = {0};
    u32 k = *i + 1;
    bool closed = false;
    while (k < n) {
        char c = s[k];
        if (c == '"') { k++; closed = true; break; }
        if (c == '\\') {
            if (k + 1 >= n) { yp_error(p, "truncated escape"); break; }
            char esc = s[k + 1];
            switch (esc) {
                case '"':  ybuf_push_char(&buf, '"');  k += 2; break;
                case '\\': ybuf_push_char(&buf, '\\'); k += 2; break;
                case '/':  ybuf_push_char(&buf, '/');  k += 2; break;
                case 'n':  ybuf_push_char(&buf, '\n'); k += 2; break;
                case 'r':  ybuf_push_char(&buf, '\r'); k += 2; break;
                case 't':  ybuf_push_char(&buf, '\t'); k += 2; break;
                case 'b':  ybuf_push_char(&buf, '\b'); k += 2; break;
                case 'f':  ybuf_push_char(&buf, '\f'); k += 2; break;
                case '0':  ybuf_push_char(&buf, '\0'); k += 2; break;
                case 'a':  ybuf_push_char(&buf, '\a'); k += 2; break;
                case 'v':  ybuf_push_char(&buf, '\v'); k += 2; break;
                case 'e':  ybuf_push_char(&buf, 0x1B); k += 2; break;
                case ' ':  ybuf_push_char(&buf, ' ');  k += 2; break;
                case 'x': {
                    u32 cp;
                    if (!yp_read_hex(s, k + 2, 2, n, &cp)) {
                        yp_error(p, "invalid \\x escape"); k = n; break;
                    }
                    yp_emit_utf8(&buf, cp);
                    k += 4;
                    break;
                }
                case 'u': {
                    u32 cp;
                    if (!yp_read_hex(s, k + 2, 4, n, &cp)) {
                        yp_error(p, "invalid \\u escape"); k = n; break;
                    }
                    yp_emit_utf8(&buf, cp);
                    k += 6;
                    break;
                }
                case 'U': {
                    u32 cp;
                    if (!yp_read_hex(s, k + 2, 8, n, &cp)) {
                        yp_error(p, "invalid \\U escape"); k = n; break;
                    }
                    yp_emit_utf8(&buf, cp);
                    k += 10;
                    break;
                }
                default:
                    yp_error(p, "unknown escape sequence");
                    k = n; break;
            }
            if (p->has_error) break;
            continue;
        }
        ybuf_push_char(&buf, c);
        k++;
    }
    if (p->has_error) { ybuf_free(&buf); return cando_null(); }
    if (!closed) {
        ybuf_free(&buf);
        yp_error(p, "unterminated double-quoted string");
        return cando_null();
    }
    if (buf.oom) {
        ybuf_free(&buf);
        yp_error(p, "out of memory in double-quoted string");
        return cando_null();
    }
    *i = k;
    CandoString *str = cando_string_new(buf.data ? buf.data : "", (u32)buf.len);
    ybuf_free(&buf);
    return cando_string_value(str);
}

/* =========================================================================
 * Flow context parsing
 *
 * yp_parse_flow_value reads a single value starting at s[*i] and advances
 * *i past it.  It dispatches to:
 *    - yp_decode_squoted / yp_decode_dquoted   (quoted strings)
 *    - yp_parse_flow_seq / yp_parse_flow_map   (nested flow containers)
 *    - yp_decode_plain_scalar                  (plain scalar)
 *
 * The flow parser is line-local: nested flow containers are required to
 * fit on the same logical line.  This matches what JSON-style YAML usage
 * needs and keeps line-driven block parsing cleanly separated.
 * ======================================================================= */

static void yp_skip_flow_ws(const char *s, u32 *i, u32 n)
{
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t')) (*i)++;
}

static CandoValue yp_parse_flow_seq(YParser *p, const char *s, u32 *i, u32 n)
{
    (*i)++;                              /* consume '[' */
    CandoValue arr_val = cando_bridge_new_array(p->vm);
    CdoObject *arr     = cando_bridge_resolve(p->vm, arr_val.as.handle);

    yp_skip_flow_ws(s, i, n);
    if (*i < n && s[*i] == ']') { (*i)++; return arr_val; }

    while (!p->has_error) {
        yp_skip_flow_ws(s, i, n);
        CandoValue elem = yp_parse_flow_value(p, s, i, n);
        if (p->has_error) { cando_value_release(elem); break; }
        CdoValue cv = yp_to_cdo(p, elem);
        cando_value_release(elem);
        cdo_array_push(arr, cv);
        cdo_value_release(cv);
        yp_skip_flow_ws(s, i, n);
        if (*i >= n) { yp_error(p, "unterminated flow sequence"); break; }
        if (s[*i] == ']') { (*i)++; break; }
        if (s[*i] != ',') { yp_error(p, "expected ',' or ']' in flow sequence"); break; }
        (*i)++;
    }
    return arr_val;
}

static CandoValue yp_parse_flow_map(YParser *p, const char *s, u32 *i, u32 n)
{
    (*i)++;                              /* consume '{' */
    CandoValue obj_val = cando_bridge_new_object(p->vm);
    CdoObject *obj     = cando_bridge_resolve(p->vm, obj_val.as.handle);

    yp_skip_flow_ws(s, i, n);
    if (*i < n && s[*i] == '}') { (*i)++; return obj_val; }

    while (!p->has_error) {
        yp_skip_flow_ws(s, i, n);

        /* Key: quoted string or plain scalar (scan until ':' or ',' or '}'). */
        CandoValue key_val = cando_null();
        if (*i < n && s[*i] == '"') {
            key_val = yp_decode_dquoted(p, s, i, n);
        } else if (*i < n && s[*i] == '\'') {
            key_val = yp_decode_squoted(p, s, i, n);
        } else {
            u32 ks = *i;
            while (*i < n && s[*i] != ':' && s[*i] != ',' && s[*i] != '}'
                   && s[*i] != '\n')
                (*i)++;
            u32 kend = *i;
            while (kend > ks && (s[kend - 1] == ' ' || s[kend - 1] == '\t')) kend--;
            key_val = yp_decode_plain_scalar(p, s + ks, kend - ks);
        }
        if (p->has_error) { cando_value_release(key_val); break; }

        /* Coerce the key to a string. */
        CandoString *kstr = NULL;
        if (cando_is_string(key_val)) {
            kstr = key_val.as.string;
        } else {
            char *tmp = cando_value_tostring(key_val);
            u32 tn = (u32)strlen(tmp);
            kstr = cando_string_new(tmp, tn);
            free(tmp);
            cando_value_release(key_val);
            key_val = cando_string_value(kstr);
        }

        yp_skip_flow_ws(s, i, n);
        CandoValue val_val = cando_null();
        if (*i < n && s[*i] == ':') {
            (*i)++;
            yp_skip_flow_ws(s, i, n);
            if (*i < n && s[*i] != ',' && s[*i] != '}') {
                val_val = yp_parse_flow_value(p, s, i, n);
                if (p->has_error) {
                    cando_value_release(key_val);
                    cando_value_release(val_val);
                    break;
                }
            }
        }

        CdoString *kintern = cdo_string_intern(kstr->data, kstr->length);
        cando_value_release(key_val);
        CdoValue cv = yp_to_cdo(p, val_val);
        cando_value_release(val_val);
        cdo_object_rawset(obj, kintern, cv, FIELD_NONE);
        cdo_value_release(cv);
        cdo_string_release(kintern);

        yp_skip_flow_ws(s, i, n);
        if (*i >= n) { yp_error(p, "unterminated flow mapping"); break; }
        if (s[*i] == '}') { (*i)++; break; }
        if (s[*i] != ',') { yp_error(p, "expected ',' or '}' in flow mapping"); break; }
        (*i)++;
    }
    return obj_val;
}

static CandoValue yp_parse_flow_value(YParser *p, const char *s, u32 *i, u32 n)
{
    yp_skip_flow_ws(s, i, n);
    if (*i >= n) { yp_error(p, "expected flow value"); return cando_null(); }
    char c = s[*i];
    if (c == '[') return yp_parse_flow_seq(p, s, i, n);
    if (c == '{') return yp_parse_flow_map(p, s, i, n);
    if (c == '"') return yp_decode_dquoted(p, s, i, n);
    if (c == '\'') return yp_decode_squoted(p, s, i, n);

    /* Plain scalar: read until ',' / ']' / '}' / EOL. */
    u32 start = *i;
    while (*i < n && s[*i] != ',' && s[*i] != ']' && s[*i] != '}'
           && s[*i] != '\n')
        (*i)++;
    u32 end = *i;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    return yp_decode_plain_scalar(p, s + start, end - start);
}

/* =========================================================================
 * yp_parse_inline_value
 *
 * Decodes a single value rendered on one logical line: a flow container,
 * a quoted scalar, or a plain scalar.  Used for "key: value" right-hand
 * sides and "- value" sequence items where the value sits on the same
 * line as its container marker.
 *
 *  s, len : the line-local content slice (already comment-stripped)
 * Returns a CandoValue.  On error sets a parser error and returns null.
 * ======================================================================= */
static CandoValue yp_parse_inline_value(YParser *p, const char *s, u32 len)
{
    if (len == 0) return cando_null();
    u32 i = 0;
    yp_skip_flow_ws(s, &i, len);
    if (i >= len) return cando_null();

    char c = s[i];
    CandoValue v;
    if (c == '[' || c == '{') {
        v = yp_parse_flow_value(p, s, &i, len);
    } else if (c == '"') {
        v = yp_decode_dquoted(p, s, &i, len);
    } else if (c == '\'') {
        v = yp_decode_squoted(p, s, &i, len);
    } else {
        /* Plain scalar to end of slice. */
        u32 end = len;
        while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
        return yp_decode_plain_scalar(p, s + i, end - i);
    }

    /* Reject trailing junk after a structured/quoted value. */
    yp_skip_flow_ws(s, &i, len);
    if (!p->has_error && i < len) {
        cando_value_release(v);
        yp_error(p, "unexpected text after inline value");
        return cando_null();
    }
    return v;
}

/* =========================================================================
 * Block scalars  (`|` literal, `>` folded)
 *
 * The first line carries only the indicator (and an optional chomping char
 * `-` strip / `+` keep — we accept them for compatibility but always emit
 * trailing-newline-clipped output, which matches the YAML 1.2 default).
 *
 * Subsequent indented lines (indent strictly greater than `parent_indent`)
 * are concatenated:
 *   - literal `|` keeps each newline
 *   - folded `>` joins lines with a single space; a blank line introduces
 *     one literal newline
 *
 * The block ends at the first non-blank line whose indent <= parent_indent.
 * On exit, p->pos points at that terminating line (or n_lines).
 * ======================================================================= */

static CandoValue yp_parse_block_scalar(YParser *p, char indicator,
                                         u32 parent_indent)
{
    p->pos++;                            /* consume the `|`/`>` line */

    /* Determine the block's indent from the first non-blank line. */
    u32 block_indent = 0;
    bool indent_known = false;

    YBuf buf = {0};
    bool prev_blank = false;
    bool first_line = true;

    while (p->pos < p->n_lines) {
        YLine *L = &p->lines[p->pos];

        if (L->blank) {
            /* Blank line inside the block: emit a newline (folded) or pass
             * through (literal handles it via the next non-blank emission). */
            if (!first_line) {
                if (indicator == '>') {
                    /* In folded style a blank line yields a real newline. */
                    ybuf_push_char(&buf, '\n');
                } else {
                    ybuf_push_char(&buf, '\n');
                }
            }
            prev_blank = true;
            p->pos++;
            continue;
        }

        if (!indent_known) {
            block_indent = L->indent;
            if (block_indent <= parent_indent) {
                /* Block contains nothing -- stop right away. */
                break;
            }
            indent_known = true;
        }

        if (L->indent < block_indent) break;     /* end of block scalar */

        /* Compute the meaningful slice: indent past block_indent + content
         * that we recorded earlier (already trimmed for trailing comment). */
        u32 line_start = L->start + block_indent;
        if (line_start > L->end) line_start = L->end;
        u32 line_end   = L->end;                 /* keep raw line contents */

        if (!first_line && !prev_blank) {
            ybuf_push_char(&buf, indicator == '>' ? ' ' : '\n');
        } else if (first_line) {
            /* leading content -- nothing to add */
        }

        ybuf_push(&buf, p->src + line_start,
                  (usize)(line_end - line_start));
        first_line = false;
        prev_blank = false;
        p->pos++;
    }

    /* Default chomping: ensure exactly one trailing '\n' (or zero if the
     * block produced an empty string). */
    while (buf.len > 0 && (buf.data[buf.len - 1] == '\n' ||
                           buf.data[buf.len - 1] == ' '))
        buf.len--;
    if (buf.len > 0) ybuf_push_char(&buf, '\n');

    if (buf.oom) {
        ybuf_free(&buf);
        yp_error(p, "out of memory in block scalar");
        return cando_null();
    }
    CandoString *str = cando_string_new(buf.data ? buf.data : "", (u32)buf.len);
    ybuf_free(&buf);
    return cando_string_value(str);
}

/* =========================================================================
 * yp_skip_blanks -- advance p->pos past blank/comment-only lines.
 * ======================================================================= */
static void yp_skip_blanks(YParser *p)
{
    while (p->pos < p->n_lines && p->lines[p->pos].blank) p->pos++;
}

/* =========================================================================
 * yp_line_has_block_map_key
 *
 * Heuristic: returns true iff the given content slice starts with a key
 * (plain or quoted) followed by `:` and either end-of-content or one or
 * more spaces.  Used to decide whether a line begins a block mapping.
 * ======================================================================= */
static bool yp_line_has_block_map_key(const char *s, u32 len)
{
    if (len == 0) return false;
    u32 i = 0;
    char first = s[0];
    if (first == '"' || first == '\'') {
        /* Skip past the matching quote. */
        char q = first;
        i = 1;
        while (i < len && s[i] != q) {
            if (q == '"' && s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (q == '\'' && s[i] == '\'' && i + 1 < len && s[i + 1] == '\'') {
                i += 2; continue;
            }
            i++;
        }
        if (i >= len) return false;
        i++;                              /* consume closing quote */
    } else if (first == '[' || first == '{' || first == '|' || first == '>'
               || first == '-') {
        return false;
    } else {
        /* Plain key: scan until ':' that is followed by a space or EOL.
         * Stop at flow indicators since they cannot appear in plain keys. */
        while (i < len && s[i] != ':' && s[i] != '\n'
               && s[i] != '[' && s[i] != ']'
               && s[i] != '{' && s[i] != '}'
               && s[i] != ',') {
            i++;
        }
    }
    /* Skip trailing space before ':'. */
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= len || s[i] != ':') return false;
    /* `:` must be followed by space, EOL or be the very last character. */
    if (i + 1 >= len) return true;
    char after = s[i + 1];
    return after == ' ' || after == '\t';
}

/* =========================================================================
 * yp_split_map_key  --  find the position of the ':' separator in a block
 * mapping line, returning the byte offset of ':' (which is also the end of
 * the key, possibly with trailing whitespace to be trimmed by the caller).
 * Assumes yp_line_has_block_map_key has already returned true.
 * ======================================================================= */
static u32 yp_split_map_key(const char *s, u32 len)
{
    u32 i = 0;
    char first = s[0];
    if (first == '"' || first == '\'') {
        char q = first;
        i = 1;
        while (i < len && s[i] != q) {
            if (q == '"' && s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (q == '\'' && s[i] == '\'' && i + 1 < len && s[i + 1] == '\'') {
                i += 2; continue;
            }
            i++;
        }
        if (i < len) i++;        /* consume closing quote */
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        return i;                /* points at ':' */
    }
    while (i < len && s[i] != ':') i++;
    return i;
}

/* =========================================================================
 * Block sequence
 *
 * Each item begins with `- ` at column == indent.  After consuming the
 * `- ` marker the rest of the line is interpreted as either:
 *   1) empty -> nested node on subsequent lines at indent > current
 *   2) inline scalar / flow value -> single-line value
 *   3) inline `key: value` mapping -> nested block map starting at the
 *      column of the key, with subsequent keys aligned to that column
 * ======================================================================= */

static CandoValue yp_parse_block_seq(YParser *p, u32 indent)
{
    CandoValue arr_val = cando_bridge_new_array(p->vm);
    CdoObject *arr     = cando_bridge_resolve(p->vm, arr_val.as.handle);

    while (!p->has_error) {
        yp_skip_blanks(p);
        if (p->pos >= p->n_lines) break;
        YLine *L = &p->lines[p->pos];
        if (L->indent != indent) break;

        u32 cs = L->content;
        u32 ce = L->content_end;
        if (cs >= ce || p->src[cs] != '-') break;
        if (cs + 1 < ce && p->src[cs + 1] != ' ' && p->src[cs + 1] != '\t')
            break;                       /* `-...` plain scalar, not seq */

        /* Position past "- " (or "-\n"). */
        u32 after = cs + 1;
        while (after < ce && (p->src[after] == ' ' || p->src[after] == '\t'))
            after++;

        CandoValue item;
        if (after >= ce) {
            /* Marker only: child node on following lines, indent > indent. */
            p->pos++;
            item = yp_parse_node(p, indent + 1);
        } else {
            /* The column where item content begins, relative to line start. */
            u32 item_col = after - L->start;

            /* Could be inline mapping: `- key: value` or `- key:`. */
            const char *line_slice = p->src + after;
            u32 slice_len = ce - after;
            if (yp_line_has_block_map_key(line_slice, slice_len)) {
                /* Treat the item line as the first entry of a virtual
                 * mapping at indent = item_col.  We need to roll back the
                 * tokenised line so the map parser sees the slice without
                 * the `- ` prefix.  We do so by editing this line's view
                 * to start at `after`. */
                YLine saved = *L;
                L->content = after;
                L->indent  = item_col;
                item = yp_parse_block_map(p, item_col);
                /* Restore (the next iteration won't look at L anyway, but
                 * keep the state consistent for diagnostics). */
                p->lines[p->pos > 0 ? p->pos - 1 : 0] = p->lines[
                    p->pos > 0 ? p->pos - 1 : 0];
                (void)saved;
            } else if (line_slice[0] == '-' &&
                       (slice_len == 1 ||
                        line_slice[1] == ' ' || line_slice[1] == '\t')) {
                /* Nested sequence on the same line: rewrite indent. */
                YLine saved = *L;
                L->content = after;
                L->indent  = item_col;
                item = yp_parse_block_seq(p, item_col);
                (void)saved;
            } else {
                /* Plain inline value. */
                item = yp_parse_inline_value(p, line_slice, slice_len);
                p->pos++;
            }
        }

        if (p->has_error) { cando_value_release(item); break; }
        CdoValue cv = yp_to_cdo(p, item);
        cando_value_release(item);
        cdo_array_push(arr, cv);
        cdo_value_release(cv);
    }

    return arr_val;
}

/* =========================================================================
 * Block mapping
 *
 * Each key/value entry occupies one or more lines starting at indent.  The
 * value may be:
 *   - inline on the same line after `: ` (scalar / flow / quoted)
 *   - omitted (key followed by `:` with nothing after) -> nested node on
 *     following lines at indent > current, or null if none
 *   - a block scalar begun on this line with `|` / `>`
 * ======================================================================= */

static CandoValue yp_parse_block_map(YParser *p, u32 indent)
{
    CandoValue obj_val = cando_bridge_new_object(p->vm);
    CdoObject *obj     = cando_bridge_resolve(p->vm, obj_val.as.handle);

    while (!p->has_error) {
        yp_skip_blanks(p);
        if (p->pos >= p->n_lines) break;
        YLine *L = &p->lines[p->pos];
        if (L->indent != indent) break;

        const char *cs = p->src + L->content;
        u32 clen = L->content_end - L->content;

        if (!yp_line_has_block_map_key(cs, clen)) break;

        /* Decode the key. */
        u32 colon_off = yp_split_map_key(cs, clen);
        u32 key_end = colon_off;
        while (key_end > 0 && (cs[key_end - 1] == ' ' ||
                               cs[key_end - 1] == '\t'))
            key_end--;

        CandoValue key_val;
        if (cs[0] == '"') {
            u32 i = 0;
            key_val = yp_decode_dquoted(p, cs, &i, key_end);
        } else if (cs[0] == '\'') {
            u32 i = 0;
            key_val = yp_decode_squoted(p, cs, &i, key_end);
        } else {
            key_val = yp_decode_plain_scalar(p, cs, key_end);
        }
        if (p->has_error) { cando_value_release(key_val); break; }

        /* Coerce key to a string if it parsed to bool/number/null. */
        CandoString *kstr;
        if (cando_is_string(key_val)) {
            kstr = key_val.as.string;
        } else {
            char *tmp = cando_value_tostring(key_val);
            u32 tn = (u32)strlen(tmp);
            kstr = cando_string_new(tmp, tn);
            free(tmp);
            cando_value_release(key_val);
            key_val = cando_string_value(kstr);
        }
        CdoString *kintern = cdo_string_intern(kstr->data, kstr->length);

        /* Locate value text after ':'. */
        u32 vs = colon_off + 1;
        while (vs < clen && (cs[vs] == ' ' || cs[vs] == '\t')) vs++;

        CandoValue val_val = cando_null();
        if (vs >= clen) {
            /* Empty right-hand side: child node on following line(s). */
            p->pos++;
            val_val = yp_parse_node(p, indent + 1);
        } else if (cs[vs] == '|' || cs[vs] == '>') {
            char ind = cs[vs];
            /* Block scalar: parse_block_scalar consumes p->pos at this line. */
            val_val = yp_parse_block_scalar(p, ind, indent);
        } else {
            val_val = yp_parse_inline_value(p, cs + vs, clen - vs);
            p->pos++;
        }
        if (p->has_error) {
            cando_value_release(key_val);
            cando_value_release(val_val);
            cdo_string_release(kintern);
            break;
        }

        CdoValue cv = yp_to_cdo(p, val_val);
        cando_value_release(val_val);
        cando_value_release(key_val);
        cdo_object_rawset(obj, kintern, cv, FIELD_NONE);
        cdo_value_release(cv);
        cdo_string_release(kintern);
    }

    return obj_val;
}

/* =========================================================================
 * yp_parse_node -- consume a node whose first line has indent >= min_indent.
 *
 * Returns null if no such line exists (the caller's container is empty).
 * Dispatches to the block sequence / mapping / scalar parser based on the
 * first non-blank line's content.
 * ======================================================================= */

static CandoValue yp_parse_node(YParser *p, u32 min_indent)
{
    yp_skip_blanks(p);
    if (p->pos >= p->n_lines) return cando_null();
    YLine *L = &p->lines[p->pos];
    if (L->indent < min_indent) return cando_null();

    const char *cs = p->src + L->content;
    u32 clen = L->content_end - L->content;
    u32 indent = L->indent;

    if (clen == 0) {                    /* defensive: blank-but-not-flagged */
        p->pos++;
        return cando_null();
    }

    char first = cs[0];

    /* Block sequence:  - ...   or just `-` */
    if (first == '-' && (clen == 1 || cs[1] == ' ' || cs[1] == '\t')) {
        return yp_parse_block_seq(p, indent);
    }

    /* Flow container on its own line. */
    if (first == '[' || first == '{') {
        u32 i = 0;
        CandoValue v = yp_parse_flow_value(p, cs, &i, clen);
        p->pos++;
        return v;
    }

    /* Block scalar:  | / > */
    if (first == '|' || first == '>') {
        return yp_parse_block_scalar(p, first, indent == 0 ? 0 : indent - 1);
    }

    /* Block mapping detection. */
    if (yp_line_has_block_map_key(cs, clen)) {
        return yp_parse_block_map(p, indent);
    }

    /* Otherwise: a single-line scalar that lives on this line only. */
    CandoValue v = yp_parse_inline_value(p, cs, clen);
    p->pos++;
    return v;
}

/* =========================================================================
 * Public parse entry point
 * ======================================================================= */

bool cando_lib_yaml_parse_buffer(CandoVM *vm,
                                  const char *src, usize len,
                                  const char *where,
                                  CandoValue *out)
{
    YParser p;
    p.src       = src;
    p.len       = (u32)len;
    p.lines     = NULL;
    p.n_lines   = 0;
    p.pos       = 0;
    p.vm        = vm;
    p.has_error = false;
    p.error[0]  = '\0';

    yl_tokenise(&p);

    /* Optional leading "---" document separator. */
    yp_skip_blanks(&p);
    if (!p.has_error && p.pos < p.n_lines) {
        YLine *L = &p.lines[p.pos];
        u32 cl = L->content_end - L->content;
        if (cl >= 3 && memcmp(p.src + L->content, "---", 3) == 0
                    && (cl == 3 || p.src[L->content + 3] == ' '
                                || p.src[L->content + 3] == '\t')) {
            p.pos++;
        }
    }

    /* Tab-indented block scalars are reasonable but block-structure tabs
     * are not.  Scan once for the latter. */
    for (u32 i = 0; !p.has_error && i < p.n_lines; i++) {
        if (p.lines[i].has_tab_indent && !p.lines[i].blank) {
            yp_error_at(&p, i, "tab character used for indentation");
            break;
        }
    }

    CandoValue result = cando_null();
    if (!p.has_error) result = yp_parse_node(&p, 0);

    /* Anything left over (other than blanks / a "..." terminator) is junk. */
    if (!p.has_error) {
        yp_skip_blanks(&p);
        if (p.pos < p.n_lines) {
            YLine *L = &p.lines[p.pos];
            u32 cl = L->content_end - L->content;
            bool is_terminator = (cl == 3 &&
                memcmp(p.src + L->content, "...", 3) == 0);
            if (!is_terminator) {
                yp_error_at(&p, p.pos, "unexpected trailing content");
            }
        }
    }

    free(p.lines);

    if (p.has_error) {
        cando_value_release(result);
        if (out) *out = cando_null();
        cando_vm_error(vm, "%s: %s", where ? where : "yaml.parse", p.error);
        return false;
    }

    if (out) *out = result;
    else     cando_value_release(result);
    return true;
}

static int yaml_parse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "yaml.parse: expected a string argument");
        return -1;
    }
    CandoValue result = cando_null();
    if (!cando_lib_yaml_parse_buffer(vm,
                                     args[0].as.string->data,
                                     (usize)args[0].as.string->length,
                                     "yaml.parse",
                                     &result)) {
        return -1;
    }
    cando_vm_push(vm, result);
    return 1;
}

/* =========================================================================
 * YAML writer
 *
 * Produces block-style YAML for objects and arrays; scalars print as a
 * single line with double-quoting only when content would otherwise be
 * misinterpreted (would round-trip back to a different type, contains
 * special leading characters, or contains control chars).
 * ======================================================================= */

typedef struct {
    YBuf      buf;
    int       indent;        /* spaces per nesting level (>= 1)             */
    CandoVM  *vm;
} YWriter;

static void yw_write_cdo_value(YWriter *w, CdoValue val, int depth, bool inline_first);
static void yw_write_string_scalar(YWriter *w, const char *s, u32 len);

static void yw_write_indent(YWriter *w, int depth)
{
    int n = depth * w->indent;
    for (int i = 0; i < n; i++) ybuf_push_char(&w->buf, ' ');
}

static bool yw_string_needs_quotes(const char *s, u32 len)
{
    if (len == 0) return true;            /* empty string -> "" */
    /* Reserved scalars that would parse back to non-string types. */
    static const char *reserved[] = {
        "null", "Null", "NULL", "~",
        "true", "True", "TRUE", "false", "False", "FALSE",
        "yes",  "Yes",  "YES",  "no",   "No",  "NO",
        "on",   "On",   "ON",   "off",  "Off", "OFF",
        ".inf", ".Inf", ".INF", ".nan", ".NaN", ".NAN",
        NULL,
    };
    for (u32 i = 0; reserved[i]; i++) {
        if (yp_str_eq_ci(s, len, reserved[i])) return true;
    }
    /* Numeric look-alikes. */
    f64 n;
    if (yp_try_parse_int(s, len, &n)) return true;
    if (yp_try_parse_float(s, len, &n)) return true;

    /* Special leading characters or control chars / structural indicators. */
    char first = s[0];
    if (first == ' ' || first == '\t' || first == '#' ||
        first == '&' || first == '*' || first == '!' ||
        first == '|' || first == '>' || first == '\'' ||
        first == '"' || first == '%' || first == '@' ||
        first == '`' || first == '?' || first == ':' ||
        first == '-' || first == '[' || first == ']' ||
        first == '{' || first == '}' || first == ',')
        return true;
    if (s[len - 1] == ' ' || s[len - 1] == '\t') return true;

    for (u32 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return true;
        /* `: ` or ` #` cannot appear in a plain scalar; they would terminate it. */
        if (c == ':' && (i + 1 == len ||
                         s[i + 1] == ' ' || s[i + 1] == '\t')) return true;
        if (c == '#' && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t'))
            return true;
    }
    return false;
}

static void yw_write_string_scalar(YWriter *w, const char *s, u32 len)
{
    if (!yw_string_needs_quotes(s, len)) {
        ybuf_push(&w->buf, s, len);
        return;
    }
    /* Emit double-quoted with JSON-compatible escapes. */
    ybuf_push_char(&w->buf, '"');
    for (u32 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  ybuf_push(&w->buf, "\\\"", 2); break;
            case '\\': ybuf_push(&w->buf, "\\\\", 2); break;
            case '\n': ybuf_push(&w->buf, "\\n",  2); break;
            case '\r': ybuf_push(&w->buf, "\\r",  2); break;
            case '\t': ybuf_push(&w->buf, "\\t",  2); break;
            case '\b': ybuf_push(&w->buf, "\\b",  2); break;
            case '\f': ybuf_push(&w->buf, "\\f",  2); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\x%02x", c);
                    ybuf_push(&w->buf, esc, 4);
                } else {
                    ybuf_push_char(&w->buf, (char)c);
                }
                break;
        }
    }
    ybuf_push_char(&w->buf, '"');
}

static void yw_write_number(YWriter *w, f64 n)
{
    if (isnan(n)) { ybuf_push(&w->buf, ".nan", 4); return; }
    if (isinf(n)) {
        ybuf_push_cstr(&w->buf, n < 0 ? "-.inf" : ".inf");
        return;
    }
    char tmp[64];
    if (n == floor(n) && fabs(n) < 1e15) {
        snprintf(tmp, sizeof(tmp), "%.0f", n);
    } else {
        snprintf(tmp, sizeof(tmp), "%.17g", n);
    }
    ybuf_push_cstr(&w->buf, tmp);
}

static bool yw_first_field_cb(CdoString *k, CdoValue *v, u8 f, void *ud)
{
    (void)k; (void)v; (void)f;
    *(bool *)ud = false;
    return false;                            /* stop after first field */
}

static bool yw_obj_is_empty(CdoObject *obj)
{
    bool empty = true;
    cdo_object_foreach(obj, yw_first_field_cb, &empty);
    return empty;
}

/* =========================================================================
 * Recursive writers for arrays and objects
 *
 * `inline_first`:
 *   - true  -> the first character of this value is rendered immediately
 *              after whatever the caller already wrote (e.g. after `key:` or
 *              after `- ` in a sequence parent).  For containers we still
 *              break to a new line because YAML cannot start a block-style
 *              container immediately after the parent indicator.
 *   - false -> caller wants this value preceded by indentation.
 * ======================================================================= */

typedef struct { YWriter *w; int depth; bool first; } YObjCtx;

static bool yw_field_cb(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)flags;
    YObjCtx *ctx = (YObjCtx *)ud;
    if (ctx->w->buf.oom) return false;

    if (!ctx->first) ybuf_push_char(&ctx->w->buf, '\n');
    ctx->first = false;

    yw_write_indent(ctx->w, ctx->depth);
    yw_write_string_scalar(ctx->w, key->data, key->length);
    ybuf_push_char(&ctx->w->buf, ':');

    /* For non-empty containers, emit on the next line; for scalars, inline. */
    bool needs_newline =
        (val->tag == CDO_ARRAY) ||
        (val->tag == CDO_OBJECT && val->as.object->kind != OBJ_ARRAY &&
         !yw_obj_is_empty(val->as.object));
    if (val->tag == CDO_ARRAY && cdo_array_len(val->as.object) == 0)
        needs_newline = false;        /* empty array -> ` []` inline */
    if (val->tag == CDO_OBJECT && val->as.object->kind == OBJ_ARRAY &&
        cdo_array_len(val->as.object) == 0)
        needs_newline = false;

    if (needs_newline) {
        ybuf_push_char(&ctx->w->buf, '\n');
        yw_write_cdo_value(ctx->w, *val, ctx->depth + 1, false);
    } else {
        ybuf_push_char(&ctx->w->buf, ' ');
        yw_write_cdo_value(ctx->w, *val, ctx->depth + 1, true);
    }
    return !ctx->w->buf.oom;
}

static void yw_write_object(YWriter *w, CdoObject *obj, int depth)
{
    if (yw_obj_is_empty(obj)) {
        ybuf_push(&w->buf, "{}", 2);
        return;
    }
    YObjCtx ctx = { .w = w, .depth = depth, .first = true };
    cdo_object_foreach(obj, yw_field_cb, &ctx);
}

static void yw_write_array(YWriter *w, CdoObject *arr, int depth)
{
    u32 len = cdo_array_len(arr);
    if (len == 0) {
        ybuf_push(&w->buf, "[]", 2);
        return;
    }
    for (u32 i = 0; i < len; i++) {
        if (i > 0) ybuf_push_char(&w->buf, '\n');
        yw_write_indent(w, depth);
        ybuf_push_char(&w->buf, '-');

        CdoValue elem = cdo_null();
        cdo_array_rawget_idx(arr, i, &elem);

        bool nested_container = (elem.tag == CDO_ARRAY) ||
            (elem.tag == CDO_OBJECT && elem.as.object->kind != OBJ_ARRAY &&
             !yw_obj_is_empty(elem.as.object));
        if (elem.tag == CDO_ARRAY && cdo_array_len(elem.as.object) == 0)
            nested_container = false;
        if (elem.tag == CDO_OBJECT && elem.as.object->kind == OBJ_ARRAY &&
            cdo_array_len(elem.as.object) == 0)
            nested_container = false;

        if (nested_container) {
            ybuf_push_char(&w->buf, '\n');
            yw_write_cdo_value(w, elem, depth + 1, false);
        } else {
            ybuf_push_char(&w->buf, ' ');
            yw_write_cdo_value(w, elem, depth + 1, true);
        }
        if (w->buf.oom) break;
    }
}

static void yw_write_cdo_value(YWriter *w, CdoValue val, int depth,
                                bool inline_first)
{
    if (w->buf.oom) return;
    switch ((CdoTypeTag)val.tag) {
        case CDO_NULL:
            if (!inline_first) yw_write_indent(w, depth);
            ybuf_push(&w->buf, "null", 4);
            break;
        case CDO_BOOL:
            if (!inline_first) yw_write_indent(w, depth);
            ybuf_push_cstr(&w->buf, val.as.boolean ? "true" : "false");
            break;
        case CDO_NUMBER:
            if (!inline_first) yw_write_indent(w, depth);
            yw_write_number(w, val.as.number);
            break;
        case CDO_STRING:
            if (!inline_first) yw_write_indent(w, depth);
            yw_write_string_scalar(w, val.as.string->data,
                                   val.as.string->length);
            break;
        case CDO_ARRAY:
            yw_write_array(w, val.as.object, depth);
            break;
        case CDO_OBJECT:
            if (val.as.object->kind == OBJ_ARRAY)
                yw_write_array(w, val.as.object, depth);
            else
                yw_write_object(w, val.as.object, depth);
            break;
        case CDO_FUNCTION:
        case CDO_NATIVE:
            /* Not representable in YAML; emit `null` to keep doc valid. */
            if (!inline_first) yw_write_indent(w, depth);
            ybuf_push(&w->buf, "null", 4);
            break;
    }
}

/* =========================================================================
 * yaml.stringify(val, indent?) -> string
 * ======================================================================= */

static int yaml_stringify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        libutil_push_cstr(vm, "null\n");
        return 1;
    }

    int indent = 2;
    if (argc >= 2 && cando_is_number(args[1])) {
        indent = (int)args[1].as.number;
        if (indent < 1)  indent = 1;
        if (indent > 16) indent = 16;
    }

    YWriter w;
    w.buf.data = NULL; w.buf.len = 0; w.buf.cap = 0; w.buf.oom = false;
    w.indent   = indent;
    w.vm       = vm;

    CandoValue val = args[0];
    switch ((TypeTag)val.tag) {
        case TYPE_NULL:
            ybuf_push(&w.buf, "null", 4); break;
        case TYPE_BOOL:
            ybuf_push_cstr(&w.buf, val.as.boolean ? "true" : "false"); break;
        case TYPE_NUMBER:
            yw_write_number(&w, val.as.number); break;
        case TYPE_STRING:
            yw_write_string_scalar(&w, val.as.string->data,
                                   val.as.string->length);
            break;
        case TYPE_OBJECT: {
            CdoObject *obj = cando_bridge_resolve(vm, val.as.handle);
            if (obj->kind == OBJ_ARRAY)
                yw_write_array(&w, obj, 0);
            else
                yw_write_object(&w, obj, 0);
            break;
        }
    }

    /* Always end with exactly one newline. */
    if (w.buf.len == 0 || w.buf.data[w.buf.len - 1] != '\n')
        ybuf_push_char(&w.buf, '\n');

    libutil_push_str(vm, w.buf.data ? w.buf.data : "", (u32)w.buf.len);
    ybuf_free(&w.buf);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_yaml_register(CandoVM *vm)
{
    CandoValue yaml_val = cando_bridge_new_object(vm);
    CdoObject *yaml_obj = cando_bridge_resolve(vm, yaml_val.as.handle);

    libutil_set_method(vm, yaml_obj, "parse",     yaml_parse);
    libutil_set_method(vm, yaml_obj, "stringify", yaml_stringify);

    cando_vm_set_global(vm, "yaml", yaml_val, true);
}
