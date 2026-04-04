/*
 * lib/json.c -- JSON encode/decode standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "json.h"
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
 * Dynamic byte buffer -- used by both parser (string accumulation) and
 * writer (output accumulation).
 * ======================================================================= */

typedef struct {
    char  *data;
    usize  len;
    usize  cap;
    bool   oom;
} JBuf;

static void jbuf_grow(JBuf *b, usize need)
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

static void jbuf_push(JBuf *b, const char *src, usize len)
{
    jbuf_grow(b, len);
    if (b->oom) return;
    memcpy(b->data + b->len, src, len);
    b->len += len;
}

static void jbuf_push_char(JBuf *b, char c) { jbuf_push(b, &c, 1); }
static void jbuf_push_cstr(JBuf *b, const char *s) { jbuf_push(b, s, strlen(s)); }

static void jbuf_free(JBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = b->cap = 0;
}

/* =========================================================================
 * JSON parser
 * ======================================================================= */

typedef struct {
    const char *src;
    usize       len;
    usize       pos;
    CandoVM    *vm;
    bool        has_error;
    char        error[256];
} JParser;

/* forward declaration */
static CandoValue jp_parse_value(JParser *p);

static void jp_error(JParser *p, const char *msg)
{
    if (!p->has_error) {
        snprintf(p->error, sizeof(p->error), "%s (at offset %zu)", msg, p->pos);
        p->has_error = true;
    }
}

static void jp_skip_ws(JParser *p)
{
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
        else break;
    }
}

static bool jp_match(JParser *p, const char *lit, usize llen)
{
    if (p->pos + llen > p->len) return false;
    if (memcmp(p->src + p->pos, lit, llen) != 0) return false;
    p->pos += llen;
    return true;
}

/* Decode a single \uXXXX escape (p->pos is just past the 'u').
 * Handles surrogate pairs.  Writes UTF-8 bytes to buf. */
static bool jp_decode_unicode(JParser *p, JBuf *buf)
{
    if (p->pos + 4 > p->len) {
        jp_error(p, "truncated \\u escape"); return false;
    }
    unsigned int cp = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->src[p->pos + i];
        unsigned int nybble;
        if      (c >= '0' && c <= '9') nybble = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') nybble = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nybble = (unsigned int)(c - 'A' + 10);
        else { jp_error(p, "invalid hex digit in \\u escape"); return false; }
        cp = (cp << 4) | nybble;
    }
    p->pos += 4;

    /* High surrogate: look for a following \uXXXX low surrogate */
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (p->pos + 6 <= p->len &&
                p->src[p->pos] == '\\' && p->src[p->pos + 1] == 'u') {
            p->pos += 2;
            unsigned int low = 0;
            for (int i = 0; i < 4; i++) {
                char c = p->src[p->pos + i];
                unsigned int ny;
                if      (c >= '0' && c <= '9') ny = (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f') ny = (unsigned int)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') ny = (unsigned int)(c - 'A' + 10);
                else { jp_error(p, "invalid hex digit in surrogate"); return false; }
                low = (low << 4) | ny;
            }
            p->pos += 4;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
            } else {
                jp_error(p, "invalid surrogate pair"); return false;
            }
        } else {
            jp_error(p, "unpaired high surrogate"); return false;
        }
    }

    /* Encode code point as UTF-8 */
    if (cp < 0x80u) {
        jbuf_push_char(buf, (char)cp);
    } else if (cp < 0x800u) {
        jbuf_push_char(buf, (char)(0xC0u | (cp >> 6)));
        jbuf_push_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        jbuf_push_char(buf, (char)(0xE0u | (cp >> 12)));
        jbuf_push_char(buf, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        jbuf_push_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else {
        jbuf_push_char(buf, (char)(0xF0u | (cp >> 18)));
        jbuf_push_char(buf, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
        jbuf_push_char(buf, (char)(0x80u | ((cp >> 6)  & 0x3Fu)));
        jbuf_push_char(buf, (char)(0x80u | (cp          & 0x3Fu)));
    }
    return true;
}

/* =========================================================================
 * jp_parse_string -- parse a JSON string literal into a CandoValue string.
 * p->pos must be pointing at the opening '"'.
 * ======================================================================= */
static CandoValue jp_parse_string(JParser *p)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        jp_error(p, "expected '\"'");
        return cando_null();
    }
    p->pos++;  /* skip opening '"' */

    JBuf buf = {0};

    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->src[p->pos];

        if (c == '"') { p->pos++; break; }

        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) {
                jp_error(p, "unexpected end of string escape"); break;
            }
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"':  jbuf_push_char(&buf, '"');  break;
                case '\\': jbuf_push_char(&buf, '\\'); break;
                case '/':  jbuf_push_char(&buf, '/');  break;
                case 'n':  jbuf_push_char(&buf, '\n'); break;
                case 'r':  jbuf_push_char(&buf, '\r'); break;
                case 't':  jbuf_push_char(&buf, '\t'); break;
                case 'b':  jbuf_push_char(&buf, '\b'); break;
                case 'f':  jbuf_push_char(&buf, '\f'); break;
                case 'u':
                    if (!jp_decode_unicode(p, &buf)) goto str_done;
                    break;
                default:
                    jp_error(p, "invalid escape sequence");
                    goto str_done;
            }
        } else if (c < 0x20) {
            jp_error(p, "unescaped control character in string");
            goto str_done;
        } else {
            jbuf_push_char(&buf, (char)c);
            p->pos++;
        }
    }

str_done:;
    CandoValue result = cando_null();
    if (!p->has_error) {
        if (buf.oom) {
            jp_error(p, "out of memory building string");
        } else {
            const char *raw = buf.data ? buf.data : "";
            CandoString *s = cando_string_new(raw, (u32)buf.len);
            result = cando_string_value(s);
        }
    }
    jbuf_free(&buf);
    return result;
}

/* =========================================================================
 * jp_parse_number -- parse a JSON number into a CandoValue number.
 * ======================================================================= */
static CandoValue jp_parse_number(JParser *p)
{
    usize start = p->pos;

    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;

    if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
        jp_error(p, "invalid number: expected digit");
        return cando_null();
    }
    if (p->src[p->pos] == '0') {
        p->pos++;
    } else {
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            jp_error(p, "expected digit after decimal point");
            return cando_null();
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            jp_error(p, "expected digit in exponent");
            return cando_null();
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    usize num_len = p->pos - start;
    char  tmp[64];
    if (num_len >= sizeof(tmp)) { jp_error(p, "number literal too long"); return cando_null(); }
    memcpy(tmp, p->src + start, num_len);
    tmp[num_len] = '\0';

    char *endptr;
    f64 n = strtod(tmp, &endptr);
    if (endptr == tmp) { jp_error(p, "strtod failed on number"); return cando_null(); }
    return cando_number(n);
}

/* Helper: convert a parsed CandoValue into a CdoValue suitable for storing
 * in an object or array.  Strings are newly-allocated CdoStrings; objects
 * and arrays expose their raw CdoObject* (the handle keeps them alive).
 * The caller is responsible for calling cdo_value_release() on the result
 * for CDO_STRING (to drop the extra ref after the container copies it). */
static CdoValue jp_to_cdo(JParser *p, CandoValue v)
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
 * jp_parse_array -- parse a JSON array into a Cando array CandoValue.
 * ======================================================================= */
static CandoValue jp_parse_array(JParser *p)
{
    p->pos++;  /* skip '[' */

    CandoValue arr_val = cando_bridge_new_array(p->vm);
    CdoObject *arr     = cando_bridge_resolve(p->vm, arr_val.as.handle);

    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') { p->pos++; return arr_val; }

    while (!p->has_error) {
        jp_skip_ws(p);
        CandoValue elem = jp_parse_value(p);
        if (p->has_error) { cando_value_release(elem); break; }

        CdoValue cv = jp_to_cdo(p, elem);
        cando_value_release(elem);
        cdo_array_push(arr, cv);
        cdo_value_release(cv);

        jp_skip_ws(p);
        if (p->pos >= p->len)          { jp_error(p, "unterminated array"); break; }
        if (p->src[p->pos] == ']')     { p->pos++; break; }
        if (p->src[p->pos] != ',')     { jp_error(p, "expected ',' or ']' in array"); break; }
        p->pos++;  /* skip ',' */
    }

    return arr_val;
}

/* =========================================================================
 * jp_parse_object -- parse a JSON object into a Cando object CandoValue.
 * ======================================================================= */
static CandoValue jp_parse_object(JParser *p)
{
    p->pos++;  /* skip '{' */

    CandoValue obj_val = cando_bridge_new_object(p->vm);
    CdoObject *obj     = cando_bridge_resolve(p->vm, obj_val.as.handle);

    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') { p->pos++; return obj_val; }

    while (!p->has_error) {
        jp_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != '"') {
            jp_error(p, "expected string key in object"); break;
        }

        CandoValue key_cando = jp_parse_string(p);
        if (p->has_error) { cando_value_release(key_cando); break; }

        jp_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            jp_error(p, "expected ':' after object key");
            cando_value_release(key_cando); break;
        }
        p->pos++;  /* skip ':' */
        jp_skip_ws(p);

        CandoValue val_cando = jp_parse_value(p);
        if (p->has_error) {
            cando_value_release(key_cando);
            cando_value_release(val_cando); break;
        }

        /* Intern the key and set the field */
        CdoString *key = cdo_string_intern(key_cando.as.string->data,
                                            key_cando.as.string->length);
        cando_value_release(key_cando);

        CdoValue cv = jp_to_cdo(p, val_cando);
        cando_value_release(val_cando);

        cdo_object_rawset(obj, key, cv, FIELD_NONE);
        cdo_value_release(cv);
        cdo_string_release(key);

        jp_skip_ws(p);
        if (p->pos >= p->len)          { jp_error(p, "unterminated object"); break; }
        if (p->src[p->pos] == '}')     { p->pos++; break; }
        if (p->src[p->pos] != ',')     { jp_error(p, "expected ',' or '}' in object"); break; }
        p->pos++;  /* skip ',' */
    }

    return obj_val;
}

/* =========================================================================
 * jp_parse_value -- dispatch to the appropriate type parser.
 * ======================================================================= */
static CandoValue jp_parse_value(JParser *p)
{
    jp_skip_ws(p);
    if (p->pos >= p->len) { jp_error(p, "unexpected end of input"); return cando_null(); }

    char c = p->src[p->pos];
    if (c == '"')                           return jp_parse_string(p);
    if (c == '[')                           return jp_parse_array(p);
    if (c == '{')                           return jp_parse_object(p);
    if (c == 't') {
        if (jp_match(p, "true",  4))        return cando_bool(true);
        jp_error(p, "invalid token"); return cando_null();
    }
    if (c == 'f') {
        if (jp_match(p, "false", 5))        return cando_bool(false);
        jp_error(p, "invalid token"); return cando_null();
    }
    if (c == 'n') {
        if (jp_match(p, "null",  4))        return cando_null();
        jp_error(p, "invalid token"); return cando_null();
    }
    if (c == '-' || isdigit((unsigned char)c)) return jp_parse_number(p);

    jp_error(p, "unexpected character");
    return cando_null();
}

/* =========================================================================
 * json.parse(str) → value | null
 * ======================================================================= */
static int json_parse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "json.parse: expected a string argument");
        return -1;
    }

    JParser p;
    p.src       = args[0].as.string->data;
    p.len       = (usize)args[0].as.string->length;
    p.pos       = 0;
    p.vm        = vm;
    p.has_error = false;
    p.error[0]  = '\0';

    CandoValue result = jp_parse_value(&p);

    if (p.has_error) {
        cando_value_release(result);
        cando_vm_error(vm, "json.parse: %s", p.error);
        return -1;
    }

    cando_vm_push(vm, result);
    return 1;
}

/* =========================================================================
 * JSON writer
 * ======================================================================= */

typedef struct {
    JBuf     buf;
    int      indent;  /* spaces per level; 0 = compact */
    CandoVM *vm;
} JWriter;

/* forward declaration */
static void jw_write_cdo_value(JWriter *w, CdoValue val, int depth);

static void jw_indent(JWriter *w, int depth)
{
    if (w->indent <= 0) return;
    jbuf_push_char(&w->buf, '\n');
    int spaces = depth * w->indent;
    for (int i = 0; i < spaces; i++) jbuf_push_char(&w->buf, ' ');
}

static void jw_write_string(JWriter *w, const char *data, u32 len)
{
    jbuf_push_char(&w->buf, '"');
    for (u32 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"':  jbuf_push(&w->buf, "\\\"", 2); break;
            case '\\': jbuf_push(&w->buf, "\\\\", 2); break;
            case '\n': jbuf_push(&w->buf, "\\n",  2); break;
            case '\r': jbuf_push(&w->buf, "\\r",  2); break;
            case '\t': jbuf_push(&w->buf, "\\t",  2); break;
            case '\b': jbuf_push(&w->buf, "\\b",  2); break;
            case '\f': jbuf_push(&w->buf, "\\f",  2); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jbuf_push(&w->buf, esc, 6);
                } else {
                    jbuf_push_char(&w->buf, (char)c);
                }
                break;
        }
    }
    jbuf_push_char(&w->buf, '"');
}

static void jw_write_number(JWriter *w, f64 n)
{
    if (isnan(n) || isinf(n)) { jbuf_push(&w->buf, "null", 4); return; }
    char buf[64];
    if (n == floor(n) && fabs(n) < 1e15) {
        snprintf(buf, sizeof(buf), "%.0f", n);
    } else {
        snprintf(buf, sizeof(buf), "%.17g", n);
    }
    jbuf_push_cstr(&w->buf, buf);
}

/* Callback context for cdo_object_foreach */
typedef struct { JWriter *w; int depth; bool first; } ObjCtx;

static bool jw_field_cb(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)flags;
    ObjCtx *ctx = (ObjCtx *)ud;
    if (ctx->w->buf.oom) return false;
    if (!ctx->first) jbuf_push_char(&ctx->w->buf, ',');
    ctx->first = false;
    jw_indent(ctx->w, ctx->depth + 1);
    jw_write_string(ctx->w, key->data, key->length);
    jbuf_push_char(&ctx->w->buf, ':');
    if (ctx->w->indent > 0) jbuf_push_char(&ctx->w->buf, ' ');
    jw_write_cdo_value(ctx->w, *val, ctx->depth + 1);
    return !ctx->w->buf.oom;
}

static void jw_write_object(JWriter *w, CdoObject *obj, int depth)
{
    jbuf_push_char(&w->buf, '{');
    ObjCtx ctx = { .w = w, .depth = depth, .first = true };
    cdo_object_foreach(obj, jw_field_cb, &ctx);
    if (!ctx.first) jw_indent(w, depth);
    jbuf_push_char(&w->buf, '}');
}

static void jw_write_array(JWriter *w, CdoObject *arr, int depth)
{
    jbuf_push_char(&w->buf, '[');
    u32 len = cdo_array_len(arr);
    for (u32 i = 0; i < len; i++) {
        if (i > 0) jbuf_push_char(&w->buf, ',');
        jw_indent(w, depth + 1);
        CdoValue elem;
        if (cdo_array_rawget_idx(arr, i, &elem)) {
            jw_write_cdo_value(w, elem, depth + 1);
        } else {
            jbuf_push(&w->buf, "null", 4);
        }
        if (w->buf.oom) break;
    }
    if (len > 0) jw_indent(w, depth);
    jbuf_push_char(&w->buf, ']');
}

static void jw_write_cdo_value(JWriter *w, CdoValue val, int depth)
{
    if (w->buf.oom) return;
    switch ((CdoTypeTag)val.tag) {
        case CDO_NULL:
            jbuf_push(&w->buf, "null",  4); break;
        case CDO_BOOL:
            jbuf_push_cstr(&w->buf, val.as.boolean ? "true" : "false"); break;
        case CDO_NUMBER:
            jw_write_number(w, val.as.number); break;
        case CDO_STRING:
            jw_write_string(w, val.as.string->data, val.as.string->length); break;
        case CDO_ARRAY:
            jw_write_array(w, val.as.object, depth); break;
        case CDO_OBJECT:
            /* The VM bridge always uses CDO_OBJECT for any TYPE_OBJECT value,
             * even when the underlying CdoObject is an array.  Check kind. */
            if (val.as.object->kind == OBJ_ARRAY)
                jw_write_array(w, val.as.object, depth);
            else
                jw_write_object(w, val.as.object, depth);
            break;
        case CDO_FUNCTION:
        case CDO_NATIVE:
            /* Not JSON-serialisable; emit null to keep output valid */
            jbuf_push(&w->buf, "null", 4); break;
    }
}

/* =========================================================================
 * json.stringify(val, indent?) → string
 * ======================================================================= */
static int json_stringify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        libutil_push_cstr(vm, "null");
        return 1;
    }

    int indent = 0;
    if (argc >= 2 && cando_is_number(args[1])) {
        indent = (int)args[1].as.number;
        if (indent < 0)  indent = 0;
        if (indent > 16) indent = 16;
    }

    JWriter w;
    w.buf.data = NULL; w.buf.len = 0; w.buf.cap = 0; w.buf.oom = false;
    w.indent   = indent;
    w.vm       = vm;

    CandoValue val = args[0];
    switch ((TypeTag)val.tag) {
        case TYPE_NULL:
            jbuf_push(&w.buf, "null", 4); break;
        case TYPE_BOOL:
            jbuf_push_cstr(&w.buf, val.as.boolean ? "true" : "false"); break;
        case TYPE_NUMBER:
            jw_write_number(&w, val.as.number); break;
        case TYPE_STRING:
            jw_write_string(&w, val.as.string->data, val.as.string->length); break;
        case TYPE_OBJECT: {
            CdoObject *obj = cando_bridge_resolve(vm, val.as.handle);
            if (obj->kind == OBJ_ARRAY)
                jw_write_array(&w, obj, 0);
            else
                jw_write_object(&w, obj, 0);
            break;
        }
    }

    if (indent > 0 && w.buf.len > 0) jbuf_push_char(&w.buf, '\n');

    libutil_push_str(vm, w.buf.data ? w.buf.data : "", (u32)w.buf.len);
    jbuf_free(&w.buf);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_json_register(CandoVM *vm)
{
    CandoValue json_val = cando_bridge_new_object(vm);
    CdoObject *json_obj = cando_bridge_resolve(vm, json_val.as.handle);

    libutil_set_method(vm, json_obj, "parse",     json_parse);
    libutil_set_method(vm, json_obj, "stringify", json_stringify);

    cando_vm_set_global(vm, "json", json_val, true);
}
