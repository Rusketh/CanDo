/*
 * lib/console_lineedit.h -- Cooked-mode line editor with history,
 * password masking, and basic editing keys.
 *
 * The line editor flips the terminal into raw mode itself (so it can
 * receive arrow / backspace events) and restores the previous mode
 * before returning.  The caller doesn't have to manage raw state.
 */

#ifndef CANDO_LIB_CONSOLE_LINEEDIT_H
#define CANDO_LIB_CONSOLE_LINEEDIT_H

#include "../core/common.h"
#include <stdbool.h>
#include <stddef.h>

/* Read one line of input with editing.
 *
 *   prompt    -- shown before the input cursor; may be NULL.
 *   password  -- when true, typed characters echo as '*'.
 *   out       -- on success, set to a heap-allocated NUL-terminated
 *                buffer holding the entered text (caller frees with
 *                cando_free).  Set to NULL on EOF.
 *   out_len   -- bytes written into *out, excluding the NUL.
 *
 * Returns:
 *   1   on a successfully entered line (Enter pressed).
 *   0   on Ctrl-D / EOF at an empty buffer (returns *out = NULL).
 *  -1   on terminal I/O error.
 */
int console_lineedit_read(const char *prompt, bool password,
                          char **out, size_t *out_len);

#endif /* CANDO_LIB_CONSOLE_LINEEDIT_H */
