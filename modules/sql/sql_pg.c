/*
 * modules/sql/sql_pg.c -- PostgreSQL frontend/backend protocol v3 driver.
 *
 * Implements the subset of the protocol the script-facing module
 * exercises:
 *
 *   - StartupMessage, optional SSLRequest, plain-password / md5 /
 *     SCRAM-SHA-256 authentication
 *   - ParameterStatus / BackendKeyData absorption
 *   - Simple Query ('Q')                       -- exec, ad-hoc query
 *   - Extended Query (Parse / Bind / Execute)  -- prepared statements
 *   - ErrorResponse / NoticeResponse decoding
 *   - Sync / ReadyForQuery framing
 *
 * Replication, COPY, function-call, and async notifications are out
 * of scope; the SQLite module's API surface gives no good place to
 * surface them, and they can be added later without breaking changes.
 *
 * All I/O is synchronous -- scripts compose concurrency themselves
 * via Cando's first-class `thread { ... }` syntax.
 */

#define _POSIX_C_SOURCE 200809L

#include "sql_pg.h"
#include "sql_buf.h"
#include "sql_crypto.h"
#include "sql_net.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Error helpers
 * ===================================================================== */

static void pg_set_error(PgConn *c, int code, const char *sqlstate,
                         const char *fmt, ...)
{
    c->last_err.code = code;
    if (sqlstate) {
        strncpy(c->last_err.sqlstate, sqlstate, sizeof(c->last_err.sqlstate) - 1);
        c->last_err.sqlstate[sizeof(c->last_err.sqlstate) - 1] = '\0';
    } else {
        c->last_err.sqlstate[0] = '\0';
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last_err.message, sizeof(c->last_err.message), fmt, ap);
    va_end(ap);
}

/* Pack a 5-letter SQLSTATE into an int so callers can compare codes
 * cheaply.  Returns 0 if `s` doesn't look like a SQLSTATE. */
static int pg_sqlstate_code(const char *s)
{
    if (!s) return 0;
    int n = (int)strlen(s);
    if (n != 5) return 0;
    int v = 0;
    for (int i = 0; i < 5; i++) v = v * 36 + (unsigned char)s[i];
    return v;
}

/* =========================================================================
 * Result lifecycle
 * ===================================================================== */

void pg_result_init(PgResult *r)
{
    memset(r, 0, sizeof(*r));
}

void pg_result_free(PgResult *r)
{
    if (r->columns) free(r->columns);
    for (int i = 0; i < r->nrows; i++) {
        free(r->rows[i].cells);
        free(r->rows[i].row_arena);
    }
    free(r->rows);
    memset(r, 0, sizeof(*r));
}

void pg_prepared_desc_free(PgPreparedDesc *d)
{
    if (d->columns) free(d->columns);
    d->columns = NULL;
    d->ncols   = 0;
}

/* =========================================================================
 * Low-level message framing
 *
 * After startup, every server message has the shape:
 *
 *   Byte type
 *   Int32 length-including-the-length-field
 *   <body>
 *
 * pg_recv_msg reads the header, allocates a body buffer big enough,
 * pulls the body in, and hands the raw bytes back.  Caller frees.
 * ===================================================================== */

static bool pg_recv_msg(PgConn *c, char *out_type,
                        unsigned char **out_body, int *out_body_len)
{
    unsigned char hdr[5];
    if (!sql_net_recv_exact(&c->net, hdr, 5)) {
        pg_set_error(c, 0, "08006",
            "postgres: connection lost while reading message header");
        return false;
    }
    *out_type = (char)hdr[0];
    int total = ((int)hdr[1] << 24) | ((int)hdr[2] << 16)
              | ((int)hdr[3] <<  8) |  (int)hdr[4];
    int body_len = total - 4;
    if (body_len < 0 || body_len > 1024 * 1024 * 1024) {
        pg_set_error(c, 0, "08006",
            "postgres: invalid message length %d", total);
        return false;
    }
    unsigned char *body = NULL;
    if (body_len > 0) {
        body = (unsigned char *)malloc((size_t)body_len);
        if (!body) {
            pg_set_error(c, 0, "08006", "postgres: out of memory");
            return false;
        }
        if (!sql_net_recv_exact(&c->net, body, (size_t)body_len)) {
            free(body);
            pg_set_error(c, 0, "08006",
                "postgres: connection lost while reading message body");
            return false;
        }
    }
    *out_body     = body;
    *out_body_len = body_len;
    return true;
}

/* Send a single message: <type><len-incl-len><body>.  type=0 means
 * "no type byte" (used only for the initial StartupMessage). */
static bool pg_send_msg(PgConn *c, char type,
                        const void *body, int body_len)
{
    SqlBuf b = {0};
    if (type) sql_buf_put_u8(&b, (uint8_t)type);
    sql_buf_put_be32(&b, (uint32_t)(body_len + 4));
    if (body_len) sql_buf_put_bytes(&b, body, (size_t)body_len);
    bool ok = sql_net_send_all(&c->net, b.data, b.len);
    sql_buf_free(&b);
    if (!ok) {
        pg_set_error(c, 0, "08006",
            "postgres: send failed (broken pipe)");
    }
    return ok;
}

/* =========================================================================
 * ErrorResponse / NoticeResponse decoder
 *
 * Body is a sequence of:
 *   Byte field-tag, String value, ... terminated by a 0 tag byte.
 *
 * Field tags we care about: 'M' (message), 'C' (sqlstate),
 * 'S' (severity), 'D' (detail), 'H' (hint).
 * ===================================================================== */

static void pg_decode_error(PgConn *c, const unsigned char *body, int n,
                            bool throw_as_error)
{
    SqlReader r = sql_reader_init(body, (size_t)n);
    char severity[16] = "ERROR";
    char message[400] = {0};
    char sqlstate[8]  = {0};
    char detail[200]  = {0};

    for (;;) {
        uint8_t tag = sql_reader_get_u8(&r);
        if (!r.ok || tag == 0) break;
        const char *s = sql_reader_get_cstr(&r);
        if (!r.ok) break;
        switch (tag) {
            case 'S': strncpy(severity, s, sizeof(severity) - 1); break;
            case 'M': strncpy(message,  s, sizeof(message)  - 1); break;
            case 'C': strncpy(sqlstate, s, sizeof(sqlstate) - 1); break;
            case 'D': strncpy(detail,   s, sizeof(detail)   - 1); break;
            default: break;
        }
    }

    if (throw_as_error) {
        pg_set_error(c, pg_sqlstate_code(sqlstate),
                     sqlstate[0] ? sqlstate : NULL,
                     "%s%s%s",
                     message[0] ? message : "PostgreSQL error",
                     detail[0]  ? ": "    : "",
                     detail[0]  ? detail  : "");
    }
}

/* =========================================================================
 * SCRAM-SHA-256 (RFC 5802 + RFC 7677)
 *
 * Used when AuthenticationSASL chooses SCRAM-SHA-256.  Channel binding
 * is offered as "n" only (we don't implement tls-server-end-point yet).
 * ===================================================================== */

typedef struct PgScramCtx {
    char           client_nonce_b64[33];   /* 24 random bytes -> 32 chars + NUL */
    char          *client_first_bare;       /* "n=,r=<nonce>" */
    char          *server_first;            /* full server-first message bytes */
    char          *server_first_bare;       /* same as server_first (no header) */
    char          *combined_nonce;          /* extracted from server-first 'r=' */
    unsigned char  salted_password[SQL_SHA256_LEN];
} PgScramCtx;

/* Build "n,,n=,r=<nonce>" client-first.  Out becomes a heap string. */
static bool pg_scram_make_client_first(PgScramCtx *sc, char **out_msg)
{
    unsigned char raw[24];
    if (!sql_random_bytes(raw, sizeof(raw))) return false;
    sql_b64_encode(raw, sizeof(raw), sc->client_nonce_b64);

    /* RFC 5802 client-first-bare = "n=" username "," "r=" client-nonce */
    /* PostgreSQL ignores the username field in SCRAM; leave it empty. */
    size_t n = strlen("n=,r=") + strlen(sc->client_nonce_b64);
    sc->client_first_bare = (char *)malloc(n + 1);
    if (!sc->client_first_bare) return false;
    sprintf(sc->client_first_bare, "n=,r=%s", sc->client_nonce_b64);

    /* Full client-first = "n,," + bare */
    size_t fn = 3 + strlen(sc->client_first_bare);
    *out_msg = (char *)malloc(fn + 1);
    if (!*out_msg) return false;
    sprintf(*out_msg, "n,,%s", sc->client_first_bare);
    return true;
}

/* Extract attribute `key=...` value (up to comma or end).  Returns
 * a new heap string; caller frees. */
static char *pg_scram_get_attr(const char *msg, char key)
{
    const char *p = msg;
    while (*p) {
        if (p[0] == key && p[1] == '=') {
            p += 2;
            const char *end = strchr(p, ',');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char *v = (char *)malloc(len + 1);
            if (!v) return NULL;
            memcpy(v, p, len);
            v[len] = '\0';
            return v;
        }
        const char *next = strchr(p, ',');
        if (!next) break;
        p = next + 1;
    }
    return NULL;
}

/* Build client-final-without-proof "c=biws,r=<combined-nonce>".
 * "biws" is base64("n,,") with no channel binding. */
static char *pg_scram_make_client_final_no_proof(PgScramCtx *sc)
{
    size_t n = strlen("c=biws,r=") + strlen(sc->combined_nonce);
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    sprintf(s, "c=biws,r=%s", sc->combined_nonce);
    return s;
}

/* Process server-first; compute SaltedPassword + AuthMessage; build
 * client-final WITH proof.  Returns heap string for the final message. */
static char *pg_scram_process_server_first(PgScramCtx *sc,
                                           const char *server_first,
                                           const char *password,
                                           PgConn *c)
{
    /* Parse r=, s=, i= */
    sc->combined_nonce = pg_scram_get_attr(server_first, 'r');
    char *salt_b64     = pg_scram_get_attr(server_first, 's');
    char *iter_str     = pg_scram_get_attr(server_first, 'i');
    if (!sc->combined_nonce || !salt_b64 || !iter_str) {
        pg_set_error(c, 0, "08006",
            "postgres: malformed SCRAM server-first");
        free(salt_b64); free(iter_str);
        return NULL;
    }

    /* Server nonce must extend the client nonce. */
    if (strncmp(sc->combined_nonce, sc->client_nonce_b64,
                strlen(sc->client_nonce_b64)) != 0) {
        pg_set_error(c, 0, "08006",
            "postgres: SCRAM combined nonce does not extend client nonce");
        free(salt_b64); free(iter_str);
        return NULL;
    }

    int iter = atoi(iter_str);
    free(iter_str);
    if (iter < 1 || iter > 1000000) {
        pg_set_error(c, 0, "08006",
            "postgres: SCRAM iteration count out of range (%d)", iter);
        free(salt_b64);
        return NULL;
    }

    unsigned char salt[256];
    int salt_len = sql_b64_decode(salt_b64, strlen(salt_b64), salt);
    free(salt_b64);
    if (salt_len < 0 || salt_len > (int)sizeof(salt)) {
        pg_set_error(c, 0, "08006",
            "postgres: SCRAM salt is not valid base64");
        return NULL;
    }

    /* SaltedPassword = PBKDF2(HMAC-SHA-256, password, salt, iter, 32) */
    if (!sql_pbkdf2_sha256(password, strlen(password),
                           salt, (size_t)salt_len,
                           iter, sc->salted_password)) {
        pg_set_error(c, 0, "08006", "postgres: PBKDF2 failed");
        return NULL;
    }

    /* ClientKey = HMAC(SaltedPassword, "Client Key") */
    unsigned char client_key[SQL_SHA256_LEN];
    sql_hmac_sha256(sc->salted_password, SQL_SHA256_LEN,
                    "Client Key", 10, client_key);

    /* StoredKey = SHA256(ClientKey) */
    unsigned char stored_key[SQL_SHA256_LEN];
    sql_sha256(client_key, SQL_SHA256_LEN, stored_key);

    /* client-final-without-proof + AuthMessage */
    char *cfwp = pg_scram_make_client_final_no_proof(sc);
    if (!cfwp) {
        pg_set_error(c, 0, "08006", "postgres: out of memory");
        return NULL;
    }

    /* AuthMessage = client-first-bare + "," + server-first + "," + cfwp */
    size_t am_len = strlen(sc->client_first_bare) + 1
                  + strlen(server_first) + 1
                  + strlen(cfwp);
    char *auth_msg = (char *)malloc(am_len + 1);
    if (!auth_msg) { free(cfwp); return NULL; }
    sprintf(auth_msg, "%s,%s,%s", sc->client_first_bare, server_first, cfwp);

    /* ClientSignature = HMAC(StoredKey, AuthMessage) */
    unsigned char client_sig[SQL_SHA256_LEN];
    sql_hmac_sha256(stored_key, SQL_SHA256_LEN,
                    auth_msg, strlen(auth_msg), client_sig);

    /* ClientProof = ClientKey XOR ClientSignature */
    unsigned char proof[SQL_SHA256_LEN];
    for (int i = 0; i < SQL_SHA256_LEN; i++)
        proof[i] = client_key[i] ^ client_sig[i];

    char proof_b64[64];
    sql_b64_encode(proof, SQL_SHA256_LEN, proof_b64);

    /* client-final = cfwp + ",p=" + proof_b64 */
    size_t fl = strlen(cfwp) + 3 + strlen(proof_b64);
    char *final = (char *)malloc(fl + 1);
    if (!final) { free(cfwp); free(auth_msg); return NULL; }
    sprintf(final, "%s,p=%s", cfwp, proof_b64);

    free(cfwp);
    free(auth_msg);
    return final;
}

static void pg_scram_free(PgScramCtx *sc)
{
    free(sc->client_first_bare);
    free(sc->server_first);
    free(sc->combined_nonce);
    sc->client_first_bare = NULL;
    sc->server_first      = NULL;
    sc->combined_nonce    = NULL;
}

/* =========================================================================
 * Authentication dispatch
 *
 * After we send the StartupMessage the server replies with one of:
 *   AuthenticationOk             -> done
 *   AuthenticationCleartextPwd   -> send password as PasswordMessage
 *   AuthenticationMD5Password    -> send md5("md5" + md5(pwd+user) + salt)
 *   AuthenticationSASL           -> SCRAM-SHA-256 round-trip
 *
 * Then a stream of ParameterStatus / BackendKeyData / NoticeResponse
 * messages followed by ReadyForQuery.
 * ===================================================================== */

static bool pg_send_password_message(PgConn *c, const char *body, size_t len)
{
    /* PasswordMessage body is a NUL-terminated string. */
    SqlBuf b = {0};
    sql_buf_put_bytes(&b, body, len);
    sql_buf_put_u8(&b, 0);
    bool ok = pg_send_msg(c, 'p', b.data, (int)b.len);
    sql_buf_free(&b);
    return ok;
}

static bool pg_handle_md5_auth(PgConn *c, const unsigned char *salt,
                               const char *user, const char *password)
{
    /* inner = md5(password + user)  -> 32 hex chars */
    SqlBuf inner = {0};
    sql_buf_put_bytes(&inner, password, strlen(password));
    sql_buf_put_bytes(&inner, user,     strlen(user));
    unsigned char md1[SQL_MD5_LEN];
    sql_md5(inner.data, inner.len, md1);
    sql_buf_free(&inner);
    char hex1[33];
    sql_hex_encode(md1, SQL_MD5_LEN, hex1);

    /* outer = md5(hex1 + salt)  -> 32 hex chars, prefix "md5" */
    SqlBuf outer = {0};
    sql_buf_put_bytes(&outer, hex1, 32);
    sql_buf_put_bytes(&outer, salt, 4);
    unsigned char md2[SQL_MD5_LEN];
    sql_md5(outer.data, outer.len, md2);
    sql_buf_free(&outer);
    char final[3 + 32 + 1];
    memcpy(final, "md5", 3);
    sql_hex_encode(md2, SQL_MD5_LEN, final + 3);

    return pg_send_password_message(c, final, strlen(final));
}

/* SCRAM round-trip.  Body of AuthenticationSASL is a list of
 * mechanism names terminated by a 0 tag.  We pick SCRAM-SHA-256 if
 * offered; otherwise we fail. */
static bool pg_handle_sasl_auth(PgConn *c, const unsigned char *body, int n,
                                const char *password)
{
    SqlReader r = sql_reader_init(body + 4, (size_t)(n - 4));   /* skip authcode */
    bool have_scram = false;
    while (r.ok && r.pos < r.len) {
        const char *m = sql_reader_get_cstr(&r);
        if (!m) break;
        if (m[0] == '\0') break;
        if (strcmp(m, "SCRAM-SHA-256") == 0) have_scram = true;
    }
    if (!have_scram) {
        pg_set_error(c, 0, "08006",
            "postgres: server did not offer SCRAM-SHA-256");
        return false;
    }

    PgScramCtx sc;
    memset(&sc, 0, sizeof(sc));
    char *client_first = NULL;
    if (!pg_scram_make_client_first(&sc, &client_first)) {
        pg_set_error(c, 0, "08006", "postgres: SCRAM init failed");
        return false;
    }

    /* SASLInitialResponse: mechanism\0 Int32(len) bytes(client-first) */
    SqlBuf b = {0};
    sql_buf_put_cstr(&b, "SCRAM-SHA-256");
    sql_buf_put_be32(&b, (uint32_t)strlen(client_first));
    sql_buf_put_bytes(&b, client_first, strlen(client_first));
    bool ok = pg_send_msg(c, 'p', b.data, (int)b.len);
    sql_buf_free(&b);
    free(client_first);
    if (!ok) { pg_scram_free(&sc); return false; }

    /* Expect AuthenticationSASLContinue (R, authcode 11). */
    char        type;
    unsigned char *body2 = NULL;
    int            blen2 = 0;
    if (!pg_recv_msg(c, &type, &body2, &blen2)) {
        pg_scram_free(&sc);
        return false;
    }
    if (type == 'E') {
        pg_decode_error(c, body2, blen2, true);
        free(body2); pg_scram_free(&sc);
        return false;
    }
    if (type != 'R' || blen2 < 4) {
        pg_set_error(c, 0, "08006",
            "postgres: expected SASLContinue, got '%c'", type);
        free(body2); pg_scram_free(&sc);
        return false;
    }
    int authcode = (body2[0] << 24) | (body2[1] << 16) | (body2[2] << 8) | body2[3];
    if (authcode != 11) {
        pg_set_error(c, 0, "08006",
            "postgres: expected SASLContinue authcode, got %d", authcode);
        free(body2); pg_scram_free(&sc);
        return false;
    }
    int sf_len = blen2 - 4;
    sc.server_first = (char *)malloc((size_t)sf_len + 1);
    if (!sc.server_first) {
        free(body2); pg_scram_free(&sc);
        pg_set_error(c, 0, "08006", "postgres: out of memory");
        return false;
    }
    memcpy(sc.server_first, body2 + 4, (size_t)sf_len);
    sc.server_first[sf_len] = '\0';
    free(body2);

    /* Compute and send client-final. */
    char *client_final = pg_scram_process_server_first(&sc, sc.server_first,
                                                       password, c);
    if (!client_final) { pg_scram_free(&sc); return false; }
    ok = pg_send_msg(c, 'p', client_final, (int)strlen(client_final));
    free(client_final);
    if (!ok) { pg_scram_free(&sc); return false; }

    /* Expect AuthenticationSASLFinal (authcode 12) then AuthenticationOk. */
    if (!pg_recv_msg(c, &type, &body2, &blen2)) {
        pg_scram_free(&sc);
        return false;
    }
    if (type == 'E') {
        pg_decode_error(c, body2, blen2, true);
        free(body2); pg_scram_free(&sc);
        return false;
    }
    if (type != 'R') {
        pg_set_error(c, 0, "08006",
            "postgres: expected SASLFinal, got '%c'", type);
        free(body2); pg_scram_free(&sc);
        return false;
    }
    free(body2);
    pg_scram_free(&sc);
    return true;
}

static bool pg_handle_auth(PgConn *c, const char *user, const char *password)
{
    for (;;) {
        char            type;
        unsigned char  *body = NULL;
        int             blen = 0;
        if (!pg_recv_msg(c, &type, &body, &blen)) return false;

        if (type == 'E') {
            pg_decode_error(c, body, blen, true);
            free(body);
            return false;
        }
        if (type == 'N') {
            /* NoticeResponse during startup -- skip. */
            free(body);
            continue;
        }
        if (type != 'R' || blen < 4) {
            pg_set_error(c, 0, "08006",
                "postgres: expected Authentication, got '%c'", type);
            free(body);
            return false;
        }
        int authcode = (body[0] << 24) | (body[1] << 16) | (body[2] << 8) | body[3];
        if (authcode == 0) {                 /* AuthenticationOk */
            free(body);
            return true;
        }
        if (authcode == 3) {                 /* CleartextPassword */
            free(body);
            if (!password) {
                pg_set_error(c, 0, "28P01",
                    "postgres: server requires password but none provided");
                return false;
            }
            if (!pg_send_password_message(c, password, strlen(password)))
                return false;
            continue;
        }
        if (authcode == 5) {                 /* MD5Password (4-byte salt) */
            if (blen < 8) {
                free(body);
                pg_set_error(c, 0, "08006", "postgres: bad MD5 challenge");
                return false;
            }
            if (!password) {
                free(body);
                pg_set_error(c, 0, "28P01",
                    "postgres: server requires password but none provided");
                return false;
            }
            unsigned char salt[4];
            memcpy(salt, body + 4, 4);
            free(body);
            if (!pg_handle_md5_auth(c, salt, user, password)) return false;
            continue;
        }
        if (authcode == 10) {                /* SASL */
            if (!password) {
                free(body);
                pg_set_error(c, 0, "28P01",
                    "postgres: server requires password but none provided");
                return false;
            }
            bool ok = pg_handle_sasl_auth(c, body, blen, password);
            free(body);
            if (!ok) return false;
            continue;
        }
        free(body);
        pg_set_error(c, 0, "0A000",
            "postgres: unsupported authentication method (code=%d)",
            authcode);
        return false;
    }
}

/* =========================================================================
 * Read messages until ReadyForQuery.  Used after successful auth and
 * after most query messages.  on_msg() is invoked for each message;
 * returning false stops the loop.  body may be NULL when blen == 0.
 * ===================================================================== */

typedef bool (*PgMsgHandler)(PgConn *c, char type,
                             const unsigned char *body, int blen,
                             void *ud);

static bool pg_read_until_ready(PgConn *c, PgMsgHandler on_msg, void *ud)
{
    for (;;) {
        char            type;
        unsigned char  *body = NULL;
        int             blen = 0;
        if (!pg_recv_msg(c, &type, &body, &blen)) return false;

        bool keep_going = true;
        if (type == 'E') {
            /* ErrorResponse -- decode and remember; loop continues so
             * we can land on ReadyForQuery and leave the connection
             * in a known state. */
            pg_decode_error(c, body, blen, true);
            /* Don't surface NoticeResponse messages as errors. */
        } else if (type == 'N') {
            /* NoticeResponse -- ignore for now. */
        } else if (type == 'S' && blen > 0) {
            /* ParameterStatus -- ignore (could be cached). */
        } else if (type == 'K' && blen >= 8) {
            /* BackendKeyData. */
            c->backend_pid        = ((int)body[0] << 24) | ((int)body[1] << 16)
                                  | ((int)body[2] <<  8) |  (int)body[3];
            c->backend_secret_key = ((int)body[4] << 24) | ((int)body[5] << 16)
                                  | ((int)body[6] <<  8) |  (int)body[7];
        } else if (on_msg) {
            keep_going = on_msg(c, type, body, blen, ud);
        }

        if (type == 'Z' && blen >= 1) {
            char status = (char)body[0];
            c->in_tx        = (status == 'T' || status == 'E');
            c->in_failed_tx = (status == 'E');
            free(body);
            return keep_going;
        }
        free(body);
        if (!keep_going) {
            /* Drain until ReadyForQuery so the next request starts
             * cleanly.  This is what libpq does on protocol error. */
            for (;;) {
                if (!pg_recv_msg(c, &type, &body, &blen)) return false;
                bool ready = (type == 'Z');
                if (type == 'Z' && blen >= 1) {
                    c->in_tx        = (body[0] == 'T' || body[0] == 'E');
                    c->in_failed_tx = (body[0] == 'E');
                }
                free(body);
                if (ready) return false;
            }
        }
    }
}

/* =========================================================================
 * pg_connect / pg_close
 * ===================================================================== */

bool pg_connect(PgConn *c, const SqlConnectOpts *opts)
{
    sql_error_clear(&c->last_err);
    sql_net_init(&c->net);
    c->backend_pid = 0;
    c->backend_secret_key = 0;
    c->in_tx = c->in_failed_tx = false;
    c->last_tag[0] = '\0';
    c->prepared_id_seq = 0;

    sockutil_one_time_init();

    char err[128] = {0};
    int port = opts->port > 0 ? opts->port : 5432;
    int family = 0;       /* AF_UNSPEC */
    c->net.fd = sockutil_tcp_connect(opts->host ? opts->host : "127.0.0.1",
                                     port, family,
                                     opts->connect_timeout_ms,
                                     err, sizeof(err));
    if (c->net.fd == SOCKUTIL_INVALID_SOCKET) {
        pg_set_error(c, 0, "08006",
            "postgres: connect to %s:%d failed%s%s",
            opts->host ? opts->host : "127.0.0.1", port,
            err[0] ? " - " : "", err);
        return false;
    }
    sockutil_set_nodelay(c->net.fd, true);
    if (opts->io_timeout_ms > 0)
        sockutil_set_timeout(c->net.fd, opts->io_timeout_ms);
    c->net.io_timeout_ms = opts->io_timeout_ms;

    /* TLS negotiation (SSLRequest) -- magic number 80877103. */
    if (opts->tls) {
        unsigned char req[8] = { 0,0,0,8,  0x04, 0xd2, 0x16, 0x2f };
        if (!sql_net_send_all(&c->net, req, 8)) {
            pg_set_error(c, 0, "08006", "postgres: SSLRequest send failed");
            sql_net_close(&c->net);
            return false;
        }
        unsigned char resp = 0;
        int rc = sockutil_recv_raw(c->net.fd, &resp, 1);
        if (rc != 1) {
            pg_set_error(c, 0, "08006", "postgres: SSLRequest reply failed");
            sql_net_close(&c->net);
            return false;
        }
        if (resp == 'S') {
            SockutilTlsClientOpts topts = {0};
            topts.verify_peer  = opts->tls_verify;
            topts.ca_pem       = opts->tls_ca_pem;
            topts.ca_pem_len   = opts->tls_ca_pem ? (uint32_t)strlen(opts->tls_ca_pem) : 0;
            topts.cert_pem     = opts->tls_client_cert;
            topts.cert_pem_len = opts->tls_client_cert ? (uint32_t)strlen(opts->tls_client_cert) : 0;
            topts.key_pem      = opts->tls_client_key;
            topts.key_pem_len  = opts->tls_client_key ? (uint32_t)strlen(opts->tls_client_key) : 0;
            c->net.ctx = sockutil_build_client_ssl_ctx(&topts, err, sizeof(err));
            if (!c->net.ctx) {
                pg_set_error(c, 0, "08006",
                    "postgres: TLS context init failed: %s", err);
                sql_net_close(&c->net);
                return false;
            }
            c->net.ssl = sockutil_tls_wrap(c->net.fd, c->net.ctx,
                                           true, opts->host,
                                           err, sizeof(err));
            if (!c->net.ssl) {
                pg_set_error(c, 0, "08006",
                    "postgres: TLS handshake failed: %s", err);
                sql_net_close(&c->net);
                return false;
            }
        } else if (resp == 'N') {
            /* Server refused TLS.  Caller asked for it; surface error. */
            pg_set_error(c, 0, "08006",
                "postgres: server refused SSL (set tls=FALSE to connect plain)");
            sql_net_close(&c->net);
            return false;
        } else {
            pg_set_error(c, 0, "08006",
                "postgres: unexpected SSLRequest reply 0x%02x", resp);
            sql_net_close(&c->net);
            return false;
        }
    }

    /* StartupMessage: Int32 protocol-version (3 << 16 == 196608),
     * pairs of NUL-strings, terminated by a 0 byte. */
    SqlBuf sm = {0};
    sql_buf_put_be32(&sm, 196608);
    sql_buf_put_cstr(&sm, "user");
    sql_buf_put_cstr(&sm, opts->user ? opts->user : "");
    if (opts->database && *opts->database) {
        sql_buf_put_cstr(&sm, "database");
        sql_buf_put_cstr(&sm, opts->database);
    }
    if (opts->application_name && *opts->application_name) {
        sql_buf_put_cstr(&sm, "application_name");
        sql_buf_put_cstr(&sm, opts->application_name);
    }
    sql_buf_put_cstr(&sm, "client_encoding");
    sql_buf_put_cstr(&sm, "UTF8");
    sql_buf_put_u8 (&sm, 0);

    if (!pg_send_msg(c, 0, sm.data, (int)sm.len)) {
        sql_buf_free(&sm);
        sql_net_close(&c->net);
        return false;
    }
    sql_buf_free(&sm);

    /* Auth round-trip. */
    if (!pg_handle_auth(c, opts->user ? opts->user : "",
                        opts->password)) {
        sql_net_close(&c->net);
        return false;
    }

    /* Drain ParameterStatus / BackendKeyData / ReadyForQuery. */
    if (!pg_read_until_ready(c, NULL, NULL)) {
        sql_net_close(&c->net);
        return false;
    }
    return true;
}

void pg_close(PgConn *c)
{
    if (c->net.fd != SOCKUTIL_INVALID_SOCKET) {
        /* Best-effort Terminate ('X'), then close. */
        unsigned char term[5] = { 'X', 0, 0, 0, 4 };
        sql_net_send_all(&c->net, term, 5);
    }
    sql_net_close(&c->net);
}

/* =========================================================================
 * Result accumulator (used by pg_query / pg_execute_prepared)
 *
 * RowDescription -> populate columns
 * DataRow        -> append a PgRow with cells decoded as TEXT/NULL
 * CommandComplete -> capture tag + parsed affected count
 * EmptyQueryResponse -> nothing to do, ReadyForQuery follows
 * ===================================================================== */

typedef struct PgAccum {
    PgResult *res;
    bool      had_row_description;
} PgAccum;

static int64_t pg_parse_tag_count(const char *tag)
{
    /* CommandComplete tag examples:
     *   "INSERT 0 7"   -- 7 rows inserted (oid=0)
     *   "UPDATE 3"
     *   "DELETE 2"
     *   "SELECT 5"
     *   "CREATE TABLE"
     */
    const char *space = strrchr(tag, ' ');
    if (!space) return 0;
    char *end = NULL;
    long long v = strtoll(space + 1, &end, 10);
    if (end == space + 1) return 0;
    return (int64_t)v;
}

static bool pg_handle_query_msg(PgConn *c, char type,
                                const unsigned char *body, int blen,
                                void *ud)
{
    PgAccum *a = (PgAccum *)ud;
    PgResult *r = a->res;

    if (type == 'T') {
        /* RowDescription:
         *   Int16 nFields
         *   for each: String name, Int32 tableOID, Int16 colAttr,
         *             Int32 typeOID, Int16 typeLen, Int32 typeMod, Int16 format */
        SqlReader rd = sql_reader_init(body, (size_t)blen);
        int n = sql_reader_get_be16(&rd);
        if (!rd.ok || n < 0) return false;
        if (r->columns) free(r->columns);
        r->columns = (SqlColumn *)calloc((size_t)n, sizeof(SqlColumn));
        if (!r->columns && n > 0) return false;
        r->ncols = n;
        for (int i = 0; i < n; i++) {
            const char *name = sql_reader_get_cstr(&rd);
            if (!rd.ok) return false;
            strncpy(r->columns[i].name, name ? name : "",
                    sizeof(r->columns[i].name) - 1);
            sql_reader_get_be32(&rd);                 /* table OID */
            sql_reader_get_be16(&rd);                 /* col attr  */
            r->columns[i].type_oid = sql_reader_get_be32(&rd);
            r->columns[i].type_len = (int16_t)sql_reader_get_be16(&rd);
            sql_reader_get_be32(&rd);                 /* typeMod */
            sql_reader_get_be16(&rd);                 /* format  */
        }
        a->had_row_description = true;
        return true;
    }
    if (type == 'D') {
        /* DataRow: Int16 nFields, then for each: Int32 length (-1=NULL), bytes. */
        SqlReader rd = sql_reader_init(body, (size_t)blen);
        int n = sql_reader_get_be16(&rd);
        if (!rd.ok) return false;

        PgRow *new_rows = (PgRow *)realloc(r->rows,
            sizeof(PgRow) * (size_t)(r->nrows + 1));
        if (!new_rows) return false;
        r->rows = new_rows;
        PgRow *row = &r->rows[r->nrows];
        memset(row, 0, sizeof(*row));

        row->ncols = n;
        row->cells = (SqlValue *)calloc((size_t)(n > 0 ? n : 1), sizeof(SqlValue));
        if (!row->cells) return false;

        /* Sum lengths first to size the arena. */
        size_t arena_size = 0;
        size_t saved_pos  = rd.pos;
        for (int i = 0; i < n; i++) {
            int32_t len = (int32_t)sql_reader_get_be32(&rd);
            if (!rd.ok) return false;
            if (len > 0) {
                arena_size += (size_t)len + 1;
                rd.pos += (size_t)len;
            }
        }
        rd.pos = saved_pos;
        rd.ok  = true;
        char *arena = NULL;
        if (arena_size > 0) {
            arena = (char *)malloc(arena_size);
            if (!arena) return false;
        }
        row->row_arena = arena;

        size_t arena_off = 0;
        for (int i = 0; i < n; i++) {
            int32_t len = (int32_t)sql_reader_get_be32(&rd);
            if (!rd.ok) return false;
            if (len < 0) {
                row->cells[i].kind = SQL_VAL_NULL;
                continue;
            }
            const unsigned char *b2 = sql_reader_get_bytes(&rd, (size_t)len);
            if (!b2 && len > 0) return false;
            char *dst = arena + arena_off;
            if (len > 0) memcpy(dst, b2, (size_t)len);
            dst[len] = '\0';
            arena_off += (size_t)len + 1;

            /* PostgreSQL text-format columns are always strings here.
             * The script layer maps numeric OIDs to numeric values. */
            row->cells[i].kind = SQL_VAL_TEXT;
            row->cells[i].data = dst;
            row->cells[i].len  = (size_t)len;
        }
        r->nrows++;
        return true;
    }
    if (type == 'C') {
        /* CommandComplete: tag\0 */
        const char *tag = (const char *)body;
        size_t tlen = strnlen(tag, (size_t)blen);
        size_t copy = tlen < sizeof(r->cmd_tag) - 1
                    ? tlen : sizeof(r->cmd_tag) - 1;
        memcpy(r->cmd_tag, tag, copy);
        r->cmd_tag[copy] = '\0';
        r->affected = pg_parse_tag_count(r->cmd_tag);
        /* Mirror into c->last_tag for exec(). */
        memcpy(c->last_tag, r->cmd_tag,
               (copy < sizeof(c->last_tag) ? copy : sizeof(c->last_tag) - 1));
        c->last_tag[(copy < sizeof(c->last_tag) ? copy : sizeof(c->last_tag) - 1)] = '\0';
        return true;
    }
    if (type == 'I') {
        /* EmptyQueryResponse */
        return true;
    }
    if (type == '1' || type == '2' || type == '3' || type == 'n') {
        /* ParseComplete / BindComplete / CloseComplete / NoData */
        return true;
    }
    if (type == 's') {
        /* PortalSuspended -- we use Execute(0) so this won't normally
         * appear, but be tolerant. */
        return true;
    }
    /* Anything else (NotificationResponse 'A', CopyInResponse 'G', etc.)
     * is unsupported in this driver -- ignore so we still reach Z. */
    (void)body; (void)blen;
    return true;
}

/* =========================================================================
 * pg_simple_exec / pg_query
 * ===================================================================== */

bool pg_simple_exec(PgConn *c, const char *sql)
{
    sql_error_clear(&c->last_err);
    SqlBuf b = {0};
    sql_buf_put_cstr(&b, sql);
    bool ok = pg_send_msg(c, 'Q', b.data, (int)b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    PgResult discard;
    pg_result_init(&discard);
    PgAccum acc = { &discard, false };
    bool done = pg_read_until_ready(c, pg_handle_query_msg, &acc);
    pg_result_free(&discard);
    return done && c->last_err.code == 0 && c->last_err.message[0] == '\0';
}

bool pg_query(PgConn *c, const char *sql, PgResult *out)
{
    sql_error_clear(&c->last_err);
    pg_result_init(out);

    SqlBuf b = {0};
    sql_buf_put_cstr(&b, sql);
    bool ok = pg_send_msg(c, 'Q', b.data, (int)b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    PgAccum acc = { out, false };
    bool done = pg_read_until_ready(c, pg_handle_query_msg, &acc);
    if (!done || c->last_err.message[0] != '\0') {
        pg_result_free(out);
        return false;
    }
    return true;
}

/* =========================================================================
 * Extended Query / Prepared Statements
 *
 * pg_prepare:        Parse + Describe(S) + Sync.  Reads ParseComplete,
 *                    ParameterDescription, and either RowDescription or
 *                    NoData.  Stores nothing server-side in our struct;
 *                    the server name is the prepared id.
 *
 * pg_execute_prepared: Bind + Execute + Sync.  Captures rows.
 *
 * Parameters are sent as TEXT format (format code 0).  This avoids
 * having to encode binary representations for every PostgreSQL type
 * and matches what the server accepts for arbitrary text-conversible
 * inputs.  Numeric and bool params are stringified.
 * ===================================================================== */

static bool pg_param_to_text(const SqlParam *p, char *small_buf, size_t small_n,
                             const char **out_data, size_t *out_len,
                             char **out_heap)
{
    *out_heap = NULL;
    switch (p->kind) {
        case SQL_PARAM_NULL:
            *out_data = NULL;
            *out_len  = 0;
            return true;
        case SQL_PARAM_BOOL:
            *out_data = p->i64 ? "true" : "false";
            *out_len  = p->i64 ? 4 : 5;
            return true;
        case SQL_PARAM_INT64: {
            int n = snprintf(small_buf, small_n, "%lld", (long long)p->i64);
            if (n < 0 || (size_t)n >= small_n) return false;
            *out_data = small_buf;
            *out_len  = (size_t)n;
            return true;
        }
        case SQL_PARAM_DOUBLE: {
            int n = snprintf(small_buf, small_n, "%.17g", p->f64);
            if (n < 0 || (size_t)n >= small_n) return false;
            *out_data = small_buf;
            *out_len  = (size_t)n;
            return true;
        }
        case SQL_PARAM_TEXT:
        case SQL_PARAM_BLOB:
            *out_data = p->data;
            *out_len  = p->len;
            return true;
    }
    return false;
}

bool pg_prepare(PgConn *c, const char *sql,
                char **out_name, PgPreparedDesc *desc)
{
    sql_error_clear(&c->last_err);
    desc->columns = NULL;
    desc->ncols   = 0;

    char name[32];
    snprintf(name, sizeof(name), "cdo_pg_%d", ++c->prepared_id_seq);

    /* Parse: name\0, sql\0, Int16 nParamTypes (0). */
    SqlBuf p = {0};
    sql_buf_put_cstr(&p, name);
    sql_buf_put_cstr(&p, sql);
    sql_buf_put_be16(&p, 0);
    bool ok = pg_send_msg(c, 'P', p.data, (int)p.len);
    sql_buf_free(&p);
    if (!ok) return false;

    /* Describe: 'S' name\0  (statement, not portal) */
    SqlBuf d = {0};
    sql_buf_put_u8(&d, 'S');
    sql_buf_put_cstr(&d, name);
    ok = pg_send_msg(c, 'D', d.data, (int)d.len);
    sql_buf_free(&d);
    if (!ok) return false;

    /* Sync */
    if (!pg_send_msg(c, 'S', NULL, 0)) return false;

    /* Read until ReadyForQuery, capturing RowDescription if any. */
    PgResult tmp;
    pg_result_init(&tmp);
    PgAccum acc = { &tmp, false };
    bool done = pg_read_until_ready(c, pg_handle_query_msg, &acc);
    if (!done || c->last_err.message[0] != '\0') {
        pg_result_free(&tmp);
        /* Best-effort cleanup of any half-prepared stmt. */
        return false;
    }

    /* Move column metadata into desc; ownership transfers. */
    desc->columns = tmp.columns;
    desc->ncols   = tmp.ncols;
    tmp.columns   = NULL;
    tmp.ncols     = 0;
    pg_result_free(&tmp);

    *out_name = strdup(name);
    return *out_name != NULL;
}

bool pg_execute_prepared(PgConn *c, const char *stmt_name,
                         const SqlParam *params, int nparams,
                         PgResult *out)
{
    sql_error_clear(&c->last_err);
    pg_result_init(out);

    /* Bind:
     *   String portal  ("")
     *   String stmt    (stmt_name)
     *   Int16 nParamFormats (0 -> all text)
     *   Int16 nParams
     *   for each: Int32 len (-1=NULL), bytes
     *   Int16 nResultFormats (0 -> all text)
     */
    SqlBuf bind = {0};
    sql_buf_put_cstr(&bind, "");
    sql_buf_put_cstr(&bind, stmt_name);
    sql_buf_put_be16(&bind, 0);                  /* all text */
    sql_buf_put_be16(&bind, (uint16_t)nparams);

    char        *heap_bufs[16] = {0};
    int          heap_idx = 0;
    char         small[32][32];                  /* per-param scratch */

    for (int i = 0; i < nparams; i++) {
        const char *data = NULL;
        size_t      len  = 0;
        char       *heap = NULL;
        if (!pg_param_to_text(&params[i], small[i % 32], sizeof(small[0]),
                              &data, &len, &heap)) {
            sql_buf_free(&bind);
            for (int j = 0; j < heap_idx; j++) free(heap_bufs[j]);
            pg_set_error(c, 0, "22023",
                "postgres: failed to encode parameter %d", i + 1);
            return false;
        }
        if (params[i].kind == SQL_PARAM_NULL) {
            sql_buf_put_be32(&bind, 0xffffffffu);   /* -1 */
        } else {
            sql_buf_put_be32(&bind, (uint32_t)len);
            sql_buf_put_bytes(&bind, data, len);
        }
        if (heap && heap_idx < (int)(sizeof(heap_bufs)/sizeof(heap_bufs[0]))) {
            heap_bufs[heap_idx++] = heap;
        }
    }
    sql_buf_put_be16(&bind, 0);                  /* all-text result */

    bool ok = pg_send_msg(c, 'B', bind.data, (int)bind.len);
    sql_buf_free(&bind);
    for (int j = 0; j < heap_idx; j++) free(heap_bufs[j]);
    if (!ok) return false;

    /* Execute: portal\0, Int32 maxRows (0 = no limit) */
    SqlBuf ex = {0};
    sql_buf_put_cstr(&ex, "");
    sql_buf_put_be32(&ex, 0);
    if (!pg_send_msg(c, 'E', ex.data, (int)ex.len)) {
        sql_buf_free(&ex);
        return false;
    }
    sql_buf_free(&ex);

    /* Sync */
    if (!pg_send_msg(c, 'S', NULL, 0)) return false;

    PgAccum acc = { out, false };
    bool done = pg_read_until_ready(c, pg_handle_query_msg, &acc);
    if (!done || c->last_err.message[0] != '\0') {
        pg_result_free(out);
        return false;
    }
    return true;
}

bool pg_close_prepared(PgConn *c, const char *stmt_name)
{
    sql_error_clear(&c->last_err);
    SqlBuf b = {0};
    sql_buf_put_u8(&b, 'S');
    sql_buf_put_cstr(&b, stmt_name);
    bool ok = pg_send_msg(c, 'C', b.data, (int)b.len);
    sql_buf_free(&b);
    if (!ok) return false;
    if (!pg_send_msg(c, 'S', NULL, 0)) return false;
    PgResult tmp;
    pg_result_init(&tmp);
    PgAccum acc = { &tmp, false };
    bool done = pg_read_until_ready(c, pg_handle_query_msg, &acc);
    pg_result_free(&tmp);
    return done;
}
