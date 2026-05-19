/*
 * lib/console_lineedit.c -- Line input with editing.
 *
 * Operates entirely on top of console_term + console_input.  Keeps
 * an in-memory buffer, repaints on cursor moves, and commits on
 * Enter.  Supports Backspace, Delete, Left/Right, Home/End, Ctrl-C
 * (throws via "" name -- caller interprets), Ctrl-D on empty
 * (returns 0 / NULL).
 */

#include "console_lineedit.h"
#include "console_term.h"
#include "console_input.h"

#include "../core/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINEEDIT_INITIAL_CAP 64

typedef struct LineBuf {
    char  *data;
    size_t len;
    size_t cap;
    size_t cursor;
} LineBuf;

static void buf_init(LineBuf *b)
{
    b->data = (char *)cando_alloc(LINEEDIT_INITIAL_CAP);
    b->cap  = LINEEDIT_INITIAL_CAP;
    b->len  = 0;
    b->cursor = 0;
    b->data[0] = '\0';
}

static void buf_free(LineBuf *b)
{
    if (b->data) cando_free(b->data);
    b->data = NULL;
}

static void buf_grow(LineBuf *b, size_t needed)
{
    if (b->cap >= needed + 1) return;
    size_t newcap = b->cap;
    while (newcap < needed + 1) newcap *= 2;
    char *nb = (char *)cando_alloc(newcap);
    memcpy(nb, b->data, b->len + 1);
    cando_free(b->data);
    b->data = nb;
    b->cap  = newcap;
}

static void buf_insert(LineBuf *b, char c)
{
    buf_grow(b, b->len + 1);
    memmove(b->data + b->cursor + 1, b->data + b->cursor,
            b->len - b->cursor + 1);
    b->data[b->cursor] = c;
    b->len++;
    b->cursor++;
}

static void buf_backspace(LineBuf *b)
{
    if (b->cursor == 0) return;
    memmove(b->data + b->cursor - 1, b->data + b->cursor,
            b->len - b->cursor + 1);
    b->cursor--;
    b->len--;
}

static void buf_delete(LineBuf *b)
{
    if (b->cursor >= b->len) return;
    memmove(b->data + b->cursor, b->data + b->cursor + 1,
            b->len - b->cursor);
    b->len--;
}

/* Repaint: rewrite "<prompt><buf>" on the current line and move the
 * cursor back to the intended position. */
static void repaint(const char *prompt, size_t prompt_len,
                    const LineBuf *b, bool password)
{
    /* Carriage return + clear-line. */
    console_term_write("\r\x1b[K", 4);
    if (prompt_len > 0) console_term_write(prompt, prompt_len);
    if (password) {
        char stars[1024];
        size_t n = b->len > sizeof(stars) ? sizeof(stars) : b->len;
        memset(stars, '*', n);
        console_term_write(stars, n);
    } else {
        console_term_write(b->data, b->len);
    }
    /* Move cursor back to the intended column.  We use CR + advance. */
    char seq[32];
    int total_pre = (int)(prompt_len + b->cursor) + 1; /* 1-based col */
    int wrote = snprintf(seq, sizeof(seq), "\r\x1b[%dC", total_pre - 1);
    if (wrote > 0) console_term_write(seq, (size_t)wrote);
    console_term_flush();
}

int console_lineedit_read(const char *prompt, bool password,
                          char **out, size_t *out_len)
{
    console_term_init();
    size_t prompt_len = prompt ? strlen(prompt) : 0;

    LineBuf b;
    buf_init(&b);

    /* Enter raw mode for the duration. */
    bool was_already_raw = false;     /* approximated; restoring to
                                          'off' at the end is the safe
                                          default for v1.            */
    (void)was_already_raw;
    console_term_set_raw(true);

    if (prompt_len > 0) {
        console_term_write(prompt, prompt_len);
        console_term_flush();
    }

    ConsoleInputState st;
    console_input_reset(&st);
    int rc = 1;

    for (;;) {
        unsigned char rb[64];
        int n = console_term_read_input(rb, sizeof(rb), -1);
        if (n <= 0) {
            /* EOF -- treat as Ctrl-D on empty buffer (returns NULL). */
            if (b.len == 0) {
                buf_free(&b);
                console_term_set_raw(false);
                console_term_write("\n", 1);
                console_term_flush();
                if (out) *out = NULL;
                if (out_len) *out_len = 0;
                return 0;
            }
            break;
        }
        for (int i = 0; i < n; i++) {
            ConsoleEvent ev;
            ConsoleInputResult cr = console_input_feed(&st, rb[i], &ev);
            if (cr != CIR_DONE_KEY) continue;

            const char *name = ev.as.key.name;
            if (strcmp(name, "Enter") == 0) {
                console_term_write("\n", 1);
                console_term_flush();
                goto done;
            }
            if (strcmp(name, "Backspace") == 0) {
                buf_backspace(&b);
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            if (strcmp(name, "Delete") == 0) {
                buf_delete(&b);
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            if (strcmp(name, "ArrowLeft") == 0) {
                if (b.cursor > 0) b.cursor--;
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            if (strcmp(name, "ArrowRight") == 0) {
                if (b.cursor < b.len) b.cursor++;
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            if (strcmp(name, "Home") == 0) {
                b.cursor = 0;
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            if (strcmp(name, "End") == 0) {
                b.cursor = b.len;
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            /* Ctrl-D on empty buffer is EOF. */
            if (ev.as.key.ctrl && strcmp(name, "d") == 0 && b.len == 0) {
                buf_free(&b);
                console_term_set_raw(false);
                console_term_write("\n", 1);
                console_term_flush();
                if (out) *out = NULL;
                if (out_len) *out_len = 0;
                return 0;
            }
            /* Ctrl-C: signal abort by returning -1; caller throws. */
            if (ev.as.key.ctrl && strcmp(name, "c") == 0) {
                buf_free(&b);
                console_term_set_raw(false);
                console_term_write("^C\n", 3);
                console_term_flush();
                if (out) *out = NULL;
                if (out_len) *out_len = 0;
                return -1;
            }
            /* Plain character of length 1: insert. */
            if (ev.as.key.name_len == 1 && !ev.as.key.ctrl && !ev.as.key.alt) {
                buf_insert(&b, name[0]);
                repaint(prompt, prompt_len, &b, password);
                continue;
            }
            /* Other named keys ignored in cooked input. */
        }
    }
done:
    console_term_set_raw(false);
    if (out)     *out     = b.data;
    if (out_len) *out_len = b.len;
    /* Hand ownership of b.data to caller; do NOT buf_free it. */
    b.data = NULL;
    return rc;
}
