/*
 * modules/sql/sql_escape.h -- SQL literal and identifier escaping for
 * PostgreSQL, MySQL/MariaDB, and SQLite.
 *
 * Prepared statements are always preferable -- the bind path keeps
 * values out of the parser entirely.  But there are real cases
 * (dynamic identifiers, IN-list builders, schema migration tools,
 * dialect-specific syntax that doesn't accept parameter placeholders)
 * where a script has to assemble SQL by hand.  These helpers turn
 * arbitrary input strings into safely-quoted SQL literals or
 * identifiers, using each engine's escape rules.
 *
 * All four functions write a freshly malloc'd NUL-terminated string
 * and return it; the caller owns the allocation and must free() it.
 *
 * `len` is the number of input bytes -- the helpers are byte-safe and
 * preserve embedded NULs for engines that accept them (MySQL via the
 * \0 escape; PostgreSQL E-strings can't carry a literal NUL, so the
 * PG escape function refuses input containing one and returns NULL).
 *
 * Header-only with `static inline` so the unit tests can include this
 * file directly and exercise the helpers without linking the module.
 */

#ifndef CANDO_SQL_ESCAPE_H
#define CANDO_SQL_ESCAPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * PostgreSQL string literal -- E'...'
 *
 * Uses the E'' escape-string form so backslash and other control
 * characters get backslash-escaped regardless of the server-side
 * `standard_conforming_strings` setting.  Bytes:
 *     '   -> ''
 *     \   -> \\
 *     \b  -> \b   (backspace)
 *     \f  -> \f
 *     \n  -> \n
 *     \r  -> \r
 *     \t  -> \t
 *     other 0x01..0x1F -> \xNN
 *
 * PostgreSQL text literals cannot contain a NUL byte; the helper
 * returns NULL when the input contains one.  Use a `bytea` (binary)
 * cast and the modules/sql blob path for byte data.
 * ===================================================================== */
static inline char *sql_escape_pg_literal(const char *in, size_t len)
{
    if (!in) {
        char *s = (char *)malloc(5);
        if (s) memcpy(s, "NULL", 5);
        return s;
    }
    /* Refuse NULs so we don't truncate the literal silently. */
    for (size_t i = 0; i < len; i++) if (in[i] == 0) return NULL;

    /* Worst case: every byte expands to "\xNN" (4 bytes), plus E''. */
    size_t cap = len * 4 + 4;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    size_t k = 0;
    out[k++] = 'E';
    out[k++] = '\'';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '\'': out[k++] = '\''; out[k++] = '\''; break;
            case '\\': out[k++] = '\\'; out[k++] = '\\'; break;
            case '\b': out[k++] = '\\'; out[k++] = 'b';  break;
            case '\f': out[k++] = '\\'; out[k++] = 'f';  break;
            case '\n': out[k++] = '\\'; out[k++] = 'n';  break;
            case '\r': out[k++] = '\\'; out[k++] = 'r';  break;
            case '\t': out[k++] = '\\'; out[k++] = 't';  break;
            default:
                if (c < 0x20) {
                    static const char H[] = "0123456789abcdef";
                    out[k++] = '\\'; out[k++] = 'x';
                    out[k++] = H[c >> 4];
                    out[k++] = H[c & 0x0f];
                } else {
                    out[k++] = (char)c;
                }
        }
    }
    out[k++] = '\'';
    out[k] = '\0';
    return out;
}

/* =========================================================================
 * PostgreSQL identifier -- "name" with embedded `"` doubled.
 * Refuses NUL bytes (same reasoning as the literal helper).
 * ===================================================================== */
static inline char *sql_escape_pg_identifier(const char *in, size_t len)
{
    if (!in || len == 0) {
        char *s = (char *)malloc(3);
        if (s) memcpy(s, "\"\"", 3);
        return s;
    }
    for (size_t i = 0; i < len; i++) if (in[i] == 0) return NULL;
    size_t cap = len * 2 + 3;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '"') { out[k++] = '"'; out[k++] = '"'; }
        else          { out[k++] = c; }
    }
    out[k++] = '"';
    out[k] = '\0';
    return out;
}

/* =========================================================================
 * MySQL string literal -- '...' with backslash escapes.
 *
 * Matches mysql_real_escape_string() output for the default
 * NO_BACKSLASH_ESCAPES=off mode:
 *     '   -> \'
 *     "   -> \"
 *     \   -> \\
 *     NUL -> \0
 *     \n  -> \n
 *     \r  -> \r
 *     \032-> \Z       (Ctrl-Z; the historical Windows EOF byte)
 *     other 0x01..0x1F -> \xNN-style is NOT applied; printable bytes
 *                          and high-bit bytes pass through (for utf8mb4).
 * ===================================================================== */
static inline char *sql_escape_my_literal(const char *in, size_t len)
{
    if (!in) {
        char *s = (char *)malloc(5);
        if (s) memcpy(s, "NULL", 5);
        return s;
    }
    size_t cap = len * 2 + 4;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '\'';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case 0:    out[k++] = '\\'; out[k++] = '0';  break;
            case '\'': out[k++] = '\\'; out[k++] = '\''; break;
            case '"':  out[k++] = '\\'; out[k++] = '"';  break;
            case '\\': out[k++] = '\\'; out[k++] = '\\'; break;
            case '\n': out[k++] = '\\'; out[k++] = 'n';  break;
            case '\r': out[k++] = '\\'; out[k++] = 'r';  break;
            case 0x1a: out[k++] = '\\'; out[k++] = 'Z';  break;
            default:   out[k++] = (char)c;               break;
        }
    }
    out[k++] = '\'';
    out[k] = '\0';
    return out;
}

/* =========================================================================
 * MySQL identifier -- `name` with embedded backtick doubled.
 * Backticks survive NUL bytes by emitting `\0` -- matches what the
 * server tolerates inside backticked identifiers.
 * ===================================================================== */
static inline char *sql_escape_my_identifier(const char *in, size_t len)
{
    if (!in || len == 0) {
        char *s = (char *)malloc(3);
        if (s) memcpy(s, "``", 3);
        return s;
    }
    size_t cap = len * 2 + 3;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '`';
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '`') { out[k++] = '`'; out[k++] = '`'; }
        else          { out[k++] = c; }
    }
    out[k++] = '`';
    out[k] = '\0';
    return out;
}

/* =========================================================================
 * SQLite string literal -- '...' with embedded `'` doubled.
 *
 * SQLite has no backslash escape; the *only* in-string escape is
 * doubling the single-quote.  This matches `sqlite3_mprintf("%Q", ...)`
 * minus the NULL handling (we treat NULL the same way).
 * ===================================================================== */
static inline char *sql_escape_sqlite_literal(const char *in, size_t len)
{
    if (!in) {
        char *s = (char *)malloc(5);
        if (s) memcpy(s, "NULL", 5);
        return s;
    }
    size_t cap = len * 2 + 3;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '\'';
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '\'') { out[k++] = '\''; out[k++] = '\''; }
        else           { out[k++] = c; }
    }
    out[k++] = '\'';
    out[k] = '\0';
    return out;
}

/* =========================================================================
 * SQLite identifier -- "name" (ANSI; SQLite also accepts `name` and
 * [name], but ANSI doublequotes are the most portable form).
 * ===================================================================== */
static inline char *sql_escape_sqlite_identifier(const char *in, size_t len)
{
    return sql_escape_pg_identifier(in, len);
}

#endif /* CANDO_SQL_ESCAPE_H */
