/*
 * tests/test_core.c -- Unit tests for source/core: common, value, lock,
 *                      handle, memory.
 *
 * Compile via Makefile:
 *   make test
 *
 * Or manually:
 *   gcc -std=c11 -pthread -D_GNU_SOURCE -I source/core \
 *       source/core/common.c source/core/value.c source/core/lock.c \
 *       source/core/handle.c source/core/memory.c \
 *       tests/test_core.c -o tests/test_core && ./tests/test_core
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "value.h"
#include "lock.h"
#include "handle.h"
#include "memory.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Minimal test harness
 * --------------------------------------------------------------------- */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_tests_run++; \
        if (cond) { \
            g_tests_passed++; \
        } else { \
            g_tests_failed++; \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))
#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-40s ", name);
    fflush(stdout);
    int before = g_tests_failed;
    fn();
    if (g_tests_failed == before)
        printf("OK\n");
    else
        printf("FAILED\n");
}

/* -----------------------------------------------------------------------
 * common.h tests
 * --------------------------------------------------------------------- */
TEST(test_typedefs) {
    EXPECT_EQ(sizeof(u8),  1u);
    EXPECT_EQ(sizeof(u16), 2u);
    EXPECT_EQ(sizeof(u32), 4u);
    EXPECT_EQ(sizeof(u64), 8u);
    EXPECT_EQ(sizeof(i8),  1u);
    EXPECT_EQ(sizeof(i16), 2u);
    EXPECT_EQ(sizeof(i32), 4u);
    EXPECT_EQ(sizeof(i64), 8u);
    EXPECT_EQ(sizeof(f32), 4u);
    EXPECT_EQ(sizeof(f64), 8u);
}

TEST(test_alloc_realloc_free) {
    void *p = cando_alloc(128);
    EXPECT_TRUE(p != NULL);
    memset(p, 0xAB, 128);

    p = cando_realloc(p, 256);
    EXPECT_TRUE(p != NULL);

    cando_free(p);
    cando_free(NULL); /* should be safe */
}

TEST(test_array_len) {
    int arr[7];
    EXPECT_EQ(CANDO_ARRAY_LEN(arr), 7u);
}

/* -----------------------------------------------------------------------
 * value.h / value.c tests
 * --------------------------------------------------------------------- */
TEST(test_null_value) {
    CandoValue v = cando_null();
    EXPECT_TRUE(cando_is_null(v));
    EXPECT_FALSE(cando_is_bool(v));
    EXPECT_FALSE(cando_is_number(v));
    EXPECT_FALSE(cando_is_string(v));
    EXPECT_FALSE(cando_is_object(v));

    char *s = cando_value_tostring(v);
    EXPECT_STR(s, "null");
    free(s);
}

TEST(test_bool_value) {
    CandoValue t = cando_bool(true);
    CandoValue f = cando_bool(false);

    EXPECT_TRUE(cando_is_bool(t));
    EXPECT_TRUE(t.as.boolean);
    EXPECT_FALSE(f.as.boolean);

    char *st = cando_value_tostring(t);
    char *sf = cando_value_tostring(f);
    EXPECT_STR(st, "true");
    EXPECT_STR(sf, "false");
    free(st);
    free(sf);
}

TEST(test_number_value) {
    CandoValue n1 = cando_number(42.0);
    CandoValue n2 = cando_number(3.14);

    EXPECT_TRUE(cando_is_number(n1));
    EXPECT_EQ(n1.as.number, 42.0);

    char *s1 = cando_value_tostring(n1);
    EXPECT_STR(s1, "42");
    free(s1);

    char *s2 = cando_value_tostring(n2);
    /* Should contain "3.14" as a substring. */
    EXPECT_TRUE(strstr(s2, "3.14") != NULL);
    free(s2);
}

TEST(test_string_value) {
    CandoString *cs = cando_string_new("hello", 5);
    EXPECT_EQ(cs->ref_count, 1u);
    EXPECT_EQ(cs->length, 5u);
    EXPECT_STR(cs->data, "hello");

    CandoValue v = cando_string_value(cs);
    EXPECT_TRUE(cando_is_string(v));

    char *s = cando_value_tostring(v);
    EXPECT_STR(s, "hello");
    free(s);

    /* retain / release */
    cando_string_retain(cs);
    EXPECT_EQ(cs->ref_count, 2u);
    cando_value_release(v); /* decrements from 2 -> 1 */
    EXPECT_EQ(cs->ref_count, 1u);
    cando_string_release(cs); /* frees */
}

TEST(test_object_value) {
    CandoValue v = cando_object_value(7);
    EXPECT_TRUE(cando_is_object(v));
    EXPECT_EQ(v.as.handle, 7u);

    char *s = cando_value_tostring(v);
    EXPECT_TRUE(strstr(s, "object") != NULL);
    free(s);
}

TEST(test_value_equality) {
    EXPECT_TRUE(cando_value_equal(cando_null(), cando_null()));
    EXPECT_TRUE(cando_value_equal(cando_bool(true), cando_bool(true)));
    EXPECT_FALSE(cando_value_equal(cando_bool(true), cando_bool(false)));
    EXPECT_TRUE(cando_value_equal(cando_number(1.0), cando_number(1.0)));
    EXPECT_FALSE(cando_value_equal(cando_number(1.0), cando_number(2.0)));
    /* Null != Bool */
    EXPECT_FALSE(cando_value_equal(cando_null(), cando_bool(false)));

    CandoString *sa = cando_string_new("foo", 3);
    CandoString *sb = cando_string_new("foo", 3);
    CandoString *sc = cando_string_new("bar", 3);
    CandoValue va = cando_string_value(sa);
    CandoValue vb = cando_string_value(sb);
    CandoValue vc = cando_string_value(sc);
    EXPECT_TRUE(cando_value_equal(va, vb));
    EXPECT_FALSE(cando_value_equal(va, vc));

    cando_string_release(sa);
    cando_string_release(sb);
    cando_string_release(sc);
}

TEST(test_value_copy) {
    CandoString *cs = cando_string_new("copy me", 7);
    CandoValue  orig = cando_string_value(cs);
    CandoValue  copy = cando_value_copy(orig);

    EXPECT_EQ(cs->ref_count, 2u);
    cando_value_release(copy);
    EXPECT_EQ(cs->ref_count, 1u);
    cando_value_release(orig);
}

TEST(test_type_name) {
    EXPECT_STR(cando_value_type_name(TYPE_NULL),   "null");
    EXPECT_STR(cando_value_type_name(TYPE_BOOL),   "bool");
    EXPECT_STR(cando_value_type_name(TYPE_NUMBER), "number");
    EXPECT_STR(cando_value_type_name(TYPE_STRING), "string");
    EXPECT_STR(cando_value_type_name(TYPE_OBJECT), "object");
}

/* -----------------------------------------------------------------------
 * lock.h / lock.c tests
 * --------------------------------------------------------------------- */
TEST(test_lock_init) {
    CandoLockHeader hdr;
    cando_lock_init(&hdr);
    EXPECT_EQ(atomic_load(&hdr.lock_id), 0ull);
    EXPECT_EQ(atomic_load(&hdr.readers), 0u);
}

TEST(test_write_lock_basic) {
    CandoLockHeader hdr;
    cando_lock_init(&hdr);

    cando_lock_write_acquire(&hdr);
    EXPECT_TRUE(cando_lock_is_write_held_by_me(&hdr));
    cando_lock_write_release(&hdr);
    EXPECT_FALSE(cando_lock_is_write_held_by_me(&hdr));
}

TEST(test_read_lock_basic) {
    CandoLockHeader hdr;
    cando_lock_init(&hdr);

    cando_lock_read_acquire(&hdr);
    EXPECT_EQ(atomic_load(&hdr.readers), 1u);
    cando_lock_read_acquire(&hdr); /* second reader */
    EXPECT_EQ(atomic_load(&hdr.readers), 2u);
    cando_lock_read_release(&hdr);
    EXPECT_EQ(atomic_load(&hdr.readers), 1u);
    cando_lock_read_release(&hdr);
    EXPECT_EQ(atomic_load(&hdr.readers), 0u);
}

TEST(test_write_lock_reentrant) {
    CandoLockHeader hdr;
    cando_lock_init(&hdr);

    cando_lock_write_acquire(&hdr);
    cando_lock_write_acquire(&hdr); /* same thread: no deadlock */
    EXPECT_TRUE(cando_lock_is_write_held_by_me(&hdr));
    cando_lock_write_release(&hdr);
    /* After one release the lock_id is cleared (simple re-entrance model). */
    /* A second release from the same test would be a bug; we only acquired once
       due to re-entrance short-circuit, so lock_id is now 0. */
}

/* Multi-threaded stress test: N readers + 1 writer, shared counter. */
#define MT_READERS 4
#define MT_ITERS   10000

typedef struct {
    CandoLockHeader *hdr;
    volatile int    *counter;
    int              iters;
} MTArg;

static void *mt_reader(void *arg) {
    MTArg *a = arg;
    for (int i = 0; i < a->iters; i++) {
        cando_lock_read_acquire(a->hdr);
        /* Just read — no assertion here; we're testing no crash / race. */
        (void)*a->counter;
        cando_lock_read_release(a->hdr);
    }
    return NULL;
}

static void *mt_writer(void *arg) {
    MTArg *a = arg;
    for (int i = 0; i < a->iters; i++) {
        cando_lock_write_acquire(a->hdr);
        (*a->counter)++;
        cando_lock_write_release(a->hdr);
    }
    return NULL;
}

TEST(test_lock_multithreaded) {
    CandoLockHeader hdr;
    cando_lock_init(&hdr);
    volatile int counter = 0;

    MTArg arg = { .hdr = &hdr, .counter = &counter, .iters = MT_ITERS };

    pthread_t readers[MT_READERS];
    pthread_t writer;

    for (int i = 0; i < MT_READERS; i++)
        pthread_create(&readers[i], NULL, mt_reader, &arg);
    pthread_create(&writer, NULL, mt_writer, &arg);

    for (int i = 0; i < MT_READERS; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    EXPECT_EQ(counter, MT_ITERS);
}

/* -----------------------------------------------------------------------
 * thread_id uniqueness
 * --------------------------------------------------------------------- */
static _Atomic(u64) g_child_tid = 0;

static void *record_tid(void *arg) {
    CANDO_UNUSED(arg);
    atomic_store(&g_child_tid, cando_thread_id());
    return NULL;
}

TEST(test_thread_id_unique) {
    u64 main_tid = cando_thread_id();
    EXPECT_NEQ(main_tid, 0ull);

    pthread_t t;
    pthread_create(&t, NULL, record_tid, NULL);
    pthread_join(t, NULL);

    u64 child_tid = atomic_load(&g_child_tid);
    EXPECT_NEQ(child_tid, 0ull);
    EXPECT_NEQ(main_tid, child_tid);
}

/* -----------------------------------------------------------------------
 * handle.h / handle.c tests
 * --------------------------------------------------------------------- */
TEST(test_handle_alloc_free) {
    CandoHandleTable t;
    cando_handle_table_init(&t, 0);

    int   dummy1 = 42;
    int   dummy2 = 99;

    HandleIndex h1 = cando_handle_alloc(&t, &dummy1);
    HandleIndex h2 = cando_handle_alloc(&t, &dummy2);

    EXPECT_NEQ(h1, CANDO_INVALID_HANDLE);
    EXPECT_NEQ(h2, CANDO_INVALID_HANDLE);
    EXPECT_NEQ(h1, h2);
    EXPECT_EQ(t.count, 2u);

    EXPECT_EQ(cando_handle_get(&t, h1), (void *)&dummy1);
    EXPECT_EQ(cando_handle_get(&t, h2), (void *)&dummy2);

    cando_handle_free(&t, h1);
    EXPECT_EQ(t.count, 1u);
    cando_handle_free(&t, h2);
    EXPECT_EQ(t.count, 0u);

    cando_handle_table_destroy(&t);
}

TEST(test_handle_set_relocate) {
    CandoHandleTable t;
    cando_handle_table_init(&t, 0);

    int a = 1, b = 2;
    HandleIndex h = cando_handle_alloc(&t, &a);
    EXPECT_EQ(cando_handle_get(&t, h), (void *)&a);

    cando_handle_set(&t, h, &b);
    EXPECT_EQ(cando_handle_get(&t, h), (void *)&b);

    cando_handle_free(&t, h);
    cando_handle_table_destroy(&t);
}

TEST(test_handle_slot_reuse) {
    CandoHandleTable t;
    cando_handle_table_init(&t, 4);

    int x = 0;
    HandleIndex h1 = cando_handle_alloc(&t, &x);
    HandleIndex h2 = cando_handle_alloc(&t, &x);
    cando_handle_free(&t, h1);
    HandleIndex h3 = cando_handle_alloc(&t, &x);
    /* h3 should reuse the slot freed by h1 */
    EXPECT_EQ(h3, h1);
    EXPECT_EQ(t.count, 2u);

    cando_handle_free(&t, h2);
    cando_handle_free(&t, h3);
    cando_handle_table_destroy(&t);
}

TEST(test_handle_grow) {
    CandoHandleTable t;
    cando_handle_table_init(&t, 4); /* small initial cap to force growth */

    int   dummy = 0;
    HandleIndex handles[16];
    for (int i = 0; i < 16; i++)
        handles[i] = cando_handle_alloc(&t, &dummy);

    EXPECT_EQ(t.count, 16u);
    EXPECT_TRUE(t.capacity >= 16u);

    for (int i = 0; i < 16; i++)
        cando_handle_free(&t, handles[i]);

    EXPECT_EQ(t.count, 0u);
    cando_handle_table_destroy(&t);
}

TEST(test_handle_gc_marks) {
    CandoHandleTable t;
    cando_handle_table_init(&t, 0);

    int x = 0;
    HandleIndex h = cando_handle_alloc(&t, &x);
    EXPECT_FALSE(cando_handle_is_marked(&t, h));

    cando_handle_mark(&t, h);
    EXPECT_TRUE(cando_handle_is_marked(&t, h));

    cando_handle_clear_marks(&t);
    EXPECT_FALSE(cando_handle_is_marked(&t, h));

    cando_handle_free(&t, h);
    cando_handle_table_destroy(&t);
}

/* -----------------------------------------------------------------------
 * memory.h / memory.c tests
 * --------------------------------------------------------------------- */
TEST(test_memctrl_alloc_free) {
    CandoMemCtrl mc;
    cando_memctrl_init(&mc);

    HandleIndex h = cando_memctrl_alloc(&mc, 64);
    EXPECT_NEQ(h, CANDO_INVALID_HANDLE);
    EXPECT_EQ(mc.handles.count, 1u);
    EXPECT_EQ(mc.live_count, 1u);

    void *p = cando_memctrl_get_ptr(&mc, h);
    EXPECT_TRUE(p != NULL);
    /* Payload should be zero-initialised */
    unsigned char *bytes = p;
    bool all_zero = true;
    for (u32 i = 0; i < 64; i++)
        if (bytes[i] != 0) { all_zero = false; break; }
    EXPECT_TRUE(all_zero);

    /* Write to payload and read back */
    *(int *)p = 0xDEADBEEF;
    EXPECT_EQ(*(int *)cando_memctrl_get_ptr(&mc, h), (int)0xDEADBEEF);

    cando_memctrl_free(&mc, h);
    EXPECT_EQ(mc.handles.count, 0u);
    EXPECT_EQ(mc.live_count, 0u);

    cando_memctrl_destroy(&mc);
}

TEST(test_memctrl_get_header) {
    CandoMemCtrl mc;
    cando_memctrl_init(&mc);

    HandleIndex h = cando_memctrl_alloc(&mc, 32);
    CandoBlockHeader *hdr = cando_memctrl_get_header(&mc, h);

    EXPECT_TRUE(hdr != NULL);
    EXPECT_EQ(hdr->handle, h);
    EXPECT_EQ(hdr->user_size, 32u);

    /* Block header embeds a valid (unlocked) lock. */
    EXPECT_EQ(atomic_load(&hdr->lock.lock_id), 0ull);
    EXPECT_EQ(atomic_load(&hdr->lock.readers), 0u);

    cando_memctrl_free(&mc, h);
    cando_memctrl_destroy(&mc);
}

TEST(test_gc_collect_sweep) {
    CandoMemCtrl mc;
    cando_memctrl_init(&mc);

    HandleIndex h1 = cando_memctrl_alloc(&mc, 16);
    HandleIndex h2 = cando_memctrl_alloc(&mc, 16); CANDO_UNUSED(h2);
    HandleIndex h3 = cando_memctrl_alloc(&mc, 16);
    EXPECT_EQ(mc.live_count, 3u);

    /* Collect with only h1 and h3 as roots; h2 should be swept. */
    HandleIndex roots[] = { h1, h3 };
    cando_gc_collect(&mc, roots, 2);

    EXPECT_EQ(mc.live_count, 2u);
    EXPECT_EQ(mc.handles.count, 2u);

    /* h1 and h3 still valid; verify their handles still exist */
    EXPECT_TRUE(cando_memctrl_get_ptr(&mc, h1) != NULL);
    EXPECT_TRUE(cando_memctrl_get_ptr(&mc, h3) != NULL);

    /* Clean up remaining live blocks */
    cando_gc_collect(&mc, NULL, 0); /* collect with no roots — free all */
    EXPECT_EQ(mc.live_count, 0u);

    cando_memctrl_destroy(&mc);
}

TEST(test_gc_mark_preserve) {
    CandoMemCtrl mc;
    cando_memctrl_init(&mc);

    HandleIndex h = cando_memctrl_alloc(&mc, 8);
    *(u64 *)cando_memctrl_get_ptr(&mc, h) = 0xCAFEBABEull;

    /* Full collection keeping h alive. */
    cando_gc_collect(&mc, &h, 1);
    EXPECT_EQ(mc.live_count, 1u);
    EXPECT_EQ(*(u64 *)cando_memctrl_get_ptr(&mc, h), 0xCAFEBABEull);

    cando_memctrl_free(&mc, h);
    cando_memctrl_destroy(&mc);
}

TEST(test_block_per_object_lock) {
    CandoMemCtrl mc;
    cando_memctrl_init(&mc);

    HandleIndex      h   = cando_memctrl_alloc(&mc, sizeof(int));
    CandoBlockHeader *hdr = cando_memctrl_get_header(&mc, h);

    cando_lock_write_acquire(&hdr->lock);
    EXPECT_TRUE(cando_lock_is_write_held_by_me(&hdr->lock));
    *(int *)CANDO_BLOCK_PAYLOAD(hdr) = 77;
    cando_lock_write_release(&hdr->lock);
    EXPECT_FALSE(cando_lock_is_write_held_by_me(&hdr->lock));

    EXPECT_EQ(*(int *)cando_memctrl_get_ptr(&mc, h), 77);

    cando_memctrl_free(&mc, h);
    cando_memctrl_destroy(&mc);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    printf("\n=== Cando core unit tests ===\n\n");

    printf("-- common --\n");
    run_test("typedefs",              test_typedefs);
    run_test("alloc/realloc/free",    test_alloc_realloc_free);
    run_test("ARRAY_LEN",             test_array_len);

    printf("\n-- value --\n");
    run_test("null value",            test_null_value);
    run_test("bool value",            test_bool_value);
    run_test("number value",          test_number_value);
    run_test("string value",          test_string_value);
    run_test("object value",          test_object_value);
    run_test("value equality",        test_value_equality);
    run_test("value copy",            test_value_copy);
    run_test("type name",             test_type_name);

    printf("\n-- lock --\n");
    run_test("lock init",             test_lock_init);
    run_test("write lock basic",      test_write_lock_basic);
    run_test("read lock basic",       test_read_lock_basic);
    run_test("write lock reentrant",  test_write_lock_reentrant);
    run_test("lock multithreaded",    test_lock_multithreaded);
    run_test("thread_id unique",      test_thread_id_unique);

    printf("\n-- handle --\n");
    run_test("alloc/free",            test_handle_alloc_free);
    run_test("set/relocate",          test_handle_set_relocate);
    run_test("slot reuse",            test_handle_slot_reuse);
    run_test("table grow",            test_handle_grow);
    run_test("gc mark/clear",         test_handle_gc_marks);

    printf("\n-- memory --\n");
    run_test("alloc/free",            test_memctrl_alloc_free);
    run_test("get header",            test_memctrl_get_header);
    run_test("gc collect/sweep",      test_gc_collect_sweep);
    run_test("gc mark preserve",      test_gc_mark_preserve);
    run_test("per-object lock",       test_block_per_object_lock);

    printf("\n=== Results: %d/%d passed ===\n",
           g_tests_passed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
