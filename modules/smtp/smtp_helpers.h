/*
 * modules/smtp/smtp_helpers.h -- Pure-C, header-only utilities for the
 * SMTP / IMAP / POP3 / MIME module.
 *
 * Everything here is `static` so the file can be #included from both
 * smtp_module.c and test_smtp.c without producing duplicate symbols.
 *
 * Provides:
 *   - Dynamic byte-buffer (sb_*) for building SMTP commands and MIME bodies.
 *   - Base64 encode / decode (RFC 4648).
 *   - Quoted-printable encode / decode (RFC 2045).
 *   - RFC 2047 encoded-word decoder for non-ASCII headers.
 *   - SMTP dot-stuffing for the DATA command body.
 *   - Address parser: "Alice <a@b>" -> {name, address}.
 *   - Address-list parser: comma-split with quoted-string awareness.
 *   - Header-injection guard (rejects bare \r or \n in user input).
 *   - CRLF line iterator for SMTP / IMAP / POP3 wire reads.
 *   - Random message-id generator.
 *
 * Must compile with gcc -std=c11 (POSIX or MinGW).
 */

#ifndef SMTP_HELPERS_H
#define SMTP_HELPERS_H

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* =========================================================================
 * Dynamic byte buffer (sb_t)
 * ======================================================================= */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} sb_t;

static inline void sb_init(sb_t *b)
{
    b->data = NULL; b->len = 0; b->cap = 0;
}

static inline void sb_free(sb_t *b)
{
    free(b->data); b->data = NULL; b->len = 0; b->cap = 0;
}

static inline bool sb_reserve(sb_t *b, size_t need)
{
    if (b->cap >= need) return true;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    char *nd = (char *)realloc(b->data, nc);
    if (!nd) return false;
    b->data = nd; b->cap = nc;
    return true;
}

static inline bool sb_append(sb_t *b, const void *src, size_t n)
{
    if (!sb_reserve(b, b->len + n + 1)) return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static inline bool sb_putc(sb_t *b, char c)
{
    return sb_append(b, &c, 1);
}

static inline bool sb_puts(sb_t *b, const char *s)
{
    return sb_append(b, s, strlen(s));
}

static inline bool sb_putf(sb_t *b, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n < sizeof(tmp)) return sb_append(b, tmp, (size_t)n);
    /* Larger -- allocate. */
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return false;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    bool ok = sb_append(b, big, (size_t)n);
    free(big);
    return ok;
}

/* =========================================================================
 * Base64 (RFC 4648)
 * ======================================================================= */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* base64_encode -- output uses "=" padding.  out_cap must be >= 4*((n+2)/3)+1.
 * Returns the number of bytes written (excluding terminating NUL). */
static size_t b64_encode(const uint8_t *in, size_t n,
                          char *out, size_t out_cap)
{
    size_t need = 4 * ((n + 2) / 3) + 1;
    if (out_cap < need) return 0;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = b64_alphabet[(v >> 6) & 0x3F];
        out[o++] = b64_alphabet[v & 0x3F];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        out[o++] = b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? b64_alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

/* Heap variant.  Caller frees. */
static char *b64_encode_alloc(const uint8_t *in, size_t n, size_t *out_len)
{
    size_t cap = 4 * ((n + 2) / 3) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t w = b64_encode(in, n, out, cap);
    if (out_len) *out_len = w;
    return out;
}

static int b64_dec_ch(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;     /* padding sentinel */
    return -1;                   /* skip whitespace */
}

/* Decode in-place into out (caller-allocated).  Whitespace is ignored.
 * Returns number of decoded bytes, or (size_t)-1 on malformed input. */
static size_t b64_decode(const char *in, size_t n, uint8_t *out, size_t out_cap)
{
    uint32_t buf = 0;
    int      bits = 0;
    size_t   o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_dec_ch((unsigned char)in[i]);
        if (v == -2) break;
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return (size_t)-1;
            out[o++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return o;
}

static uint8_t *b64_decode_alloc(const char *in, size_t n, size_t *out_len)
{
    uint8_t *out = (uint8_t *)malloc(n + 1);
    if (!out) return NULL;
    size_t w = b64_decode(in, n, out, n);
    if (w == (size_t)-1) { free(out); return NULL; }
    if (out_len) *out_len = w;
    return out;
}

/* =========================================================================
 * Quoted-printable (RFC 2045 §6.7)
 * ======================================================================= */

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* qp_encode -- standard QP body encoding, 76-char line wrap with =EOLs.
 * Returns true on success; on out-of-memory frees and returns false. */
static bool qp_encode(const uint8_t *in, size_t n, sb_t *out)
{
    int line = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        bool literal = (c >= 33 && c <= 126 && c != '=');
        /* Tab and space: literal except at end of line. */
        if (c == ' ' || c == '\t') {
            bool eol_next = (i + 1 == n) ||
                            in[i+1] == '\n' || in[i+1] == '\r';
            if (eol_next) literal = false;
            else literal = true;
        }
        /* Hard line break: emit CRLF, reset wrap counter. */
        if (c == '\n') {
            if (!sb_puts(out, "\r\n")) return false;
            line = 0;
            continue;
        }
        if (c == '\r') continue;     /* normalised away; LF will produce CRLF */
        if (line >= 73 && literal) {
            /* Soft break before next char. */
            if (!sb_puts(out, "=\r\n")) return false;
            line = 0;
        } else if (line >= 71 && !literal) {
            if (!sb_puts(out, "=\r\n")) return false;
            line = 0;
        }
        if (literal) {
            if (!sb_putc(out, (char)c)) return false;
            line++;
        } else {
            if (!sb_putf(out, "=%02X", c)) return false;
            line += 3;
        }
    }
    return true;
}

/* qp_decode -- decode quoted-printable. */
static bool qp_decode(const char *in, size_t n, sb_t *out)
{
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '=') {
            if (i + 1 < n && (in[i+1] == '\r' || in[i+1] == '\n')) {
                /* Soft line break. */
                if (in[i+1] == '\r' && i + 2 < n && in[i+2] == '\n') i += 2;
                else i += 1;
                continue;
            }
            if (i + 2 >= n) { if (!sb_putc(out, c)) return false; continue; }
            int hi = hex_val((unsigned char)in[i+1]);
            int lo = hex_val((unsigned char)in[i+2]);
            if (hi < 0 || lo < 0) {
                if (!sb_putc(out, c)) return false;
                continue;
            }
            if (!sb_putc(out, (char)((hi << 4) | lo))) return false;
            i += 2;
        } else {
            if (!sb_putc(out, c)) return false;
        }
    }
    return true;
}

/* =========================================================================
 * RFC 2047 encoded-word decoding (best-effort, ASCII / UTF-8 / Latin-1)
 * Format: "=?charset?B?...?=" or "=?charset?Q?...?="
 *
 * We pass the bytes through unchanged for UTF-8 and ASCII; for Latin-1
 * we expand to UTF-8 inline.  Anything else is left as-is.
 * ======================================================================= */

static bool is_utf8_charset(const char *cs, size_t n)
{
    if (n != 5 && n != 8) return false;
    static const char *list[] = { "utf-8", "utf8", "us-ascii" };
    for (size_t i = 0; i < sizeof(list) / sizeof(*list); i++) {
        size_t L = strlen(list[i]);
        if (L == n) {
            int eq = 1;
            for (size_t j = 0; j < n; j++)
                if (tolower((unsigned char)cs[j]) != list[i][j]) { eq = 0; break; }
            if (eq) return true;
        }
    }
    return false;
}

static bool is_latin1_charset(const char *cs, size_t n)
{
    static const char *list[] = { "iso-8859-1", "latin1", "latin-1", "windows-1252" };
    for (size_t i = 0; i < sizeof(list) / sizeof(*list); i++) {
        size_t L = strlen(list[i]);
        if (L == n) {
            int eq = 1;
            for (size_t j = 0; j < n; j++)
                if (tolower((unsigned char)cs[j]) != list[i][j]) { eq = 0; break; }
            if (eq) return true;
        }
    }
    return false;
}

/* Append byte `c` as UTF-8 of the corresponding Latin-1 codepoint. */
static bool utf8_putc_latin1(sb_t *out, uint8_t c)
{
    if (c < 0x80) return sb_putc(out, (char)c);
    /* 2-byte UTF-8: 110xxxxx 10xxxxxx */
    char buf[2] = { (char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F)) };
    return sb_append(out, buf, 2);
}

/* Decode a single =?...?...?...?= word; src points to first '='.
 * Returns number of bytes consumed, or 0 if not a valid encoded-word. */
static size_t decode_one_encoded_word(const char *src, size_t n, sb_t *out)
{
    if (n < 6 || src[0] != '=' || src[1] != '?') return 0;
    /* Find charset end. */
    size_t cs0 = 2;
    size_t cs1 = cs0;
    while (cs1 < n && src[cs1] != '?') cs1++;
    if (cs1 >= n - 3) return 0;
    /* Encoding char. */
    char enc = (char)tolower((unsigned char)src[cs1 + 1]);
    if ((enc != 'b' && enc != 'q') || src[cs1 + 2] != '?') return 0;
    size_t body0 = cs1 + 3;
    /* Find "?=" terminator. */
    size_t end = body0;
    while (end + 1 < n && !(src[end] == '?' && src[end + 1] == '=')) end++;
    if (end + 1 >= n) return 0;
    size_t blen = end - body0;
    const char *cs = src + cs0;
    size_t cslen = cs1 - cs0;

    sb_t raw; sb_init(&raw);
    if (enc == 'b') {
        size_t out_cap = blen;
        uint8_t *tmp = (uint8_t *)malloc(out_cap ? out_cap : 1);
        if (!tmp) { sb_free(&raw); return 0; }
        size_t w = b64_decode(src + body0, blen, tmp, out_cap);
        if (w == (size_t)-1) { free(tmp); sb_free(&raw); return 0; }
        sb_append(&raw, tmp, w);
        free(tmp);
    } else {
        /* Q-encoding: like quoted-printable but '_' represents space. */
        for (size_t i = 0; i < blen; i++) {
            char c = src[body0 + i];
            if (c == '_') sb_putc(&raw, ' ');
            else if (c == '=' && i + 2 < blen) {
                int hi = hex_val((unsigned char)src[body0 + i + 1]);
                int lo = hex_val((unsigned char)src[body0 + i + 2]);
                if (hi >= 0 && lo >= 0) {
                    sb_putc(&raw, (char)((hi << 4) | lo));
                    i += 2;
                } else sb_putc(&raw, c);
            } else sb_putc(&raw, c);
        }
    }

    if (is_latin1_charset(cs, cslen)) {
        for (size_t i = 0; i < raw.len; i++)
            utf8_putc_latin1(out, (uint8_t)raw.data[i]);
    } else {
        sb_append(out, raw.data, raw.len);
    }
    sb_free(&raw);
    return end + 2 - 0;  /* number consumed from src start */
}

/* Decode a header-value string: replace any "=?...?=" sequences in place
 * with their decoded form, leave everything else as-is. */
static bool rfc2047_decode(const char *in, size_t n, sb_t *out)
{
    size_t i = 0;
    while (i < n) {
        if (i + 1 < n && in[i] == '=' && in[i+1] == '?') {
            size_t k = decode_one_encoded_word(in + i, n - i, out);
            if (k > 0) { i += k; continue; }
        }
        if (!sb_putc(out, in[i])) return false;
        i++;
    }
    return true;
}

/* Encode a string as a single RFC 2047 Q-encoded word.  Always uses
 * UTF-8.  Returns false on OOM.  The caller is responsible for not
 * exceeding 75-character word limits if very long. */
static bool rfc2047_encode_q(const char *in, size_t n, sb_t *out)
{
    if (!sb_puts(out, "=?UTF-8?Q?")) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c == ' ') { sb_putc(out, '_'); continue; }
        if (c >= 33 && c <= 126 && c != '=' && c != '?' && c != '_') {
            sb_putc(out, (char)c);
        } else {
            sb_putf(out, "=%02X", c);
        }
    }
    return sb_puts(out, "?=");
}

/* Encode if it contains any non-ASCII or any of the troublesome chars,
 * otherwise pass through verbatim. */
static bool header_encode_if_needed(const char *in, size_t n, sb_t *out)
{
    bool need = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c >= 0x80 || c == '\r' || c == '\n') { need = true; break; }
    }
    if (!need) return sb_append(out, in, n);
    return rfc2047_encode_q(in, n, out);
}

/* =========================================================================
 * SMTP dot-stuffing (RFC 5321 §4.5.2)
 *
 * Doubles up any line that begins with '.' so the lone "." terminator is
 * unambiguous.  Also normalises bare LF / CR to CRLF.
 * ======================================================================= */

static bool dot_stuff(const char *in, size_t n, sb_t *out)
{
    bool at_line_start = true;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (at_line_start && c == '.') {
            if (!sb_putc(out, '.')) return false;
        }
        if (c == '\r') {
            /* Look ahead for CRLF; emit CRLF either way. */
            if (i + 1 < n && in[i+1] == '\n') i++;
            if (!sb_puts(out, "\r\n")) return false;
            at_line_start = true;
        } else if (c == '\n') {
            if (!sb_puts(out, "\r\n")) return false;
            at_line_start = true;
        } else {
            if (!sb_putc(out, c)) return false;
            at_line_start = false;
        }
    }
    /* Ensure final CRLF before the terminating "." */
    if (out->len < 2 || out->data[out->len - 2] != '\r' ||
        out->data[out->len - 1] != '\n') {
        if (!sb_puts(out, "\r\n")) return false;
    }
    return true;
}

/* =========================================================================
 * Header-injection guard
 *
 * User-supplied addresses, subjects, and custom header values must not
 * contain bare CR or LF -- they could otherwise be used to inject extra
 * headers into the outgoing message.  Returns true if `s` is safe.
 * ======================================================================= */

static bool header_value_safe(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n' || s[i] == '\0') return false;
    return true;
}

/* =========================================================================
 * Address parser: "Display Name <local@host>" or "local@host"
 *
 * Output buffers must be sized appropriately by the caller (>= n+1 each).
 * Returns true on success.
 * ======================================================================= */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static bool parse_one_address(const char *in, size_t n,
                              char *name_out, size_t name_cap,
                              char *addr_out, size_t addr_cap)
{
    const char *p   = in;
    const char *end = in + n;
    p = skip_ws(p, end);
    if (p >= end) return false;

    /* Two forms:
     *   1. local@host
     *   2. Display Name <local@host>
     *   3. "Quoted, Name" <local@host>
     */
    const char *lt = NULL;
    /* Find first unquoted '<'. */
    bool in_quote = false;
    for (const char *q = p; q < end; q++) {
        if (*q == '"') in_quote = !in_quote;
        if (!in_quote && *q == '<') { lt = q; break; }
    }

    const char *addr_start, *addr_end;
    const char *name_start = NULL, *name_end = NULL;
    if (lt) {
        const char *gt = NULL;
        for (const char *q = lt + 1; q < end; q++) if (*q == '>') { gt = q; break; }
        if (!gt) return false;
        addr_start = lt + 1;
        addr_end   = gt;
        name_start = p;
        name_end   = lt;
        /* Strip trailing whitespace from name. */
        while (name_end > name_start &&
               (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
        /* Strip surrounding quotes if any. */
        if (name_end > name_start + 1 &&
            *name_start == '"' && name_end[-1] == '"') {
            name_start++; name_end--;
        }
    } else {
        addr_start = p;
        addr_end   = end;
        while (addr_end > addr_start &&
               (addr_end[-1] == ' ' || addr_end[-1] == '\t' ||
                addr_end[-1] == '\r' || addr_end[-1] == '\n')) addr_end--;
    }

    size_t alen = (size_t)(addr_end - addr_start);
    if (alen == 0 || alen >= addr_cap) return false;
    /* Strip surrounding whitespace from addr. */
    while (alen && (*addr_start == ' ' || *addr_start == '\t')) {
        addr_start++; alen--;
    }
    while (alen && (addr_start[alen-1] == ' ' || addr_start[alen-1] == '\t')) {
        alen--;
    }
    memcpy(addr_out, addr_start, alen);
    addr_out[alen] = '\0';

    if (name_start && name_end > name_start) {
        size_t nlen = (size_t)(name_end - name_start);
        if (nlen >= name_cap) nlen = name_cap - 1;
        /* Decode any RFC 2047 encoded-words inline. */
        sb_t dec; sb_init(&dec);
        rfc2047_decode(name_start, nlen, &dec);
        if (dec.len >= name_cap) dec.len = name_cap - 1;
        memcpy(name_out, dec.data ? dec.data : "", dec.len);
        name_out[dec.len] = '\0';
        sb_free(&dec);
    } else {
        name_out[0] = '\0';
    }
    return true;
}

/* Split an address-list string by commas, respecting quoted strings and
 * angle brackets.  Calls `cb(item, item_len, ud)` for each item.  The
 * trimmed whitespace bookends are NOT removed -- callers do that step. */
typedef bool (*addrlist_cb)(const char *item, size_t item_len, void *ud);

static void split_addr_list(const char *in, size_t n, addrlist_cb cb, void *ud)
{
    bool in_quote = false;
    int  depth    = 0;
    size_t start  = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '"' && (i == 0 || in[i-1] != '\\')) in_quote = !in_quote;
        else if (!in_quote && c == '<') depth++;
        else if (!in_quote && c == '>') { if (depth) depth--; }
        else if (!in_quote && depth == 0 && c == ',') {
            cb(in + start, i - start, ud);
            start = i + 1;
        }
    }
    if (start < n) cb(in + start, n - start, ud);
}

/* =========================================================================
 * CRLF line iterator -- read one CRLF-terminated line from a TCP/TLS
 * socket using sockutil_recv_raw / sockutil_tls_recv.
 *
 * Implements an internal byte buffer the caller owns; subsequent calls
 * accumulate more bytes until a complete line is produced.
 * ======================================================================= */

#define LINEBUF_MAX (8 * 1024 * 1024)   /* 8 MiB upper bound (IMAP literals) */

typedef struct {
    sb_t   buf;
    size_t consumed;
} linebuf_t;

static inline void linebuf_init(linebuf_t *L) { sb_init(&L->buf); L->consumed = 0; }
static inline void linebuf_free(linebuf_t *L) { sb_free(&L->buf); L->consumed = 0; }

/* Compact the buffer (move unread bytes to start) when consumed grows. */
static void linebuf_compact(linebuf_t *L)
{
    if (L->consumed == 0) return;
    size_t rem = L->buf.len - L->consumed;
    memmove(L->buf.data, L->buf.data + L->consumed, rem);
    L->buf.len = rem;
    L->buf.data[rem] = '\0';
    L->consumed = 0;
}

/* Try to extract one line (without CRLF) from the buffer.  Returns
 * the byte length on success and sets *out_ptr to the line start within
 * the buffer.  Returns -1 if no full line is available yet, -2 on
 * line-too-long. */
static long linebuf_take(linebuf_t *L, const char **out_ptr)
{
    const char *p = L->buf.data + L->consumed;
    size_t      n = L->buf.len   - L->consumed;
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] == '\r' && p[i+1] == '\n') {
            *out_ptr = p;
            L->consumed += i + 2;
            if (L->consumed > L->buf.len / 2) linebuf_compact(L);
            return (long)i;
        }
    }
    if (n > LINEBUF_MAX) return -2;
    return -1;
}

/* =========================================================================
 * Random message-id generator
 * ======================================================================= */

static void random_hex(char *out, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    /* Mix process clock + heap-pointer entropy for the test build; a
     * production build can swap in a CSPRNG.  Good enough for Message-ID
     * uniqueness, which is the only use site here. */
    static uint64_t seed = 0;
    if (!seed) {
        seed = (uint64_t)time(NULL) ^ (uintptr_t)&seed ^ 0x9E3779B97F4A7C15ULL;
    }
    for (size_t i = 0; i < n; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = hex[(seed >> 56) & 0xF];
    }
}

static void make_message_id(char *out, size_t cap, const char *domain)
{
    char hex[24];
    random_hex(hex, sizeof(hex));
    snprintf(out, cap, "<%lld.%.*s@%s>",
             (long long)time(NULL), (int)sizeof(hex), hex,
             domain && *domain ? domain : "localhost");
}

/* =========================================================================
 * RFC 5322 Date header
 * ======================================================================= */

static void rfc5322_date(char *out, size_t cap)
{
    time_t    t   = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    /* gmtime_s arg order reversed on MSVC; MinGW provides POSIX-ish
     * variants depending on flags.  Use the thread-safe form when
     * available, fall back otherwise. */
#  if defined(__MINGW32__)
    struct tm *tp = gmtime(&t);
    if (tp) tmv = *tp; else memset(&tmv, 0, sizeof(tmv));
#  else
    gmtime_s(&tmv, &t);
#  endif
#else
    gmtime_r(&t, &tmv);
#endif
    static const char *days[]   = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    snprintf(out, cap, "%s, %d %s %d %02d:%02d:%02d +0000",
             days[tmv.tm_wday], tmv.tm_mday, months[tmv.tm_mon],
             1900 + tmv.tm_year, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* =========================================================================
 * Case-insensitive string ops
 * ======================================================================= */

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int ci_eq_n(const char *a, size_t la, const char *b)
{
    size_t lb = strlen(b);
    if (la != lb) return 0;
    for (size_t i = 0; i < la; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

#endif /* SMTP_HELPERS_H */
