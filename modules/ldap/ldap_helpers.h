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

#endif /* CANDO_LDAP_HELPERS_H */
