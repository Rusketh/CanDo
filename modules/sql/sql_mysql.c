/*
 * modules/sql/sql_mysql.c -- MySQL / MariaDB client protocol driver.
 *
 * Implements the subset of the protocol the script-facing module
 * exercises:
 *
 *   - Initial handshake, capability negotiation, optional TLS upgrade
 *   - mysql_native_password authentication
 *   - caching_sha2_password authentication (fast-auth path; full-auth
 *     requires either TLS or "allow plain over local socket")
 *   - COM_QUERY                                 -- text protocol query
 *   - COM_STMT_PREPARE / COM_STMT_EXECUTE / COM_STMT_CLOSE / COM_PING
 *   - OK / ERR / EOF packet decoding
 *   - Result-set parsing (text + binary)
 *
 * Packet framing:
 *   Every packet is <length:le24><seq:u8><payload>.
 *   The seq counter resets to 0 at the start of each command.
 *
 * All I/O is synchronous; scripts compose concurrency via Cando's
 * `thread { ... }` syntax.
 */

#define _POSIX_C_SOURCE 200809L

#include "sql_mysql.h"
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

static void my_set_error(MyConn *c, int code, const char *sqlstate,
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

/* =========================================================================
 * Packet framing
 *
 * MySQL packets are <le24 length><u8 seq><payload>.  The seq counter
 * resets to 0 at the start of each command and increments by 1 per
 * packet sent or received.  Packets longer than 16MB - 1 are split into
 * multiple frames; we don't implement multi-frame packets here (the
 * server doesn't send them for typical queries).
 * ===================================================================== */

static bool my_send_packet(MyConn *c, const void *payload, size_t len)
{
    if (len > 0xffffff) {
        my_set_error(c, 0, "08S01",
            "mysql: packet larger than 16MB not supported");
        return false;
    }
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(len);
    hdr[1] = (unsigned char)(len >> 8);
    hdr[2] = (unsigned char)(len >> 16);
    hdr[3] = c->seq++;
    if (!sql_net_send_all(&c->net, hdr, 4)) {
        my_set_error(c, 0, "08S01", "mysql: header send failed");
        return false;
    }
    if (len && !sql_net_send_all(&c->net, payload, len)) {
        my_set_error(c, 0, "08S01", "mysql: payload send failed");
        return false;
    }
    return true;
}

/* my_recv_packet -- read one packet; allocate the payload.  *out_payload
 * is NULL when the payload has zero length.  Caller frees. */
static bool my_recv_packet(MyConn *c, unsigned char **out_payload,
                           size_t *out_len)
{
    unsigned char hdr[4];
    if (!sql_net_recv_exact(&c->net, hdr, 4)) {
        my_set_error(c, 0, "08S01",
            "mysql: connection lost while reading header");
        return false;
    }
    size_t len = (size_t)hdr[0]
               | ((size_t)hdr[1] << 8)
               | ((size_t)hdr[2] << 16);
    c->seq = hdr[3] + 1;
    unsigned char *p = NULL;
    if (len) {
        p = (unsigned char *)malloc(len);
        if (!p) {
            my_set_error(c, 0, "08S01", "mysql: out of memory");
            return false;
        }
        if (!sql_net_recv_exact(&c->net, p, len)) {
            free(p);
            my_set_error(c, 0, "08S01",
                "mysql: connection lost while reading payload");
            return false;
        }
    }
    *out_payload = p;
    *out_len     = len;
    return true;
}

/* =========================================================================
 * OK / ERR / EOF packet decoders
 *
 * The first byte of a packet identifies its kind:
 *   0x00          OK packet (or AuthMoreData when 0x01 in some contexts)
 *   0xff          ERR packet
 *   0xfe          EOF packet (length < 9) or, when CLIENT_DEPRECATE_EOF
 *                 is negotiated, an OK packet with a 0xfe header
 * ===================================================================== */

static bool my_decode_err(MyConn *c, const unsigned char *p, size_t n)
{
    /* ERR packet:
     *   0xff
     *   le16 error_code
     *   if CLIENT_PROTOCOL_41:
     *     '#' sql_state_marker (1 byte) + sql_state (5 bytes)
     *   string<EOF> error_message
     */
    if (n < 3) {
        my_set_error(c, 0, "HY000", "mysql: malformed ERR packet");
        return false;
    }
    int errno_ = p[1] | (p[2] << 8);
    size_t msg_off = 3;
    char sqlstate[8] = {0};
    if (n >= 9 && p[3] == '#') {
        memcpy(sqlstate, p + 4, 5);
        sqlstate[5] = '\0';
        msg_off = 9;
    }
    char msg[400];
    size_t mlen = n > msg_off ? n - msg_off : 0;
    if (mlen >= sizeof(msg)) mlen = sizeof(msg) - 1;
    memcpy(msg, p + msg_off, mlen);
    msg[mlen] = '\0';
    my_set_error(c, errno_, sqlstate[0] ? sqlstate : NULL,
                 "mysql error %d: %s", errno_, msg);
    return false;
}

/* my_decode_ok -- payload begins with 0x00 (or 0xfe when DEPRECATE_EOF).
 * Reads affected_rows, last_insert_id, status, warnings. */
static bool my_decode_ok(MyConn *c, const unsigned char *p, size_t n)
{
    SqlReader r = sql_reader_init(p, n);
    sql_reader_get_u8(&r);                    /* header */
    bool is_null = false;
    c->last_affected  = sql_reader_get_lenenc(&r, &is_null);
    c->last_insert_id = sql_reader_get_lenenc(&r, &is_null);
    if (c->client_caps & MY_CAP_PROTOCOL_41) {
        c->last_status   = sql_reader_get_le16(&r);
        c->last_warnings = sql_reader_get_le16(&r);
    } else if (c->client_caps & MY_CAP_TRANSACTIONS) {
        c->last_status   = sql_reader_get_le16(&r);
    }
    return r.ok;
}

/* Header byte tests. */
static inline bool my_pkt_is_ok (const unsigned char *p, size_t n)
{
    /* OK:  header 0x00  AND length >= 7
     *      header 0xfe (DEPRECATE_EOF) AND length >= 7 (so it isn't EOF) */
    if (n == 0) return false;
    if (p[0] == 0x00 && n >= 7) return true;
    if (p[0] == 0xfe && n >= 7) return true;       /* OK in DEPRECATE_EOF */
    return false;
}
static inline bool my_pkt_is_err(const unsigned char *p, size_t n)
{
    return n > 0 && p[0] == 0xff;
}
static inline bool my_pkt_is_eof(const unsigned char *p, size_t n)
{
    /* EOF: header 0xfe AND length < 9 */
    return n > 0 && p[0] == 0xfe && n < 9;
}

/* =========================================================================
 * Authentication
 *
 * Initial handshake (server -> client):
 *   u8 protocol_version           (10)
 *   string<NUL> server_version
 *   u32 thread_id
 *   string<8> auth_plugin_data_part_1
 *   u8 filler 0
 *   u16 capability_flags_lower
 *   u8 charset
 *   u16 status_flags
 *   u16 capability_flags_upper
 *   u8 auth_plugin_data_len  (or 0)
 *   string<10> reserved (zeros)
 *   string<MAX(13, len-8)> auth_plugin_data_part_2 (NUL-terminated)
 *   string<NUL> auth_plugin_name
 *
 * The salt is auth_plugin_data_part_1 (8 bytes) + part_2 (12 bytes;
 * part_2 is 13 bytes long but the last byte is a NUL terminator and
 * NOT included in the SHA inputs).
 * ===================================================================== */

static bool my_compute_native_password(const char *password,
                                       const unsigned char *salt /* 20 */,
                                       unsigned char out[SQL_SHA1_LEN])
{
    /* token = SHA1(password) XOR SHA1(salt + SHA1(SHA1(password))) */
    unsigned char h1[SQL_SHA1_LEN];
    unsigned char h2[SQL_SHA1_LEN];
    unsigned char h3[SQL_SHA1_LEN];
    if (!sql_sha1(password, strlen(password), h1)) return false;
    if (!sql_sha1(h1, SQL_SHA1_LEN, h2))           return false;

    unsigned char buf[20 + SQL_SHA1_LEN];
    memcpy(buf, salt, 20);
    memcpy(buf + 20, h2, SQL_SHA1_LEN);
    if (!sql_sha1(buf, sizeof(buf), h3)) return false;

    for (int i = 0; i < SQL_SHA1_LEN; i++) out[i] = h1[i] ^ h3[i];
    return true;
}

static bool my_compute_caching_sha2(const char *password,
                                    const unsigned char *salt /* 20 */,
                                    unsigned char out[SQL_SHA256_LEN])
{
    /* token = SHA256(password) XOR SHA256(SHA256(SHA256(password)) + salt) */
    unsigned char h1[SQL_SHA256_LEN];
    unsigned char h2[SQL_SHA256_LEN];
    unsigned char h3[SQL_SHA256_LEN];
    if (!sql_sha256(password, strlen(password), h1)) return false;
    if (!sql_sha256(h1, SQL_SHA256_LEN, h2))         return false;

    unsigned char buf[SQL_SHA256_LEN + 20];
    memcpy(buf, h2, SQL_SHA256_LEN);
    memcpy(buf + SQL_SHA256_LEN, salt, 20);
    if (!sql_sha256(buf, sizeof(buf), h3)) return false;

    for (int i = 0; i < SQL_SHA256_LEN; i++) out[i] = h1[i] ^ h3[i];
    return true;
}

/* Parse the initial handshake; populate c with capabilities and
 * server-version, write the 20-byte auth challenge to *salt and the
 * server's preferred plugin name to *plugin_out. */
static bool my_parse_handshake(MyConn *c,
                               const unsigned char *p, size_t n,
                               unsigned char salt[20],
                               char *plugin_out, size_t plugin_max)
{
    SqlReader r = sql_reader_init(p, n);
    int proto = sql_reader_get_u8(&r);
    if (proto != 10) {
        my_set_error(c, 0, "08001",
            "mysql: unsupported protocol version %d (expected 10)", proto);
        return false;
    }
    c->protocol_version = proto;

    const char *sv = sql_reader_get_cstr(&r);
    if (sv) {
        strncpy(c->server_version, sv, sizeof(c->server_version) - 1);
        c->server_version[sizeof(c->server_version) - 1] = '\0';
    }
    c->server_thread_id = (int)sql_reader_get_le32(&r);

    /* Auth plugin data part 1: 8 bytes */
    const unsigned char *p1 = sql_reader_get_bytes(&r, 8);
    if (!p1) goto malformed;
    memcpy(salt, p1, 8);
    sql_reader_get_u8(&r);                           /* filler */

    uint16_t cap_low  = sql_reader_get_le16(&r);
    if (!r.ok) goto malformed;

    if (r.pos < n) {
        c->charset       = sql_reader_get_u8(&r);
        sql_reader_get_le16(&r);                     /* status flags */
        uint16_t cap_high = sql_reader_get_le16(&r);
        c->server_caps    = ((uint32_t)cap_high << 16) | cap_low;

        uint8_t auth_data_len = sql_reader_get_u8(&r);
        sql_reader_get_bytes(&r, 10);                /* reserved */

        size_t part2_len = auth_data_len > 8 ? (size_t)auth_data_len - 8 : 13;
        if (part2_len < 13) part2_len = 13;
        const unsigned char *p2 = sql_reader_get_bytes(&r, part2_len);
        if (!p2) goto malformed;
        memcpy(salt + 8, p2, 12);                    /* drop NUL at position 12 */

        if (c->server_caps & MY_CAP_PLUGIN_AUTH) {
            const char *pn = sql_reader_get_cstr(&r);
            if (pn) {
                strncpy(plugin_out, pn, plugin_max - 1);
                plugin_out[plugin_max - 1] = '\0';
            }
        } else {
            strncpy(plugin_out, "mysql_native_password", plugin_max - 1);
            plugin_out[plugin_max - 1] = '\0';
        }
    } else {
        c->server_caps = cap_low;
        strncpy(plugin_out, "mysql_native_password", plugin_max - 1);
        plugin_out[plugin_max - 1] = '\0';
    }
    return true;

malformed:
    my_set_error(c, 0, "08S01", "mysql: malformed handshake packet");
    return false;
}

/* Build HandshakeResponse41 payload. */
static bool my_build_handshake_response(MyConn *c, SqlBuf *out,
                                        const SqlConnectOpts *opts,
                                        const unsigned char *auth_response,
                                        size_t auth_len,
                                        const char *plugin_name)
{
    sql_buf_put_le32(out, c->client_caps);
    sql_buf_put_le32(out, 0x01000000);                 /* max_packet 16MB */
    sql_buf_put_u8 (out, c->charset ? c->charset : 45 /* utf8mb4_general_ci */);
    for (int i = 0; i < 23; i++) sql_buf_put_u8(out, 0);  /* reserved */
    sql_buf_put_cstr(out, opts->user ? opts->user : "");

    if (c->server_caps & MY_CAP_PLUGIN_AUTH_LENENC) {
        sql_buf_put_lenenc(out, (uint64_t)auth_len);
        sql_buf_put_bytes(out, auth_response, auth_len);
    } else if (c->server_caps & MY_CAP_SECURE_CONNECTION) {
        sql_buf_put_u8(out, (uint8_t)auth_len);
        sql_buf_put_bytes(out, auth_response, auth_len);
    } else {
        sql_buf_put_bytes(out, auth_response, auth_len);
        sql_buf_put_u8(out, 0);
    }

    if ((c->client_caps & MY_CAP_CONNECT_WITH_DB) && opts->database
        && *opts->database) {
        sql_buf_put_cstr(out, opts->database);
    }
    if (c->client_caps & MY_CAP_PLUGIN_AUTH) {
        sql_buf_put_cstr(out, plugin_name);
    }
    return true;
}

/* Compute auth response for the named plugin. */
static bool my_make_auth_response(MyConn *c,
                                  const char *plugin,
                                  const char *password,
                                  const unsigned char salt[20],
                                  unsigned char *out, size_t out_max,
                                  size_t *out_len)
{
    if (!password || password[0] == '\0') {
        *out_len = 0;
        return true;
    }
    if (strcmp(plugin, "mysql_native_password") == 0) {
        if (out_max < SQL_SHA1_LEN) return false;
        if (!my_compute_native_password(password, salt, out)) return false;
        *out_len = SQL_SHA1_LEN;
        return true;
    }
    if (strcmp(plugin, "caching_sha2_password") == 0) {
        if (out_max < SQL_SHA256_LEN) return false;
        if (!my_compute_caching_sha2(password, salt, out)) return false;
        *out_len = SQL_SHA256_LEN;
        return true;
    }
    /* Fallback: plain old empty response (server will fail us). */
    my_set_error(c, 0, "08004",
        "mysql: unsupported auth plugin '%s'", plugin);
    return false;
}

/* =========================================================================
 * my_connect / my_close
 * ===================================================================== */

static bool my_do_tls(MyConn *c, const SqlConnectOpts *opts)
{
    /* Send SSLRequest -- a HandshakeResponse41 with no auth/user/db. */
    SqlBuf b = {0};
    sql_buf_put_le32(&b, c->client_caps);
    sql_buf_put_le32(&b, 0x01000000);
    sql_buf_put_u8 (&b, c->charset ? c->charset : 45);
    for (int i = 0; i < 23; i++) sql_buf_put_u8(&b, 0);
    bool ok = my_send_packet(c, b.data, b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    char err[128] = {0};
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
        my_set_error(c, 0, "08S01",
            "mysql: TLS context init failed: %s", err);
        return false;
    }
    c->net.ssl = sockutil_tls_wrap(c->net.fd, c->net.ctx, true,
                                   opts->host, err, sizeof(err));
    if (!c->net.ssl) {
        my_set_error(c, 0, "08S01",
            "mysql: TLS handshake failed: %s", err);
        return false;
    }
    return true;
}

bool my_connect(MyConn *c, const SqlConnectOpts *opts)
{
    sql_error_clear(&c->last_err);
    sql_net_init(&c->net);
    c->seq = 0;
    c->server_caps = 0;
    c->client_caps = 0;
    c->charset = 45;          /* utf8mb4_general_ci */
    c->protocol_version = 0;
    c->server_version[0] = '\0';
    c->server_thread_id = 0;
    c->last_affected = c->last_insert_id = 0;
    c->last_status = c->last_warnings = 0;

    sockutil_one_time_init();

    char err[128] = {0};
    int port = opts->port > 0 ? opts->port : 3306;
    c->net.fd = sockutil_tcp_connect(opts->host ? opts->host : "127.0.0.1",
                                     port, 0,
                                     opts->connect_timeout_ms,
                                     err, sizeof(err));
    if (c->net.fd == SOCKUTIL_INVALID_SOCKET) {
        my_set_error(c, 0, "08001",
            "mysql: connect to %s:%d failed%s%s",
            opts->host ? opts->host : "127.0.0.1", port,
            err[0] ? " - " : "", err);
        return false;
    }
    sockutil_set_nodelay(c->net.fd, true);
    if (opts->io_timeout_ms > 0)
        sockutil_set_timeout(c->net.fd, opts->io_timeout_ms);
    c->net.io_timeout_ms = opts->io_timeout_ms;

    /* Read initial handshake. */
    unsigned char *pkt = NULL;
    size_t         plen = 0;
    if (!my_recv_packet(c, &pkt, &plen)) {
        sql_net_close(&c->net);
        return false;
    }
    if (my_pkt_is_err(pkt, plen)) {
        my_decode_err(c, pkt, plen);
        free(pkt);
        sql_net_close(&c->net);
        return false;
    }
    unsigned char salt[20] = {0};
    char plugin[64] = "mysql_native_password";
    bool ok = my_parse_handshake(c, pkt, plen, salt, plugin, sizeof(plugin));
    free(pkt);
    if (!ok) { sql_net_close(&c->net); return false; }

    /* Negotiate client capabilities. */
    c->client_caps = MY_CAP_LONG_PASSWORD | MY_CAP_LONG_FLAG
                   | MY_CAP_PROTOCOL_41   | MY_CAP_TRANSACTIONS
                   | MY_CAP_SECURE_CONNECTION
                   | MY_CAP_PLUGIN_AUTH
                   | MY_CAP_PLUGIN_AUTH_LENENC
                   | MY_CAP_DEPRECATE_EOF;
    if (opts->database && *opts->database)
        c->client_caps |= MY_CAP_CONNECT_WITH_DB;
    if (opts->tls && (c->server_caps & MY_CAP_SSL))
        c->client_caps |= MY_CAP_SSL;
    /* Mask client caps to those advertised by the server. */
    c->client_caps &= (c->server_caps | MY_CAP_PROTOCOL_41 | MY_CAP_LONG_PASSWORD);

    /* TLS upgrade if requested. */
    if (opts->tls) {
        if (!(c->server_caps & MY_CAP_SSL)) {
            my_set_error(c, 0, "08S01", "mysql: server does not support TLS");
            sql_net_close(&c->net);
            return false;
        }
        if (!my_do_tls(c, opts)) {
            sql_net_close(&c->net);
            return false;
        }
    }

    /* Build HandshakeResponse41. */
    unsigned char auth_resp[64];
    size_t        auth_len = 0;
    if (!my_make_auth_response(c, plugin, opts->password, salt,
                               auth_resp, sizeof(auth_resp), &auth_len)) {
        sql_net_close(&c->net);
        return false;
    }
    SqlBuf hr = {0};
    my_build_handshake_response(c, &hr, opts, auth_resp, auth_len, plugin);
    ok = my_send_packet(c, hr.data, hr.len);
    sql_buf_free(&hr);
    if (!ok) { sql_net_close(&c->net); return false; }

    /* Read auth result loop.  Possible packets:
     *   OK              -> success
     *   ERR             -> failure
     *   AuthSwitch (0xfe + plugin\0 + salt) -> recompute and resend
     *   AuthMoreData (0x01 + ...) -> caching_sha2_password fast/full
     */
    for (int round = 0; round < 4; round++) {
        if (!my_recv_packet(c, &pkt, &plen)) {
            sql_net_close(&c->net);
            return false;
        }
        if (my_pkt_is_err(pkt, plen)) {
            my_decode_err(c, pkt, plen);
            free(pkt);
            sql_net_close(&c->net);
            return false;
        }
        if (my_pkt_is_ok(pkt, plen)) {
            my_decode_ok(c, pkt, plen);
            free(pkt);
            return true;
        }
        if (plen >= 1 && pkt[0] == 0xfe) {
            /* AuthSwitchRequest:
             *   0xfe, plugin\0, plugin_data\0  (or short: 0xfe == old-pwd) */
            const char    *new_plugin = (const char *)(pkt + 1);
            size_t pname_len = strnlen(new_plugin, plen - 1);
            if (pname_len + 1 >= plen) {
                free(pkt);
                my_set_error(c, 0, "08S01",
                    "mysql: malformed AuthSwitchRequest");
                sql_net_close(&c->net);
                return false;
            }
            const unsigned char *new_salt = pkt + 1 + pname_len + 1;
            size_t ns = plen - (1 + pname_len + 1);
            unsigned char salt2[20] = {0};
            if (ns >= 20) memcpy(salt2, new_salt, 20);
            else if (ns > 0) memcpy(salt2, new_salt, ns);
            strncpy(plugin, new_plugin, sizeof(plugin) - 1);
            plugin[sizeof(plugin) - 1] = '\0';
            free(pkt);

            unsigned char resp[64];
            size_t        rlen = 0;
            if (!my_make_auth_response(c, plugin, opts->password,
                                       salt2, resp, sizeof(resp), &rlen)) {
                sql_net_close(&c->net);
                return false;
            }
            if (!my_send_packet(c, resp, rlen)) {
                sql_net_close(&c->net);
                return false;
            }
            continue;
        }
        if (plen >= 1 && pkt[0] == 0x01) {
            /* AuthMoreData -- caching_sha2_password protocol. */
            if (plen >= 2 && pkt[1] == 0x03) {
                /* fast_auth_success -- next packet is OK. */
                free(pkt);
                continue;
            }
            if (plen >= 2 && pkt[1] == 0x04) {
                /* full authentication required. */
                free(pkt);
                if (c->net.ssl) {
                    /* Over TLS we may send the password in cleartext. */
                    size_t pl = strlen(opts->password) + 1;
                    if (!my_send_packet(c, opts->password, pl)) {
                        sql_net_close(&c->net);
                        return false;
                    }
                    continue;
                }
                my_set_error(c, 0, "08004",
                    "mysql: caching_sha2_password requires TLS for full auth");
                sql_net_close(&c->net);
                return false;
            }
            free(pkt);
            my_set_error(c, 0, "08004",
                "mysql: unexpected AuthMoreData payload");
            sql_net_close(&c->net);
            return false;
        }
        free(pkt);
        my_set_error(c, 0, "08004",
            "mysql: unexpected packet during auth");
        sql_net_close(&c->net);
        return false;
    }
    my_set_error(c, 0, "08004", "mysql: auth did not complete");
    sql_net_close(&c->net);
    return false;
}

void my_close(MyConn *c)
{
    if (c->net.fd != SOCKUTIL_INVALID_SOCKET) {
        c->seq = 0;
        unsigned char quit = 0x01;        /* COM_QUIT */
        my_send_packet(c, &quit, 1);
    }
    sql_net_close(&c->net);
}

/* =========================================================================
 * Result lifecycle
 * ===================================================================== */

void my_result_init(MyResult *r) { memset(r, 0, sizeof(*r)); }

void my_result_free(MyResult *r)
{
    if (r->columns) free(r->columns);
    for (int i = 0; i < r->nrows; i++) {
        free(r->rows[i].cells);
        free(r->rows[i].row_arena);
    }
    free(r->rows);
    memset(r, 0, sizeof(*r));
}

void my_stmt_free(MyStmt *st)
{
    if (st->param_columns)  free(st->param_columns);
    if (st->result_columns) free(st->result_columns);
    memset(st, 0, sizeof(*st));
}

/* =========================================================================
 * ColumnDefinition41 -- compact form of the column metadata packet.
 *
 *   string<lenenc> catalog
 *   string<lenenc> schema
 *   string<lenenc> table
 *   string<lenenc> org_table
 *   string<lenenc> name
 *   string<lenenc> org_name
 *   u8 next-length (always 0x0c)
 *   le16 character_set
 *   le32 column_length
 *   u8 type
 *   le16 flags
 *   u8 decimals
 *   u16 filler
 * ===================================================================== */

static bool my_decode_lenenc_str(SqlReader *r, const char **out_data,
                                 size_t *out_len)
{
    bool is_null = false;
    uint64_t n = sql_reader_get_lenenc(r, &is_null);
    if (!r->ok) return false;
    if (is_null) { *out_data = NULL; *out_len = 0; return true; }
    const unsigned char *p = sql_reader_get_bytes(r, (size_t)n);
    if (!p && n > 0) return false;
    *out_data = (const char *)p;
    *out_len  = (size_t)n;
    return true;
}

static bool my_parse_column(SqlColumn *col,
                            const unsigned char *p, size_t n)
{
    SqlReader r = sql_reader_init(p, n);
    const char *s; size_t sl;
    /* catalog, schema, table, org_table */
    for (int i = 0; i < 4; i++) {
        if (!my_decode_lenenc_str(&r, &s, &sl)) return false;
    }
    if (!my_decode_lenenc_str(&r, &s, &sl)) return false;
    size_t copy = sl < sizeof(col->name) - 1 ? sl : sizeof(col->name) - 1;
    if (sl) memcpy(col->name, s, copy);
    col->name[copy] = '\0';
    if (!my_decode_lenenc_str(&r, &s, &sl)) return false;        /* org_name */
    sql_reader_get_u8(&r);                                       /* next-len */
    sql_reader_get_le16(&r);                                     /* charset */
    col->type_len = (int)sql_reader_get_le32(&r);
    col->type_oid = sql_reader_get_u8(&r);
    col->flags    = sql_reader_get_le16(&r);
    sql_reader_get_u8(&r);                                       /* decimals */
    sql_reader_get_le16(&r);                                     /* filler */
    return r.ok;
}

/* =========================================================================
 * Read a result-set after a COM_QUERY:
 *   <column_count : lenenc>
 *   N column-def packets
 *   [EOF]                 (when !DEPRECATE_EOF)
 *   row packets...
 *   [EOF | OK]            (terminator)
 *
 * Each text-protocol row is a sequence of length-encoded strings; the
 * NULL marker is the single byte 0xfb.
 * ===================================================================== */

static bool my_read_columns(MyConn *c, int ncols,
                            SqlColumn **out_cols)
{
    SqlColumn *cols = (SqlColumn *)calloc(ncols > 0 ? (size_t)ncols : 1,
                                          sizeof(SqlColumn));
    if (!cols && ncols > 0) {
        my_set_error(c, 0, "HY001", "mysql: out of memory");
        return false;
    }
    for (int i = 0; i < ncols; i++) {
        unsigned char *pkt = NULL;
        size_t         plen = 0;
        if (!my_recv_packet(c, &pkt, &plen)) { free(cols); return false; }
        if (my_pkt_is_err(pkt, plen)) {
            my_decode_err(c, pkt, plen);
            free(pkt); free(cols);
            return false;
        }
        bool ok = my_parse_column(&cols[i], pkt, plen);
        free(pkt);
        if (!ok) {
            my_set_error(c, 0, "HY000", "mysql: bad column-def packet");
            free(cols);
            return false;
        }
    }
    /* If the server doesn't deprecate EOF, swallow the EOF terminator
     * after the column definitions. */
    if (!(c->client_caps & MY_CAP_DEPRECATE_EOF)) {
        unsigned char *pkt = NULL;
        size_t plen = 0;
        if (!my_recv_packet(c, &pkt, &plen)) { free(cols); return false; }
        if (my_pkt_is_err(pkt, plen)) {
            my_decode_err(c, pkt, plen);
            free(pkt); free(cols); return false;
        }
        free(pkt);
    }
    *out_cols = cols;
    return true;
}

/* Parse a text-protocol row.  cells point into row_arena. */
static bool my_decode_text_row(MyResult *r,
                               const unsigned char *p, size_t n)
{
    int ncols = r->ncols;
    MyRow *new_rows = (MyRow *)realloc(r->rows,
        sizeof(MyRow) * (size_t)(r->nrows + 1));
    if (!new_rows) return false;
    r->rows = new_rows;
    MyRow *row = &r->rows[r->nrows];
    memset(row, 0, sizeof(*row));
    row->ncols = ncols;
    row->cells = (SqlValue *)calloc(ncols > 0 ? (size_t)ncols : 1,
                                    sizeof(SqlValue));
    if (!row->cells) return false;

    SqlReader rd = sql_reader_init(p, n);

    /* Two-pass: size the arena. */
    size_t arena_size = 0;
    {
        SqlReader scan = rd;
        for (int i = 0; i < ncols; i++) {
            const char *s; size_t sl;
            bool is_null = false;
            uint8_t b = scan.data[scan.pos];
            if (b == 0xfb) { scan.pos++; continue; }
            if (!my_decode_lenenc_str(&scan, &s, &sl)) return false;
            (void)is_null;
            arena_size += sl + 1;
        }
    }
    char *arena = NULL;
    if (arena_size > 0) {
        arena = (char *)malloc(arena_size);
        if (!arena) return false;
    }
    row->row_arena = arena;
    size_t off = 0;

    for (int i = 0; i < ncols; i++) {
        if (!sql_reader_have(&rd, 1)) return false;
        if (rd.data[rd.pos] == 0xfb) {
            rd.pos++;
            row->cells[i].kind = SQL_VAL_NULL;
            continue;
        }
        const char *s; size_t sl;
        if (!my_decode_lenenc_str(&rd, &s, &sl)) return false;
        char *dst = arena + off;
        if (sl) memcpy(dst, s, sl);
        dst[sl] = '\0';
        off += sl + 1;
        row->cells[i].kind = SQL_VAL_TEXT;
        row->cells[i].data = dst;
        row->cells[i].len  = sl;
    }
    r->nrows++;
    return true;
}

static bool my_read_text_rows(MyConn *c, MyResult *out)
{
    for (;;) {
        unsigned char *pkt = NULL;
        size_t         plen = 0;
        if (!my_recv_packet(c, &pkt, &plen)) return false;
        if (my_pkt_is_err(pkt, plen)) {
            my_decode_err(c, pkt, plen);
            free(pkt);
            return false;
        }
        /* Terminator: OK (when DEPRECATE_EOF) or EOF.  0xfe with len<9
         * is the legacy EOF; 0xfe with len>=7 in DEPRECATE_EOF is OK. */
        if ((pkt[0] == 0xfe && plen < 9) || my_pkt_is_ok(pkt, plen)) {
            if (my_pkt_is_ok(pkt, plen)) my_decode_ok(c, pkt, plen);
            free(pkt);
            return true;
        }
        bool ok = my_decode_text_row(out, pkt, plen);
        free(pkt);
        if (!ok) {
            my_set_error(c, 0, "HY000", "mysql: failed to decode row");
            return false;
        }
    }
}

bool my_query(MyConn *c, const char *sql, MyResult *out)
{
    sql_error_clear(&c->last_err);
    my_result_init(out);
    c->seq = 0;
    /* COM_QUERY: 0x03 + sql */
    SqlBuf b = {0};
    sql_buf_put_u8(&b, 0x03);
    sql_buf_put_bytes(&b, sql, strlen(sql));
    bool ok = my_send_packet(c, b.data, b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    /* First reply: column-count, OK, ERR, or LOCAL INFILE (0xfb). */
    unsigned char *pkt = NULL;
    size_t plen = 0;
    if (!my_recv_packet(c, &pkt, &plen)) return false;
    if (my_pkt_is_err(pkt, plen)) {
        my_decode_err(c, pkt, plen);
        free(pkt);
        return false;
    }
    if (my_pkt_is_ok(pkt, plen)) {
        my_decode_ok(c, pkt, plen);
        free(pkt);
        out->affected  = c->last_affected;
        out->insert_id = c->last_insert_id;
        return true;
    }
    if (pkt[0] == 0xfb) {
        free(pkt);
        my_set_error(c, 0, "0A000",
            "mysql: LOCAL INFILE protocol is not supported");
        return false;
    }
    SqlReader r = sql_reader_init(pkt, plen);
    bool is_null = false;
    uint64_t n = sql_reader_get_lenenc(&r, &is_null);
    free(pkt);
    out->ncols = (int)n;
    if (!my_read_columns(c, out->ncols, &out->columns)) {
        my_result_free(out);
        return false;
    }
    if (!my_read_text_rows(c, out)) {
        my_result_free(out);
        return false;
    }
    out->affected  = c->last_affected;
    out->insert_id = c->last_insert_id;
    return true;
}

bool my_simple_exec(MyConn *c, const char *sql)
{
    MyResult tmp;
    bool ok = my_query(c, sql, &tmp);
    my_result_free(&tmp);
    return ok;
}

/* =========================================================================
 * Prepared statements
 *
 * COM_STMT_PREPARE response (Status 0x00):
 *   le32 statement_id
 *   le16 num_columns
 *   le16 num_params
 *   u8 reserved (0)
 *   le16 warning_count
 *   N param column-defs [+ EOF when !DEPRECATE_EOF]
 *   M result column-defs [+ EOF when !DEPRECATE_EOF]
 *
 * COM_STMT_EXECUTE: 0x17, le32 stmt_id, u8 flags(0), le32 iter(1),
 *   if num_params > 0:
 *     null-bitmap (ceil(np/8) bytes)
 *     u8 new_params_bound_flag
 *     if new_params_bound_flag == 1:
 *       per-param: u16 type_byte+unsigned_flag
 *     per non-null param: binary value
 *
 * Binary row layout:
 *   u8 0x00
 *   null-bitmap ((ncols + 7 + 2) / 8)  -- offset by 2 bits!
 *   for each non-null col: type-specific binary value
 * ===================================================================== */

bool my_prepare(MyConn *c, const char *sql, MyStmt *out)
{
    sql_error_clear(&c->last_err);
    memset(out, 0, sizeof(*out));
    c->seq = 0;

    SqlBuf b = {0};
    sql_buf_put_u8(&b, 0x16);                 /* COM_STMT_PREPARE */
    sql_buf_put_bytes(&b, sql, strlen(sql));
    bool ok = my_send_packet(c, b.data, b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    unsigned char *pkt = NULL;
    size_t plen = 0;
    if (!my_recv_packet(c, &pkt, &plen)) return false;
    if (my_pkt_is_err(pkt, plen)) {
        my_decode_err(c, pkt, plen);
        free(pkt);
        return false;
    }
    if (plen < 12 || pkt[0] != 0x00) {
        free(pkt);
        my_set_error(c, 0, "HY000", "mysql: bad PREPARE response");
        return false;
    }
    SqlReader r = sql_reader_init(pkt, plen);
    sql_reader_get_u8(&r);
    out->stmt_id = sql_reader_get_le32(&r);
    out->ncols   = sql_reader_get_le16(&r);
    out->nparams = sql_reader_get_le16(&r);
    free(pkt);

    if (out->nparams > 0) {
        if (!my_read_columns(c, out->nparams, &out->param_columns))
            return false;
    }
    if (out->ncols > 0) {
        if (!my_read_columns(c, out->ncols, &out->result_columns))
            return false;
    }
    return true;
}

/* Translate a SqlParam to (mysql_type, payload-bytes). */
static bool my_encode_param(const SqlParam *p, uint8_t *out_type,
                            SqlBuf *body, char *scratch, size_t scratch_n)
{
    switch (p->kind) {
        case SQL_PARAM_NULL:
            *out_type = MY_TYPE_NULL;
            return true;
        case SQL_PARAM_BOOL:
            *out_type = MY_TYPE_TINY;
            return sql_buf_put_u8(body, p->i64 ? 1 : 0);
        case SQL_PARAM_INT64:
            *out_type = MY_TYPE_LONGLONG;
            return sql_buf_put_le64(body, (uint64_t)p->i64);
        case SQL_PARAM_DOUBLE: {
            *out_type = MY_TYPE_DOUBLE;
            uint64_t bits;
            memcpy(&bits, &p->f64, 8);
            return sql_buf_put_le64(body, bits);
        }
        case SQL_PARAM_TEXT:
            *out_type = MY_TYPE_VAR_STRING;
            return sql_buf_put_lenenc_str(body, p->data, p->len);
        case SQL_PARAM_BLOB:
            *out_type = MY_TYPE_LONG_BLOB;
            return sql_buf_put_lenenc_str(body, p->data, p->len);
    }
    (void)scratch; (void)scratch_n;
    return false;
}

/* Decode one binary-protocol cell into `cell`, advancing rd and writing
 * any text data into arena[*off..]. */
static bool my_decode_binary_cell(SqlReader *rd, SqlValue *cell,
                                  uint8_t mtype, char *arena, size_t *off)
{
    switch (mtype) {
        case MY_TYPE_NULL:
            cell->kind = SQL_VAL_NULL;
            return true;
        case MY_TYPE_TINY: {
            uint8_t v = sql_reader_get_u8(rd);
            cell->kind = SQL_VAL_INTEGER;
            cell->i64  = (int8_t)v;
            return rd->ok;
        }
        case MY_TYPE_SHORT:
        case MY_TYPE_YEAR: {
            uint16_t v = sql_reader_get_le16(rd);
            cell->kind = SQL_VAL_INTEGER;
            cell->i64  = (int16_t)v;
            return rd->ok;
        }
        case MY_TYPE_INT24:
        case MY_TYPE_LONG: {
            uint32_t v = sql_reader_get_le32(rd);
            cell->kind = SQL_VAL_INTEGER;
            cell->i64  = (int32_t)v;
            return rd->ok;
        }
        case MY_TYPE_LONGLONG: {
            uint64_t v = sql_reader_get_le64(rd);
            cell->kind = SQL_VAL_INTEGER;
            cell->i64  = (int64_t)v;
            return rd->ok;
        }
        case MY_TYPE_FLOAT: {
            uint32_t v = sql_reader_get_le32(rd);
            float f;
            memcpy(&f, &v, 4);
            cell->kind = SQL_VAL_DOUBLE;
            cell->f64  = (double)f;
            return rd->ok;
        }
        case MY_TYPE_DOUBLE: {
            uint64_t v = sql_reader_get_le64(rd);
            double d;
            memcpy(&d, &v, 8);
            cell->kind = SQL_VAL_DOUBLE;
            cell->f64  = d;
            return rd->ok;
        }
        default: {
            /* All other types arrive as length-encoded strings. */
            const char *s; size_t sl;
            if (!my_decode_lenenc_str(rd, &s, &sl)) return false;
            char *dst = arena + *off;
            if (sl) memcpy(dst, s, sl);
            dst[sl] = '\0';
            *off += sl + 1;
            cell->kind = (mtype == MY_TYPE_TINY_BLOB
                       || mtype == MY_TYPE_MEDIUM_BLOB
                       || mtype == MY_TYPE_LONG_BLOB
                       || mtype == MY_TYPE_BLOB)
                       ? SQL_VAL_BLOB : SQL_VAL_TEXT;
            cell->data = dst;
            cell->len  = sl;
            return true;
        }
    }
}

static bool my_decode_binary_row(MyResult *r, MyStmt *st,
                                 const unsigned char *p, size_t n)
{
    if (n < 1 || p[0] != 0x00) return false;
    int ncols = st->ncols;
    int nbits = (ncols + 7 + 2) / 8;
    if ((size_t)1 + (size_t)nbits > n) return false;

    MyRow *new_rows = (MyRow *)realloc(r->rows,
        sizeof(MyRow) * (size_t)(r->nrows + 1));
    if (!new_rows) return false;
    r->rows = new_rows;
    MyRow *row = &r->rows[r->nrows];
    memset(row, 0, sizeof(*row));
    row->ncols = ncols;
    row->cells = (SqlValue *)calloc(ncols > 0 ? (size_t)ncols : 1,
                                    sizeof(SqlValue));
    if (!row->cells) return false;

    const unsigned char *nullmap = p + 1;
    SqlReader rd = sql_reader_init(p + 1 + nbits, n - 1 - nbits);

    /* Worst case: every cell is the longest text payload.  Just size the
     * arena to the entire packet body length plus per-cell NULs. */
    size_t arena_size = n + (size_t)ncols + 1;
    char *arena = (char *)malloc(arena_size);
    if (!arena) return false;
    row->row_arena = arena;
    size_t off = 0;

    for (int i = 0; i < ncols; i++) {
        int bit = i + 2;
        bool is_null = (nullmap[bit / 8] >> (bit % 8)) & 1;
        if (is_null) {
            row->cells[i].kind = SQL_VAL_NULL;
            continue;
        }
        uint8_t mtype = (uint8_t)st->result_columns[i].type_oid;
        if (!my_decode_binary_cell(&rd, &row->cells[i], mtype, arena, &off))
            return false;
    }
    r->nrows++;
    return true;
}

bool my_execute(MyConn *c, MyStmt *st,
                const SqlParam *params, int nparams,
                MyResult *out)
{
    sql_error_clear(&c->last_err);
    my_result_init(out);
    c->seq = 0;

    if (nparams != st->nparams) {
        my_set_error(c, 0, "07001",
            "mysql: parameter count mismatch (provided %d, statement expects %d)",
            nparams, (int)st->nparams);
        return false;
    }

    SqlBuf b = {0};
    sql_buf_put_u8 (&b, 0x17);                  /* COM_STMT_EXECUTE */
    sql_buf_put_le32(&b, st->stmt_id);
    sql_buf_put_u8 (&b, 0x00);                  /* CURSOR_TYPE_NO_CURSOR */
    sql_buf_put_le32(&b, 1);                    /* iteration count */

    if (nparams > 0) {
        int nbits = (nparams + 7) / 8;
        unsigned char nullmap[64] = {0};
        if (nbits > (int)sizeof(nullmap)) {
            my_set_error(c, 0, "HY000",
                "mysql: too many parameters (max %d)",
                (int)sizeof(nullmap) * 8);
            sql_buf_free(&b);
            return false;
        }
        for (int i = 0; i < nparams; i++) {
            if (params[i].kind == SQL_PARAM_NULL)
                nullmap[i / 8] |= (unsigned char)(1 << (i % 8));
        }
        sql_buf_put_bytes(&b, nullmap, (size_t)nbits);
        sql_buf_put_u8(&b, 1);                  /* new-params-bound = 1 */

        /* type bytes first */
        SqlBuf body = {0};
        char scratch[64];
        for (int i = 0; i < nparams; i++) {
            uint8_t t = MY_TYPE_NULL;
            if (params[i].kind != SQL_PARAM_NULL) {
                /* peek-encode to the body buf, but we'll do this in two
                 * passes: first the type bytes, then values. */
                switch (params[i].kind) {
                    case SQL_PARAM_BOOL:    t = MY_TYPE_TINY;       break;
                    case SQL_PARAM_INT64:   t = MY_TYPE_LONGLONG;   break;
                    case SQL_PARAM_DOUBLE:  t = MY_TYPE_DOUBLE;     break;
                    case SQL_PARAM_TEXT:    t = MY_TYPE_VAR_STRING; break;
                    case SQL_PARAM_BLOB:    t = MY_TYPE_LONG_BLOB;  break;
                    default:                t = MY_TYPE_NULL;       break;
                }
            }
            sql_buf_put_u8(&b, t);
            sql_buf_put_u8(&b, 0);              /* unsigned-flag = 0 */

            uint8_t dummy;
            if (params[i].kind == SQL_PARAM_NULL) continue;
            if (!my_encode_param(&params[i], &dummy, &body,
                                 scratch, sizeof(scratch))) {
                my_set_error(c, 0, "22023",
                    "mysql: failed to encode parameter %d", i + 1);
                sql_buf_free(&body);
                sql_buf_free(&b);
                return false;
            }
        }
        sql_buf_put_bytes(&b, body.data, body.len);
        sql_buf_free(&body);
    }

    bool ok = my_send_packet(c, b.data, b.len);
    sql_buf_free(&b);
    if (!ok) return false;

    /* First reply packet. */
    unsigned char *pkt = NULL;
    size_t plen = 0;
    if (!my_recv_packet(c, &pkt, &plen)) return false;
    if (my_pkt_is_err(pkt, plen)) {
        my_decode_err(c, pkt, plen);
        free(pkt);
        return false;
    }
    if (my_pkt_is_ok(pkt, plen)) {
        my_decode_ok(c, pkt, plen);
        free(pkt);
        out->affected  = c->last_affected;
        out->insert_id = c->last_insert_id;
        return true;
    }

    SqlReader r = sql_reader_init(pkt, plen);
    bool is_null = false;
    uint64_t ncols = sql_reader_get_lenenc(&r, &is_null);
    free(pkt);
    out->ncols = (int)ncols;

    /* Skip column-defs (we already have st->result_columns), but the
     * server still streams them.  Reuse my_read_columns into a temp,
     * then free.  Saves us implementing a "skip N defs" routine. */
    SqlColumn *throwaway = NULL;
    if (!my_read_columns(c, out->ncols, &throwaway)) {
        my_result_free(out);
        return false;
    }
    /* Move column metadata into result.  Prefer the freshly-read names
     * over the original prepared-time ones, since the binary types are
     * recorded against the cached st->result_columns already. */
    out->columns = throwaway;

    /* Read binary rows until EOF / OK terminator. */
    for (;;) {
        if (!my_recv_packet(c, &pkt, &plen)) {
            my_result_free(out);
            return false;
        }
        if (my_pkt_is_err(pkt, plen)) {
            my_decode_err(c, pkt, plen);
            free(pkt);
            my_result_free(out);
            return false;
        }
        if ((pkt[0] == 0xfe && plen < 9) || my_pkt_is_ok(pkt, plen)) {
            if (my_pkt_is_ok(pkt, plen)) my_decode_ok(c, pkt, plen);
            free(pkt);
            out->affected  = c->last_affected;
            out->insert_id = c->last_insert_id;
            return true;
        }
        bool dok = my_decode_binary_row(out, st, pkt, plen);
        free(pkt);
        if (!dok) {
            my_set_error(c, 0, "HY000", "mysql: failed to decode binary row");
            my_result_free(out);
            return false;
        }
    }
}

bool my_close_stmt(MyConn *c, MyStmt *st)
{
    if (!st->stmt_id) return true;
    c->seq = 0;
    unsigned char buf[5];
    buf[0] = 0x19;                  /* COM_STMT_CLOSE */
    buf[1] = (unsigned char)(st->stmt_id);
    buf[2] = (unsigned char)(st->stmt_id >> 8);
    buf[3] = (unsigned char)(st->stmt_id >> 16);
    buf[4] = (unsigned char)(st->stmt_id >> 24);
    /* COM_STMT_CLOSE is fire-and-forget: no reply expected. */
    bool ok = my_send_packet(c, buf, 5);
    st->stmt_id = 0;
    return ok;
}
