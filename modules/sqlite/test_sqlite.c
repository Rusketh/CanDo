/*
 * modules/sqlite/test_sqlite.c -- C unit tests for the SQLite module's
 * pure-C helpers.
 *
 * Built and run with:
 *
 *     make -C modules/sqlite test
 *
 * Chunk 1 carries only a smoke test: link against the vendored
 * amalgamation and verify sqlite3_libversion() returns a non-empty
 * string starting with "3.".  Subsequent chunks add helper-level tests
 * for the slot pool, error formatter, and parameter binder.
 */

#include "vendor/sqlite3.h"

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

int main(void)
{
    printf("sqlite module C unit tests\n");
    printf("--------------------------\n");

    const char *v = sqlite3_libversion();
    EXPECT("sqlite3_libversion not NULL",         v != NULL);
    EXPECT("sqlite3_libversion non-empty",        v && v[0] != '\0');
    EXPECT("sqlite3_libversion looks like 3.x",   v && v[0] == '3' && v[1] == '.');

    /* sqlite3_threadsafe should report serialized (1) given our build flags. */
    EXPECT("compiled with SQLITE_THREADSAFE=1",   sqlite3_threadsafe() == 1);

    /* End-to-end sanity: open in-memory, exec, close. */
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    EXPECT("open :memory: succeeds", rc == SQLITE_OK && db != NULL);

    if (db) {
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
        sqlite3_close(db);
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
