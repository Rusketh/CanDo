/*
 * modules/forms/test_forms.c -- C unit tests for forms-module helpers.
 *
 * Built and run with:
 *
 *     make -C modules/forms test
 *
 * This file is compiled with -DFORMS_MODULE_TEST_BUILD which strips
 * libcando + Win32 from forms_module.c and exposes a small C-level
 * test API (forms_test_*).  That keeps the tests runnable on any
 * host even though the runtime backend is Windows-only.
 */

#define FORMS_MODULE_TEST_BUILD 1
#include "forms_module.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(name, cond) do {                                            \
    if (cond) {                                                            \
        printf("  PASS  %s\n", name);                                      \
    } else {                                                               \
        printf("  FAIL  %s (%s:%d)\n", name, __FILE__, __LINE__);          \
        failures++;                                                        \
    }                                                                      \
} while (0)

static void test_event_queue_basic(void)
{
    forms_test_event_queue_reset();
    EXPECT("queue starts empty", forms_test_event_queue_is_empty());
    EXPECT("queue starts non-full", !forms_test_event_queue_is_full());

    forms_test_event_push(2 /*EV_CLICK*/, 7);
    EXPECT("after push: not empty", !forms_test_event_queue_is_empty());

    int kind = -1, slot = -1;
    int popped = forms_test_event_pop(&kind, &slot);
    EXPECT("pop returns 1",  popped == 1);
    EXPECT("popped kind",     kind == 2);
    EXPECT("popped slot",     slot == 7);
    EXPECT("queue empty again", forms_test_event_queue_is_empty());

    EXPECT("pop on empty returns 0",
           forms_test_event_pop(&kind, &slot) == 0);
}

static void test_event_queue_fifo_order(void)
{
    forms_test_event_queue_reset();
    for (int i = 0; i < 16; i++) {
        forms_test_event_push(1, i);
    }
    for (int i = 0; i < 16; i++) {
        int k = -1, s = -1;
        if (!forms_test_event_pop(&k, &s)) {
            EXPECT("FIFO: queue not exhausted early", 0);
            return;
        }
        char name[64];
        snprintf(name, sizeof(name), "FIFO order at index %d (slot=%d)", i, s);
        EXPECT(name, s == i);
    }
    EXPECT("FIFO: queue empty after draining", forms_test_event_queue_is_empty());
}

static void test_event_queue_overflow_drops_newest(void)
{
    forms_test_event_queue_reset();
    /* The ring buffer reserves one slot to disambiguate full/empty, so
     * effective capacity is FORMS_EV_QUEUE_CAP - 1. */
    int pushed = 0;
    for (int i = 0; i < 10000; i++) {
        if (forms_test_event_queue_is_full()) break;
        forms_test_event_push(1, i);
        pushed++;
    }
    EXPECT("overflow: queue reports full at capacity",
           forms_test_event_queue_is_full());
    EXPECT("overflow: pushed at least one batch", pushed > 0);

    /* Push one more -- should be dropped (newest dropped, oldest preserved). */
    forms_test_event_push(1, 9999);

    /* First popped event should still be slot 0 (oldest), not 9999. */
    int k = 0, s = -1;
    forms_test_event_pop(&k, &s);
    EXPECT("overflow: oldest event preserved (slot 0 first)", s == 0);
}

static void test_slot_allocator(void)
{
    int a = forms_test_slot_alloc(1 /*KIND_FORM*/, -1);
    int b = forms_test_slot_alloc(2 /*KIND_BUTTON*/, a);
    int c = forms_test_slot_alloc(3 /*KIND_LABEL*/,  a);

    EXPECT("slot_alloc returns >= 1", a >= 1);
    EXPECT("slot index 0 is reserved (never returned)", a > 0 && b > 0 && c > 0);
    EXPECT("slots are unique",        a != b && b != c && a != c);
    EXPECT("slot kind FORM",          forms_test_slot_kind(a) == 1);
    EXPECT("slot kind BUTTON",        forms_test_slot_kind(b) == 2);

    forms_test_slot_free(b);
    int d = forms_test_slot_alloc(4 /*KIND_TEXTBOX*/, a);
    EXPECT("freed slot is reused",    d == b);
    EXPECT("recycled slot has new kind", forms_test_slot_kind(d) == 4);

    forms_test_slot_free(a);
    forms_test_slot_free(c);
    forms_test_slot_free(d);
}

/* Generation must advance on every (re)allocation so a stale handle to
 * a recycled slot is rejected when slot_from_inst checks it. */
static void test_slot_generation_advances(void)
{
    int s1 = forms_test_slot_alloc(1, -1);
    int g1 = forms_test_slot_generation(s1);
    forms_test_slot_free(s1);

    int s2 = forms_test_slot_alloc(2, -1);
    int g2 = forms_test_slot_generation(s2);

    EXPECT("recycled slot index matches",       s1 == s2);
    EXPECT("generation advances on recycle",    g2 > g1);
    EXPECT("generation difference is exactly 1",g2 - g1 == 1);

    forms_test_slot_free(s2);
}

/* Round-trip every payload field through the queue.  The dispatcher
 * uses these to populate callback args (e.g. mouse X/Y, key code), so
 * preservation across push/pop matters. */
static void test_event_payload_preservation(void)
{
    forms_test_event_queue_reset();
    forms_test_event_push_full(2 /*EV_CLICK*/, 11, 7,  /* slot, gen */
                               1, 22, 33,              /* button, key extras */
                               123.5, 456.75);

    int kind, slot, gen, i0, i1, i2;
    double d0, d1;
    int ok = forms_test_event_pop_full(&kind, &slot, &gen,
                                       &i0, &i1, &i2, &d0, &d1);
    EXPECT("payload pop ok",                    ok == 1);
    EXPECT("payload kind preserved",            kind == 2);
    EXPECT("payload slot preserved",            slot == 11);
    EXPECT("payload generation preserved",      gen == 7);
    EXPECT("payload i0 preserved",              i0 == 1);
    EXPECT("payload i1 preserved",              i1 == 22);
    EXPECT("payload i2 preserved",              i2 == 33);
    EXPECT("payload d0 preserved",              d0 == 123.5);
    EXPECT("payload d1 preserved",              d1 == 456.75);
}

/* Repeated free + alloc must converge on the same slot index forever
 * (no leak), and the queue head/tail must keep wrapping cleanly across
 * many cycles -- this catches off-by-one bugs in modulo arithmetic
 * that only surface at the wrap boundary. */
static void test_long_running_stability(void)
{
    /* 10x the queue capacity, to wrap the ring head several times. */
    forms_test_event_queue_reset();
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 100; i++) forms_test_event_push(1, i);
        for (int i = 0; i < 100; i++) {
            int k, s;
            if (!forms_test_event_pop(&k, &s)) {
                EXPECT("ring stability: pop succeeded", 0);
                return;
            }
        }
    }
    EXPECT("ring stability: empty after 1000 push/pop cycles",
           forms_test_event_queue_is_empty());

    /* Slot churn -- alloc/free 1024 times, the slot index must remain
     * bounded (i.e. we don't lose slots). */
    int seen_max = 0;
    for (int i = 0; i < 1024; i++) {
        int s = forms_test_slot_alloc(1, -1);
        if (s > seen_max) seen_max = s;
        forms_test_slot_free(s);
    }
    EXPECT("slot churn: never grew past index 1 (single-slot reuse)",
           seen_max == 1);
}

int main(void)
{
    printf("== modules/forms: C tests ==\n");
    forms_test_init_sync();

    test_event_queue_basic();
    test_event_queue_fifo_order();
    test_event_queue_overflow_drops_newest();
    test_event_payload_preservation();
    test_slot_allocator();
    test_slot_generation_advances();
    test_long_running_stability();

    if (failures == 0) {
        printf("All forms C tests passed.\n");
        return 0;
    }
    printf("%d forms C test(s) failed.\n", failures);
    return 1;
}
