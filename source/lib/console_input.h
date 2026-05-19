/*
 * lib/console_input.h -- Key + mouse decoder for the console library.
 *
 * Pure-C state machine that consumes bytes one at a time and
 * decodes them into ConsoleKeyEvent / ConsoleMouseEvent records.
 * Designed for cross-platform use: on POSIX the input stream is the
 * raw terminal pty; on Windows console_term.c shims INPUT_RECORDs
 * into the same VT escape sequence form so this decoder handles both.
 *
 * Unit-testable without a real terminal -- tests/test_console.c
 * drives feeds of known byte sequences and checks the decoded
 * events.
 */

#ifndef CANDO_LIB_CONSOLE_INPUT_H
#define CANDO_LIB_CONSOLE_INPUT_H

#include "../core/common.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Event records
 * ======================================================================= */

typedef enum ConsoleEventKind {
    CEV_NONE = 0,
    CEV_KEY,
    CEV_MOUSE,
} ConsoleEventKind;

#define CONSOLE_KEY_NAME_MAX 16
#define CONSOLE_KEY_RAW_MAX  16

typedef struct ConsoleKeyEvent {
    char     name[CONSOLE_KEY_NAME_MAX];  /* "a", "Enter", "ArrowUp", "F5", ... */
    uint8_t  name_len;
    char     raw[CONSOLE_KEY_RAW_MAX];    /* original byte sequence */
    uint8_t  raw_len;
    bool     ctrl;
    bool     alt;
    bool     shift;
    bool     meta;
} ConsoleKeyEvent;

typedef struct ConsoleMouseEvent {
    int   x, y;                /* 1-based */
    char  button[12];          /* "left", "middle", "right", "wheel-up", "wheel-down" */
    char  action[10];          /* "press", "release", "move", "drag" */
    bool  ctrl;
    bool  alt;
    bool  shift;
} ConsoleMouseEvent;

typedef struct ConsoleEvent {
    ConsoleEventKind kind;
    union {
        ConsoleKeyEvent   key;
        ConsoleMouseEvent mouse;
    } as;
} ConsoleEvent;

/* =========================================================================
 * Decoder
 *
 * The decoder owns a small byte buffer; the caller feeds bytes one at
 * a time (or in batches via console_input_feed_n), and on each call
 * either DONE (an event was produced) or PENDING (need more bytes)
 * is returned.  When DONE, the caller reads from `*out` and may feed
 * more bytes after.  When PENDING, the caller should call again
 * with more bytes.
 *
 * On a bare ESC byte with no follow-up within ~50ms, the dispatcher
 * calls console_input_flush() to commit the pending ESC as an
 * Escape key event.
 * ======================================================================= */

typedef enum ConsoleInputResult {
    CIR_PENDING = 0,
    CIR_DONE_KEY,
    CIR_DONE_MOUSE,
    CIR_DROP,      /* recognised but dropped (focus, paste markers) */
} ConsoleInputResult;

typedef struct ConsoleInputState {
    uint8_t  buf[32];
    uint8_t  len;
    /* When the parser is mid-escape, the dispatcher records the
     * time of the leading ESC so flush() can commit a bare Escape
     * after a short timeout.  Optional; defaults zero. */
    uint64_t esc_started_us;
} ConsoleInputState;

void console_input_reset(ConsoleInputState *st);

ConsoleInputResult console_input_feed(ConsoleInputState *st,
                                      uint8_t byte,
                                      ConsoleEvent *out);

/* Commit any pending bytes as a best-effort event.  Used when stdin
 * returns 0 (EOF) or the dispatcher times out waiting for more bytes
 * of a partial escape sequence. */
ConsoleInputResult console_input_flush(ConsoleInputState *st,
                                       ConsoleEvent *out);

#endif /* CANDO_LIB_CONSOLE_INPUT_H */
