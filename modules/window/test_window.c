/*
 * modules/window/test_window.c -- C unit tests for window-module helpers.
 *
 * Built and run with:
 *
 *     make -C modules/window test
 *
 * This is the skeleton chunk: there are no helpers to test yet, so this
 * file exists to keep `make modules-test` green and to reserve the
 * EXPECT/EXPECT_STREQ macros for later chunks (event-queue ring buffer,
 * slot table, key-code translation, etc.).
 */

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(name, cond) do {                                            \
    if (cond) {                                                            \
        printf("  PASS  %s\n", name);                                      \
    } else {                                                               \
        printf("  FAIL  %s\n", name);                                      \
        failures++;                                                        \
    }                                                                      \
} while (0)

int main(void)
{
    printf("== modules/window: skeleton C tests ==\n");
    EXPECT("placeholder: skeleton compiles and runs", 1 == 1);

    if (failures == 0) {
        printf("All window C tests passed.\n");
        return 0;
    }
    printf("%d window C test(s) failed.\n", failures);
    return 1;
}
