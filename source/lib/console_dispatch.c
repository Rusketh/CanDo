/*
 * lib/console_dispatch.c -- Console dispatcher worker thread.
 *
 * Mirrors the forms-module dispatch pattern:
 *
 *   1. console.start() allocates a ConsoleDispatch holding (a) a
 *      child VM cloned from the parent and (b) a thread handle.
 *   2. The worker loop reads bytes (with a short timeout so we can
 *      poll the stop flag), decodes them, and -- for each event --
 *      looks up the matching `onKey` / `onMouse` / `onResize`
 *      handler on the console object and invokes it via
 *      cando_vm_call_value on the child VM.
 *   3. console.stop() flips an atomic flag; the worker exits on the
 *      next wake.  console.wait() joins.
 *
 * The child VM shares globals + handles with the parent (so script
 * code can mutate state from inside a callback and the main thread
 * sees the change) but has its own value stack, so callback runs
 * don't interfere with whatever code the main thread is doing
 * (typically blocking in console.wait()).
 */

#include "console_dispatch.h"
#include "console_term.h"
#include "console_input.h"
#include "console_events.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"
#include "../object/array.h"
#include "../core/common.h"
#include "../core/thread_platform.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

struct ConsoleDispatch {
    CandoVM      *parent;
    HandleIndex   console_handle;

    cando_thread_t thread;
    bool           thread_alive;

    /* Worker-owned child VM.  Allocated when start() runs; freed in
     * destroy(). */
    CandoVM      *child;

    atomic_bool   running;
    atomic_bool   stop;
};

/* --------------------------------------------------------------- */

/* Look up a field on the console CdoObject and return its CandoValue
 * representation; returns cando_null() if the field is missing. */
static CandoValue lookup_handler(CandoVM *child, HandleIndex h, const char *name)
{
    CdoObject *o = cando_bridge_resolve(child, h);
    if (!o) return cando_null();
    CdoString *k = cdo_string_intern(name, (u32)strlen(name));
    CdoValue v;
    bool ok = cdo_object_rawget(o, k, &v);
    cdo_string_release(k);
    if (!ok) return cando_null();
    return cando_bridge_to_cando(child, v);
}

static void set_string_field(CdoObject *obj, const char *key,
                              const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static void set_bool_field(CdoObject *obj, const char *key, bool v)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(v), FIELD_NONE);
    cdo_string_release(k);
}

static void set_num_field(CdoObject *obj, const char *key, double v)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(v), FIELD_NONE);
    cdo_string_release(k);
}

/* Construct a CandoValue for a ConsoleKeyEvent.  Allocates on the
 * given (child) VM. */
static CandoValue make_key_value(CandoVM *vm, const ConsoleKeyEvent *ke)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    set_string_field(o, "key", ke->name, ke->name_len);
    set_string_field(o, "raw", ke->raw,  ke->raw_len);
    set_bool_field(o, "ctrl",  ke->ctrl);
    set_bool_field(o, "alt",   ke->alt);
    set_bool_field(o, "shift", ke->shift);
    set_bool_field(o, "meta",  ke->meta);
    return v;
}

static CandoValue make_mouse_value(CandoVM *vm, const ConsoleMouseEvent *me)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    set_num_field(o, "x", me->x);
    set_num_field(o, "y", me->y);
    set_string_field(o, "button", me->button, (u32)strlen(me->button));
    set_string_field(o, "action", me->action, (u32)strlen(me->action));
    set_bool_field(o, "ctrl",  me->ctrl);
    set_bool_field(o, "alt",   me->alt);
    set_bool_field(o, "shift", me->shift);
    return v;
}

static CandoValue make_size_value(CandoVM *vm, int cols, int rows)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    set_num_field(o, "cols", cols);
    set_num_field(o, "rows", rows);
    return v;
}

/* Fire one event on the child VM.  Looks up the appropriate handler
 * field on the console object and calls it. */
static void dispatch_event(ConsoleDispatch *d, const ConsoleEvent *ev)
{
    CandoVM *child = d->child;
    const char *field = NULL;
    CandoValue arg = cando_null();
    if (ev->kind == CEV_KEY) {
        field = "onKey";
        arg = make_key_value(child, &ev->as.key);
    } else if (ev->kind == CEV_MOUSE) {
        field = "onMouse";
        arg = make_mouse_value(child, &ev->as.mouse);
    } else {
        return;
    }
    CandoValue handler = lookup_handler(child, d->console_handle, field);
    if (cando_is_null(handler)) {
        cando_value_release(arg);
        return;
    }
    cando_vm_call_value(child, handler, &arg, 1);
    if (child->has_error) {
        /* Try onError; otherwise log + clear.  The error message
         * itself isn't passed to onError -- v1 just signals that
         * something threw inside the previous callback.  Scripts
         * can use TRY/CATCH inside their own onKey/onMouse for
         * full error access. */
        CandoValue on_err = lookup_handler(child, d->console_handle, "onError");
        if (!cando_is_null(on_err)) {
            child->has_error = false;
            CandoValue err_arg = cando_null();
            cando_vm_call_value(child, on_err, &err_arg, 1);
            if (child->has_error) {
                cando_vm_log_uncaught(child, "console.onError");
                child->has_error = false;
            }
        } else {
            cando_vm_log_uncaught(child, "console callback");
            child->has_error = false;
        }
        cando_value_release(handler);
        cando_value_release(arg);
        return;
    }
    /* Discard any return value the callback pushed. */
    /* Drop any return values left on the child's value stack. */
    while (child->stack_top > child->stack) {
        cando_value_release(*--child->stack_top);
    }
    cando_value_release(handler);
    cando_value_release(arg);
}

static void fire_resize(ConsoleDispatch *d)
{
    CandoVM *child = d->child;
    int cols = 0, rows = 0;
    console_term_size(&cols, &rows);
    CandoValue arg = make_size_value(child, cols, rows);
    CandoValue h   = lookup_handler(child, d->console_handle, "onResize");
    if (!cando_is_null(h)) {
        cando_vm_call_value(child, h, &arg, 1);
        if (child->has_error) {
            cando_vm_log_uncaught(child, "console.onResize");
            child->has_error = false;
        }
        /* Drop any return values left on the child's value stack. */
    while (child->stack_top > child->stack) {
        cando_value_release(*--child->stack_top);
    }
    }
    cando_value_release(h);
    cando_value_release(arg);
}

/* --------------------------------------------------------------- */

static CANDO_THREAD_RETURN worker_main(void *ud)
{
    ConsoleDispatch *d = (ConsoleDispatch *)ud;
    ConsoleInputState st;
    console_input_reset(&st);

    while (!atomic_load(&d->stop)) {
        /* Resize check (sticky flag set by SIGWINCH / WINDOW_BUFFER_SIZE_EVENT). */
        if (console_term_resize_pending()) {
            console_term_resize_clear();
            fire_resize(d);
        }

        unsigned char buf[64];
        int n = console_term_read_input(buf, sizeof(buf), 50 /* ms */);
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) {
            ConsoleEvent ev;
            ConsoleInputResult cr = console_input_feed(&st, buf[i], &ev);
            if (cr == CIR_DONE_KEY || cr == CIR_DONE_MOUSE) {
                dispatch_event(d, &ev);
            }
        }
    }
    atomic_store(&d->running, false);
    return CANDO_THREAD_RETURN_VAL;
}

/* --------------------------------------------------------------- */

ConsoleDispatch *console_dispatch_create(CandoVM *parent,
                                          HandleIndex console_handle)
{
    ConsoleDispatch *d = (ConsoleDispatch *)cando_alloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->parent         = parent;
    d->console_handle = console_handle;
    atomic_init(&d->running, false);
    atomic_init(&d->stop,    false);
    return d;
}

void console_dispatch_start(ConsoleDispatch *d)
{
    if (atomic_load(&d->running)) return;
    /* Lazy-allocate the child VM the first time the worker starts. */
    if (!d->child) {
        d->child = (CandoVM *)cando_alloc(sizeof(CandoVM));
        cando_vm_init_child(d->child, d->parent);
    }
    atomic_store(&d->stop, false);
    atomic_store(&d->running, true);
    if (cando_os_thread_create(&d->thread, worker_main, d)) {
        d->thread_alive = true;
    } else {
        atomic_store(&d->running, false);
    }
}

void console_dispatch_stop(ConsoleDispatch *d)
{
    atomic_store(&d->stop, true);
}

void console_dispatch_wait(ConsoleDispatch *d)
{
    if (!d->thread_alive) return;
    cando_os_thread_join(d->thread);
    d->thread_alive = false;
    atomic_store(&d->running, false);
}

bool console_dispatch_running(const ConsoleDispatch *d)
{
    return atomic_load((atomic_bool *)&d->running);
}

void console_dispatch_destroy(ConsoleDispatch *d)
{
    if (!d) return;
    if (d->thread_alive) {
        atomic_store(&d->stop, true);
        cando_os_thread_join(d->thread);
        d->thread_alive = false;
    }
    /* Child VM lifetime: cando_vm_init_child doesn't have a paired
     * destroy in the public API; the runtime cleans it up when the
     * shared global state refcount drops.  Just free the slab. */
    if (d->child) {
        cando_free(d->child);
        d->child = NULL;
    }
    cando_free(d);
}
