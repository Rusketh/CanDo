/*
 * lib/console_input.c -- Key + mouse decoder implementation.
 *
 * Implements a small state machine over the bytes the terminal
 * pushes through stdin (or that console_term.c synthesises from
 * Windows INPUT_RECORDs).  Recognises:
 *
 *   - Plain ASCII / UTF-8 control + visible characters.
 *   - Ctrl-letter (0x01..0x1A => ctrl-A..Z) and Ctrl-special bytes.
 *   - ESC sequences: ESC [ ... ; ESC O P/Q/R/S for F1-F4 ;
 *     ESC <letter> for Alt-letter.
 *   - SGR mouse: ESC [ < b ; x ; y M | m
 *   - Bracketed paste markers (drop silently).
 *   - Focus in/out (drop silently).
 *
 * State is encapsulated in ConsoleInputState; the decoder is
 * re-entrant and free of static state.
 */

#include "console_input.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

static void set_name(ConsoleKeyEvent *k, const char *name)
{
    size_t n = strlen(name);
    if (n >= CONSOLE_KEY_NAME_MAX) n = CONSOLE_KEY_NAME_MAX - 1;
    memcpy(k->name, name, n);
    k->name[n]  = '\0';
    k->name_len = (uint8_t)n;
}

static void set_raw(ConsoleKeyEvent *k, const uint8_t *buf, size_t len)
{
    if (len >= CONSOLE_KEY_RAW_MAX) len = CONSOLE_KEY_RAW_MAX - 1;
    memcpy(k->raw, buf, len);
    k->raw[len] = '\0';
    k->raw_len  = (uint8_t)len;
}

static void clear_key(ConsoleKeyEvent *k)
{
    memset(k, 0, sizeof(*k));
}

static void clear_mouse(ConsoleMouseEvent *m)
{
    memset(m, 0, sizeof(*m));
}

/* -------------------------------------------------------------------------
 * State machine
 * --------------------------------------------------------------------- */

void console_input_reset(ConsoleInputState *st)
{
    memset(st, 0, sizeof(*st));
}

/* Try to recognise a complete escape sequence held in st->buf.
 * Returns the result kind and, on success, fills `out` and advances
 * the buffer.  When PENDING is returned the buffer is left unchanged. */
static ConsoleInputResult try_decode_escape(ConsoleInputState *st,
                                             ConsoleEvent *out)
{
    uint8_t       *b = st->buf;
    size_t         n = st->len;
    ConsoleKeyEvent *k = &out->as.key;

    /* Bare ESC: still need at least one more byte to decide whether
     * it's Escape-alone or a prefix.  Dispatcher's timeout calls
     * flush() to commit it. */
    if (n == 1) return CIR_PENDING;

    /* ESC <letter>  ->  Alt-letter (also catches Alt-digit / Alt-symbol). */
    if (n == 2 && b[1] != '[' && b[1] != 'O') {
        clear_key(k);
        char c = (char)b[1];
        if (c == 0x1b) {
            /* ESC ESC -- treat as a literal Escape press, consume one. */
            set_name(k, "Escape");
            set_raw(k, b, 1);
            /* Shift the remaining ESC down. */
            b[0] = b[1];
            st->len = 1;
            out->kind = CEV_KEY;
            return CIR_DONE_KEY;
        }
        char nm[2] = { c, 0 };
        set_name(k, nm);
        k->alt = true;
        set_raw(k, b, 2);
        st->len = 0;
        out->kind = CEV_KEY;
        return CIR_DONE_KEY;
    }

    /* ESC O P/Q/R/S  ->  F1..F4 */
    if (n == 3 && b[1] == 'O' && b[2] >= 'P' && b[2] <= 'S') {
        clear_key(k);
        static const char *names[] = { "F1", "F2", "F3", "F4" };
        set_name(k, names[b[2] - 'P']);
        set_raw(k, b, 3);
        st->len = 0;
        out->kind = CEV_KEY;
        return CIR_DONE_KEY;
    }

    /* ESC [ ... -- CSI sequence.  We need at least the final byte. */
    if (n >= 2 && b[1] == '[') {
        /* Mouse SGR: ESC [ < b ; x ; y (M|m) */
        if (n >= 4 && b[2] == '<') {
            /* Scan for terminator M or m. */
            size_t end = 3;
            while (end < n && b[end] != 'M' && b[end] != 'm') end++;
            if (end >= n) return CIR_PENDING;

            int bcode = 0, x = 0, y = 0;
            int field = 0;
            int *targets[3] = { &bcode, &x, &y };
            size_t p = 3;
            while (p < end) {
                if (b[p] == ';') { field++; if (field > 2) break; p++; continue; }
                if (b[p] < '0' || b[p] > '9') break;
                *targets[field] = (*targets[field]) * 10 + (b[p] - '0');
                p++;
            }
            bool press = b[end] == 'M';

            ConsoleMouseEvent *m = &out->as.mouse;
            clear_mouse(m);
            m->x = x;
            m->y = y;
            int btn = bcode & 0x03;
            m->shift = (bcode & 0x04) != 0;
            m->alt   = (bcode & 0x08) != 0;
            m->ctrl  = (bcode & 0x10) != 0;
            bool motion = (bcode & 0x20) != 0;
            bool wheel  = (bcode & 0x40) != 0;
            if (wheel) {
                strcpy(m->button, btn == 0 ? "wheel-up" : "wheel-down");
                strcpy(m->action, "press");
            } else {
                const char *bn = "left";
                if (btn == 1) bn = "middle";
                else if (btn == 2) bn = "right";
                else if (btn == 3) bn = "none";
                strcpy(m->button, bn);
                if (motion) {
                    strcpy(m->action, btn == 3 ? "move" : "drag");
                } else {
                    strcpy(m->action, press ? "press" : "release");
                }
            }
            st->len = 0;
            out->kind = CEV_MOUSE;
            return CIR_DONE_MOUSE;
        }

        /* Find the final byte of the CSI sequence.  Per ECMA-48,
         * the final byte is in the range 0x40..0x7E -- that includes
         * uppercase + lowercase letters AND `~` (used for many xterm
         * function-key sequences). */
        size_t fin = 2;
        while (fin < n && !(b[fin] >= 0x40 && b[fin] <= 0x7E)) {
            fin++;
        }
        if (fin >= n) return CIR_PENDING;

        clear_key(k);
        char final = (char)b[fin];

        /* Parse params (semicolon-separated decimal); we only ever
         * need up to two for our recognition. */
        int params[4] = { 0, 0, 0, 0 };
        int nparam = 0;
        bool had_digit = false;
        for (size_t p = 2; p < fin; p++) {
            if (b[p] == ';') {
                nparam = (nparam < 3) ? nparam + 1 : nparam;
                had_digit = false;
                continue;
            }
            if (b[p] >= '0' && b[p] <= '9') {
                if (!had_digit && nparam == 0 && p == 2) nparam = 0; /* first */
                params[nparam < 4 ? nparam : 3] =
                    params[nparam < 4 ? nparam : 3] * 10 + (b[p] - '0');
                had_digit = true;
            }
        }
        if (had_digit) nparam++;

        /* Modifier from the second param (per xterm: code = 1+ctrl(4)+alt(2)+shift(1)). */
        int mod = (nparam >= 2) ? params[1] : 0;
        if (mod > 0) {
            int m = mod - 1;
            k->shift = (m & 1) != 0;
            k->alt   = (m & 2) != 0;
            k->ctrl  = (m & 4) != 0;
        }

        /* Final-byte dispatch. */
        switch (final) {
            case 'A': set_name(k, "ArrowUp");    break;
            case 'B': set_name(k, "ArrowDown");  break;
            case 'C': set_name(k, "ArrowRight"); break;
            case 'D': set_name(k, "ArrowLeft");  break;
            case 'H': set_name(k, "Home");       break;
            case 'F': set_name(k, "End");        break;
            case 'P': set_name(k, "F1");         break;
            case 'Q': set_name(k, "F2");         break;
            case 'R': set_name(k, "F3");         break;
            case 'S': set_name(k, "F4");         break;
            case 'Z': set_name(k, "Tab"); k->shift = true; break;
            case 'I': /* focus in */    st->len = 0; return CIR_DROP;
            case 'O': /* focus out */   st->len = 0; return CIR_DROP;
            case '~': {
                /* ESC [ N ~  with N in {2,3,5,6,15,17,18,19,20,21,23,24}. */
                switch (params[0]) {
                    case 2:  set_name(k, "Insert");   break;
                    case 3:  set_name(k, "Delete");   break;
                    case 5:  set_name(k, "PageUp");   break;
                    case 6:  set_name(k, "PageDown"); break;
                    case 11: set_name(k, "F1");       break;
                    case 12: set_name(k, "F2");       break;
                    case 13: set_name(k, "F3");       break;
                    case 14: set_name(k, "F4");       break;
                    case 15: set_name(k, "F5");       break;
                    case 17: set_name(k, "F6");       break;
                    case 18: set_name(k, "F7");       break;
                    case 19: set_name(k, "F8");       break;
                    case 20: set_name(k, "F9");       break;
                    case 21: set_name(k, "F10");      break;
                    case 23: set_name(k, "F11");      break;
                    case 24: set_name(k, "F12");      break;
                    case 200: /* bracketed paste start */
                    case 201: /* bracketed paste end */
                        st->len = 0; return CIR_DROP;
                    default: set_name(k, "Unknown"); break;
                }
                break;
            }
            default: set_name(k, "Unknown"); break;
        }

        set_raw(k, b, fin + 1);
        st->len = 0;
        out->kind = CEV_KEY;
        return CIR_DONE_KEY;
    }

    return CIR_PENDING;
}

/* Emit a key event for a plain printable / control byte, no
 * escape sequence involved. */
static void emit_plain(ConsoleEvent *out, uint8_t byte)
{
    ConsoleKeyEvent *k = &out->as.key;
    clear_key(k);
    if (byte == '\r' || byte == '\n') {
        set_name(k, "Enter");
    } else if (byte == 0x7f || byte == 0x08) {
        set_name(k, "Backspace");
    } else if (byte == '\t') {
        set_name(k, "Tab");
    } else if (byte == ' ') {
        set_name(k, "Space");
    } else if (byte >= 1 && byte <= 26 && byte != '\r' && byte != '\n' && byte != '\t') {
        /* Ctrl-letter. */
        char nm[2] = { (char)('a' + byte - 1), 0 };
        set_name(k, nm);
        k->ctrl = true;
    } else if (byte >= 32 && byte < 127) {
        char nm[2] = { (char)byte, 0 };
        set_name(k, nm);
    } else {
        /* Non-ASCII / high byte: emit it as a single-byte 'name'.
         * Real UTF-8 multi-byte sequences get joined by the caller
         * if needed; for v1 we surface bytes literally. */
        char nm[2] = { (char)byte, 0 };
        set_name(k, nm);
    }
    uint8_t buf[1] = { byte };
    set_raw(k, buf, 1);
    out->kind = CEV_KEY;
}

ConsoleInputResult console_input_feed(ConsoleInputState *st,
                                      uint8_t byte,
                                      ConsoleEvent *out)
{
    /* Buffer-overflow guard: drop the oldest byte (treat as Escape). */
    if (st->len >= sizeof(st->buf)) {
        st->len = 0;
    }

    /* If we're inside an escape, append + try to decode. */
    if (st->len > 0) {
        st->buf[st->len++] = byte;
        return try_decode_escape(st, out);
    }

    /* Fresh byte. */
    if (byte == 0x1b) {
        st->buf[0] = byte;
        st->len    = 1;
        return CIR_PENDING;
    }

    emit_plain(out, byte);
    return CIR_DONE_KEY;
}

ConsoleInputResult console_input_flush(ConsoleInputState *st,
                                       ConsoleEvent *out)
{
    if (st->len == 0) return CIR_PENDING;
    if (st->buf[0] == 0x1b && st->len == 1) {
        /* Bare ESC -> Escape key. */
        ConsoleKeyEvent *k = &out->as.key;
        clear_key(k);
        set_name(k, "Escape");
        uint8_t esc = 0x1b;
        set_raw(k, &esc, 1);
        st->len = 0;
        out->kind = CEV_KEY;
        return CIR_DONE_KEY;
    }
    /* Partial sequence; emit the leading byte as plain and shift. */
    uint8_t lead = st->buf[0];
    memmove(st->buf, st->buf + 1, st->len - 1);
    st->len -= 1;
    emit_plain(out, lead);
    return CIR_DONE_KEY;
}
