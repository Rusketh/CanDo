/*
 * modules/smtp/mime.h -- MIME (RFC 5322 + 2045..2049) parser and builder.
 *
 * Header-only.  Both the script-facing natives and the C unit tests
 * include this file directly.
 *
 * The parser is non-validating: it accepts mostly-correct mail and
 * recovers from common malformed-header cases (folded lines, missing
 * Content-Transfer-Encoding) the way real mail clients do, rather than
 * rejecting the message.
 *
 * The builder produces RFC-conformant CRLF-line output suitable for
 * piping straight into SMTP DATA after dot-stuffing.
 */

#ifndef SMTP_MIME_H
#define SMTP_MIME_H

#include "smtp_helpers.h"

/* =========================================================================
 * Parsed representation
 * ======================================================================= */

typedef struct mime_header {
    char *name;          /* lower-cased canonical name (malloc'd) */
    char *raw_name;      /* original casing (malloc'd) */
    char *value;         /* value with folding unfolded, decoded UTF-8 */
    char *raw_value;     /* original raw value bytes */
    struct mime_header *next;
} mime_header_t;

typedef struct mime_part {
    mime_header_t *headers;
    char          *content_type;     /* lower-case "type/subtype"; default "text/plain" */
    char          *charset;          /* "utf-8" if missing */
    char          *boundary;         /* multipart boundary; NULL otherwise */
    char          *content_id;       /* e.g. "<cid>" without angle brackets */
    char          *disposition;      /* "inline" / "attachment" / NULL */
    char          *filename;         /* from Content-Disposition or Content-Type name= */
    char          *transfer_encoding;/* "7bit"/"8bit"/"base64"/"quoted-printable" */
    /* Body bytes, already transfer-decoded for leaf parts.  For multipart
     * parts the body is empty and the children list is non-NULL. */
    uint8_t       *body;
    size_t         body_len;
    /* Children (for multipart). */
    struct mime_part *children;
    size_t            n_children;
} mime_part_t;

/* =========================================================================
 * Parser
 * ======================================================================= */

static void mime_header_free(mime_header_t *h)
{
    while (h) {
        mime_header_t *next = h->next;
        free(h->name); free(h->raw_name);
        free(h->value); free(h->raw_value);
        free(h); h = next;
    }
}

static void mime_part_free(mime_part_t *p);

static void mime_part_free_inner(mime_part_t *p)
{
    if (!p) return;
    mime_header_free(p->headers);
    free(p->content_type);
    free(p->charset);
    free(p->boundary);
    free(p->content_id);
    free(p->disposition);
    free(p->filename);
    free(p->transfer_encoding);
    free(p->body);
    for (size_t i = 0; i < p->n_children; i++) mime_part_free_inner(&p->children[i]);
    free(p->children);
}

static void mime_part_free(mime_part_t *p)
{
    if (!p) return;
    mime_part_free_inner(p);
    free(p);
}

/* Lower-case a malloc'd copy of [src..src+n). */
static char *strdup_lower_n(const char *src, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)src[i]);
    out[n] = '\0';
    return out;
}

static char *strdup_n(const char *src, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n); out[n] = '\0';
    return out;
}

/* Find header end ("\r\n\r\n" or "\n\n").  Returns offset of the byte
 * AFTER the blank-line, or `n` if no body separator was found. */
static size_t find_body_offset(const uint8_t *src, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (src[i] == '\r' && src[i+1] == '\n' &&
            src[i+2] == '\r' && src[i+3] == '\n') return i + 4;
        if (src[i] == '\n' && src[i+1] == '\n') return i + 2;
    }
    return n;
}

/* Walk the header block, calling `cb(name, name_len, value, value_len, ud)`
 * once per logical header.  Folded continuation lines are unfolded. */
typedef void (*hdr_cb)(const char *name, size_t nl,
                       const char *value, size_t vl, void *ud);

static void parse_headers(const uint8_t *src, size_t n, hdr_cb cb, void *ud)
{
    size_t i = 0;
    while (i < n) {
        size_t line_start = i;
        while (i < n && src[i] != '\n') i++;
        size_t line_end = i;
        if (line_end > line_start && src[line_end-1] == '\r') line_end--;
        if (line_end == line_start) { /* blank: end of headers */
            if (i < n) i++;
            return;
        }
        /* Read continuation lines (start with SP or HT). */
        size_t logical_end = line_end;
        size_t scan = (i < n) ? i + 1 : i;
        while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) {
            size_t cs = scan;
            while (scan < n && src[scan] != '\n') scan++;
            size_t ce = scan;
            if (ce > cs && src[ce-1] == '\r') ce--;
            /* Append " " + cs..ce to logical line; we represent this by
             * extending the indices conceptually -- simpler to materialise
             * a fresh buffer below. */
            (void)cs; (void)ce;
            logical_end = scan;
            if (scan < n) scan++;
        }
        /* Materialise the logical line by collapsing folds. */
        sb_t logical; sb_init(&logical);
        size_t p = line_start;
        sb_append(&logical, (const char *)(src + p), line_end - p);
        size_t cont = (i < n) ? i + 1 : i;
        while (cont < n && (src[cont] == ' ' || src[cont] == '\t')) {
            size_t cs = cont;
            while (cont < n && src[cont] != '\n') cont++;
            size_t ce = cont;
            if (ce > cs && src[ce-1] == '\r') ce--;
            sb_putc(&logical, ' ');
            sb_append(&logical, (const char *)(src + cs + 1), ce - cs - 1);
            if (cont < n) cont++;
        }
        i = cont;
        /* Split at first colon. */
        size_t colon = 0;
        while (colon < logical.len && logical.data[colon] != ':') colon++;
        if (colon < logical.len) {
            size_t vstart = colon + 1;
            while (vstart < logical.len &&
                   (logical.data[vstart] == ' ' || logical.data[vstart] == '\t')) vstart++;
            cb(logical.data, colon,
               logical.data + vstart, logical.len - vstart, ud);
        }
        sb_free(&logical);
        /* Advance past the LF terminator of the last folded line. */
        (void)logical_end;
    }
}

static void hdr_collect(const char *name, size_t nl,
                        const char *value, size_t vl, void *ud)
{
    mime_header_t **head = (mime_header_t **)ud;
    mime_header_t  *h    = (mime_header_t *)calloc(1, sizeof(*h));
    if (!h) return;
    h->raw_name  = strdup_n(name, nl);
    h->name      = strdup_lower_n(name, nl);
    h->raw_value = strdup_n(value, vl);
    /* Decoded value -- RFC 2047 expansion. */
    sb_t dec; sb_init(&dec);
    rfc2047_decode(value, vl, &dec);
    h->value = strdup_n(dec.data ? dec.data : "", dec.len);
    sb_free(&dec);
    h->next = NULL;
    /* Append. */
    if (!*head) { *head = h; return; }
    mime_header_t *t = *head;
    while (t->next) t = t->next;
    t->next = h;
}

static const mime_header_t *mime_find_header(const mime_part_t *p, const char *lname)
{
    for (mime_header_t *h = p->headers; h; h = h->next)
        if (strcmp(h->name, lname) == 0) return h;
    return NULL;
}

/* Parse "name=value; key=value; ..." attribute list.  Looks up `key`
 * and copies the unquoted value into a freshly malloc'd string. */
static char *parse_param(const char *src, const char *key)
{
    if (!src) return NULL;
    size_t klen = strlen(key);
    const char *p = src;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        const char *kstart = p;
        while (*p && *p != '=' && *p != ';') p++;
        size_t klen2 = (size_t)(p - kstart);
        while (klen2 && (kstart[klen2-1] == ' ' || kstart[klen2-1] == '\t')) klen2--;
        if (*p == '=') {
            p++;
            const char *vstart;
            const char *vend;
            if (*p == '"') {
                p++;
                vstart = p;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                vend = p;
                if (*p == '"') p++;
            } else {
                vstart = p;
                while (*p && *p != ';' && *p != ' ' && *p != '\t') p++;
                vend = p;
            }
            if (klen2 == klen) {
                int eq = 1;
                for (size_t i = 0; i < klen; i++)
                    if (tolower((unsigned char)kstart[i]) !=
                        tolower((unsigned char)key[i])) { eq = 0; break; }
                if (eq) return strdup_n(vstart, (size_t)(vend - vstart));
            }
        } else {
            while (*p && *p != ';') p++;
        }
    }
    return NULL;
}

/* Decode body bytes according to transfer_encoding. */
static void decode_body(const char *enc,
                        const uint8_t *src, size_t n,
                        uint8_t **out, size_t *out_len)
{
    if (!enc) enc = "7bit";
    if (ci_eq(enc, "base64")) {
        size_t cap = n + 1;
        uint8_t *o = (uint8_t *)malloc(cap);
        size_t w = b64_decode((const char *)src, n, o, cap);
        if (w == (size_t)-1) w = 0;
        *out = o; *out_len = w;
        return;
    }
    if (ci_eq(enc, "quoted-printable")) {
        sb_t b; sb_init(&b);
        qp_decode((const char *)src, n, &b);
        *out = (uint8_t *)b.data; *out_len = b.len;
        return;
    }
    /* 7bit / 8bit / binary -- pass-through. */
    uint8_t *o = (uint8_t *)malloc(n ? n : 1);
    if (n) memcpy(o, src, n);
    *out = o; *out_len = n;
}

static void parse_part(const uint8_t *src, size_t n, mime_part_t *out);

static void parse_multipart(const uint8_t *src, size_t n,
                            const char *boundary, mime_part_t *out)
{
    size_t bl = strlen(boundary);
    /* Find each --boundary line. */
    size_t starts[256]; size_t ends[256]; size_t n_parts = 0;
    bool have_open = false; size_t cur_start = 0;
    size_t i = 0;
    while (i < n) {
        /* line start? */
        if ((i == 0 || src[i-1] == '\n') &&
            i + 2 + bl <= n && src[i] == '-' && src[i+1] == '-' &&
            memcmp(src + i + 2, boundary, bl) == 0) {
            size_t after = i + 2 + bl;
            bool is_close = (after + 2 <= n &&
                             src[after] == '-' && src[after+1] == '-');
            /* Skip to end of line. */
            size_t eol = after + (is_close ? 2 : 0);
            while (eol < n && src[eol] != '\n') eol++;
            if (eol < n) eol++;
            if (have_open && n_parts < 256) {
                ends[n_parts] = i;
                starts[n_parts] = cur_start;
                /* Trim trailing CRLF before the boundary line. */
                while (ends[n_parts] > starts[n_parts] &&
                       (src[ends[n_parts]-1] == '\n' ||
                        src[ends[n_parts]-1] == '\r')) ends[n_parts]--;
                n_parts++;
            }
            if (is_close) break;
            cur_start = eol;
            have_open = true;
            i = eol;
            continue;
        }
        /* Skip to next line. */
        while (i < n && src[i] != '\n') i++;
        if (i < n) i++;
    }

    out->children   = (mime_part_t *)calloc(n_parts ? n_parts : 1, sizeof(mime_part_t));
    out->n_children = n_parts;
    for (size_t k = 0; k < n_parts; k++) {
        parse_part(src + starts[k], ends[k] - starts[k], &out->children[k]);
    }
}

static void parse_part(const uint8_t *src, size_t n, mime_part_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t body_off = find_body_offset(src, n);
    parse_headers(src, body_off, hdr_collect, &out->headers);

    const mime_header_t *h_ct = mime_find_header(out, "content-type");
    const mime_header_t *h_te = mime_find_header(out, "content-transfer-encoding");
    const mime_header_t *h_cd = mime_find_header(out, "content-disposition");
    const mime_header_t *h_cid= mime_find_header(out, "content-id");

    if (h_ct && h_ct->raw_value) {
        const char *semi = strchr(h_ct->raw_value, ';');
        size_t typ_len = semi ? (size_t)(semi - h_ct->raw_value)
                               : strlen(h_ct->raw_value);
        /* Trim. */
        while (typ_len && (h_ct->raw_value[typ_len-1] == ' ' ||
                           h_ct->raw_value[typ_len-1] == '\t')) typ_len--;
        out->content_type = strdup_lower_n(h_ct->raw_value, typ_len);
        out->charset      = parse_param(h_ct->raw_value, "charset");
        out->boundary     = parse_param(h_ct->raw_value, "boundary");
        if (!out->filename) out->filename = parse_param(h_ct->raw_value, "name");
    }
    if (!out->content_type) out->content_type = strdup_n("text/plain", 10);
    if (!out->charset)      out->charset      = strdup_n("utf-8", 5);

    if (h_te && h_te->raw_value) {
        size_t L = strlen(h_te->raw_value);
        while (L && (h_te->raw_value[L-1] == ' ' || h_te->raw_value[L-1] == '\t')) L--;
        out->transfer_encoding = strdup_lower_n(h_te->raw_value, L);
    } else {
        out->transfer_encoding = strdup_n("7bit", 4);
    }
    if (h_cd && h_cd->raw_value) {
        const char *semi = strchr(h_cd->raw_value, ';');
        size_t L = semi ? (size_t)(semi - h_cd->raw_value) : strlen(h_cd->raw_value);
        while (L && (h_cd->raw_value[L-1] == ' ' || h_cd->raw_value[L-1] == '\t')) L--;
        out->disposition = strdup_lower_n(h_cd->raw_value, L);
        char *fn = parse_param(h_cd->raw_value, "filename");
        if (fn) { free(out->filename); out->filename = fn; }
    }
    if (h_cid && h_cid->raw_value) {
        const char *p = h_cid->raw_value;
        while (*p == ' ' || *p == '\t' || *p == '<') p++;
        const char *e = p;
        while (*e && *e != '>') e++;
        out->content_id = strdup_n(p, (size_t)(e - p));
    }

    /* Multipart? */
    if (out->boundary && strncmp(out->content_type, "multipart/", 10) == 0) {
        parse_multipart(src + body_off, n - body_off, out->boundary, out);
    } else {
        decode_body(out->transfer_encoding,
                    src + body_off, n - body_off,
                    &out->body, &out->body_len);
    }
}

static mime_part_t *mime_parse(const uint8_t *src, size_t n)
{
    mime_part_t *p = (mime_part_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    parse_part(src, n, p);
    return p;
}

/* Return a pointer to the first leaf part with content_type == "text/plain"
 * (or "text/html" if want_html), recursively walking multiparts.  NULL if
 * no such part exists. */
static const mime_part_t *find_text_part(const mime_part_t *p, bool want_html)
{
    if (!p) return NULL;
    if (p->n_children) {
        for (size_t i = 0; i < p->n_children; i++) {
            const mime_part_t *r = find_text_part(&p->children[i], want_html);
            if (r) return r;
        }
        return NULL;
    }
    if (!p->content_type) return NULL;
    if ( want_html && strcmp(p->content_type, "text/html") == 0)  return p;
    if (!want_html && strcmp(p->content_type, "text/plain") == 0) return p;
    return NULL;
}

/* Walk leaves; collect anything with disposition=attachment OR a filename. */
typedef void (*att_cb)(const mime_part_t *p, void *ud);

static void walk_attachments(const mime_part_t *p, att_cb cb, void *ud)
{
    if (!p) return;
    if (p->n_children) {
        for (size_t i = 0; i < p->n_children; i++)
            walk_attachments(&p->children[i], cb, ud);
        return;
    }
    bool is_attach = false;
    if (p->disposition && strcmp(p->disposition, "attachment") == 0) is_attach = true;
    if (p->filename && p->filename[0] != '\0') is_attach = true;
    if (p->content_type &&
        strncmp(p->content_type, "text/", 5) != 0 &&
        strncmp(p->content_type, "multipart/", 10) != 0) is_attach = true;
    if (is_attach) cb(p, ud);
}

/* =========================================================================
 * Builder
 * ======================================================================= */

typedef struct {
    const char *name;       /* nullable */
    const char *body;       /* file contents OR path */
    size_t      body_len;
    const char *content_type;  /* default "application/octet-stream" */
    bool        is_inline;
    const char *content_id;    /* only used when inline */
} mime_attach_in_t;

typedef struct {
    const char *from;
    const char *to;             /* comma-joined */
    const char *cc;
    const char *bcc;            /* used only as separate envelope rcpts */
    const char *reply_to;
    const char *subject;
    const char *text;           /* may be NULL */
    const char *html;           /* may be NULL */
    const char *message_id;     /* may be NULL → auto-generated */
    const char *date;           /* may be NULL → auto */

    mime_attach_in_t *attachments;
    size_t            n_attachments;

    /* Custom additional headers, repeated as name1\0val1\0name2\0val2\0...
     * with a trailing double NUL.  May be NULL. */
    const char *extra_headers;

    const char *user_agent;
} mime_build_t;

static void boundary_make(char *out, size_t cap)
{
    char rnd[16]; random_hex(rnd, sizeof(rnd));
    snprintf(out, cap, "----=_CanDo_%.*s", (int)sizeof(rnd), rnd);
}

static bool emit_header(sb_t *out, const char *name, const char *value)
{
    if (!value || !*value) return true;
    if (!header_value_safe(value, strlen(value))) return false;
    if (!sb_puts(out, name) || !sb_puts(out, ": ")) return false;
    if (!header_encode_if_needed(value, strlen(value), out)) return false;
    return sb_puts(out, "\r\n");
}

/* Best-effort folding of a long header line at 78 chars on commas. */
static bool emit_address_header(sb_t *out, const char *name, const char *value)
{
    if (!value || !*value) return true;
    if (!header_value_safe(value, strlen(value))) return false;
    sb_puts(out, name); sb_puts(out, ": ");
    size_t col = strlen(name) + 2;
    const char *p = value;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t segl = comma ? (size_t)(comma - p) : strlen(p);
        /* Skip leading whitespace. */
        while (segl && (*p == ' ' || *p == '\t')) { p++; segl--; }
        if (col + segl > 76 && col > 4) {
            sb_puts(out, "\r\n\t");
            col = 8;
        }
        header_encode_if_needed(p, segl, out);
        col += segl;
        p += segl;
        if (*p == ',') { sb_putc(out, ','); sb_putc(out, ' '); col += 2; p++; }
    }
    return sb_puts(out, "\r\n");
}

/* Wrap a long base64 body to 76-char lines. */
static bool emit_b64_wrapped(sb_t *out, const char *src, size_t n)
{
    const size_t W = 76;
    for (size_t i = 0; i < n; i += W) {
        size_t k = (n - i > W) ? W : n - i;
        if (!sb_append(out, src + i, k)) return false;
        if (!sb_puts(out, "\r\n")) return false;
    }
    return true;
}

/* Emit a single body part (Content-Type + Content-Transfer-Encoding +
 * empty line + body) to `out`.  `body` is treated as 8-bit input;
 * we always emit base64 to make CRLF/8bit-safe and stay inside 998-char
 * line limits. */
static bool emit_leaf_part(sb_t *out, const char *content_type,
                           const char *charset, const char *disposition,
                           const char *filename, const char *content_id,
                           const uint8_t *body, size_t body_len)
{
    if (charset && *charset)
        sb_putf(out, "Content-Type: %s; charset=%s\r\n", content_type, charset);
    else
        sb_putf(out, "Content-Type: %s\r\n", content_type);
    if (filename) {
        sb_putf(out, "Content-Disposition: %s; filename=\"%s\"\r\n",
                disposition ? disposition : "attachment", filename);
    } else if (disposition) {
        sb_putf(out, "Content-Disposition: %s\r\n", disposition);
    }
    if (content_id && *content_id)
        sb_putf(out, "Content-ID: <%s>\r\n", content_id);
    sb_puts(out, "Content-Transfer-Encoding: base64\r\n\r\n");

    char  *enc = NULL;
    size_t enc_len = 0;
    enc = b64_encode_alloc(body, body_len, &enc_len);
    if (!enc) return false;
    bool ok = emit_b64_wrapped(out, enc, enc_len);
    free(enc);
    return ok;
}

static bool mime_build(const mime_build_t *in, sb_t *out)
{
    char date_buf[64];
    char msgid_buf[128];
    bool has_text = in->text && *in->text;
    bool has_html = in->html && *in->html;
    bool has_attach = in->n_attachments > 0;

    /* Top-level headers. */
    if (!emit_header(out, "MIME-Version", "1.0")) return false;
    if (in->date && *in->date)
        emit_header(out, "Date", in->date);
    else { rfc5322_date(date_buf, sizeof(date_buf)); emit_header(out, "Date", date_buf); }
    emit_address_header(out, "From",     in->from);
    emit_address_header(out, "To",       in->to);
    emit_address_header(out, "Cc",       in->cc);
    emit_address_header(out, "Reply-To", in->reply_to);
    emit_header(out, "Subject", in->subject);
    if (in->message_id && *in->message_id) {
        emit_header(out, "Message-ID", in->message_id);
    } else {
        const char *at = in->from ? strchr(in->from, '@') : NULL;
        const char *gt;
        char dom[256] = "localhost";
        if (at) {
            at++;
            gt = strchr(at, '>');
            size_t dl = gt ? (size_t)(gt - at) : strlen(at);
            if (dl >= sizeof(dom)) dl = sizeof(dom) - 1;
            memcpy(dom, at, dl); dom[dl] = '\0';
        }
        make_message_id(msgid_buf, sizeof(msgid_buf), dom);
        emit_header(out, "Message-ID", msgid_buf);
    }
    if (in->user_agent) emit_header(out, "User-Agent", in->user_agent);
    /* Extra headers. */
    if (in->extra_headers) {
        const char *p = in->extra_headers;
        while (*p) {
            const char *name = p;
            while (*p) p++;       /* name */
            p++;                  /* skip NUL */
            const char *val = p;
            while (*p) p++;
            p++;
            if (*name == '\0') break;
            emit_header(out, name, val);
        }
    }

    char outer[80] = {0};
    char alt[80]   = {0};
    bool need_outer_multipart = (has_attach && (has_text || has_html));
    bool need_alt_multipart   = (has_text && has_html);

    if (need_outer_multipart) {
        boundary_make(outer, sizeof(outer));
        sb_putf(out, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n", outer);
        sb_puts(out, "\r\n");
        sb_putf(out, "--%s\r\n", outer);
        if (need_alt_multipart) {
            boundary_make(alt, sizeof(alt));
            sb_putf(out, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n", alt);
            sb_puts(out, "\r\n");
            sb_putf(out, "--%s\r\n", alt);
            emit_leaf_part(out, "text/plain", "utf-8", NULL, NULL, NULL,
                           (const uint8_t *)in->text, strlen(in->text));
            sb_putf(out, "\r\n--%s\r\n", alt);
            emit_leaf_part(out, "text/html", "utf-8", NULL, NULL, NULL,
                           (const uint8_t *)in->html, strlen(in->html));
            sb_putf(out, "\r\n--%s--\r\n", alt);
        } else if (has_text) {
            emit_leaf_part(out, "text/plain", "utf-8", NULL, NULL, NULL,
                           (const uint8_t *)in->text, strlen(in->text));
        } else if (has_html) {
            emit_leaf_part(out, "text/html", "utf-8", NULL, NULL, NULL,
                           (const uint8_t *)in->html, strlen(in->html));
        }
        for (size_t i = 0; i < in->n_attachments; i++) {
            sb_putf(out, "\r\n--%s\r\n", outer);
            const mime_attach_in_t *a = &in->attachments[i];
            const char *ctype = a->content_type && *a->content_type
                ? a->content_type : "application/octet-stream";
            emit_leaf_part(out, ctype, NULL,
                           a->is_inline ? "inline" : "attachment",
                           a->name, a->content_id,
                           (const uint8_t *)a->body, a->body_len);
        }
        sb_putf(out, "\r\n--%s--\r\n", outer);
    } else if (need_alt_multipart) {
        boundary_make(alt, sizeof(alt));
        sb_putf(out, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n", alt);
        sb_puts(out, "\r\n");
        sb_putf(out, "--%s\r\n", alt);
        emit_leaf_part(out, "text/plain", "utf-8", NULL, NULL, NULL,
                       (const uint8_t *)in->text, strlen(in->text));
        sb_putf(out, "\r\n--%s\r\n", alt);
        emit_leaf_part(out, "text/html", "utf-8", NULL, NULL, NULL,
                       (const uint8_t *)in->html, strlen(in->html));
        sb_putf(out, "\r\n--%s--\r\n", alt);
    } else if (has_html) {
        emit_leaf_part(out, "text/html", "utf-8", NULL, NULL, NULL,
                       (const uint8_t *)in->html, strlen(in->html));
    } else if (has_text || (!has_attach && !has_html)) {
        const char *t = has_text ? in->text : "";
        emit_leaf_part(out, "text/plain", "utf-8", NULL, NULL, NULL,
                       (const uint8_t *)t, strlen(t));
    } else if (has_attach) {
        /* Just a single attachment with no body. */
        emit_leaf_part(out, "text/plain", "utf-8", NULL, NULL, NULL,
                       (const uint8_t *)"", 0);
        for (size_t i = 0; i < in->n_attachments; i++) {
            const mime_attach_in_t *a = &in->attachments[i];
            const char *ctype = a->content_type && *a->content_type
                ? a->content_type : "application/octet-stream";
            emit_leaf_part(out, ctype, NULL,
                           a->is_inline ? "inline" : "attachment",
                           a->name, a->content_id,
                           (const uint8_t *)a->body, a->body_len);
        }
    }
    return true;
}

#endif /* SMTP_MIME_H */
