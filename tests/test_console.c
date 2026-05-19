/*
 * tests/test_console.c -- C unit tests for the console standard
 * library's pure-C helpers.
 *
 * Covers:
 *   - The VT-escape key decoder (printable, ctrl, alt, F-keys,
 *     arrows, Home/End/Insert/Delete/PageUp/Down, SGR mouse).
 *   - Bracketed-paste + focus event dropping.
 *   - Event queue push/pop + drop-newest on overflow.
 *   - Encoding helpers (no Linux/Windows specifics).
 *
 * Does NOT link against libcando -- the helpers are pure-C.  The
 * Makefile entry compiles this file with the same `iquote` flags
 * the rest of source/lib uses.
 *
 * Exit 0 on success.
 */

#include "lib/console_input.h"
#include "lib/console_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0, g_passed = 0, g_failed = 0;

#define EXPECT(cond) do { \
    g_run++; \
    if (cond) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define EXPECT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    g_run++; \
    if (_a && _b && strcmp(_a, _b) == 0) { g_passed++; } \
    else { g_failed++; \
           fprintf(stderr, "FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                   _a ? _a : "(null)", _b ? _b : "(null)"); } \
} while (0)

/* Feed a literal byte sequence; assert the decoder produces a key
 * event with the expected name + modifier flags. */
static void expect_key_seq(const char *desc, const char *bytes, size_t len,
                           const char *expect_name,
                           bool ctrl, bool alt, bool shift)
{
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    ConsoleInputResult cr = CIR_PENDING;
    for (size_t i = 0; i < len; i++) {
        cr = console_input_feed(&st, (uint8_t)bytes[i], &ev);
        if (cr == CIR_DONE_KEY) break;
    }
    g_run++;
    if (cr != CIR_DONE_KEY) {
        g_failed++;
        fprintf(stderr, "FAIL key '%s': decoder didn't complete (result=%d)\n",
                desc, (int)cr);
        return;
    }
    if (strcmp(ev.as.key.name, expect_name) != 0) {
        g_failed++;
        fprintf(stderr, "FAIL key '%s': name was \"%s\", expected \"%s\"\n",
                desc, ev.as.key.name, expect_name);
        return;
    }
    if (ev.as.key.ctrl != ctrl || ev.as.key.alt != alt
        || ev.as.key.shift != shift) {
        g_failed++;
        fprintf(stderr, "FAIL key '%s': mods c/a/s = %d/%d/%d, expected %d/%d/%d\n",
                desc, ev.as.key.ctrl, ev.as.key.alt, ev.as.key.shift,
                ctrl, alt, shift);
        return;
    }
    g_passed++;
}

/* --------------- decoder tests --------------- */

static void test_plain_keys(void)
{
    expect_key_seq("a",           "a",     1, "a",         false, false, false);
    expect_key_seq("z",           "z",     1, "z",         false, false, false);
    expect_key_seq("space",       " ",     1, "Space",     false, false, false);
    expect_key_seq("Enter (LF)",  "\n",    1, "Enter",     false, false, false);
    expect_key_seq("Enter (CR)",  "\r",    1, "Enter",     false, false, false);
    expect_key_seq("Tab",         "\t",    1, "Tab",       false, false, false);
    expect_key_seq("Backspace 0x7f",  "\x7f",  1, "Backspace", false, false, false);
    expect_key_seq("Backspace 0x08",  "\x08",  1, "Backspace", false, false, false);
}

static void test_ctrl_keys(void)
{
    expect_key_seq("Ctrl-A", "\x01", 1, "a", true, false, false);
    expect_key_seq("Ctrl-C", "\x03", 1, "c", true, false, false);
    expect_key_seq("Ctrl-D", "\x04", 1, "d", true, false, false);
    expect_key_seq("Ctrl-Z", "\x1a", 1, "z", true, false, false);
}

static void test_arrow_keys(void)
{
    expect_key_seq("ArrowUp",    "\x1b[A", 3, "ArrowUp",    false, false, false);
    expect_key_seq("ArrowDown",  "\x1b[B", 3, "ArrowDown",  false, false, false);
    expect_key_seq("ArrowRight", "\x1b[C", 3, "ArrowRight", false, false, false);
    expect_key_seq("ArrowLeft",  "\x1b[D", 3, "ArrowLeft",  false, false, false);
    expect_key_seq("Home",       "\x1b[H", 3, "Home",       false, false, false);
    expect_key_seq("End",        "\x1b[F", 3, "End",        false, false, false);
}

static void test_arrow_with_modifier(void)
{
    /* ESC[1;2A == Shift-Up.  ESC[1;5A == Ctrl-Up. */
    expect_key_seq("Shift+Up", "\x1b[1;2A", 6, "ArrowUp", false, false, true);
    expect_key_seq("Ctrl+Up",  "\x1b[1;5A", 6, "ArrowUp", true,  false, false);
    expect_key_seq("Alt+Up",   "\x1b[1;3A", 6, "ArrowUp", false, true,  false);
    expect_key_seq("Ctrl+Shift+Up",
                   "\x1b[1;6A", 6, "ArrowUp", true, false, true);
}

static void test_function_keys(void)
{
    expect_key_seq("F1 (ESC O P)",     "\x1bOP",     3, "F1",  false, false, false);
    expect_key_seq("F2",               "\x1bOQ",     3, "F2",  false, false, false);
    expect_key_seq("F3",               "\x1bOR",     3, "F3",  false, false, false);
    expect_key_seq("F4",               "\x1bOS",     3, "F4",  false, false, false);
    expect_key_seq("F5 (ESC[15~)",     "\x1b[15~",   5, "F5",  false, false, false);
    expect_key_seq("F12",              "\x1b[24~",   5, "F12", false, false, false);
}

static void test_pageup_pagedown_insert_delete(void)
{
    expect_key_seq("Insert",   "\x1b[2~", 4, "Insert",   false, false, false);
    expect_key_seq("Delete",   "\x1b[3~", 4, "Delete",   false, false, false);
    expect_key_seq("PageUp",   "\x1b[5~", 4, "PageUp",   false, false, false);
    expect_key_seq("PageDown", "\x1b[6~", 4, "PageDown", false, false, false);
}

static void test_alt_letter(void)
{
    /* ESC + 'a' is Alt-a.  Split the hex escape so the compiler
     * doesn't fold the trailing letter into the hex literal. */
    expect_key_seq("Alt-a", "\x1b" "a", 2, "a", false, true, false);
    expect_key_seq("Alt-z", "\x1b" "z", 2, "z", false, true, false);
}

static void test_escape_flush(void)
{
    /* Bare ESC: must be committed via flush() since the decoder can't
     * disambiguate from a prefix without a timeout. */
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    EXPECT(console_input_feed(&st, 0x1b, &ev) == CIR_PENDING);
    EXPECT(console_input_flush(&st, &ev)      == CIR_DONE_KEY);
    EXPECT(strcmp(ev.as.key.name, "Escape") == 0);
}

static void test_bracketed_paste_dropped(void)
{
    /* ESC[200~ (paste start) should be DROPped, not fire onKey. */
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    ConsoleInputResult cr = CIR_PENDING;
    const char *seq = "\x1b[200~";
    for (size_t i = 0; i < strlen(seq); i++) {
        cr = console_input_feed(&st, (uint8_t)seq[i], &ev);
    }
    EXPECT(cr == CIR_DROP);
}

static void test_mouse_sgr_press(void)
{
    /* ESC[<0;10;5M  ->  left button press at (10, 5). */
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    ConsoleInputResult cr = CIR_PENDING;
    const char *seq = "\x1b[<0;10;5M";
    for (size_t i = 0; i < strlen(seq); i++) {
        cr = console_input_feed(&st, (uint8_t)seq[i], &ev);
    }
    EXPECT(cr == CIR_DONE_MOUSE);
    EXPECT(ev.as.mouse.x == 10);
    EXPECT(ev.as.mouse.y == 5);
    EXPECT_STREQ(ev.as.mouse.button, "left");
    EXPECT_STREQ(ev.as.mouse.action, "press");
    EXPECT(ev.as.mouse.shift == false);
    EXPECT(ev.as.mouse.alt   == false);
    EXPECT(ev.as.mouse.ctrl  == false);
}

static void test_mouse_sgr_release(void)
{
    /* The lowercase terminator means "release". */
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    ConsoleInputResult cr = CIR_PENDING;
    const char *seq = "\x1b[<2;3;4m";
    for (size_t i = 0; i < strlen(seq); i++) {
        cr = console_input_feed(&st, (uint8_t)seq[i], &ev);
    }
    EXPECT(cr == CIR_DONE_MOUSE);
    EXPECT(ev.as.mouse.x == 3);
    EXPECT(ev.as.mouse.y == 4);
    EXPECT_STREQ(ev.as.mouse.button, "right");
    EXPECT_STREQ(ev.as.mouse.action, "release");
}

static void test_mouse_wheel(void)
{
    /* Wheel up = button code 64. */
    ConsoleInputState st;
    console_input_reset(&st);
    ConsoleEvent ev;
    ConsoleInputResult cr = CIR_PENDING;
    const char *seq = "\x1b[<64;1;1M";
    for (size_t i = 0; i < strlen(seq); i++) {
        cr = console_input_feed(&st, (uint8_t)seq[i], &ev);
    }
    EXPECT(cr == CIR_DONE_MOUSE);
    EXPECT_STREQ(ev.as.mouse.button, "wheel-up");
}

/* --------------- event queue tests --------------- */

static void make_key_event(ConsoleEvent *ev, const char *name)
{
    memset(ev, 0, sizeof(*ev));
    ev->kind = CEV_KEY;
    strncpy(ev->as.key.name, name, CONSOLE_KEY_NAME_MAX - 1);
    ev->as.key.name_len = (uint8_t)strlen(ev->as.key.name);
}

static void test_eventq_basics(void)
{
    ConsoleEventQueue q = {0};
    console_eventq_init(&q);

    EXPECT(console_eventq_count(&q) == 0);

    ConsoleEvent ev;
    EXPECT(console_eventq_pop(&q, &ev) == false);

    make_key_event(&ev, "a");
    EXPECT(console_eventq_push(&q, &ev) == true);
    EXPECT(console_eventq_count(&q) == 1);

    ConsoleEvent out;
    EXPECT(console_eventq_pop(&q, &out) == true);
    EXPECT_STREQ(out.as.key.name, "a");
    EXPECT(console_eventq_count(&q) == 0);

    console_eventq_destroy(&q);
}

static void test_eventq_fifo_order(void)
{
    ConsoleEventQueue q = {0};
    console_eventq_init(&q);
    ConsoleEvent ev;
    for (int i = 0; i < 5; i++) {
        char name[2] = { (char)('a' + i), 0 };
        make_key_event(&ev, name);
        console_eventq_push(&q, &ev);
    }
    for (int i = 0; i < 5; i++) {
        ConsoleEvent out;
        EXPECT(console_eventq_pop(&q, &out) == true);
        char expected[2] = { (char)('a' + i), 0 };
        EXPECT_STREQ(out.as.key.name, expected);
    }
    console_eventq_destroy(&q);
}

static void test_eventq_drop_newest_on_overflow(void)
{
    ConsoleEventQueue q = {0};
    console_eventq_init(&q);
    ConsoleEvent ev;

    /* Fill the queue. */
    for (int i = 0; i < CONSOLE_EVENT_QUEUE_CAP; i++) {
        make_key_event(&ev, "oldest");
        EXPECT(console_eventq_push(&q, &ev) == true);
    }
    /* Next push should be dropped. */
    make_key_event(&ev, "newest");
    EXPECT(console_eventq_push(&q, &ev) == false);

    /* Pop one: should still be "oldest" (the original). */
    ConsoleEvent out;
    EXPECT(console_eventq_pop(&q, &out) == true);
    EXPECT_STREQ(out.as.key.name, "oldest");

    console_eventq_destroy(&q);
}

/* --------------- main --------------- */

int main(void)
{
    test_plain_keys();
    test_ctrl_keys();
    test_arrow_keys();
    test_arrow_with_modifier();
    test_function_keys();
    test_pageup_pagedown_insert_delete();
    test_alt_letter();
    test_escape_flush();
    test_bracketed_paste_dropped();
    test_mouse_sgr_press();
    test_mouse_sgr_release();
    test_mouse_wheel();
    test_eventq_basics();
    test_eventq_fifo_order();
    test_eventq_drop_newest_on_overflow();

    printf("test_console: %d run, %d passed, %d failed\n",
           g_run, g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
