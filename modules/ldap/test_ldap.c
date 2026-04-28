/*
 * modules/ldap/test_ldap.c -- Unit tests for the LDAP / Active Directory
 * module's pure-C helpers.
 *
 * Build with the per-module Makefile:
 *
 *     make -C modules/ldap test
 *
 * The tests cover:
 *   - scope-name parsing (string -> LDAP_SCOPE_*)
 *   - modification op-name parsing (string -> LDAP_MOD_*)
 *   - RDN extraction (handling of escaped commas)
 *   - the ldap_err2string round-trip used by the error reporter
 *
 * The full network-facing surface (connect / bind / search / add / modify
 * / delete / move / rename) is exercised by the script-level integration
 * test in tests/integration/scripts/ldap_module.cdo, which runs against
 * the binary module loaded through include().
 */

#include "ldap_helpers.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <winldap.h>
#else
#  include <ldap.h>
#endif

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(name, cond)                                                  \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  PASS  %s\n", name);                                   \
            g_pass++;                                                       \
        } else {                                                            \
            printf("  FAIL  %s  (line %d)\n", name, __LINE__);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static void test_parse_scope(void)
{
    int s = -1;
    EXPECT("scope: base",
        ldap_helpers_parse_scope("base", &s) == 1 && s == LDAP_SCOPE_BASE);
    s = -1;
    EXPECT("scope: one",
        ldap_helpers_parse_scope("one", &s) == 1 && s == LDAP_SCOPE_ONELEVEL);
    s = -1;
    EXPECT("scope: onelevel",
        ldap_helpers_parse_scope("onelevel", &s) == 1 && s == LDAP_SCOPE_ONELEVEL);
    s = -1;
    EXPECT("scope: sub",
        ldap_helpers_parse_scope("sub", &s) == 1 && s == LDAP_SCOPE_SUBTREE);
    s = -1;
    EXPECT("scope: subtree",
        ldap_helpers_parse_scope("subtree", &s) == 1 && s == LDAP_SCOPE_SUBTREE);

    /* Unknown name -- must not modify *out and must return 0. */
    int sentinel = 0xCAFE;
    EXPECT("scope: unknown returns 0",
        ldap_helpers_parse_scope("bogus", &sentinel) == 0);
    EXPECT("scope: unknown leaves out untouched",
        sentinel == 0xCAFE);

    /* NULL inputs must not crash and must return 0. */
    EXPECT("scope: NULL name",  ldap_helpers_parse_scope(NULL, &sentinel) == 0);
    EXPECT("scope: NULL out",   ldap_helpers_parse_scope("sub", NULL) == 0);
}

static void test_parse_mod_op(void)
{
    int op = -1;
    EXPECT("modop: add",
        ldap_helpers_parse_mod_op("add", &op) == 1 && op == LDAP_MOD_ADD);
    op = -1;
    EXPECT("modop: replace",
        ldap_helpers_parse_mod_op("replace", &op) == 1
            && op == LDAP_MOD_REPLACE);
    op = -1;
    EXPECT("modop: delete",
        ldap_helpers_parse_mod_op("delete", &op) == 1
            && op == LDAP_MOD_DELETE);

    int sentinel = 0xBEEF;
    EXPECT("modop: unknown returns 0",
        ldap_helpers_parse_mod_op("modify", &sentinel) == 0);
    EXPECT("modop: unknown leaves out untouched",
        sentinel == 0xBEEF);
}

static void test_extract_rdn(void)
{
    char buf[256];

    EXPECT("rdn: simple",
        ldap_helpers_extract_rdn("cn=Ada,ou=Users,dc=ex,dc=com",
                                  buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Ada") == 0);

    /* Comma is escaped with a backslash -- must not split there. */
    EXPECT("rdn: escaped comma",
        ldap_helpers_extract_rdn("cn=Doe\\, Jane,ou=Users,dc=ex,dc=com",
                                  buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Doe\\, Jane") == 0);

    /* No comma at all -- the whole string is the RDN. */
    EXPECT("rdn: no comma",
        ldap_helpers_extract_rdn("cn=Solo", buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Solo") == 0);

    /* Buffer overflow -- function must reject. */
    char tiny[4];
    EXPECT("rdn: buffer too small",
        ldap_helpers_extract_rdn("cn=Ada,dc=x", tiny, sizeof(tiny)) == 0);

    /* NULL inputs -- must return 0 without crashing. */
    EXPECT("rdn: NULL dn",  ldap_helpers_extract_rdn(NULL, buf, sizeof(buf)) == 0);
    EXPECT("rdn: NULL buf", ldap_helpers_extract_rdn("cn=x", NULL, 16) == 0);
    EXPECT("rdn: zero len", ldap_helpers_extract_rdn("cn=x", buf, 0) == 0);
}

static void test_err2string(void)
{
#if defined(_WIN32) || defined(_WIN64)
    const char *msg = (const char *)ldap_err2string(LDAP_SUCCESS);
#else
    const char *msg = ldap_err2string(LDAP_SUCCESS);
#endif
    EXPECT("err2string: SUCCESS not NULL",   msg != NULL);
    EXPECT("err2string: SUCCESS is non-empty", msg && msg[0] != '\0');

#if defined(_WIN32) || defined(_WIN64)
    const char *err = (const char *)ldap_err2string(LDAP_NO_SUCH_OBJECT);
#else
    const char *err = ldap_err2string(LDAP_NO_SUCH_OBJECT);
#endif
    EXPECT("err2string: NO_SUCH_OBJECT not NULL", err != NULL);
}

int main(void)
{
    printf("ldap module unit tests\n");
    printf("----------------------\n");

    test_parse_scope();
    test_parse_mod_op();
    test_extract_rdn();
    test_err2string();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
