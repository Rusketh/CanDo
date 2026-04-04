/*
 * tests/test_thread.c -- Unit tests for the CdoThread object type.
 *
 * Tests lifecycle, state transitions, wait/signal, and basic concurrency
 * of the CdoThread primitive independent of the VM.
 *
 * Compile:
 *   gcc -std=c11 -pthread -D_GNU_SOURCE \
 *       -iquote source/core -iquote source/object \
 *       source/core/common.c source/core/lock.c \
 *       source/core/thread_platform.c \
 *       source/object/string.c source/object/value.c source/object/object.c \
 *       source/object/array.c source/object/function.c source/object/class.c \
 *       source/object/thread.c \
 *       tests/test_thread.c -o tests/test_thread
 *   ./tests/test_thread
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "value.h"
#include "thread.h"
#include "thread_platform.h"
#include "lock.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* -----------------------------------------------------------------------
 * Minimal test harness (mirrors test_object.c)
 * --------------------------------------------------------------------- */
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_run++; \
        if (cond) { g_passed++; } \
        else { g_failed++; \
               fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-52s ", name);
    fflush(stdout);
    int before = g_failed;
    fn();
    printf("%s\n", g_failed == before ? "OK" : "FAILED");
}

/* -----------------------------------------------------------------------
 * CdoThread lifecycle tests
 * --------------------------------------------------------------------- */

TEST(test_thread_new) {
    CandoValue fn = cando_null();   /* placeholder; no real execution here */
    CdoThread *t  = cdo_thread_new(fn);

    EXPECT_TRUE(t != NULL);
    EXPECT_EQ(t->kind, (ObjectKind)OBJ_THREAD);
    EXPECT_EQ((int)atomic_load(&t->state), (int)CDO_THREAD_PENDING);
    EXPECT_EQ(t->result_count, 0u);
    EXPECT_FALSE(cdo_thread_is_done(t));

    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_thread_set_results) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    CandoValue vals[2] = { cando_number(1.0), cando_number(2.0) };
    cdo_thread_set_results(t, vals, 2);

    EXPECT_EQ((int)atomic_load(&t->state), (int)CDO_THREAD_DONE);
    EXPECT_TRUE(cdo_thread_is_done(t));
    EXPECT_EQ(t->result_count, 2u);
    EXPECT_EQ(t->results[0].as.number, 1.0);
    EXPECT_EQ(t->results[1].as.number, 2.0);

    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_thread_set_error) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    CandoValue err = cando_number(404.0);
    cdo_thread_set_error(t, err);

    EXPECT_EQ((int)atomic_load(&t->state), (int)CDO_THREAD_ERROR);
    EXPECT_TRUE(cdo_thread_is_done(t));
    EXPECT_EQ(t->error.as.number, 404.0);

    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_thread_is_done_cancelled) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    EXPECT_FALSE(cdo_thread_is_done(t));

    CdoThreadState expected = CDO_THREAD_PENDING;
    atomic_compare_exchange_strong(&t->state, &expected, CDO_THREAD_CANCELLED);

    EXPECT_TRUE(cdo_thread_is_done(t));

    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * Wait/signal tests: a background pthread resolves the thread.
 * --------------------------------------------------------------------- */

typedef struct {
    CdoThread *thread;
    f64        value;
    u32        delay_ms;
} WaiterArg;

static CANDO_THREAD_RETURN bg_set_result(void *arg)
{
    WaiterArg *a = (WaiterArg *)arg;
    if (a->delay_ms)
        cando_os_thread_sleep_ms(a->delay_ms);

    /* Transition PENDING -> RUNNING first (mirrors vm_thread_trampoline) */
    CdoThreadState expected = CDO_THREAD_PENDING;
    atomic_compare_exchange_strong(&a->thread->state, &expected,
                                   CDO_THREAD_RUNNING);

    CandoValue result = cando_number(a->value);
    cdo_thread_set_results(a->thread, &result, 1);
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_thread_wait_blocks_until_done) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    WaiterArg arg = { t, 42.0, 5 /* ms */ };
    cando_thread_t os_t;
    bool ok = cando_os_thread_create(&os_t, bg_set_result, &arg);
    EXPECT_TRUE(ok);

    /* cdo_thread_wait must block until the background thread calls
     * cdo_thread_set_results and broadcasts on done_cond. */
    cdo_thread_wait(t);

    EXPECT_EQ((int)atomic_load(&t->state), (int)CDO_THREAD_DONE);
    EXPECT_EQ(t->result_count, 1u);
    EXPECT_EQ(t->results[0].as.number, 42.0);

    cando_os_thread_join(os_t);
    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_thread_wait_already_done) {
    /* If the thread is already DONE, cdo_thread_wait must return immediately. */
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    CandoValue v = cando_number(7.0);
    cdo_thread_set_results(t, &v, 1);

    /* Should not block. */
    cdo_thread_wait(t);

    EXPECT_TRUE(cdo_thread_is_done(t));

    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * Concurrent result-set: multiple threads write results; only the first
 * wins because the state CAS is atomic.
 * --------------------------------------------------------------------- */

#define RACE_THREADS 8

typedef struct {
    CdoThread  *thread;
    f64         value;
    _Atomic(int) won; /* shared counter of how many threads saw DONE      */
} RaceArg;

static CANDO_THREAD_RETURN bg_race_setter(void *arg)
{
    RaceArg *a = (RaceArg *)arg;

    /* All threads race to set RUNNING from PENDING. */
    CdoThreadState expected = CDO_THREAD_PENDING;
    bool first = atomic_compare_exchange_strong(&a->thread->state, &expected,
                                                CDO_THREAD_RUNNING);
    if (first) {
        CandoValue v = cando_number(a->value);
        cdo_thread_set_results(a->thread, &v, 1);
        atomic_fetch_add(&a->won, 1);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_concurrent_result_set) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    RaceArg arg;
    arg.thread = t;
    arg.value  = 1.0;
    atomic_store(&arg.won, 0);

    cando_thread_t threads[RACE_THREADS];
    for (int i = 0; i < RACE_THREADS; i++) {
        cando_os_thread_create(&threads[i], bg_race_setter, &arg);
    }
    for (int i = 0; i < RACE_THREADS; i++) {
        cando_os_thread_join(threads[i]);
    }

    /* Exactly one thread should have won the CAS race. */
    EXPECT_EQ(atomic_load(&arg.won), 1);
    EXPECT_EQ((int)atomic_load(&t->state), (int)CDO_THREAD_DONE);
    EXPECT_EQ(t->result_count, 1u);

    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * then_fn / catch_fn field tests (object layer — no VM)
 * --------------------------------------------------------------------- */

TEST(test_then_fn_initialises_null) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);
    EXPECT_TRUE(t->then_fn.tag == TYPE_NULL);
    EXPECT_TRUE(t->catch_fn.tag == TYPE_NULL);
    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_then_fn_stored_and_released) {
    /* Simulate storing a number sentinel as then_fn; verify destroy frees it. */
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    CandoValue fake_fn = cando_number(1.0); /* any non-null value */
    cando_value_release(t->then_fn);
    t->then_fn = cando_value_copy(fake_fn);

    EXPECT_EQ(t->then_fn.as.number, 1.0);
    /* cdo_thread_destroy must release then_fn without crashing. */
    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * handle_idx field tests
 * --------------------------------------------------------------------- */

TEST(test_handle_idx_initialises_invalid) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);
    EXPECT_EQ(t->handle_idx, CANDO_INVALID_HANDLE);
    cdo_thread_destroy(t);
    cando_free(t);
}

TEST(test_handle_idx_settable) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);
    t->handle_idx = 42u;
    EXPECT_EQ(t->handle_idx, 42u);
    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * Callback fire under done_mutex: then_fn grabbed by set_results caller
 * --------------------------------------------------------------------- */

typedef struct {
    CdoThread   *thread;
    CandoValue   stored_cb;   /* "function" to store before results arrive  */
    u32          delay_ms;
} CallbackRaceArg;

static _Atomic(int) g_cb_fire_count;

static CANDO_THREAD_RETURN bg_set_results_with_cb(void *arg)
{
    CallbackRaceArg *a = (CallbackRaceArg *)arg;
    if (a->delay_ms)
        cando_os_thread_sleep_ms(a->delay_ms);

    /* Store a sentinel callback value, then immediately set results.
     * The test verifies then_fn is non-null after set_results (the
     * object layer doesn't fire callbacks — that's vm.c's job, so we
     * only check the field is still intact after the mutex dance).    */
    cando_os_mutex_lock(&a->thread->done_mutex);
    cando_value_release(a->thread->then_fn);
    a->thread->then_fn = cando_value_copy(a->stored_cb);
    cando_os_mutex_unlock(&a->thread->done_mutex);

    CdoThreadState expected = CDO_THREAD_PENDING;
    atomic_compare_exchange_strong(&a->thread->state, &expected,
                                   CDO_THREAD_RUNNING);

    CandoValue v = cando_number(1.0);
    cdo_thread_set_results(a->thread, &v, 1);
    atomic_fetch_add(&g_cb_fire_count, 1);
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_then_fn_survives_set_results) {
    CandoValue fn = cando_null();
    CdoThread *t  = cdo_thread_new(fn);

    CandoValue fake_cb = cando_number(99.0);
    atomic_store(&g_cb_fire_count, 0);

    CallbackRaceArg arg = { t, fake_cb, 2 };
    cando_thread_t os_t;
    cando_os_thread_create(&os_t, bg_set_results_with_cb, &arg);
    cdo_thread_wait(t);
    cando_os_thread_join(os_t);

    /* then_fn was set before set_results; object layer preserves it. */
    EXPECT_EQ(t->then_fn.as.number, 99.0);
    EXPECT_EQ(atomic_load(&g_cb_fire_count), 1);

    cdo_thread_destroy(t);
    cando_free(t);
}

/* -----------------------------------------------------------------------
 * thread.id() / cando_thread_id() tests
 * --------------------------------------------------------------------- */

TEST(test_thread_id_nonzero) {
    u64 id = cando_thread_id();
    EXPECT_NEQ(id, 0u);
}

static _Atomic(u64) g_child_id;

static CANDO_THREAD_RETURN bg_record_id(void *arg)
{
    (void)arg;
    atomic_store(&g_child_id, cando_thread_id());
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_thread_id_differs_between_threads) {
    u64 parent_id = cando_thread_id();
    atomic_store(&g_child_id, 0u);

    cando_thread_t t;
    cando_os_thread_create(&t, bg_record_id, NULL);
    cando_os_thread_join(t);

    u64 child_id = atomic_load(&g_child_id);
    EXPECT_NEQ(child_id, 0u);
    EXPECT_NEQ(child_id, parent_id);
}

/* -----------------------------------------------------------------------
 * sleep smoke test
 * --------------------------------------------------------------------- */

TEST(test_sleep_ms_does_not_crash) {
    /* Just verify the call completes without error; timing is not checked. */
    cando_os_thread_sleep_ms(1);
    EXPECT_TRUE(true);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    printf("\n=== CdoThread unit tests ===\n\n");

    printf("-- Lifecycle --\n");
    run_test("thread_new initialises correctly",  test_thread_new);
    run_test("set_results → DONE state",          test_thread_set_results);
    run_test("set_error → ERROR state",           test_thread_set_error);
    run_test("is_done returns true for CANCELLED",test_thread_is_done_cancelled);

    printf("\n-- Wait / signal --\n");
    run_test("wait blocks until background done", test_thread_wait_blocks_until_done);
    run_test("wait returns immediately if done",  test_thread_wait_already_done);

    printf("\n-- Concurrency --\n");
    run_test("concurrent result-set: one winner", test_concurrent_result_set);

    printf("\n-- Callbacks (object layer) --\n");
    run_test("then_fn / catch_fn init to null",    test_then_fn_initialises_null);
    run_test("then_fn stored and released safely", test_then_fn_stored_and_released);
    run_test("handle_idx initialises to invalid",  test_handle_idx_initialises_invalid);
    run_test("handle_idx is settable",             test_handle_idx_settable);
    run_test("then_fn survives set_results",       test_then_fn_survives_set_results);

    printf("\n-- Thread ID --\n");
    run_test("cando_thread_id() is non-zero",     test_thread_id_nonzero);
    run_test("child thread has different id",     test_thread_id_differs_between_threads);

    printf("\n-- Platform --\n");
    run_test("sleep_ms does not crash",           test_sleep_ms_does_not_crash);

    printf("\n--------------------------\n");
    printf("Results: %d/%d passed", g_passed, g_run);
    if (g_failed)
        printf(", %d FAILED", g_failed);
    printf("\n");

    return g_failed ? 1 : 0;
}
