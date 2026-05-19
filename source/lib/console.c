/*
 * lib/console.c -- Console / terminal standard library.
 *
 * Built on top of console_term (terminal control), console_input
 * (key/mouse decoder), console_lineedit (line editor), and
 * console_dispatch (async worker thread).  This file is the script-
 * facing layer: it registers every `console.<fn>` native, marshals
 * arguments, and respects vm->console_enabled.
 *
 * Layout:
 *
 *   - require_enabled / push helpers
 *   - colour / SGR encoding
 *   - output natives
 *   - mode-control natives
 *   - input natives (blocking + non-blocking)
 *   - dispatcher natives (start/stop/wait + handler fields)
 *   - lifecycle natives (enable/disable/exists/attach/detach/hide/show/title)
 *   - cando_lib_console_register
 */

#include "console.h"
#include "console_term.h"
#include "console_input.h"
#include "console_lineedit.h"
#include "console_dispatch.h"
#include "libutil.h"

#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"
#include "../object/value.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

/* =========================================================================
 * Global dispatcher reference
 *
 * Process-wide singleton so multiple scripts inside the same VM can
 * share one dispatcher.  Pointed-at by start(); cleared by stop() +
 * destroy_atexit().
 * ======================================================================= */

static ConsoleDispatch *g_dispatch = NULL;
static HandleIndex      g_console_handle = 0;

/* =========================================================================
 * Enable-check guard
 * ======================================================================= */

#define REQUIRE_ENABLED(vm, fn_name) do {                                  \
    if (!(vm)->console_enabled) {                                          \
        cando_vm_error((vm), "console.%s: console is disabled", fn_name);  \
        return -1;                                                         \
    }                                                                      \
} while (0)

/* =========================================================================
 * Field helpers
 * ======================================================================= */

static void set_str(CdoObject *o, const char *k, const char *v, u32 len)
{
    CdoString *ks = cdo_string_intern(k, (u32)strlen(k));
    CdoString *vs = cdo_string_intern(v, len);
    cdo_object_rawset(o, ks, cdo_string_value(vs), FIELD_NONE);
    cdo_string_release(vs);
    cdo_string_release(ks);
}

static void set_num(CdoObject *o, const char *k, double v)
{
    CdoString *ks = cdo_string_intern(k, (u32)strlen(k));
    cdo_object_rawset(o, ks, cdo_number(v), FIELD_NONE);
    cdo_string_release(ks);
}

static void set_bool_(CdoObject *o, const char *k, bool v)
{
    CdoString *ks = cdo_string_intern(k, (u32)strlen(k));
    cdo_object_rawset(o, ks, cdo_bool(v), FIELD_NONE);
    cdo_string_release(ks);
}

/* =========================================================================
 * Colour parsing  -- "red" / 0..255 index / [r,g,b] truecolor
 *
 * Builds an SGR fragment in `out` (caller-provided buffer >= 32 bytes).
 * Returns the number of bytes written, 0 on parse failure.
 * ======================================================================= */

static int color_named(const char *name, bool fg, char *out, size_t cap)
{
    /* Standard 8-colour + bright table. */
    static const struct { const char *name; int fg; int bg; } table[] = {
        { "black",          30, 40 },
        { "red",            31, 41 },
        { "green",          32, 42 },
        { "yellow",         33, 43 },
        { "blue",           34, 44 },
        { "magenta",        35, 45 },
        { "cyan",           36, 46 },
        { "white",          37, 47 },
        { "default",        39, 49 },
        { "bright-black",   90, 100 },
        { "bright-red",     91, 101 },
        { "bright-green",   92, 102 },
        { "bright-yellow",  93, 103 },
        { "bright-blue",    94, 104 },
        { "bright-magenta", 95, 105 },
        { "bright-cyan",    96, 106 },
        { "bright-white",   97, 107 },
        { NULL, 0, 0 }
    };
    for (int i = 0; table[i].name; i++) {
        if (strcmp(name, table[i].name) == 0) {
            int n = snprintf(out, cap, "%d;", fg ? table[i].fg : table[i].bg);
            return n > 0 ? n : 0;
        }
    }
    return 0;
}

static int color_indexed(int idx, bool fg, char *out, size_t cap)
{
    if (idx < 0 || idx > 255) return 0;
    int n = snprintf(out, cap, "%d;5;%d;", fg ? 38 : 48, idx);
    return n > 0 ? n : 0;
}

static int color_rgb(int r, int g, int b, bool fg, char *out, size_t cap)
{
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return 0;
    int n = snprintf(out, cap, "%d;2;%d;%d;%d;", fg ? 38 : 48, r, g, b);
    return n > 0 ? n : 0;
}

/* Resolve `colour` (string / number / array) into an SGR fragment. */
static int color_to_sgr(CandoVM *vm, CandoValue val, bool fg,
                        char *out, size_t cap)
{
    if (cando_is_string(val)) {
        return color_named(cando_as_string(val)->data, fg, out, cap);
    }
    if (cando_is_number(val)) {
        return color_indexed((int)cando_as_number(val), fg, out, cap);
    }
    if (cando_is_object(val)) {
        CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(val));
        if (o && o->kind == OBJ_ARRAY && cdo_array_len(o) >= 3) {
            CdoValue r, g, b;
            cdo_array_rawget_idx(o, 0, &r);
            cdo_array_rawget_idx(o, 1, &g);
            cdo_array_rawget_idx(o, 2, &b);
            if (r.tag == CDO_NUMBER && g.tag == CDO_NUMBER && b.tag == CDO_NUMBER) {
                return color_rgb((int)r.as.number, (int)g.as.number,
                                  (int)b.as.number, fg, out, cap);
            }
        }
    }
    return 0;
}

/* Build an SGR escape sequence from an options object.  Returns
 * bytes written into `out`; 0 if no styling was requested. */
static int build_sgr(CandoVM *vm, CdoObject *opts, char *out, size_t cap)
{
    if (!opts) return 0;
    size_t off = 0;
    int n;
    char frag[32];

    static const struct { const char *key; int code; } flags[] = {
        { "bold",      1 },
        { "dim",       2 },
        { "italic",    3 },
        { "underline", 4 },
        { "blink",     5 },
        { "inverse",   7 },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        CdoString *k = cdo_string_intern(flags[i].key, (u32)strlen(flags[i].key));
        CdoValue v;
        bool ok = cdo_object_rawget(opts, k, &v);
        cdo_string_release(k);
        if (ok && v.tag == CDO_BOOL && v.as.boolean) {
            n = snprintf(frag, sizeof(frag), "%d;", flags[i].code);
            if (n > 0 && off + (size_t)n < cap) {
                memcpy(out + off, frag, (size_t)n);
                off += (size_t)n;
            }
        }
    }

    CdoString *kfg = cdo_string_intern("fg", 2);
    CdoValue vfg;
    if (cdo_object_rawget(opts, kfg, &vfg)) {
        CandoValue cv = cando_bridge_to_cando(vm, vfg);
        n = color_to_sgr(vm, cv, true, frag, sizeof(frag));
        if (n > 0 && off + (size_t)n < cap) {
            memcpy(out + off, frag, (size_t)n);
            off += (size_t)n;
        }
        cando_value_release(cv);
    }
    cdo_string_release(kfg);

    CdoString *kbg = cdo_string_intern("bg", 2);
    CdoValue vbg;
    if (cdo_object_rawget(opts, kbg, &vbg)) {
        CandoValue cv = cando_bridge_to_cando(vm, vbg);
        n = color_to_sgr(vm, cv, false, frag, sizeof(frag));
        if (n > 0 && off + (size_t)n < cap) {
            memcpy(out + off, frag, (size_t)n);
            off += (size_t)n;
        }
        cando_value_release(cv);
    }
    cdo_string_release(kbg);

    return (int)off;
}

/* =========================================================================
 * Output natives
 * ======================================================================= */

static int n_write(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "write");
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { cando_vm_push(vm, cando_null()); return 1; }

    /* Build the SGR prefix if opts present. */
    CdoObject *opts = NULL;
    if (argc >= 2 && cando_is_object(args[1])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[1]));
    }
    char sgr[128];
    int sgr_n = opts ? build_sgr(vm, opts, sgr, sizeof(sgr) - 1) : 0;

    if (sgr_n > 0) {
        /* "\x1b[" + sgr + "m"  ->  one SGR escape.
         * sgr is "1;31;" -- we replace the trailing ';' with 'm'. */
        sgr[sgr_n - 1] = 'm';
        console_term_write("\x1b[", 2);
        console_term_write(sgr, (size_t)sgr_n);
    }
    console_term_write(s->data, s->length);
    if (sgr_n > 0) {
        console_term_write("\x1b[0m", 4);
    }
    console_term_flush();
    cando_vm_push(vm, cando_null());
    return 1;
}

static int n_print(CandoVM *vm, int argc, CandoValue *args)
{
    int rc = n_write(vm, argc, args);
    if (rc < 0) return rc;
    if (!vm->console_enabled) return -1;  /* shouldn't reach -- write handled it */
    console_term_write("\n", 1);
    console_term_flush();
    return 1;
}

static int n_moveCursor(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "moveCursor");
    int col = (int)libutil_arg_num_at(args, argc, 0, 1);
    int row = (int)libutil_arg_num_at(args, argc, 1, 1);
    if (col < 1) col = 1;
    if (row < 1) row = 1;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    console_term_write(buf, (size_t)n);
    console_term_flush();
    cando_vm_push(vm, cando_null());
    return 1;
}

static int cursor_relative(CandoVM *vm, int argc, CandoValue *args, char direction)
{
    int n = (int)libutil_arg_num_at(args, argc, 0, 1);
    if (n < 1) n = 1;
    char buf[16];
    int nn = snprintf(buf, sizeof(buf), "\x1b[%d%c", n, direction);
    console_term_write(buf, (size_t)nn);
    console_term_flush();
    cando_vm_push(vm, cando_null());
    return 1;
}

static int n_cursorUp(CandoVM *vm, int argc, CandoValue *args)
{ REQUIRE_ENABLED(vm, "cursorUp");    return cursor_relative(vm, argc, args, 'A'); }
static int n_cursorDown(CandoVM *vm, int argc, CandoValue *args)
{ REQUIRE_ENABLED(vm, "cursorDown");  return cursor_relative(vm, argc, args, 'B'); }
static int n_cursorRight(CandoVM *vm, int argc, CandoValue *args)
{ REQUIRE_ENABLED(vm, "cursorRight"); return cursor_relative(vm, argc, args, 'C'); }
static int n_cursorLeft(CandoVM *vm, int argc, CandoValue *args)
{ REQUIRE_ENABLED(vm, "cursorLeft");  return cursor_relative(vm, argc, args, 'D'); }

static int n_saveCursor(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "saveCursor");
  console_term_write("\x1b[s", 3); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }
static int n_restoreCursor(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "restoreCursor");
  console_term_write("\x1b[u", 3); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }

static int n_clear(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "clear");
  console_term_write("\x1b[2J\x1b[H", 7); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }
static int n_clearLine(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "clearLine");
  console_term_write("\x1b[2K", 4); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }
static int n_clearToEnd(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "clearToEnd");
  console_term_write("\x1b[K", 3); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }
static int n_clearToStart(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "clearToStart");
  console_term_write("\x1b[1K", 4); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }

static int n_scrollUp(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "scrollUp");
    int n = (int)libutil_arg_num_at(args, argc, 0, 1);
    if (n < 1) n = 1;
    char buf[16];
    int nn = snprintf(buf, sizeof(buf), "\x1b[%dS", n);
    console_term_write(buf, (size_t)nn);
    console_term_flush();
    cando_vm_push(vm, cando_null()); return 1;
}
static int n_scrollDown(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "scrollDown");
    int n = (int)libutil_arg_num_at(args, argc, 0, 1);
    if (n < 1) n = 1;
    char buf[16];
    int nn = snprintf(buf, sizeof(buf), "\x1b[%dT", n);
    console_term_write(buf, (size_t)nn);
    console_term_flush();
    cando_vm_push(vm, cando_null()); return 1;
}

static int n_setScrollRegion(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "setScrollRegion");
    int top = (int)libutil_arg_num_at(args, argc, 0, 1);
    int bot = (int)libutil_arg_num_at(args, argc, 1, 9999);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dr", top, bot);
    console_term_write(buf, (size_t)n);
    console_term_flush();
    cando_vm_push(vm, cando_null()); return 1;
}

static int n_resetScrollRegion(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; REQUIRE_ENABLED(vm, "resetScrollRegion");
  console_term_write("\x1b[r", 3); console_term_flush();
  cando_vm_push(vm, cando_null()); return 1; }

/* =========================================================================
 * Mode control
 * ======================================================================= */

static int n_rawMode(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "rawMode");
    bool on = argc >= 1 && cando_is_bool(args[0]) ? cando_as_bool(args[0]) : true;
    bool ok = console_term_set_raw(on);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

static int n_echo(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "echo");
    bool on = argc >= 1 && cando_is_bool(args[0]) ? cando_as_bool(args[0]) : true;
    cando_vm_push(vm, cando_bool(console_term_set_echo(on)));
    return 1;
}

static int n_cursorVisible(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "cursorVisible");
    bool on = argc >= 1 && cando_is_bool(args[0]) ? cando_as_bool(args[0]) : true;
    console_term_set_cursor_visible(on);
    cando_vm_push(vm, cando_null()); return 1;
}

static int n_alternateScreen(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "alternateScreen");
    bool on = argc >= 1 && cando_is_bool(args[0]) ? cando_as_bool(args[0]) : true;
    console_term_set_alternate_screen(on);
    cando_vm_push(vm, cando_null()); return 1;
}

static int n_enableMouse(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "enableMouse");
    bool on = argc >= 1 && cando_is_bool(args[0]) ? cando_as_bool(args[0]) : true;
    console_term_set_mouse(on);
    cando_vm_push(vm, cando_null()); return 1;
}

static int n_isatty(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args;
  cando_vm_push(vm, cando_bool(console_term_is_tty()));
  return 1; }

static int n_size(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    int cols = 0, rows = 0;
    console_term_size(&cols, &rows);
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    set_num(o, "cols", cols);
    set_num(o, "rows", rows);
    cando_vm_push(vm, v);
    return 1;
}

static int n_title(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "title");
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (s) console_term_set_title(s->data, s->length);
    cando_vm_push(vm, cando_null()); return 1;
}

/* =========================================================================
 * Input natives
 * ======================================================================= */

/* Drain bytes into the decoder and return the first decoded key
 * event.  timeout_ms = -1 blocks forever; 0 means "non-blocking".
 * Returns 1 if produced a key event, 0 on timeout, -1 on error. */
static int read_one_key(int timeout_ms, ConsoleKeyEvent *out)
{
    ConsoleInputState st;
    console_input_reset(&st);
    unsigned char buf[64];
    int total_waited = 0;
    while (1) {
        int chunk_timeout = timeout_ms < 0 ? -1
                          : (timeout_ms - total_waited < 0 ? 0 : timeout_ms - total_waited);
        int n = console_term_read_input(buf, sizeof(buf), chunk_timeout);
        if (n < 0) return -1;
        if (n == 0) {
            /* Timeout.  If we have pending escape bytes, flush them. */
            if (st.len > 0) {
                ConsoleEvent ev;
                if (console_input_flush(&st, &ev) == CIR_DONE_KEY) {
                    *out = ev.as.key;
                    return 1;
                }
            }
            return 0;
        }
        for (int i = 0; i < n; i++) {
            ConsoleEvent ev;
            ConsoleInputResult cr = console_input_feed(&st, buf[i], &ev);
            if (cr == CIR_DONE_KEY) { *out = ev.as.key; return 1; }
        }
        /* All bytes were part of an incomplete escape; keep reading
         * with a short follow-up timeout (50ms) to disambiguate ESC. */
        if (timeout_ms < 0) continue;
        if (total_waited >= timeout_ms) return 0;
        total_waited += 50;
    }
}

static void push_key_event(CandoVM *vm, const ConsoleKeyEvent *ke)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    set_str(o, "key", ke->name, ke->name_len);
    set_str(o, "raw", ke->raw,  ke->raw_len);
    set_bool_(o, "ctrl",  ke->ctrl);
    set_bool_(o, "alt",   ke->alt);
    set_bool_(o, "shift", ke->shift);
    set_bool_(o, "meta",  ke->meta);
    cando_vm_push(vm, v);
}

static int n_readKey(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    REQUIRE_ENABLED(vm, "readKey");
    ConsoleKeyEvent ke;
    int rc = read_one_key(-1, &ke);
    if (rc <= 0) { cando_vm_push(vm, cando_null()); return 1; }
    push_key_event(vm, &ke);
    return 1;
}

static int n_readKeyTimeout(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "readKeyTimeout");
    int ms = (int)libutil_arg_num_at(args, argc, 0, 0);
    ConsoleKeyEvent ke;
    int rc = read_one_key(ms, &ke);
    if (rc <= 0) { cando_vm_push(vm, cando_null()); return 1; }
    push_key_event(vm, &ke);
    return 1;
}

static int n_pollKey(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    REQUIRE_ENABLED(vm, "pollKey");
    ConsoleKeyEvent ke;
    int rc = read_one_key(0, &ke);
    if (rc <= 0) { cando_vm_push(vm, cando_null()); return 1; }
    push_key_event(vm, &ke);
    return 1;
}

static int n_readLine(CandoVM *vm, int argc, CandoValue *args)
{
    REQUIRE_ENABLED(vm, "readLine");
    const char *prompt = libutil_arg_cstr_at(args, argc, 0);
    char  *line = NULL;
    size_t len  = 0;
    int rc = console_lineedit_read(prompt, /*password=*/false, &line, &len);
    if (rc == 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (rc < 0) {
        cando_vm_error(vm, "console.readLine: interrupted");
        return -1;
    }
    libutil_push_str(vm, line, (u32)len);
    if (line) cando_free(line);
    return 1;
}

static int n_flushInput(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    REQUIRE_ENABLED(vm, "flushInput");
    unsigned char buf[64];
    while (console_term_read_input(buf, sizeof(buf), 0) > 0) { /* drain */ }
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Async dispatcher natives
 * ======================================================================= */

static int n_start(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    REQUIRE_ENABLED(vm, "start");
    if (!g_dispatch) {
        g_dispatch = console_dispatch_create(vm, g_console_handle);
    }
    console_dispatch_start(g_dispatch);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int n_stop(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    if (g_dispatch) console_dispatch_stop(g_dispatch);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int n_wait(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    if (g_dispatch) console_dispatch_wait(g_dispatch);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int n_running(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_bool(g_dispatch && console_dispatch_running(g_dispatch)));
    return 1;
}

/* =========================================================================
 * Lifecycle natives
 * ======================================================================= */

static int n_enable(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; vm->console_enabled = true;
  cando_vm_push(vm, cando_null()); return 1; }
static int n_disable(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; vm->console_enabled = false;
  cando_vm_push(vm, cando_null()); return 1; }
static int n_enabled(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; cando_vm_push(vm, cando_bool(vm->console_enabled)); return 1; }
static int n_exists(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; cando_vm_push(vm, cando_bool(console_term_exists())); return 1; }
static int n_attach(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; cando_vm_push(vm, cando_bool(console_term_attach())); return 1; }
static int n_detach(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; console_term_detach(); vm->console_enabled = false;
  cando_vm_push(vm, cando_null()); return 1; }
static int n_hide(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; console_term_hide();
  cando_vm_push(vm, cando_null()); return 1; }
static int n_show(CandoVM *vm, int argc, CandoValue *args)
{ (void)argc; (void)args; console_term_show();
  cando_vm_push(vm, cando_null()); return 1; }

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry console_methods[] = {
    /* Output */
    { "write",          n_write          },
    { "print",          n_print          },
    { "moveCursor",     n_moveCursor     },
    { "cursorUp",       n_cursorUp       },
    { "cursorDown",     n_cursorDown     },
    { "cursorRight",    n_cursorRight    },
    { "cursorLeft",     n_cursorLeft     },
    { "saveCursor",     n_saveCursor     },
    { "restoreCursor",  n_restoreCursor  },
    { "clear",          n_clear          },
    { "clearLine",      n_clearLine      },
    { "clearToEnd",     n_clearToEnd     },
    { "clearToStart",   n_clearToStart   },
    { "scrollUp",       n_scrollUp       },
    { "scrollDown",     n_scrollDown     },
    { "setScrollRegion",   n_setScrollRegion   },
    { "resetScrollRegion", n_resetScrollRegion },

    /* Mode */
    { "rawMode",        n_rawMode        },
    { "echo",           n_echo           },
    { "cursorVisible",  n_cursorVisible  },
    { "alternateScreen",n_alternateScreen},
    { "enableMouse",    n_enableMouse    },
    { "isatty",         n_isatty         },
    { "size",           n_size           },
    { "title",          n_title          },

    /* Input */
    { "readKey",        n_readKey        },
    { "readKeyTimeout", n_readKeyTimeout },
    { "pollKey",        n_pollKey        },
    { "readLine",       n_readLine       },
    { "flushInput",     n_flushInput     },

    /* Async */
    { "start",          n_start          },
    { "stop",           n_stop           },
    { "wait",           n_wait           },
    { "running",        n_running        },

    /* Lifecycle */
    { "enable",         n_enable         },
    { "disable",        n_disable        },
    { "enabled",        n_enabled        },
    { "exists",         n_exists         },
    { "attach",         n_attach         },
    { "detach",         n_detach         },
    { "hide",           n_hide           },
    { "show",           n_show           },
};

void cando_lib_console_register(CandoVM *vm)
{
    /* Initialise the terminal layer once; idempotent. */
    console_term_init();

    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));

    libutil_register_methods(vm, obj, console_methods,
                             CANDO_ARRAY_LEN(console_methods));

    /* onKey / onMouse / onResize / onError are plain fields users
     * assign to; default null. */
    CdoString *kn;
    static const char *handler_fields[] = {
        "onKey", "onMouse", "onResize", "onError", "onLine", NULL
    };
    for (int i = 0; handler_fields[i]; i++) {
        kn = cdo_string_intern(handler_fields[i],
                               (u32)strlen(handler_fields[i]));
        cdo_object_rawset(obj, kn, cdo_null(), FIELD_NONE);
        cdo_string_release(kn);
    }

    /* Record the console handle so the dispatcher can resolve it
     * from the worker thread. */
    g_console_handle = cando_as_handle(val);

    cando_vm_set_global(vm, "console", val, true);
}
