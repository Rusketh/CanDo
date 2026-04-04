/*
 * common.c -- Central allocator implementation.
 */

#include "common.h"

void *cando_alloc(usize size) {
    CANDO_ASSERT(size > 0);
    void *ptr = malloc(size);
    if (CANDO_UNLIKELY(!ptr)) {
        fprintf(stderr, "cando: out of memory allocating %zu bytes\n", size);
        abort();
    }
    return ptr;
}

void *cando_realloc(void *ptr, usize new_size) {
    CANDO_ASSERT(new_size > 0);
    void *result = realloc(ptr, new_size);
    if (CANDO_UNLIKELY(!result)) {
        fprintf(stderr, "cando: out of memory reallocating to %zu bytes\n", new_size);
        abort();
    }
    return result;
}

void cando_free(void *ptr) {
    free(ptr);
}
