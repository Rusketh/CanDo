/*
 * modules/draw/test_draw.c -- C unit tests for draw-module helpers.
 *
 * Built and run with:
 *
 *     make -C modules/draw test
 *
 * This is the skeleton chunk: there are no helpers to test yet.
 * Transform-stack, scissor-stack, atlas packer, and colour packing
 * tests arrive alongside the corresponding implementations.
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
    printf("== modules/draw: skeleton C tests ==\n");
    EXPECT("placeholder: skeleton compiles and runs", 1 == 1);

    if (failures == 0) {
        printf("All draw C tests passed.\n");
        return 0;
    }
    printf("%d draw C test(s) failed.\n", failures);
    return 1;
}
