/*
 * modules/sqlite/test_sqlite.c -- C unit tests for the SQLite module's
 * pure-C helpers and an end-to-end smoke against the vendored
 * amalgamation.
 *
 * Built and run with:
 *
 *     make -C modules/sqlite test
 *
 * These tests do not link against libcando -- they exercise the pieces
 * that don't touch the VM (the error-code-name table and a quick
 * round-trip through sqlite3_exec).  Script-level coverage of the
 * module surface lives in test_sqlite.cdo.
 */

#include "vendor/sqlite3.h"
#include "sqlite_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(name, cond) do {                            \
    if (cond) {                                            \
        printf("  PASS  %s\n", name);                      \
    } else {                                               \
        printf("  FAIL  %s\n", name);                      \
        failures++;                                        \
    }                                                      \
} while (0)

#define EXPECT_STREQ(name, a, b) do {                                  \
    const char *_a = (a), *_b = (b);                                   \
    if (_a && _b && strcmp(_a, _b) == 0) {                             \
        printf("  PASS  %s\n", name);                                  \
    } else {                                                           \
        printf("  FAIL  %s  (\"%s\" != \"%s\")\n", name,               \
               _a ? _a : "(null)", _b ? _b : "(null)");                \
        failures++;                                                    \
    }                                                                  \
} while (0)

static void test_libversion(void)
{
    printf("\n[libversion]\n");
    const char *v = sqlite3_libversion();
    EXPECT("sqlite3_libversion not NULL",         v != NULL);
    EXPECT("sqlite3_libversion non-empty",        v && v[0] != '\0');
    EXPECT("sqlite3_libversion looks like 3.x",   v && v[0] == '3' && v[1] == '.');
    EXPECT("compiled with SQLITE_THREADSAFE=1",   sqlite3_threadsafe() == 1);
}

static void test_errcode_name(void)
{
    printf("\n[errcode_name]\n");

    /* Primary codes -- each maps to its symbolic name. */
    EXPECT_STREQ("SQLITE_OK",            sqlite_errcode_name(SQLITE_OK),            "SQLITE_OK");
    EXPECT_STREQ("SQLITE_ERROR",         sqlite_errcode_name(SQLITE_ERROR),         "SQLITE_ERROR");
    EXPECT_STREQ("SQLITE_BUSY",          sqlite_errcode_name(SQLITE_BUSY),          "SQLITE_BUSY");
    EXPECT_STREQ("SQLITE_LOCKED",        sqlite_errcode_name(SQLITE_LOCKED),        "SQLITE_LOCKED");
    EXPECT_STREQ("SQLITE_READONLY",      sqlite_errcode_name(SQLITE_READONLY),      "SQLITE_READONLY");
    EXPECT_STREQ("SQLITE_CONSTRAINT",    sqlite_errcode_name(SQLITE_CONSTRAINT),    "SQLITE_CONSTRAINT");
    EXPECT_STREQ("SQLITE_NOTFOUND",      sqlite_errcode_name(SQLITE_NOTFOUND),      "SQLITE_NOTFOUND");
    EXPECT_STREQ("SQLITE_DONE",          sqlite_errcode_name(SQLITE_DONE),          "SQLITE_DONE");
    EXPECT_STREQ("SQLITE_ROW",           sqlite_errcode_name(SQLITE_ROW),           "SQLITE_ROW");

    /* Extended codes -- each must hit its specific arm, not the family fallback. */
    EXPECT_STREQ("SQLITE_BUSY_RECOVERY",
        sqlite_errcode_name(SQLITE_BUSY_RECOVERY), "SQLITE_BUSY_RECOVERY");
    EXPECT_STREQ("SQLITE_CONSTRAINT_UNIQUE",
        sqlite_errcode_name(SQLITE_CONSTRAINT_UNIQUE), "SQLITE_CONSTRAINT_UNIQUE");
    EXPECT_STREQ("SQLITE_CONSTRAINT_FOREIGNKEY",
        sqlite_errcode_name(SQLITE_CONSTRAINT_FOREIGNKEY), "SQLITE_CONSTRAINT_FOREIGNKEY");
    EXPECT_STREQ("SQLITE_READONLY_DBMOVED",
        sqlite_errcode_name(SQLITE_READONLY_DBMOVED), "SQLITE_READONLY_DBMOVED");

    /* An unknown extended code falls back to the family name (low byte). */
    int unknown_busy = SQLITE_BUSY | (0x7f << 8);
    EXPECT_STREQ("unknown ext under BUSY -> SQLITE_BUSY",
        sqlite_errcode_name(unknown_busy), "SQLITE_BUSY");

    /* A wholly unknown code returns the catch-all. */
    EXPECT_STREQ("totally bogus -> SQLITE_UNKNOWN",
        sqlite_errcode_name(0xdead), "SQLITE_UNKNOWN");
}

static void test_end_to_end(void)
{
    printf("\n[end_to_end]\n");

    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    EXPECT("open :memory: succeeds", rc == SQLITE_OK && db != NULL);
    if (!db) return;

    rc = sqlite3_exec(db, "CREATE TABLE t(x INTEGER); INSERT INTO t VALUES(42);",
                      NULL, NULL, NULL);
    EXPECT("exec CREATE+INSERT succeeds", rc == SQLITE_OK);

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT x FROM t", -1, &st, NULL);
    EXPECT("prepare SELECT succeeds", rc == SQLITE_OK && st != NULL);

    if (st) {
        rc = sqlite3_step(st);
        EXPECT("step returns SQLITE_ROW", rc == SQLITE_ROW);
        EXPECT("column 0 == 42",          sqlite3_column_int(st, 0) == 42);
        sqlite3_finalize(st);
    }

    /* Force a constraint violation so the extended-code path is
     * exercised by something other than a unit-test stub. */
    rc = sqlite3_exec(db,
        "CREATE TABLE u(x INTEGER UNIQUE);"
        "INSERT INTO u VALUES(1);"
        "INSERT INTO u VALUES(1);",
        NULL, NULL, NULL);
    EXPECT("UNIQUE violation returns CONSTRAINT", rc == SQLITE_CONSTRAINT);
    int ext = sqlite3_extended_errcode(db);
    EXPECT_STREQ("extended code -> SQLITE_CONSTRAINT_UNIQUE",
        sqlite_errcode_name(ext), "SQLITE_CONSTRAINT_UNIQUE");

    sqlite3_close(db);
}

int main(void)
{
    printf("sqlite module C unit tests\n");
    printf("--------------------------\n");

    test_libversion();
    test_errcode_name();
    test_end_to_end();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
