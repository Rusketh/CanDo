/*
 * lib/csv.c -- CSV encode/decode standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "csv.h"
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

/* =========================================================================
 * Dynamic byte buffer -- shared by both parse (field accumulation) and
 * stringify (output accumulation).
 * ======================================================================= */

typedef struct {
    char  *data;
    usize  len;
    usize  cap;
    bool   oom;
} CBuf;

static void cbuf_grow(CBuf *b, usize need)
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

static void cbuf_push(CBuf *b, const char *src, usize len)
{
    cbuf_grow(b, len);
    if (b->oom) return;
    memcpy(b->data + b->len, src, len);
    b->len += len;
}

static void cbuf_push_char(CBuf *b, char c) { cbuf_push(b, &c, 1); }
static void cbuf_push_cstr(CBuf *b, const char *s) { cbuf_push(b, s, strlen(s)); }

static void cbuf_free(CBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = b->cap = 0;
}

/* =========================================================================
 * CSV parser
 * ======================================================================= */

typedef struct {
    const char *src;
    usize       len;
    usize       pos;
    char        delim;
    CandoVM    *vm;
} CsvParser;

/* Skip an optional CR before a LF line terminator. */
static void csv_skip_crlf(CsvParser *p)
{
    if (p->pos < p->len && p->src[p->pos] == '\r') p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '\n') p->pos++;
}

/* Parse one CSV field from p->pos.  The field may be:
 *   - Quoted:   starts with '"', ends with '"'; embedded '"' → '""'.
 *     Quoted fields may span multiple lines.
 *   - Unquoted: extends until the next delimiter or unquoted newline.
 *
 * Does NOT consume the trailing delimiter or line terminator.
 * Returns a newly-allocated CandoValue string (TYPE_STRING). */
static CandoValue csv_parse_field(CsvParser *p)
{
    CBuf buf = {0};

    if (p->pos < p->len && p->src[p->pos] == '"') {
        /* Quoted field */
        p->pos++;  /* skip opening '"' */
        while (p->pos < p->len) {
            char c = p->src[p->pos];
            if (c == '"') {
                p->pos++;
                if (p->pos < p->len && p->src[p->pos] == '"') {
                    /* Escaped quote: "" → " */
                    cbuf_push_char(&buf, '"');
                    p->pos++;
                } else {
                    /* End of quoted field */
                    break;
                }
            } else {
                cbuf_push_char(&buf, c);
                p->pos++;
            }
        }
    } else {
        /* Unquoted field: read until delimiter or newline */
        while (p->pos < p->len) {
            char c = p->src[p->pos];
            if (c == p->delim || c == '\n' || c == '\r') break;
            cbuf_push_char(&buf, c);
            p->pos++;
        }
    }

    CandoValue result;
    if (buf.oom) {
        result = cando_null();
    } else {
        const char *raw = buf.data ? buf.data : "";
        CandoString *s  = cando_string_new(raw, (u32)buf.len);
        result = cando_string_value(s);
    }
    cbuf_free(&buf);
    return result;
}

/* Parse one row of CSV fields into a newly-allocated Cando array.
 * Returns the array CandoValue.  Advances p->pos past the row terminator.
 * Returns cando_null() if the parser is already at EOF. */
static CandoValue csv_parse_row(CsvParser *p)
{
    CandoValue arr_val = cando_bridge_new_array(p->vm);
    CdoObject *arr     = cando_bridge_resolve(p->vm, arr_val.as.handle);

    while (true) {
        CandoValue field = csv_parse_field(p);

        /* Store the field string as a CdoValue */
        if (cando_is_string(field)) {
            CdoString *ds = cdo_string_new(field.as.string->data,
                                            field.as.string->length);
            CdoValue   dv = cdo_string_value(ds);
            cdo_array_push(arr, dv);
            cdo_value_release(dv);
            cando_value_release(field);
        } else {
            /* oom: push empty string placeholder */
            CdoString *ds = cdo_string_new("", 0);
            CdoValue   dv = cdo_string_value(ds);
            cdo_array_push(arr, dv);
            cdo_value_release(dv);
        }

        if (p->pos >= p->len) break;
        char next = p->src[p->pos];
        if (next == p->delim) {
            p->pos++;  /* consume delimiter, parse next field */
        } else {
            /* newline or end-of-input: end of row */
            csv_skip_crlf(p);
            break;
        }
    }

    return arr_val;
}

/* =========================================================================
 * csv.parse(str, delim?, header?) → array of arrays | array of objects
 * ======================================================================= */
static int csv_parse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "csv.parse: expected a string argument");
        return -1;
    }

    /* delimiter (default ',') */
    char delim = ',';
    if (argc >= 2 && cando_is_string(args[1]) && args[1].as.string->length > 0) {
        delim = args[1].as.string->data[0];
    }

    /* header mode */
    bool header_mode = false;
    if (argc >= 3 && cando_is_bool(args[2])) {
        header_mode = args[2].as.boolean;
    }

    CsvParser p;
    p.src   = args[0].as.string->data;
    p.len   = (usize)args[0].as.string->length;
    p.pos   = 0;
    p.delim = delim;
    p.vm    = vm;

    /* Result is an array of rows */
    CandoValue result_val = cando_bridge_new_array(vm);
    CdoObject *result     = cando_bridge_resolve(vm, result_val.as.handle);

    /* Parse header row first if needed */
    CandoValue header_row_val = cando_null();
    CdoObject *header_row     = NULL;
    u32        header_count   = 0;

    if (header_mode && p.pos < p.len) {
        header_row_val = csv_parse_row(&p);
        header_row     = cando_bridge_resolve(vm, header_row_val.as.handle);
        header_count   = cdo_array_len(header_row);
    }

    /* Parse remaining rows */
    while (p.pos < p.len) {
        CandoValue row_val = csv_parse_row(&p);

        if (!header_mode) {
            /* Plain mode: push the array directly */
            CdoObject *row = cando_bridge_resolve(vm, row_val.as.handle);
            cdo_array_push(result, cdo_array_value(row));
        } else {
            /* Header mode: convert row array → object using header keys */
            CdoObject *row     = cando_bridge_resolve(vm, row_val.as.handle);
            u32        row_len = cdo_array_len(row);

            CandoValue obj_val = cando_bridge_new_object(vm);
            CdoObject *obj     = cando_bridge_resolve(vm, obj_val.as.handle);

            u32 count = (row_len < header_count) ? row_len : header_count;
            for (u32 i = 0; i < count; i++) {
                CdoValue hdr_cell;
                CdoValue row_cell;
                if (!cdo_array_rawget_idx(header_row, i, &hdr_cell)) continue;
                if (!cdo_array_rawget_idx(row, i, &row_cell)) continue;
                if (hdr_cell.tag != CDO_STRING) continue;

                CdoString *key = cdo_string_intern(hdr_cell.as.string->data,
                                                    hdr_cell.as.string->length);
                /* Store a copy of the cell value */
                CdoValue cell_copy = cdo_value_copy(row_cell);
                cdo_object_rawset(obj, key, cell_copy, FIELD_NONE);
                cdo_value_release(cell_copy);
                cdo_string_release(key);
            }

            cdo_array_push(result, cdo_object_value(obj));
        }
    }

    cando_vm_push(vm, result_val);
    return 1;
}

/* =========================================================================
 * CSV stringify helpers
 * ======================================================================= */

/* Convert a CdoValue to a C string in buf.  For non-string types a
 * best-effort conversion is applied.  Object/array → empty string. */
static void csv_cell_to_str(CBuf *out, CdoValue v)
{
    switch ((CdoTypeTag)v.tag) {
        case CDO_NULL:
            /* empty cell */
            break;
        case CDO_BOOL:
            cbuf_push_cstr(out, v.as.boolean ? "true" : "false");
            break;
        case CDO_NUMBER: {
            char tmp[64];
            f64 n = v.as.number;
            if (n == floor(n) && !isinf(n) && fabs(n) < 1e15) {
                snprintf(tmp, sizeof(tmp), "%.0f", n);
            } else {
                snprintf(tmp, sizeof(tmp), "%.17g", n);
            }
            cbuf_push_cstr(out, tmp);
            break;
        }
        case CDO_STRING:
            cbuf_push(out, v.as.string->data, v.as.string->length);
            break;
        case CDO_OBJECT:
        case CDO_ARRAY:
        case CDO_FUNCTION:
        case CDO_NATIVE:
            /* Not representable in CSV: emit empty cell */
            break;
    }
}

/* Write one quoted or bare CSV field from the content in `cell`.
 * Quotes are added when `cell` contains delim, '"', CR, or LF. */
static void csv_write_field(CBuf *out, const char *cell, usize cell_len, char delim)
{
    bool needs_quote = false;
    for (usize i = 0; i < cell_len; i++) {
        char c = cell[i];
        if (c == '"' || c == '\r' || c == '\n' || c == delim) {
            needs_quote = true; break;
        }
    }

    if (!needs_quote) {
        cbuf_push(out, cell, cell_len);
        return;
    }

    cbuf_push_char(out, '"');
    for (usize i = 0; i < cell_len; i++) {
        if (cell[i] == '"') cbuf_push_char(out, '"');  /* escape: "" */
        cbuf_push_char(out, cell[i]);
    }
    cbuf_push_char(out, '"');
}

/* =========================================================================
 * Header extraction: build a parallel array of CdoString* from a Cando
 * array of strings.  Returns the count; fills keys[] (up to max).
 * Caller must cdo_string_release() each returned key.
 * ======================================================================= */
static u32 csv_extract_headers(CdoObject *hdr_arr, CdoString **keys, u32 max)
{
    u32 n = cdo_array_len(hdr_arr);
    if (n > max) n = max;
    for (u32 i = 0; i < n; i++) {
        CdoValue v;
        if (!cdo_array_rawget_idx(hdr_arr, i, &v) || v.tag != CDO_STRING) {
            keys[i] = NULL; continue;
        }
        keys[i] = cdo_string_intern(v.as.string->data, v.as.string->length);
    }
    return n;
}

/* =========================================================================
 * Key accumulation callback -- collects keys from the first data object
 * when no explicit headers are provided.
 * ======================================================================= */
#define CSV_MAX_COLS 4096

typedef struct {
    CdoString **keys;
    u32         count;
    u32         cap;
} KeyAccum;

static bool csv_key_accum_cb(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)val; (void)flags;
    KeyAccum *ka = (KeyAccum *)ud;
    if (ka->count >= ka->cap) return false;
    ka->keys[ka->count++] = cdo_string_intern(key->data, key->length);
    return true;
}

/* =========================================================================
 * csv.stringify(data, delim?, headers?) → string
 * ======================================================================= */
static int csv_stringify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm, "csv.stringify: expected an array argument");
        return -1;
    }

    /* delimiter */
    char delim = ',';
    if (argc >= 2 && cando_is_string(args[1]) && args[1].as.string->length > 0) {
        delim = args[1].as.string->data[0];
    }

    /* headers (optional array of strings) */
    CdoObject *hdr_arr   = NULL;
    bool       has_hdrs  = false;
    if (argc >= 3 && cando_is_object(args[2])) {
        hdr_arr  = cando_bridge_resolve(vm, args[2].as.handle);
        has_hdrs = (hdr_arr->kind == OBJ_ARRAY);
    }

    CdoObject *data = cando_bridge_resolve(vm, args[0].as.handle);
    if (data->kind != OBJ_ARRAY) {
        cando_vm_error(vm, "csv.stringify: data must be an array");
        return -1;
    }

    u32 row_count = cdo_array_len(data);

    /* Determine column keys (for object-mode rows) */
    CdoString *col_keys[CSV_MAX_COLS];
    u32        col_count = 0;
    bool       obj_mode  = false;

    if (has_hdrs) {
        col_count = csv_extract_headers(hdr_arr, col_keys, CSV_MAX_COLS);
        obj_mode  = true;  /* headers imply object mode */
    } else if (row_count > 0) {
        /* Peek at the first row to decide whether it's array or object.
         * The VM stores all objects (including arrays) as CDO_OBJECT; use kind. */
        CdoValue first_v;
        if (cdo_array_rawget_idx(data, 0, &first_v) &&
                (first_v.tag == CDO_OBJECT || first_v.tag == CDO_ARRAY)) {
            bool first_is_array = (first_v.tag == CDO_ARRAY) ||
                                  (first_v.as.object->kind == OBJ_ARRAY);
            if (!first_is_array) {
                obj_mode = true;
                /* Collect keys from first object in FIFO order */
                KeyAccum ka = { .keys = col_keys, .count = 0, .cap = CSV_MAX_COLS };
                cdo_object_foreach(first_v.as.object, csv_key_accum_cb, &ka);
                col_count = ka.count;
            }
        }
    }

    CBuf out = {0};

    /* Write explicit header row if provided */
    if (has_hdrs) {
        for (u32 c = 0; c < col_count; c++) {
            if (c > 0) cbuf_push_char(&out, delim);
            if (col_keys[c]) {
                csv_write_field(&out, col_keys[c]->data,
                                col_keys[c]->length, delim);
            }
        }
        cbuf_push(&out, "\r\n", 2);
    } else if (obj_mode && col_count > 0) {
        /* Auto-write header row from discovered keys */
        for (u32 c = 0; c < col_count; c++) {
            if (c > 0) cbuf_push_char(&out, delim);
            if (col_keys[c]) {
                csv_write_field(&out, col_keys[c]->data,
                                col_keys[c]->length, delim);
            }
        }
        cbuf_push(&out, "\r\n", 2);
    }

    /* Write data rows */
    for (u32 r = 0; r < row_count; r++) {
        CdoValue row_v;
        if (!cdo_array_rawget_idx(data, r, &row_v)) continue;

        if (!obj_mode) {
            /* Array row -- VM may store arrays as CDO_OBJECT; check kind */
            bool row_is_array = (row_v.tag == CDO_ARRAY) ||
                                ((row_v.tag == CDO_OBJECT) &&
                                 row_v.as.object->kind == OBJ_ARRAY);
            if (!row_is_array) continue;
            CdoObject *row     = row_v.as.object;
            u32        row_len = cdo_array_len(row);
            for (u32 c = 0; c < row_len; c++) {
                if (c > 0) cbuf_push_char(&out, delim);
                CdoValue cell;
                CBuf cell_str = {0};
                if (cdo_array_rawget_idx(row, c, &cell)) {
                    csv_cell_to_str(&cell_str, cell);
                }
                csv_write_field(&out,
                                cell_str.data ? cell_str.data : "",
                                cell_str.len, delim);
                cbuf_free(&cell_str);
            }
        } else {
            /* Object row: emit one cell per header key.
             * Skip array rows that ended up in an object-mode table. */
            bool row_is_obj = (row_v.tag == CDO_OBJECT) &&
                              (row_v.as.object->kind == OBJ_OBJECT);
            if (!row_is_obj) {
                /* Not an object in this row: emit empty cells */
                for (u32 c = 0; c < col_count; c++) {
                    if (c > 0) cbuf_push_char(&out, delim);
                }
            } else {
                CdoObject *row = row_v.as.object;
                for (u32 c = 0; c < col_count; c++) {
                    if (c > 0) cbuf_push_char(&out, delim);
                    if (!col_keys[c]) continue;
                    CdoValue cell;
                    CBuf cell_str = {0};
                    if (cdo_object_rawget(row, col_keys[c], &cell)) {
                        csv_cell_to_str(&cell_str, cell);
                    }
                    csv_write_field(&out,
                                    cell_str.data ? cell_str.data : "",
                                    cell_str.len, delim);
                    cbuf_free(&cell_str);
                }
            }
        }

        cbuf_push(&out, "\r\n", 2);
        if (out.oom) break;
    }

    /* Release interned column keys */
    for (u32 c = 0; c < col_count; c++) {
        if (col_keys[c]) cdo_string_release(col_keys[c]);
    }

    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    cbuf_free(&out);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_csv_register(CandoVM *vm)
{
    CandoValue csv_val = cando_bridge_new_object(vm);
    CdoObject *csv_obj = cando_bridge_resolve(vm, csv_val.as.handle);

    libutil_set_method(vm, csv_obj, "parse",     csv_parse);
    libutil_set_method(vm, csv_obj, "stringify", csv_stringify);

    cando_vm_set_global(vm, "csv", csv_val, true);
}
