/*
 * modules/sql/sql_placeholder.h -- Translate Node-style `?` placeholders
 * into PostgreSQL-style `$1, $2, ...` numbered placeholders.
 *
 * Both modules/sqlite and modules/sql expose the same prepared-statement
 * API as node:sqlite -- a `?` per parameter, bound positionally.  SQLite
 * accepts that natively; PostgreSQL does not, so the SQL module's
 * Postgres driver runs every prepare-time SQL through this translator
 * first.
 *
 * The translator must NOT touch `?` characters that appear inside
 * string literals, identifiers, comments, or PG dollar-quoted strings,
 * since those aren't placeholders.  It must also leave `??` (the JDBC
 * "literal question mark" escape) unchanged -- both characters pass
 * through.  The implementation is a simple state machine over the
 * input.
 *
 * Header-only with `static inline` so test_sql.c can include it
 * directly and exercise the translator without linking the rest of
 * the module.
 */

#ifndef CANDO_SQL_PLACEHOLDER_H
#define CANDO_SQL_PLACEHOLDER_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * sql_translate_placeholders -- copy `in` into a freshly malloc'd
 * heap buffer, replacing each unquoted `?` with `$N` (1-indexed).
 *
 * On success: returns the heap buffer (caller frees) and writes the
 * total placeholder count to *out_n.
 *
 * On allocation failure: returns NULL and leaves *out_n unchanged.
 *
 * The function never reports a syntax error -- if the input has an
 * unterminated string or comment, the leftover bytes are emitted
 * verbatim.  The server will reject the malformed SQL with its own
 * (much better) error message at parse time.
 */
static inline char *sql_translate_placeholders(const char *in, int *out_n)
{
    if (!in) { if (out_n) *out_n = 0; return NULL; }
    size_t in_len = strlen(in);
    /* Worst case: every input char is `?` and gets replaced by `$N`,
     * with N reaching 6 digits (>10^5 placeholders) -- give ourselves
     * 8 bytes per `?` plus the original length. */
    size_t cap = in_len + 32;
    for (size_t i = 0; i < in_len; i++) if (in[i] == '?') cap += 12;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    size_t op = 0;
    int    next_id = 1;
    size_t ip = 0;

    while (ip < in_len) {
        char c = in[ip];

        /* -- single-line comment ----------------------------------- */
        if (c == '-' && ip + 1 < in_len && in[ip + 1] == '-') {
            while (ip < in_len && in[ip] != '\n') out[op++] = in[ip++];
            continue;
        }

/* block comment delimited by slash-star ... star-slash */
        if (c == '/' && ip + 1 < in_len && in[ip + 1] == '*') {
            out[op++] = in[ip++]; out[op++] = in[ip++];
            while (ip < in_len) {
                if (in[ip] == '*' && ip + 1 < in_len && in[ip + 1] == '/') {
                    out[op++] = in[ip++]; out[op++] = in[ip++];
                    break;
                }
                out[op++] = in[ip++];
            }
            continue;
        }

        /* 'single-quoted string' -- '' is the escape for an embedded ' */
        if (c == '\'') {
            out[op++] = in[ip++];
            while (ip < in_len) {
                if (in[ip] == '\'' && ip + 1 < in_len && in[ip + 1] == '\'') {
                    out[op++] = in[ip++]; out[op++] = in[ip++];
                    continue;
                }
                if (in[ip] == '\'') { out[op++] = in[ip++]; break; }
                out[op++] = in[ip++];
            }
            continue;
        }

        /* "double-quoted identifier" (PG, ANSI) -- "" is the embedded-" */
        if (c == '"') {
            out[op++] = in[ip++];
            while (ip < in_len) {
                if (in[ip] == '"' && ip + 1 < in_len && in[ip + 1] == '"') {
                    out[op++] = in[ip++]; out[op++] = in[ip++];
                    continue;
                }
                if (in[ip] == '"') { out[op++] = in[ip++]; break; }
                out[op++] = in[ip++];
            }
            continue;
        }

        /* PostgreSQL dollar-quoted string: $tag$ ... $tag$ where `tag`
         * is zero or more letters / digits / underscores.  Once we
         * recognise the opening tag we copy until the matching close.
         * If the candidate isn't a real dollar-quote (e.g. `$1`) we
         * just emit the `$` and resume normal scanning. */
        if (c == '$') {
            size_t k = ip + 1;
            while (k < in_len) {
                char tc = in[k];
                if ((tc >= 'A' && tc <= 'Z')
                 || (tc >= 'a' && tc <= 'z')
                 || (tc >= '0' && tc <= '9')
                 ||  tc == '_') { k++; continue; }
                break;
            }
            if (k < in_len && in[k] == '$') {
                /* It's a dollar-quote opener: $tag$ ... $tag$. */
                size_t tag_start = ip;
                size_t tag_end   = k + 1;        /* one past closing $ */
                size_t tag_len   = tag_end - tag_start;
                /* Emit the opener verbatim. */
                memcpy(out + op, in + tag_start, tag_len);
                op += tag_len;
                ip = tag_end;
                /* Scan for the matching closer. */
                while (ip < in_len) {
                    if (in[ip] == '$'
                        && ip + tag_len <= in_len
                        && memcmp(in + ip, in + tag_start, tag_len) == 0) {
                        memcpy(out + op, in + ip, tag_len);
                        op += tag_len;
                        ip += tag_len;
                        break;
                    }
                    out[op++] = in[ip++];
                }
                continue;
            }
            /* Not a dollar-quote -- fall through to normal copy. */
        }

        if (c == '?') {
            /* Pass-through escape: `??` is taken to mean a literal `?`
             * (matches JDBC; useful when writing JSON path operators
             * on PG, e.g. `data->>'k' ?| array[...]`).  Emit one `?`
             * and skip the second. */
            if (ip + 1 < in_len && in[ip + 1] == '?') {
                out[op++] = '?';
                ip += 2;
                continue;
            }
            int written = snprintf(out + op, 16, "$%d", next_id++);
            if (written < 0) { free(out); return NULL; }
            op += (size_t)written;
            ip++;
            continue;
        }

        out[op++] = c;
        ip++;
    }
    out[op] = '\0';
    if (out_n) *out_n = next_id - 1;
    return out;
}

#endif /* CANDO_SQL_PLACEHOLDER_H */
