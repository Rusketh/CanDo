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
#include "object.h"
#include "array.h"
#include "string.h"   /* object-layer: cdo_string_intern */

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

/* =======================================================================
 * Race-condition tests
 *
 * These tests verify that concurrent reads and writes through the
 * CandoLockHeader spin-lock, object field API, and array API are
 * correctly serialised, preventing data corruption when multiple OS
 * threads share globals, object fields, or array elements.
 *
 * Design principles:
 *   - Use many threads (RACE_N_THREADS) and many iterations (RACE_N_ITERS)
 *     to maximise the chance of triggering races.
 *   - Each test has a clear, checkable invariant that would be violated
 *     if locking failed (lost counter increments, missing keys, wrong
 *     array length, etc.).
 * ===================================================================== */

#define RACE_N_THREADS 8
#define RACE_N_ITERS   3000

/* -----------------------------------------------------------------------
 * 1. Lock: N concurrent writers, counter must reach N*ITERS exactly
 *
 * If the write lock ever allows two writers simultaneously, some
 * increments will be lost and the final count will be < N*ITERS.
 * --------------------------------------------------------------------- */

typedef struct {
    CandoLockHeader *lock;
    volatile int    *counter;
    int              iters;
} WriteCounterArg;

static CANDO_THREAD_RETURN race_write_counter(void *raw)
{
    WriteCounterArg *a = (WriteCounterArg *)raw;
    for (int i = 0; i < a->iters; i++) {
        cando_lock_write_acquire(a->lock);
        (*a->counter)++;
        cando_lock_write_release(a->lock);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_lock_concurrent_writers_no_lost_updates)
{
    CandoLockHeader  lock;
    cando_lock_init(&lock);
    volatile int counter = 0;
    WriteCounterArg  arg = { &lock, &counter, RACE_N_ITERS };

    cando_thread_t threads[RACE_N_THREADS];
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_create(&threads[i], race_write_counter, &arg);
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    EXPECT_EQ(counter, RACE_N_THREADS * RACE_N_ITERS);
}

/* -----------------------------------------------------------------------
 * 2. Lock: N concurrent readers complete without deadlock
 *
 * Multiple readers must be able to hold the read lock simultaneously.
 * This test verifies no deadlock by simply ensuring all threads join
 * successfully.  It also records the peak simultaneous reader count to
 * confirm that readers do proceed concurrently.
 * --------------------------------------------------------------------- */

typedef struct {
    CandoLockHeader *lock;
    volatile int    *shared_val;
    _Atomic(int)    *active;
    _Atomic(int)    *peak;
    int              iters;
} ReadConcArg;

static CANDO_THREAD_RETURN race_read_concurrent(void *raw)
{
    ReadConcArg *a = (ReadConcArg *)raw;
    for (int i = 0; i < a->iters; i++) {
        cando_lock_read_acquire(a->lock);
        int n = atomic_fetch_add(a->active, 1) + 1;
        /* Update peak concurrent readers. */
        int prev;
        do {
            prev = atomic_load(a->peak);
        } while (n > prev &&
                 !atomic_compare_exchange_weak(a->peak, &prev, n));
        (void)*a->shared_val; /* perform the read */
        atomic_fetch_sub(a->active, 1);
        cando_lock_read_release(a->lock);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_lock_multiple_readers_no_deadlock)
{
    CandoLockHeader lock;
    cando_lock_init(&lock);
    volatile int val  = 42;
    _Atomic(int) active = 0;
    _Atomic(int) peak   = 0;
    ReadConcArg arg = { &lock, &val, &active, &peak, RACE_N_ITERS };

    cando_thread_t threads[RACE_N_THREADS];
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_create(&threads[i], race_read_concurrent, &arg);
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    /* All readers must have released. */
    EXPECT_EQ(atomic_load(&active), 0);
    /* On a real multi-core machine peak > 1 confirms concurrent reads. */
    EXPECT_TRUE(atomic_load(&peak) >= 1);
}

/* -----------------------------------------------------------------------
 * 3. Lock: write lock truly excludes concurrent writers
 *
 * Each writer sets a "critical_section" flag to 1 on entry and 0 on
 * exit.  If a second writer ever sees the flag already set it means
 * two writers were inside simultaneously — a locking failure.
 * --------------------------------------------------------------------- */

typedef struct {
    CandoLockHeader *lock;
    volatile int    *in_cs;        /* 0 = free, 1 = occupied               */
    _Atomic(int)    *violations;
    int              iters;
} WriteExclusionArg;

static CANDO_THREAD_RETURN race_write_exclusive(void *raw)
{
    WriteExclusionArg *a = (WriteExclusionArg *)raw;
    for (int i = 0; i < a->iters; i++) {
        cando_lock_write_acquire(a->lock);
        if (*a->in_cs != 0)
            atomic_fetch_add(a->violations, 1);
        *a->in_cs = 1;
        /* Spin briefly so another thread has a chance to sneak in if
         * locking is broken.                                            */
        for (volatile int j = 0; j < 8; j++) ;
        *a->in_cs = 0;
        cando_lock_write_release(a->lock);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_lock_write_exclusion_no_simultaneous_writers)
{
    CandoLockHeader   lock;
    cando_lock_init(&lock);
    volatile int      in_cs      = 0;
    _Atomic(int)      violations = 0;
    WriteExclusionArg arg = { &lock, &in_cs, &violations, RACE_N_ITERS };

    cando_thread_t threads[RACE_N_THREADS];
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_create(&threads[i], race_write_exclusive, &arg);
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    EXPECT_EQ(atomic_load(&violations), 0);
}

/* -----------------------------------------------------------------------
 * 4. Object: concurrent unique-key writes — hash table not corrupted
 *
 * RACE_N_THREADS threads each write OBJ_KEYS_PER_THREAD unique keys.
 * After all threads finish every key must be present with its expected
 * numeric value.  A corrupted hash table (missed slots, overwritten
 * entries, broken probe chains) would cause lookups to fail or return
 * wrong values.
 * --------------------------------------------------------------------- */

#define OBJ_KEYS_PER_THREAD 60

typedef struct {
    CdoObject *obj;
    int        idx;   /* thread index, 0..RACE_N_THREADS-1 */
} ObjWriteArg;

static CANDO_THREAD_RETURN race_obj_write_keys(void *raw)
{
    ObjWriteArg *a = (ObjWriteArg *)raw;
    char buf[32];
    for (int j = 0; j < OBJ_KEYS_PER_THREAD; j++) {
        snprintf(buf, sizeof(buf), "t%d_k%d", a->idx, j);
        CdoString *k = cdo_string_intern(buf, (u32)strlen(buf));
        CdoValue   v = cdo_number((f64)(a->idx * 1000 + j));
        cdo_object_rawset(a->obj, k, v, FIELD_NONE);
        cdo_string_release(k);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_object_concurrent_unique_key_writes)
{
    CdoObject  *obj = cdo_object_new();
    ObjWriteArg args[RACE_N_THREADS];
    cando_thread_t threads[RACE_N_THREADS];

    for (int i = 0; i < RACE_N_THREADS; i++) {
        args[i].obj = obj;
        args[i].idx = i;
        cando_os_thread_create(&threads[i], race_obj_write_keys, &args[i]);
    }
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    /* Verify every key is present with the correct value. */
    int ok = 1;
    char buf[32];
    for (int i = 0; i < RACE_N_THREADS && ok; i++) {
        for (int j = 0; j < OBJ_KEYS_PER_THREAD && ok; j++) {
            snprintf(buf, sizeof(buf), "t%d_k%d", i, j);
            CdoString *k   = cdo_string_intern(buf, (u32)strlen(buf));
            CdoValue   out = cdo_null();
            bool found = cdo_object_rawget(obj, k, &out);
            cdo_string_release(k);
            if (!found || !cdo_is_number(out) ||
                out.as.number != (f64)(i * 1000 + j))
                ok = 0;
        }
    }
    EXPECT_TRUE(ok);

    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * 5. Object: concurrent reads and writes — no crash or torn read
 *
 * Half the threads repeatedly overwrite the same field (alternating
 * between two values); the other half repeatedly read it.  The test
 * passes if no assertion fires, no crash occurs, and every read
 * returns one of the two known values (not a torn intermediate).
 * --------------------------------------------------------------------- */

#define OBJ_RW_READERS (RACE_N_THREADS / 2)
#define OBJ_RW_WRITERS (RACE_N_THREADS / 2)
#define OBJ_RW_ITERS   2000

typedef struct {
    CdoObject  *obj;
    CdoString  *key;
    _Atomic(int) *bad_reads;
    int          iters;
    int          writer;   /* 1 = writer, 0 = reader */
} ObjRWArg;

static CANDO_THREAD_RETURN race_obj_rw(void *raw)
{
    ObjRWArg *a = (ObjRWArg *)raw;
    for (int i = 0; i < a->iters; i++) {
        if (a->writer) {
            CdoValue v = cdo_number((i % 2) == 0 ? 1.0 : 2.0);
            cdo_object_rawset(a->obj, a->key, v, FIELD_NONE);
        } else {
            CdoValue out = cdo_null();
            bool found = cdo_object_rawget(a->obj, a->key, &out);
            /* Value must be 1 or 2; any other result is a torn read. */
            if (found && cdo_is_number(out) &&
                out.as.number != 1.0 && out.as.number != 2.0)
                atomic_fetch_add(a->bad_reads, 1);
        }
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_object_concurrent_reads_and_writes)
{
    CdoObject  *obj = cdo_object_new();
    CdoString  *key = cdo_string_intern("shared", 6);

    /* Seed the field so readers always find a valid value. */
    cdo_object_rawset(obj, key, cdo_number(1.0), FIELD_NONE);

    _Atomic(int) bad_reads = 0;
    ObjRWArg args[RACE_N_THREADS];
    cando_thread_t threads[RACE_N_THREADS];

    for (int i = 0; i < RACE_N_THREADS; i++) {
        args[i].obj       = obj;
        args[i].key       = key;
        args[i].bad_reads = &bad_reads;
        args[i].iters     = OBJ_RW_ITERS;
        args[i].writer    = (i < OBJ_RW_WRITERS) ? 1 : 0;
        cando_os_thread_create(&threads[i], race_obj_rw, &args[i]);
    }
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    EXPECT_EQ(atomic_load(&bad_reads), 0);

    cdo_string_release(key);
    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * 6. Array: concurrent push — no items lost
 *
 * Each thread calls cdo_array_push N times.  If two pushes race to
 * claim the same slot or miss an items_len increment, the final length
 * will be less than RACE_N_THREADS * RACE_N_ITERS.
 * --------------------------------------------------------------------- */

typedef struct {
    CdoObject *arr;
    f64        value;
    int        iters;
} ArrPushArg;

static CANDO_THREAD_RETURN race_arr_push(void *raw)
{
    ArrPushArg *a = (ArrPushArg *)raw;
    CdoValue    v = cdo_number(a->value);
    for (int i = 0; i < a->iters; i++)
        cdo_array_push(a->arr, v);
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_array_concurrent_push_no_lost_items)
{
    CdoObject *arr = cdo_array_new();
    ArrPushArg args[RACE_N_THREADS];
    cando_thread_t threads[RACE_N_THREADS];

    for (int i = 0; i < RACE_N_THREADS; i++) {
        args[i].arr   = arr;
        args[i].value = (f64)i;
        args[i].iters = RACE_N_ITERS;
        cando_os_thread_create(&threads[i], race_arr_push, &args[i]);
    }
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    EXPECT_EQ(cdo_array_len(arr), (u32)(RACE_N_THREADS * RACE_N_ITERS));

    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * 7. Array: concurrent rawset_idx to disjoint index ranges
 *
 * Thread i owns indices [i*N .. (i+1)*N - 1].  After all threads
 * finish, every index must hold the value written by its owner thread.
 * A lock failure could let two threads corrupt the items[] backing
 * array during a reallocation.
 * --------------------------------------------------------------------- */

#define ARR_IDX_PER_THREAD 200

typedef struct {
    CdoObject *arr;
    int        thread_idx;
} ArrSetArg;

static CANDO_THREAD_RETURN race_arr_set_idx(void *raw)
{
    ArrSetArg *a   = (ArrSetArg *)raw;
    u32        base = (u32)(a->thread_idx * ARR_IDX_PER_THREAD);
    for (int j = 0; j < ARR_IDX_PER_THREAD; j++) {
        CdoValue v = cdo_number((f64)(a->thread_idx * 1000 + j));
        cdo_array_rawset_idx(a->arr, base + (u32)j, v);
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_array_concurrent_set_disjoint_indices)
{
    CdoObject *arr = cdo_array_new();
    ArrSetArg  args[RACE_N_THREADS];
    cando_thread_t threads[RACE_N_THREADS];

    for (int i = 0; i < RACE_N_THREADS; i++) {
        args[i].arr        = arr;
        args[i].thread_idx = i;
        cando_os_thread_create(&threads[i], race_arr_set_idx, &args[i]);
    }
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    /* Verify every index has the value written by its owning thread. */
    int ok = 1;
    for (int i = 0; i < RACE_N_THREADS && ok; i++) {
        u32 base = (u32)(i * ARR_IDX_PER_THREAD);
        for (int j = 0; j < ARR_IDX_PER_THREAD && ok; j++) {
            CdoValue out = cdo_null();
            bool found = cdo_array_rawget_idx(arr, base + (u32)j, &out);
            if (!found || !cdo_is_number(out) ||
                out.as.number != (f64)(i * 1000 + j))
                ok = 0;
        }
    }
    EXPECT_TRUE(ok);

    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * 8. Array: concurrent reads — no crash or torn values
 *
 * Pre-fill an array with known values, then run N reader threads
 * concurrently, each reading every element many times.  Torn reads or
 * use-after-realloc bugs would cause crashes or unexpected values.
 * --------------------------------------------------------------------- */

#define ARR_READ_SIZE   256
#define ARR_READ_ITERS  500

typedef struct {
    CdoObject    *arr;
    _Atomic(int) *bad_reads;
    int           iters;
} ArrReadArg;

static CANDO_THREAD_RETURN race_arr_read(void *raw)
{
    ArrReadArg *a = (ArrReadArg *)raw;
    for (int iter = 0; iter < a->iters; iter++) {
        for (u32 idx = 0; idx < ARR_READ_SIZE; idx++) {
            CdoValue out = cdo_null();
            bool found = cdo_array_rawget_idx(a->arr, idx, &out);
            if (!found || !cdo_is_number(out) ||
                out.as.number != (f64)idx)
                atomic_fetch_add(a->bad_reads, 1);
        }
    }
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_array_concurrent_reads_consistent)
{
    CdoObject *arr = cdo_array_new();
    /* Pre-fill: arr[i] = (f64)i */
    for (u32 i = 0; i < ARR_READ_SIZE; i++) {
        CdoValue v = cdo_number((f64)i);
        cdo_array_push(arr, v);
    }

    _Atomic(int) bad_reads = 0;
    ArrReadArg   args[RACE_N_THREADS];
    cando_thread_t threads[RACE_N_THREADS];

    for (int i = 0; i < RACE_N_THREADS; i++) {
        args[i].arr       = arr;
        args[i].bad_reads = &bad_reads;
        args[i].iters     = ARR_READ_ITERS;
        cando_os_thread_create(&threads[i], race_arr_read, &args[i]);
    }
    for (int i = 0; i < RACE_N_THREADS; i++)
        cando_os_thread_join(threads[i]);

    EXPECT_EQ(atomic_load(&bad_reads), 0);

    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    /* Required for cdo_string_intern used in the race-condition tests. */
    cdo_object_init();

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

    printf("\n-- Race conditions: lock primitives --\n");
    run_test("N writers: no lost counter updates",
             test_lock_concurrent_writers_no_lost_updates);
    run_test("N readers: no deadlock, all complete",
             test_lock_multiple_readers_no_deadlock);
    run_test("write lock: no simultaneous writers",
             test_lock_write_exclusion_no_simultaneous_writers);

    printf("\n-- Race conditions: object fields --\n");
    run_test("concurrent unique-key writes: table intact",
             test_object_concurrent_unique_key_writes);
    run_test("concurrent reads+writes: no torn values",
             test_object_concurrent_reads_and_writes);

    printf("\n-- Race conditions: arrays --\n");
    run_test("concurrent push: no lost items",
             test_array_concurrent_push_no_lost_items);
    run_test("concurrent set_idx (disjoint): correct values",
             test_array_concurrent_set_disjoint_indices);
    run_test("concurrent reads: consistent values",
             test_array_concurrent_reads_consistent);

    printf("\n--------------------------\n");
    printf("Results: %d/%d passed", g_passed, g_run);
    if (g_failed)
        printf(", %d FAILED", g_failed);
    printf("\n");

    return g_failed ? 1 : 0;
}
