/*
 * lib/console_term.h -- Cross-platform terminal control for the
 * console standard library.
 *
 * POSIX (Linux / macOS): termios for raw mode, ANSI escape sequences
 * for output, TIOCGWINSZ for size, SIGWINCH for resize, /dev/null +
 * setsid pattern for detach.
 *
 * Windows: SetConsoleMode (ENABLE_VIRTUAL_TERMINAL_INPUT /
 * VIRTUAL_TERMINAL_PROCESSING / MOUSE_INPUT), GetConsoleScreenBufferInfo
 * for size, ReadConsoleInput for events, FreeConsole / AllocConsole /
 * ShowWindow for attach / detach / hide.
 *
 * All emission goes through `console_term_write_raw` which respects
 * a global enabled flag so a host that called cando_console_detach()
 * before any VM existed doesn't end up writing to NUL repeatedly.
 *
 * Pure-C, no VM headers.  Higher-level natives in console.c call
 * into here; tests in tests/test_console.c can exercise the helpers
 * directly.
 */

#ifndef CANDO_LIB_CONSOLE_TERM_H
#define CANDO_LIB_CONSOLE_TERM_H

#include "../core/common.h"
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 * Lifecycle
 * ======================================================================= */

/* Initialise process-global terminal state.  Idempotent.  Called
 * implicitly on first console.* call, but embedders may call it
 * earlier (e.g. so SIGWINCH is wired before the VM starts). */
void console_term_init(void);

/* Restore the original terminal state (cooked mode, alternate screen
 * off, mouse off, cursor visible).  Called from an atexit hook so
 * scripts that exit without cleanup don't leave the terminal in raw
 * mode. */
void console_term_shutdown(void);

/* Process-wide detach: closes / reopens stdio onto NUL or /dev/null
 * and -- on Windows -- calls FreeConsole().  Safe to call repeatedly;
 * safe before any VM exists. */
void console_term_detach(void);

/* Process-wide attach: AllocConsole + reopen stdio on Windows; on
 * POSIX, reopen fd 0/1/2 onto /dev/tty if available.  Returns true
 * if a usable console is now attached. */
bool console_term_attach(void);

/* Hide / show the console window (Windows only).  No-op on POSIX --
 * a terminal emulator window is owned by the user's terminal app, not
 * by us. */
void console_term_hide(void);
void console_term_show(void);

/* Returns true if a console is currently attached.  Windows: GetConsoleWindow().
 * POSIX: isatty(STDOUT_FILENO). */
bool console_term_exists(void);

/* Set the window/tab title (xterm OSC 0 on POSIX, SetConsoleTitle on
 * Windows). */
void console_term_set_title(const char *title, size_t len);

/* =========================================================================
 * Output
 * ======================================================================= */

/* Raw byte write to the terminal.  Returns false when the global
 * disabled flag is on (caller decides whether that's an error). */
bool console_term_write(const char *data, size_t len);

/* Flush whatever the underlying stdout buffer is. */
void console_term_flush(void);

/* =========================================================================
 * Mode control
 * ======================================================================= */

/* Switch the terminal in/out of raw mode (POSIX cfmakeraw; Windows
 * ENABLE_VIRTUAL_TERMINAL_INPUT). */
bool console_term_set_raw(bool on);

/* Toggle echoing of typed characters. */
bool console_term_set_echo(bool on);

/* Hide / show the cursor.  Cross-platform via ANSI sequences. */
void console_term_set_cursor_visible(bool on);

/* Switch the alternate screen on/off. */
void console_term_set_alternate_screen(bool on);

/* Enable / disable SGR mouse tracking. */
void console_term_set_mouse(bool on);

/* Returns 1 if stdout is a real terminal. */
bool console_term_is_tty(void);

/* Query the current terminal size; cols/rows are 1-based.  Returns
 * 0/0 on failure. */
void console_term_size(int *cols, int *rows);

/* =========================================================================
 * Resize-event support
 *
 * On POSIX, a SIGWINCH handler sets a sticky `resized` flag that
 * the dispatcher polls.  On Windows, window-buffer-size events from
 * ReadConsoleInput drive the same flag.  Either way, the dispatcher
 * tests + clears the flag with these two calls.
 * ======================================================================= */

bool console_term_resize_pending(void);
void console_term_resize_clear(void);

/* =========================================================================
 * Low-level input file-descriptor / handle access
 *
 * The dispatcher reads bytes from this fd / handle through select /
 * poll / WaitForSingleObject.  Exposed so test code can drive the
 * input layer over a pipe.
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
HANDLE console_term_input_handle(void);
#else
int console_term_input_fd(void);
#endif

/* Read up to `cap` bytes of input with a `timeout_ms` upper bound.
 * Returns the number of bytes read, 0 on timeout, -1 on error.
 * Use timeout_ms = 0 for non-blocking, -1 for "block forever".  */
int console_term_read_input(unsigned char *buf, size_t cap, int timeout_ms);

#endif /* CANDO_LIB_CONSOLE_TERM_H */
