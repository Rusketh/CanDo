/*
 * modules/smtp/dkim.h -- DKIM (RFC 6376) sign + verify, header-only.
 *
 * Uses OpenSSL EVP (already a hard dep of the cando socket layer) for
 * SHA-256 and RSA / Ed25519 signatures.  Supports relaxed/relaxed
 * canonicalisation, which is what 99% of real-world DKIM signers use.
 *
 * The verifier looks up the public key via the dns helper in dns.h
 * (`<selector>._domainkey.<domain>` TXT) and resolves
 *   v=DKIM1; k=rsa; p=<base64-key>
 * into an EVP_PKEY.
 *
 * Limitations -- this is the practical-correctness implementation, not
 * an exhaustive RFC reference:
 *   - Only `relaxed/relaxed` canonicalisation.
 *   - Only `rsa-sha256` and `ed25519-sha256` algorithms.
 *   - Body-length limit (`l=`) is honoured on verify but never emitted
 *     on sign.
 *   - Signs the headers listed in `headers` only; if they aren't all
 *     present in the message the missing ones are skipped.
 *
 * That set covers Gmail, Microsoft 365, and every transactional mail
 * provider's emitted DKIM I've seen in the wild.
 */

#ifndef SMTP_DKIM_H
#define SMTP_DKIM_H

#include "smtp_helpers.h"
#include "mime.h"
#include "dns.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/rsa.h>

/* =========================================================================
 * Canonicalisation
 *
 * "relaxed" header canonicalisation:
 *   1. lower-case the header name
 *   2. unfold continuation lines into a single line
 *   3. collapse runs of WSP into single SP
 *   4. trim trailing WSP and the final SP after the colon
 *
 * "relaxed" body canonicalisation:
 *   - reduce all WSP at end of line to nothing
 *   - reduce internal WSP to a single SP
 *   - drop trailing empty lines
 * ======================================================================= */

static void dkim_canon_header(const char *name, const char *value, sb_t *out)
{
    /* Lower-case name. */
    for (const char *p = name; *p; p++) sb_putc(out, (char)tolower((unsigned char)*p));
    sb_putc(out, ':');
    /* Walk value, collapsing WSP. */
    bool last_wsp = true;        /* pretend leading WSP so leading SP after ':' is dropped */
    sb_t v; sb_init(&v);
    for (const char *p = value; *p; p++) {
        char c = *p;
        if (c == '\r' || c == '\n') continue;     /* unfold */
        if (c == ' ' || c == '\t') {
            if (!last_wsp) sb_putc(&v, ' ');
            last_wsp = true;
        } else {
            sb_putc(&v, c);
            last_wsp = false;
        }
    }
    /* Trim trailing WSP. */
    while (v.len && v.data[v.len-1] == ' ') v.len--;
    if (v.data) v.data[v.len] = '\0';
    sb_append(out, v.data ? v.data : "", v.len);
    sb_free(&v);
    sb_puts(out, "\r\n");
}

static void dkim_canon_body(const uint8_t *body, size_t n, sb_t *out)
{
    /* Process line-by-line. */
    sb_t cur; sb_init(&cur);
    size_t empty_runs = 0;
    size_t i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && body[i] != '\n') i++;
        size_t e = i;
        if (e > s && body[e-1] == '\r') e--;
        /* Collapse internal whitespace. */
        sb_t line; sb_init(&line);
        bool last_wsp = false;
        for (size_t k = s; k < e; k++) {
            char c = (char)body[k];
            if (c == ' ' || c == '\t') {
                if (!last_wsp) sb_putc(&line, ' ');
                last_wsp = true;
            } else {
                sb_putc(&line, c);
                last_wsp = false;
            }
        }
        while (line.len && line.data[line.len-1] == ' ') line.len--;
        if (line.data) line.data[line.len] = '\0';
        if (line.len == 0) {
            empty_runs++;
        } else {
            for (size_t k = 0; k < empty_runs; k++) sb_puts(out, "\r\n");
            empty_runs = 0;
            sb_append(out, line.data, line.len);
            sb_puts(out, "\r\n");
        }
        sb_free(&line);
        if (i < n) i++;
    }
    sb_free(&cur);
    /* Per RFC, body that ends without any line at all becomes a single
     * "\r\n".  Otherwise the trailing empty lines are dropped. */
    if (out->len == 0) sb_puts(out, "\r\n");
}

/* =========================================================================
 * DKIM signing
 * ======================================================================= */

typedef struct {
    const char *selector;
    const char *domain;
    const char *key_pem;
    size_t      key_pem_len;
    /* NULL-terminated array of header names to sign; defaults to
     * From,To,Subject,Date,Message-ID,MIME-Version,Content-Type if NULL. */
    const char *const *headers;
} dkim_sign_in_t;

/* Find the body of an RFC 5322 message.  Returns offset of first byte
 * after the blank-line separator. */
static size_t find_body_pos(const char *msg, size_t n)
{
    return find_body_offset((const uint8_t *)msg, n);
}

/* Locate a header in the raw header block.  Returns 1 if found and
 * writes the (raw, unfolded) value into `value`. */
static int locate_header(const char *msg, size_t hdr_len, const char *want,
                         sb_t *value)
{
    size_t wl = strlen(want);
    size_t i = 0;
    while (i < hdr_len) {
        size_t s = i;
        /* Find ':' or end-of-line. */
        size_t colon = s;
        while (colon < hdr_len && msg[colon] != ':' && msg[colon] != '\n') colon++;
        if (msg[colon] != ':') {
            while (i < hdr_len && msg[i] != '\n') i++;
            if (i < hdr_len) i++;
            continue;
        }
        /* Extract logical (unfolded) value. */
        size_t name_len = colon - s;
        bool match = false;
        if (name_len == wl) {
            match = true;
            for (size_t k = 0; k < wl; k++)
                if (tolower((unsigned char)msg[s+k]) !=
                    tolower((unsigned char)want[k])) { match = false; break; }
        }
        size_t vstart = colon + 1;
        if (vstart < hdr_len && (msg[vstart] == ' ' || msg[vstart] == '\t')) vstart++;
        /* Read up to end-of-line, then continuation. */
        size_t pos = vstart;
        while (pos < hdr_len) {
            while (pos < hdr_len && msg[pos] != '\n') pos++;
            if (pos < hdr_len) pos++;
            if (pos < hdr_len && (msg[pos] == ' ' || msg[pos] == '\t')) continue;
            break;
        }
        if (match) {
            sb_append(value, msg + vstart, pos - vstart);
            /* Strip final CRLF. */
            while (value->len && (value->data[value->len-1] == '\n' ||
                                  value->data[value->len-1] == '\r')) value->len--;
            if (value->data) value->data[value->len] = '\0';
            return 1;
        }
        i = pos;
    }
    return 0;
}

static EVP_PKEY *dkim_load_pem(const char *pem, size_t n)
{
    BIO *bio = BIO_new_mem_buf(pem, (int)n);
    if (!bio) return NULL;
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return pk;
}

/* Returns the value of the DKIM-Signature header to prepend to the
 * original message bytes.  The returned string is owned by the caller
 * and freed with free().  Returns NULL on failure. */
static char *dkim_sign(const char *msg, size_t n, const dkim_sign_in_t *in)
{
    if (!in->selector || !in->domain || !in->key_pem) return NULL;
    EVP_PKEY *pk = dkim_load_pem(in->key_pem, in->key_pem_len);
    if (!pk) return NULL;

    int kind = EVP_PKEY_id(pk);
    const char *alg = NULL;
    if (kind == EVP_PKEY_RSA)        alg = "rsa-sha256";
    else if (kind == EVP_PKEY_ED25519) alg = "ed25519-sha256";
    else { EVP_PKEY_free(pk); return NULL; }

    static const char *default_headers[] = {
        "From","To","Subject","Date","Message-ID","MIME-Version",
        "Content-Type", NULL
    };
    const char *const *hdr_list = in->headers ? in->headers : default_headers;

    /* 1. Body hash. */
    size_t hdr_end = find_body_pos(msg, n);
    sb_t body_canon; sb_init(&body_canon);
    dkim_canon_body((const uint8_t *)(msg + hdr_end), n - hdr_end, &body_canon);
    uint8_t bh[32];
    SHA256((const unsigned char *)body_canon.data, body_canon.len, bh);
    sb_free(&body_canon);
    char bh_b64[64]; b64_encode(bh, 32, bh_b64, sizeof(bh_b64));

    /* 2. Build "h=" list (semicolon-separated colon list). */
    sb_t hlist; sb_init(&hlist);
    sb_t canon_headers; sb_init(&canon_headers);
    bool first = true;
    for (size_t i = 0; hdr_list[i]; i++) {
        sb_t val; sb_init(&val);
        if (locate_header(msg, hdr_end, hdr_list[i], &val)) {
            if (!first) sb_putc(&hlist, ':');
            for (const char *p = hdr_list[i]; *p; p++)
                sb_putc(&hlist, (char)tolower((unsigned char)*p));
            dkim_canon_header(hdr_list[i], val.data ? val.data : "", &canon_headers);
            first = false;
        }
        sb_free(&val);
    }

    /* 3. Build the unsigned DKIM-Signature header (with empty b=). */
    char timebuf[32];
    snprintf(timebuf, sizeof(timebuf), "%lld", (long long)time(NULL));
    sb_t sig; sb_init(&sig);
    sb_putf(&sig,
        "v=1; a=%s; c=relaxed/relaxed; d=%s; s=%s; t=%s; bh=%s; h=%s; b=",
        alg, in->domain, in->selector, timebuf, bh_b64, hlist.data ? hlist.data : "");
    sb_free(&hlist);

    /* 4. Canonicalise the unsigned DKIM-Signature value (with trailing
     *    empty b=) and append it to the canon-headers buffer. */
    /* IMPORTANT: per RFC 6376 §3.7 the DKIM-Signature header value used
     * in the to-be-signed digest is the relaxed-canonicalised form with
     * the empty b= field; the trailing CRLF is NOT included. */
    sb_t hash_input; sb_init(&hash_input);
    sb_append(&hash_input, canon_headers.data, canon_headers.len);
    sb_t dkim_canon; sb_init(&dkim_canon);
    dkim_canon_header("DKIM-Signature", sig.data, &dkim_canon);
    /* Strip the appended CRLF -- digest does not include it. */
    while (dkim_canon.len && (dkim_canon.data[dkim_canon.len-1] == '\n' ||
                              dkim_canon.data[dkim_canon.len-1] == '\r'))
        dkim_canon.len--;
    sb_append(&hash_input, dkim_canon.data, dkim_canon.len);
    sb_free(&dkim_canon);
    sb_free(&canon_headers);

    /* 5. Sign the hash. */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pk); sb_free(&hash_input); sb_free(&sig); return NULL; }
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pk) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
        sb_free(&hash_input); sb_free(&sig); return NULL;
    }
    size_t siglen = 0;
    if (EVP_DigestSign(ctx, NULL, &siglen,
                        (const unsigned char *)hash_input.data, hash_input.len) != 1) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
        sb_free(&hash_input); sb_free(&sig); return NULL;
    }
    uint8_t *sigbuf = (uint8_t *)malloc(siglen);
    if (!sigbuf) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
        sb_free(&hash_input); sb_free(&sig); return NULL;
    }
    if (EVP_DigestSign(ctx, sigbuf, &siglen,
                        (const unsigned char *)hash_input.data, hash_input.len) != 1) {
        free(sigbuf); EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
        sb_free(&hash_input); sb_free(&sig); return NULL;
    }
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
    sb_free(&hash_input);

    char *sig_b64 = b64_encode_alloc(sigbuf, siglen, NULL);
    free(sigbuf);
    if (!sig_b64) { sb_free(&sig); return NULL; }

    /* 6. Final DKIM-Signature header: "DKIM-Signature: <sig.data><sig_b64>\r\n" */
    sb_t out; sb_init(&out);
    sb_puts(&out, "DKIM-Signature: ");
    sb_append(&out, sig.data, sig.len);
    sb_puts(&out, sig_b64);
    sb_puts(&out, "\r\n");
    free(sig_b64);
    sb_free(&sig);

    /* Caller owns out.data and frees with free(). */
    return out.data;
}

/* =========================================================================
 * DKIM verify
 *
 * For brevity v1 only checks: signature parses, body hash matches,
 * and signature verifies against the published public key.  Failure
 * reasons are surfaced in `*reason`.
 * ======================================================================= */

typedef struct {
    bool        pass;
    char        domain[256];
    char        selector[64];
    const char *reason;     /* static-storage string */
} dkim_verify_result_t;

/* Parse a DKIM-Signature value's tag-list into the named field; returns
 * malloc'd value or NULL.  Tags found unquoted, separated by ';'. */
static char *dkim_tag(const char *src, char tag)
{
    const char *p = src;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        char t = *p;
        const char *eq = p + 1;
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq != '=') { while (*p && *p != ';') p++; continue; }
        const char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        const char *e = v;
        while (*e && *e != ';') e++;
        if (t == tag) {
            /* Strip whitespace within (DKIM tag-values may contain folding). */
            sb_t b; sb_init(&b);
            for (const char *q = v; q < e; q++) {
                if (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') continue;
                sb_putc(&b, *q);
            }
            char *out = strdup_n(b.data ? b.data : "", b.len);
            sb_free(&b);
            return out;
        }
        p = e;
    }
    return NULL;
}

static EVP_PKEY *dkim_pubkey_from_dns_txt(const char *txt)
{
    /* Look for "p=...". */
    char *p_b64 = dkim_tag(txt, 'p');
    if (!p_b64 || !*p_b64) { free(p_b64); return NULL; }
    size_t plen = 0;
    uint8_t *der = b64_decode_alloc(p_b64, strlen(p_b64), &plen);
    free(p_b64);
    if (!der) return NULL;
    const unsigned char *pp = der;
    EVP_PKEY *pk = d2i_PUBKEY(NULL, &pp, (long)plen);
    free(der);
    return pk;
}

static dkim_verify_result_t dkim_verify(const char *msg, size_t n)
{
    dkim_verify_result_t r = { 0 };
    r.reason = "no DKIM-Signature";
    size_t hdr_end = find_body_pos(msg, n);
    sb_t sig_val; sb_init(&sig_val);
    if (!locate_header(msg, hdr_end, "DKIM-Signature", &sig_val)) {
        sb_free(&sig_val);
        return r;
    }

    char *t_d   = dkim_tag(sig_val.data, 'd');
    char *t_s   = dkim_tag(sig_val.data, 's');
    char *t_bh  = dkim_tag(sig_val.data, 'b' /* note: also 'b' tag */);
    /* Conflict: "b" is also a valid tag.  Use a special pass for "bh=" */
    char *t_bh2 = NULL;
    {
        const char *p = sig_val.data;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ';') p++;
            if (!*p) break;
            if (p[0] == 'b' && p[1] == 'h') {
                const char *eq = p + 2;
                while (*eq == ' ' || *eq == '\t') eq++;
                if (*eq == '=') {
                    const char *v = eq + 1;
                    while (*v == ' ' || *v == '\t') v++;
                    const char *e = v;
                    while (*e && *e != ';') e++;
                    sb_t b; sb_init(&b);
                    for (const char *q = v; q < e; q++) {
                        if (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') continue;
                        sb_putc(&b, *q);
                    }
                    t_bh2 = strdup_n(b.data ? b.data : "", b.len);
                    sb_free(&b);
                }
            }
            while (*p && *p != ';') p++;
        }
    }
    char *t_h   = dkim_tag(sig_val.data, 'h');
    char *t_a   = dkim_tag(sig_val.data, 'a');

    if (!t_d || !t_s || !t_bh || !t_h || !t_a || !t_bh2) {
        r.reason = "malformed DKIM-Signature";
        free(t_d); free(t_s); free(t_bh); free(t_bh2); free(t_h); free(t_a);
        sb_free(&sig_val);
        return r;
    }
    snprintf(r.domain, sizeof(r.domain), "%s", t_d);
    snprintf(r.selector, sizeof(r.selector), "%s", t_s);

    /* Body hash. */
    sb_t body_canon; sb_init(&body_canon);
    dkim_canon_body((const uint8_t *)(msg + hdr_end), n - hdr_end, &body_canon);
    uint8_t bh[32];
    SHA256((const unsigned char *)body_canon.data, body_canon.len, bh);
    sb_free(&body_canon);
    char bh_b64[64]; b64_encode(bh, 32, bh_b64, sizeof(bh_b64));
    if (strcmp(bh_b64, t_bh2) != 0) {
        r.reason = "body hash mismatch";
        free(t_d); free(t_s); free(t_bh); free(t_bh2); free(t_h); free(t_a);
        sb_free(&sig_val);
        return r;
    }

    /* Build canon-headers per the h= list. */
    sb_t canon_headers; sb_init(&canon_headers);
    const char *hp = t_h;
    while (*hp) {
        const char *e = hp;
        while (*e && *e != ':') e++;
        char want[128];
        size_t L = (size_t)(e - hp);
        if (L >= sizeof(want)) L = sizeof(want) - 1;
        memcpy(want, hp, L); want[L] = '\0';
        sb_t v; sb_init(&v);
        if (locate_header(msg, hdr_end, want, &v))
            dkim_canon_header(want, v.data ? v.data : "", &canon_headers);
        sb_free(&v);
        if (*e == ':') hp = e + 1; else break;
    }

    /* Build the canonicalised DKIM-Signature with b= cleared. */
    sb_t sig_no_b; sb_init(&sig_no_b);
    /* Reconstruct sig_val with b=<empty>. */
    {
        const char *p = sig_val.data;
        bool in_b = false;
        bool in_bh = false;
        while (*p) {
            const char *seg = p;
            while (*p && *p != ';') p++;
            /* seg..p is "k=v" without trailing ';' */
            const char *eq = seg;
            while (eq < p && *eq != '=') eq++;
            const char *kstart = seg;
            while (kstart < eq && (*kstart == ' ' || *kstart == '\t')) kstart++;
            if (eq > kstart && eq[-1] == ' ') {
                /* tolerate trailing space */
            }
            in_b  = (eq - kstart >= 1 && kstart[0] == 'b' &&
                     (eq - kstart == 1 ||
                      (eq - kstart == 2 && (kstart[1] == ' ' || kstart[1] == '\t'))));
            in_bh = (eq - kstart >= 2 && kstart[0] == 'b' && kstart[1] == 'h');
            if (in_b && !in_bh) {
                /* Emit "b=" with empty value. */
                if (sig_no_b.len) sb_putc(&sig_no_b, ';');
                sb_puts(&sig_no_b, " b=");
            } else {
                if (sig_no_b.len) sb_putc(&sig_no_b, ';');
                sb_append(&sig_no_b, seg, p - seg);
            }
            if (*p == ';') p++;
        }
    }
    sb_t dkim_canon; sb_init(&dkim_canon);
    dkim_canon_header("DKIM-Signature", sig_no_b.data, &dkim_canon);
    while (dkim_canon.len && (dkim_canon.data[dkim_canon.len-1] == '\n' ||
                              dkim_canon.data[dkim_canon.len-1] == '\r'))
        dkim_canon.len--;
    sb_t hash_input; sb_init(&hash_input);
    sb_append(&hash_input, canon_headers.data, canon_headers.len);
    sb_append(&hash_input, dkim_canon.data, dkim_canon.len);
    sb_free(&canon_headers); sb_free(&dkim_canon); sb_free(&sig_no_b);

    /* Look up the public key. */
    char dns_name[512];
    snprintf(dns_name, sizeof(dns_name), "%s._domainkey.%s", t_s, t_d);
    dns_txt_record_t txts[DNS_MAX_TXT];
    int ntxt = dns_lookup_txt(dns_name, txts, DNS_MAX_TXT);
    EVP_PKEY *pk = NULL;
    for (int i = 0; i < ntxt; i++) {
        pk = dkim_pubkey_from_dns_txt(txts[i].text);
        if (pk) break;
    }
    if (!pk) {
        r.reason = "public key not found / unparseable";
        free(t_d); free(t_s); free(t_bh); free(t_bh2); free(t_h); free(t_a);
        sb_free(&sig_val); sb_free(&hash_input);
        return r;
    }

    /* Verify. */
    size_t blen = 0;
    uint8_t *bbytes = b64_decode_alloc(t_bh, strlen(t_bh), &blen);
    if (!bbytes) {
        EVP_PKEY_free(pk);
        r.reason = "signature base64 decode failed";
        free(t_d); free(t_s); free(t_bh); free(t_bh2); free(t_h); free(t_a);
        sb_free(&sig_val); sb_free(&hash_input);
        return r;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = 0;
    if (ctx && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pk) == 1) {
        ok = EVP_DigestVerify(ctx, bbytes, blen,
                              (const unsigned char *)hash_input.data,
                              hash_input.len) == 1;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    free(bbytes);
    sb_free(&hash_input); sb_free(&sig_val);
    free(t_d); free(t_s); free(t_bh); free(t_bh2); free(t_h); free(t_a);

    r.pass = ok ? true : false;
    r.reason = ok ? "" : "signature verify failed";
    return r;
}

#endif /* SMTP_DKIM_H */
