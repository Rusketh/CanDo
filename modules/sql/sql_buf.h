/*
 * modules/sql/sql_buf.h -- Wire-protocol buffer helpers shared by the
 * PostgreSQL and MySQL drivers.
 *
 * Both protocols require pushing typed integers and length-prefixed
 * strings into a growing byte buffer, and pulling them back from a
 * fixed slice during message decode.  The two helpers here -- SqlBuf
 * (writer) and SqlReader (reader) -- factor those primitives out so
 * each driver can express protocol-specific framing without
 * re-implementing big-/little-endian shifts and bounds checks.
 *
 * Header-only (`static inline`) so the unit tests can include it
 * directly and exercise each helper without linking the module.
 */

#ifndef CANDO_SQL_BUF_H
#define CANDO_SQL_BUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * SqlBuf -- growable byte writer.
 *
 * Construct on the stack with `SqlBuf b = {0};`, write with the
 * sql_buf_put_* helpers, and free the heap storage with sql_buf_free.
 * The caller may pass &b->data + b->len to send() once the message is
 * complete.
 * ===================================================================== */

typedef struct SqlBuf {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} SqlBuf;

static inline void sql_buf_free(SqlBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static inline bool sql_buf_reserve(SqlBuf *b, size_t need)
{
    if (b->len + need <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < b->len + need) ncap *= 2;
    unsigned char *p = (unsigned char *)realloc(b->data, ncap);
    if (!p) return false;
    b->data = p;
    b->cap  = ncap;
    return true;
}

static inline bool sql_buf_put_bytes(SqlBuf *b, const void *p, size_t n)
{
    if (!sql_buf_reserve(b, n)) return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

static inline bool sql_buf_put_u8(SqlBuf *b, uint8_t v)
{
    if (!sql_buf_reserve(b, 1)) return false;
    b->data[b->len++] = v;
    return true;
}

/* Big-endian (PostgreSQL) */
static inline bool sql_buf_put_be16(SqlBuf *b, uint16_t v)
{
    unsigned char tmp[2] = { (unsigned char)(v >> 8), (unsigned char)v };
    return sql_buf_put_bytes(b, tmp, 2);
}
static inline bool sql_buf_put_be32(SqlBuf *b, uint32_t v)
{
    unsigned char tmp[4] = {
        (unsigned char)(v >> 24), (unsigned char)(v >> 16),
        (unsigned char)(v >> 8),  (unsigned char)v };
    return sql_buf_put_bytes(b, tmp, 4);
}

/* Little-endian (MySQL) */
static inline bool sql_buf_put_le16(SqlBuf *b, uint16_t v)
{
    unsigned char tmp[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    return sql_buf_put_bytes(b, tmp, 2);
}
static inline bool sql_buf_put_le24(SqlBuf *b, uint32_t v)
{
    unsigned char tmp[3] = {
        (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16) };
    return sql_buf_put_bytes(b, tmp, 3);
}
static inline bool sql_buf_put_le32(SqlBuf *b, uint32_t v)
{
    unsigned char tmp[4] = {
        (unsigned char)v, (unsigned char)(v >> 8),
        (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return sql_buf_put_bytes(b, tmp, 4);
}
static inline bool sql_buf_put_le64(SqlBuf *b, uint64_t v)
{
    unsigned char tmp[8];
    for (int i = 0; i < 8; i++) tmp[i] = (unsigned char)(v >> (8 * i));
    return sql_buf_put_bytes(b, tmp, 8);
}

/* NUL-terminated C string -- common in PostgreSQL framing. */
static inline bool sql_buf_put_cstr(SqlBuf *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!sql_buf_put_bytes(b, s ? s : "", n)) return false;
    return sql_buf_put_u8(b, 0);
}

/*
 * sql_buf_put_lenenc -- MySQL "length-encoded integer" prefix.
 * 0..250        -> 1 byte
 * 251..2^16     -> 0xfc + 2-byte LE
 * 2^16..2^24    -> 0xfd + 3-byte LE
 * 2^24..2^64    -> 0xfe + 8-byte LE
 */
static inline bool sql_buf_put_lenenc(SqlBuf *b, uint64_t v)
{
    if (v < 251) return sql_buf_put_u8(b, (uint8_t)v);
    if (v < 0x10000ULL) {
        if (!sql_buf_put_u8(b, 0xfc)) return false;
        return sql_buf_put_le16(b, (uint16_t)v);
    }
    if (v < 0x1000000ULL) {
        if (!sql_buf_put_u8(b, 0xfd)) return false;
        return sql_buf_put_le24(b, (uint32_t)v);
    }
    if (!sql_buf_put_u8(b, 0xfe)) return false;
    return sql_buf_put_le64(b, v);
}

static inline bool sql_buf_put_lenenc_str(SqlBuf *b, const char *s, size_t n)
{
    if (!sql_buf_put_lenenc(b, (uint64_t)n)) return false;
    return n ? sql_buf_put_bytes(b, s, n) : true;
}

/* =========================================================================
 * SqlReader -- read-only slice with bounds-checked accessors.
 *
 * `ok` is sticky: once any access overflows the slice, every subsequent
 * call returns 0 / NULL and ok stays false.  Callers can therefore make
 * a series of reads and check ok once at the end.
 * ===================================================================== */

typedef struct SqlReader {
    const unsigned char *data;
    size_t               len;
    size_t               pos;
    bool                 ok;
} SqlReader;

static inline SqlReader sql_reader_init(const void *data, size_t len)
{
    SqlReader r;
    r.data = (const unsigned char *)data;
    r.len  = len;
    r.pos  = 0;
    r.ok   = true;
    return r;
}

static inline bool sql_reader_have(SqlReader *r, size_t n)
{
    if (!r->ok || r->pos + n > r->len) { r->ok = false; return false; }
    return true;
}

static inline uint8_t sql_reader_get_u8(SqlReader *r)
{
    if (!sql_reader_have(r, 1)) return 0;
    return r->data[r->pos++];
}

/* Big-endian (PostgreSQL) */
static inline uint16_t sql_reader_get_be16(SqlReader *r)
{
    if (!sql_reader_have(r, 2)) return 0;
    uint16_t v = ((uint16_t)r->data[r->pos] << 8) | r->data[r->pos + 1];
    r->pos += 2;
    return v;
}
static inline uint32_t sql_reader_get_be32(SqlReader *r)
{
    if (!sql_reader_have(r, 4)) return 0;
    uint32_t v = ((uint32_t)r->data[r->pos]     << 24)
               | ((uint32_t)r->data[r->pos + 1] << 16)
               | ((uint32_t)r->data[r->pos + 2] << 8)
               |  (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return v;
}

/* Little-endian (MySQL) */
static inline uint16_t sql_reader_get_le16(SqlReader *r)
{
    if (!sql_reader_have(r, 2)) return 0;
    uint16_t v = (uint16_t)r->data[r->pos]
               | ((uint16_t)r->data[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}
static inline uint32_t sql_reader_get_le24(SqlReader *r)
{
    if (!sql_reader_have(r, 3)) return 0;
    uint32_t v = (uint32_t)r->data[r->pos]
               | ((uint32_t)r->data[r->pos + 1] << 8)
               | ((uint32_t)r->data[r->pos + 2] << 16);
    r->pos += 3;
    return v;
}
static inline uint32_t sql_reader_get_le32(SqlReader *r)
{
    if (!sql_reader_have(r, 4)) return 0;
    uint32_t v = (uint32_t)r->data[r->pos]
               | ((uint32_t)r->data[r->pos + 1] << 8)
               | ((uint32_t)r->data[r->pos + 2] << 16)
               | ((uint32_t)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}
static inline uint64_t sql_reader_get_le64(SqlReader *r)
{
    if (!sql_reader_have(r, 8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)r->data[r->pos + i]) << (8 * i);
    r->pos += 8;
    return v;
}

/* Returns a pointer into the slice and advances past `n` bytes.  The
 * returned pointer is valid only as long as the slice is. */
static inline const unsigned char *sql_reader_get_bytes(SqlReader *r, size_t n)
{
    if (!sql_reader_have(r, n)) return NULL;
    const unsigned char *p = r->data + r->pos;
    r->pos += n;
    return p;
}

/* PostgreSQL: read a NUL-terminated C string and return a pointer to
 * the start.  Advances past the trailing NUL.  Returns NULL on EOF. */
static inline const char *sql_reader_get_cstr(SqlReader *r)
{
    if (!r->ok) return NULL;
    size_t start = r->pos;
    while (r->pos < r->len && r->data[r->pos] != 0) r->pos++;
    if (r->pos >= r->len) { r->ok = false; return NULL; }
    const char *s = (const char *)(r->data + start);
    r->pos++;          /* skip NUL */
    return s;
}

/* MySQL length-encoded integer.  *out_null is set when the leading
 * byte is 0xfb (NULL marker), in which case the returned value is 0
 * and one byte was consumed. */
static inline uint64_t sql_reader_get_lenenc(SqlReader *r, bool *out_null)
{
    if (out_null) *out_null = false;
    uint8_t b = sql_reader_get_u8(r);
    if (!r->ok) return 0;
    if (b < 0xfb) return b;
    if (b == 0xfb) { if (out_null) *out_null = true; return 0; }
    if (b == 0xfc) return sql_reader_get_le16(r);
    if (b == 0xfd) return sql_reader_get_le24(r);
    if (b == 0xfe) return sql_reader_get_le64(r);
    /* 0xff is "ERR packet" header; not legal as a length-enc. */
    r->ok = false;
    return 0;
}

#endif /* CANDO_SQL_BUF_H */
