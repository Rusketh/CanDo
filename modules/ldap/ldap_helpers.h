/*
 * modules/ldap/ldap_helpers.h -- Pure-C, cando-independent helpers for the
 * LDAP module.  Kept in a header (not a separate .c file) so both the
 * shared-library build and the standalone unit-test build can pick them up
 * without an extra link target.
 *
 * Functions here MUST NOT depend on the Cando VM, the bridge layer, or any
 * heap helper from libcando -- they should only depend on the standard
 * C library and (optionally) the platform LDAP header.
 */

#ifndef CANDO_LDAP_HELPERS_H
#define CANDO_LDAP_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <winldap.h>
#else
#  include <ldap.h>
#endif

/*
 * ldap_helpers_parse_scope -- map a textual LDAP scope name to its
 * numeric LDAP_SCOPE_* constant.
 *
 * Accepted names (case-sensitive): "base", "one", "onelevel", "sub",
 * "subtree", "children" (where supported).
 *
 * Returns 1 on success and writes *out, 0 on unknown name.
 */
static inline int ldap_helpers_parse_scope(const char *name, int *out)
{
    if (!name || !out) return 0;
    if (strcmp(name, "base")     == 0) { *out = LDAP_SCOPE_BASE;     return 1; }
    if (strcmp(name, "one")      == 0) { *out = LDAP_SCOPE_ONELEVEL; return 1; }
    if (strcmp(name, "onelevel") == 0) { *out = LDAP_SCOPE_ONELEVEL; return 1; }
    if (strcmp(name, "sub")      == 0) { *out = LDAP_SCOPE_SUBTREE;  return 1; }
    if (strcmp(name, "subtree")  == 0) { *out = LDAP_SCOPE_SUBTREE;  return 1; }
#if defined(LDAP_SCOPE_CHILDREN)
    if (strcmp(name, "children") == 0) { *out = LDAP_SCOPE_CHILDREN; return 1; }
#endif
    return 0;
}

/*
 * ldap_helpers_parse_mod_op -- map a textual modification op to its
 * numeric LDAP_MOD_* constant.
 *
 * Accepted: "add", "replace", "delete".
 *
 * Returns 1 on success and writes *out, 0 otherwise.
 */
static inline int ldap_helpers_parse_mod_op(const char *name, int *out)
{
    if (!name || !out) return 0;
    if (strcmp(name, "add")     == 0) { *out = LDAP_MOD_ADD;     return 1; }
    if (strcmp(name, "replace") == 0) { *out = LDAP_MOD_REPLACE; return 1; }
    if (strcmp(name, "delete")  == 0) { *out = LDAP_MOD_DELETE;  return 1; }
    return 0;
}

/*
 * ldap_helpers_extract_rdn -- copy the leftmost RDN component of `dn` into
 * `buf` (size `buflen`).  Handles backslash escapes for embedded commas.
 *
 * Returns 1 on success, 0 if the buffer is too small or `dn` is NULL.
 */
static inline int ldap_helpers_extract_rdn(const char *dn,
                                           char *buf, size_t buflen)
{
    if (!dn || !buf || buflen == 0) return 0;
    const char *p = dn;
    while (*p && *p != ',') {
        if (*p == '\\' && *(p + 1)) p++;  /* skip the escape pair */
        p++;
    }
    size_t l = (size_t)(p - dn);
    if (l + 1 > buflen) return 0;
    memcpy(buf, dn, l);
    buf[l] = '\0';
    return 1;
}

/*
 * ldap_helpers_escape_filter -- escape `value` per RFC 4515 section 3
 * (assertion-value escaping).  The bytes 0x00, 0x28 ("("), 0x29 (")"),
 * 0x2A ("*"), 0x5C ("\") are encoded as `\xx` hex pairs; all other bytes
 * pass through.
 *
 * On success writes a NUL-terminated escaped string into out (size buflen)
 * and returns the number of output bytes excluding the NUL.  Returns -1
 * if the buffer is too small.  Pass buflen == 0 to query the required
 * size: returns the byte count not counting the NUL.
 */
static inline long ldap_helpers_escape_filter(const char *value, size_t in_len,
                                              char *out, size_t buflen)
{
    if (!value) return -1;
    size_t need = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == 0x00 || c == 0x28 || c == 0x29 || c == 0x2A || c == 0x5C)
            need += 3;
        else
            need += 1;
    }
    if (buflen == 0) return (long)need;
    if (out == NULL) return -1;
    if (need + 1 > buflen) return -1;

    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == 0x00 || c == 0x28 || c == 0x29 || c == 0x2A || c == 0x5C) {
            out[o++] = '\\';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (long)o;
}

/*
 * ldap_helpers_escape_dn -- escape `value` per RFC 4514 section 2.4
 * (string representation of an attribute-value).  Special characters
 * `,`, `+`, `"`, `\`, `<`, `>`, `;` are backslash-escaped, leading `#`
 * and leading/trailing space are escaped, and NULs are encoded as `\00`.
 *
 * Returns output length excluding NUL on success, or -1 if the buffer is
 * too small.  Pass buflen == 0 to query the required size.
 */
static inline long ldap_helpers_escape_dn(const char *value, size_t in_len,
                                          char *out, size_t buflen)
{
    if (!value) return -1;
    /* Two-pass: count then write. */
    static const char hex[] = "0123456789abcdef";
    size_t need = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)value[i];
        bool special = (c == ',' || c == '+' || c == '"' || c == '\\'
                        || c == '<' || c == '>' || c == ';');
        bool first   = (i == 0);
        bool last    = (i + 1 == in_len);
        bool leading = first && (c == ' ' || c == '#');
        bool trailing= last  && (c == ' ');
        if (c == 0x00)            need += 3;          /* \00 */
        else if (special)         need += 2;          /* \X  */
        else if (leading||trailing) need += 2;        /* \X  */
        else                      need += 1;
    }
    if (buflen == 0) return (long)need;
    if (out == NULL) return -1;
    if (need + 1 > buflen) return -1;

    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)value[i];
        bool special = (c == ',' || c == '+' || c == '"' || c == '\\'
                        || c == '<' || c == '>' || c == ';');
        bool first   = (i == 0);
        bool last    = (i + 1 == in_len);
        bool leading = first && (c == ' ' || c == '#');
        bool trailing= last  && (c == ' ');
        if (c == 0x00) {
            out[o++] = '\\';
            out[o++] = hex[0];
            out[o++] = hex[0];
        } else if (special || leading || trailing) {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (long)o;
}

#endif /* CANDO_LDAP_HELPERS_H */
