/*
 * natives.c -- Native function implementations for the Cando interpreter.
 *
 * Must compile with gcc -std=c11.
 */

#include "natives.h"
#include "vm/bridge.h"
#include "object/array.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration: dispatch a resolved callable CdoValue meta-method.
 * Defined in vm/vm.c; declared here without pulling object/value.h into the
 * public vm.h surface. */
bool cando_vm_dispatch_callable(CandoVM *vm, const struct CdoValue *raw,
                                 CandoValue *args, u32 argc);

static bool meta_is_callable(CdoValue v)
{
    return v.tag == CDO_FUNCTION || v.tag == CDO_NATIVE ||
           v.tag == CDO_NUMBER;
}

/* =========================================================================
 * Dispatch and name tables
 * Sized to CANDO_NATIVE_MAX; unused trailing slots are zero-initialised
 * (NULL), so callers can iterate until they hit a NULL entry.
 * ===================================================================== */
CandoNativeFn cando_native_table[CANDO_NATIVE_MAX] = {
    cando_native_print,     /* index 0, sentinel -1.0 */
    cando_native_type,      /* index 1, sentinel -2.0 */
    cando_native_tostring,  /* index 2, sentinel -3.0 */
    cando_native_inspect,   /* index 3, sentinel -4.0 */
};

const char *cando_native_names[CANDO_NATIVE_MAX] = {
    "print",
    "type",
    "toString",
    "inspect",
};

/* =========================================================================
 * print(...) -- variadic, space-separated, newline-terminated.
 * Pushes 0 return values onto the stack; returns 0.
 * ===================================================================== */
int cando_native_print(CandoVM *vm, int argc, CandoValue *args)
{
    bool first = true;
    for (int i = 0; i < argc; i++) {
        /* Arrays (including range results) are expanded element-by-element. */
        if (cando_is_object(args[i])) {
            CdoObject *obj = cando_bridge_resolve(vm, args[i].as.handle);
            if (obj && obj->kind == OBJ_ARRAY) {
                u32 len = cdo_array_len(obj);
                for (u32 j = 0; j < len; j++) {
                    if (!first) putchar(' ');
                    first = false;
                    CdoValue cv = cdo_null();
                    cdo_array_rawget_idx(obj, j, &cv);
                    CandoValue v = cando_bridge_to_cando(vm, cv);
                    char *s = cando_value_tostring(v);
                    fputs(s, stdout);
                    free(s);
                }
                continue;
            }
        }
        if (!first) putchar(' ');
        first = false;
        char *s = cando_value_tostring(args[i]);
        fputs(s, stdout);
        free(s);
    }
    putchar('\n');
    fflush(stdout);
    return 0;
}

/* =========================================================================
 * type(val) -- push the type name of val as a CandoString; returns 1.
 * If the value is an object with a __type field, return that field's value.
 * __type may be a string (returned directly) or a callable (invoked as
 * __type(val) -> string).
 * ===================================================================== */
int cando_native_type(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (cando_is_object(args[0]) && g_meta_type) {
        /* Follow the prototype (__index) chain so a class instance whose
         * class declares __type reports that class as its type. */
        CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
        CdoValue type_val;
        if (cdo_object_get(obj, g_meta_type, &type_val)) {
            if (cdo_is_string(type_val)) {
                cando_vm_push(vm, cando_bridge_to_cando(vm, type_val));
                return 1;
            }
            if (meta_is_callable(type_val)) {
                if (cando_vm_dispatch_callable(vm, &type_val, args, 1))
                    return 1;
                if (vm->has_error) return -1;
            }
        }
    }
    /* Default: built-in type tag name. */
    const char *name = cando_value_type_name((TypeTag)args[0].tag);
    u32 len = (u32)strlen(name);
    CandoString *s = cando_string_new(name, len);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

/* =========================================================================
 * toString(val) -- push string representation as a CandoString; returns 1.
 * If the value is an object with a __tostring meta-method, return its value
 * directly when it is a string, or invoke it when it is a callable.
 * ===================================================================== */
int cando_native_tostring(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (cando_is_object(args[0]) && g_meta_tostring) {
        CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
        CdoValue ts_val;
        if (cdo_object_get(obj, g_meta_tostring, &ts_val)) {
            if (cdo_is_string(ts_val)) {
                cando_vm_push(vm, cando_bridge_to_cando(vm, ts_val));
                return 1;
            }
            if (meta_is_callable(ts_val)) {
                if (cando_vm_dispatch_callable(vm, &ts_val, args, 1))
                    return 1;
                if (vm->has_error) return -1;
            }
        }
    }
    /* Default: built-in tostring. */
    char *s = cando_value_tostring(args[0]);
    u32 len = (u32)strlen(s);
    CandoString *cs = cando_string_new(s, len);
    free(s);
    cando_vm_push(vm, cando_string_value(cs));
    return 1;
}

/* =========================================================================
 * inspect(val, depth*) -- debug-printer for arrays / objects.
 *
 *  depth = 0 (default) means unlimited recursion; cycles are always
 *  short-circuited with `<circular>`.  depth = N > 0 truncates nested
 *  arrays / objects beyond that level to `[...]` / `{...}`.
 *
 * Output is pretty-printed: empty containers render as `[]` / `{}`, while
 * non-empty ones break across lines with 2-space indentation per nesting
 * level.  Truncation markers and circular references stay compact.
 * ===================================================================== */

typedef struct {
    char       *buf;
    u32         len;
    u32         cap;
    bool        oom;
    int         max_depth;     /* 0 = unlimited */
    CdoObject **path;          /* visited stack for cycle detection */
    u32         path_len;
    u32         path_cap;
} InspectCtx;

static void inspect_buf_reserve(InspectCtx *ctx, u32 need)
{
    if (ctx->oom) return;
    if (ctx->len + need + 1 <= ctx->cap) return;
    u32 nc = ctx->cap ? ctx->cap * 2 : 64;
    while (nc < ctx->len + need + 1) nc *= 2;
    char *p = (char *)realloc(ctx->buf, nc);
    if (!p) { ctx->oom = true; return; }
    ctx->buf = p;
    ctx->cap = nc;
}

static void inspect_push(InspectCtx *ctx, const char *s, u32 n)
{
    inspect_buf_reserve(ctx, n);
    if (ctx->oom) return;
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
}

static void inspect_push_cstr(InspectCtx *ctx, const char *s)
{
    inspect_push(ctx, s, (u32)strlen(s));
}

static void inspect_push_char(InspectCtx *ctx, char c)
{
    inspect_push(ctx, &c, 1);
}

static bool inspect_path_contains(const InspectCtx *ctx, const CdoObject *o)
{
    for (u32 i = 0; i < ctx->path_len; i++)
        if (ctx->path[i] == o) return true;
    return false;
}

static bool inspect_path_push(InspectCtx *ctx, CdoObject *o)
{
    if (ctx->path_len == ctx->path_cap) {
        u32 nc = ctx->path_cap ? ctx->path_cap * 2 : 8;
        CdoObject **p = (CdoObject **)realloc(ctx->path, nc * sizeof(*p));
        if (!p) { ctx->oom = true; return false; }
        ctx->path     = p;
        ctx->path_cap = nc;
    }
    ctx->path[ctx->path_len++] = o;
    return true;
}

static void inspect_path_pop(InspectCtx *ctx)
{
    if (ctx->path_len > 0) ctx->path_len--;
}

/* True if name is a non-empty C-style identifier: [A-Za-z_][A-Za-z0-9_]*.
 * Used to decide whether an object key needs quoting. */
static bool inspect_key_is_ident(const char *s, u32 len)
{
    if (len == 0) return false;
    unsigned char c0 = (unsigned char)s[0];
    if (!(isalpha(c0) || c0 == '_')) return false;
    for (u32 i = 1; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_')) return false;
    }
    return true;
}

static void inspect_write_quoted(InspectCtx *ctx, const char *data, u32 len)
{
    inspect_push_char(ctx, '"');
    for (u32 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"':  inspect_push(ctx, "\\\"", 2); break;
            case '\\': inspect_push(ctx, "\\\\", 2); break;
            case '\n': inspect_push(ctx, "\\n",  2); break;
            case '\r': inspect_push(ctx, "\\r",  2); break;
            case '\t': inspect_push(ctx, "\\t",  2); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    int  m = snprintf(esc, sizeof(esc), "\\x%02X", c);
                    if (m > 0) inspect_push(ctx, esc, (u32)m);
                } else {
                    inspect_push_char(ctx, (char)c);
                }
        }
    }
    inspect_push_char(ctx, '"');
}

static void inspect_write_number(InspectCtx *ctx, f64 n)
{
    char buf[64];
    int  m;
    if (n == (i64)n)
        m = snprintf(buf, sizeof(buf), "%" PRId64, (i64)n);
    else
        m = snprintf(buf, sizeof(buf), "%.17g", n);
    if (m > 0) inspect_push(ctx, buf, (u32)m);
}

/* Indentation step is fixed at 2 spaces.  `depth` here counts levels of
 * nesting, not the user-facing max_depth limit. */
static void inspect_indent(InspectCtx *ctx, int depth)
{
    for (int i = 0; i < depth; i++)
        inspect_push(ctx, "  ", 2);
}

static void inspect_cdo(InspectCtx *ctx, CdoValue v, int depth);

static void inspect_array(InspectCtx *ctx, CdoObject *arr, int depth)
{
    if (inspect_path_contains(ctx, arr)) {
        inspect_push_cstr(ctx, "<circular>");
        return;
    }
    if (ctx->max_depth > 0 && depth >= ctx->max_depth) {
        inspect_push_cstr(ctx, "[...]");
        return;
    }
    u32 n = cdo_array_len(arr);
    if (n == 0) {
        inspect_push(ctx, "[]", 2);
        return;
    }
    if (!inspect_path_push(ctx, arr)) return;

    inspect_push(ctx, "[\n", 2);
    for (u32 i = 0; i < n; i++) {
        if (i > 0) inspect_push(ctx, ",\n", 2);
        inspect_indent(ctx, depth + 1);
        CdoValue elem = cdo_null();
        cdo_array_rawget_idx(arr, i, &elem);
        inspect_cdo(ctx, elem, depth + 1);
        if (ctx->oom) break;
    }
    inspect_push_char(ctx, '\n');
    inspect_indent(ctx, depth);
    inspect_push_char(ctx, ']');

    inspect_path_pop(ctx);
}

typedef struct { InspectCtx *ctx; int depth; bool first; } InspectIter;

static bool inspect_field_cb(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)flags;
    InspectIter *it  = (InspectIter *)ud;
    InspectCtx  *ctx = it->ctx;
    if (ctx->oom) return false;

    if (!it->first) inspect_push(ctx, ",\n", 2);
    it->first = false;

    inspect_indent(ctx, it->depth + 1);

    if (inspect_key_is_ident(key->data, key->length))
        inspect_push(ctx, key->data, key->length);
    else
        inspect_write_quoted(ctx, key->data, key->length);

    inspect_push(ctx, ": ", 2);
    inspect_cdo(ctx, *val, it->depth + 1);
    return !ctx->oom;
}

/* Probe to decide whether an object is empty without performing the recursive
 * write — needed because cdo_object_foreach has no length accessor that
 * cleanly maps to the iteration order used elsewhere. */
static bool obj_first_cb(CdoString *k, CdoValue *v, u8 f, void *ud)
{
    (void)k; (void)v; (void)f;
    *(bool *)ud = false;
    return false;
}

static void inspect_object(InspectCtx *ctx, CdoObject *obj, int depth)
{
    if (inspect_path_contains(ctx, obj)) {
        inspect_push_cstr(ctx, "<circular>");
        return;
    }
    if (ctx->max_depth > 0 && depth >= ctx->max_depth) {
        inspect_push_cstr(ctx, "{...}");
        return;
    }

    bool empty = true;
    cdo_object_foreach(obj, obj_first_cb, &empty);
    if (empty) {
        inspect_push(ctx, "{}", 2);
        return;
    }

    if (!inspect_path_push(ctx, obj)) return;

    InspectIter it = { .ctx = ctx, .depth = depth, .first = true };
    inspect_push(ctx, "{\n", 2);
    cdo_object_foreach(obj, inspect_field_cb, &it);
    inspect_push_char(ctx, '\n');
    inspect_indent(ctx, depth);
    inspect_push_char(ctx, '}');

    inspect_path_pop(ctx);
}

static void inspect_cdo(InspectCtx *ctx, CdoValue v, int depth)
{
    if (ctx->oom) return;
    switch ((CdoTypeTag)v.tag) {
        case CDO_NULL:
            inspect_push(ctx, "null", 4); break;
        case CDO_BOOL:
            inspect_push_cstr(ctx, v.as.boolean ? "true" : "false"); break;
        case CDO_NUMBER:
            inspect_write_number(ctx, v.as.number); break;
        case CDO_STRING:
            inspect_write_quoted(ctx, v.as.string->data, v.as.string->length);
            break;
        case CDO_ARRAY:
            inspect_array(ctx, v.as.object, depth); break;
        case CDO_OBJECT:
            /* The bridge layer may yield CDO_OBJECT for an array-kind object;
             * dispatch by kind to be safe. */
            if (v.as.object->kind == OBJ_ARRAY)
                inspect_array(ctx, v.as.object, depth);
            else if (v.as.object->kind == OBJ_THREAD)
                inspect_push_cstr(ctx, "<thread>");
            else
                inspect_object(ctx, v.as.object, depth);
            break;
        case CDO_FUNCTION:
            inspect_push_cstr(ctx, "<function>"); break;
        case CDO_NATIVE:
            inspect_push_cstr(ctx, "<native>"); break;
    }
}

int cando_native_inspect(CandoVM *vm, int argc, CandoValue *args)
{
    InspectCtx ctx = {0};
    if (argc >= 2 && cando_is_number(args[1])) {
        int d = (int)args[1].as.number;
        ctx.max_depth = (d > 0) ? d : 0;
    }

    if (argc < 1) {
        inspect_push(&ctx, "null", 4);
    } else {
        CandoValue v = args[0];
        switch ((TypeTag)v.tag) {
            case TYPE_NULL:
                inspect_push(&ctx, "null", 4); break;
            case TYPE_BOOL:
                inspect_push_cstr(&ctx, v.as.boolean ? "true" : "false"); break;
            case TYPE_NUMBER:
                inspect_write_number(&ctx, v.as.number); break;
            case TYPE_STRING:
                inspect_write_quoted(&ctx, v.as.string->data,
                                           v.as.string->length);
                break;
            case TYPE_OBJECT: {
                CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
                if (!obj) {
                    inspect_push(&ctx, "null", 4);
                } else if (obj->kind == OBJ_ARRAY) {
                    inspect_array(&ctx, obj, 0);
                } else if (obj->kind == OBJ_FUNCTION) {
                    inspect_push_cstr(&ctx, "<function>");
                } else if (obj->kind == OBJ_NATIVE) {
                    inspect_push_cstr(&ctx, "<native>");
                } else if (obj->kind == OBJ_THREAD) {
                    inspect_push_cstr(&ctx, "<thread>");
                } else {
                    inspect_object(&ctx, obj, 0);
                }
                break;
            }
        }
    }

    if (ctx.oom) {
        free(ctx.buf);
        free(ctx.path);
        cando_vm_error(vm, "inspect: out of memory");
        return -1;
    }

    CandoString *s = cando_string_new(ctx.buf ? ctx.buf : "", ctx.len);
    free(ctx.buf);
    free(ctx.path);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}
