/*
 * src/core/events.h -- forms-module event queue.
 *
 * Single global ring buffer drained by the dispatcher thread.  Win32
 * callbacks (WndProc, control notifications) post Event records here;
 * the dispatcher pops them and invokes the user-supplied script
 * callbacks via the child VM.
 *
 * Single-producer (manager thread), single-consumer (dispatcher
 * thread), but lock-protected so the test harness can drive it from
 * any thread.  Push uses drop-newest on overflow.
 *
 * EventKind values are stable: scripts don't see them, but the C
 * tests pin specific numeric values, and forms_test_event_push_full
 * passes them straight through.  Append new kinds at the end.
 */

#ifndef CANDO_FORMS_CORE_EVENTS_H
#define CANDO_FORMS_CORE_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_NONE = 0,
    EV_CLICK,
    EV_CLOSE,
    EV_TEXT_CHANGED,
    EV_VALUE_CHANGED,
    EV_SELECTION_CHANGED,
    EV_KEY_DOWN,
    EV_KEY_UP,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_FOCUS,
    EV_BLUR,
    EV_RESIZE,
    EV_SHOWN,
    /* Phase 2.2b -- TabControl notifications.  i0 carries the new
     * selected tab index (or -1 on TCN_SELCHANGING when canceled). */
    EV_TAB_CHANGED,
    /* Phase 3.1 -- TreeView notifications.  i0 / d0 carry the
     * relevant HTREEITEM cast through uintptr_t -> double. */
    EV_NODE_SELECTED,
    EV_NODE_EXPANDED,
    EV_NODE_COLLAPSED,
    /* Phase 3.2 -- ListView notifications.  i0 carries the row index. */
    EV_ITEM_ACTIVATED,        /* NM_DBLCLK / RETURN -- "open" intent */
    EV_LIST_SELECTION_CHANGED,
    /* Phase 4 -- timer + menu + tray + paint. */
    EV_TICK,                  /* Timer.onTick(self)                       */
    EV_MENU_ITEM_CLICKED,     /* MenuItem.onClick(self) -- i0 = item id   */
    EV_NOTIFY_CLICK,          /* NotifyIcon.onClick(self, button)         */
    EV_PAINT,                 /* PaintSurface.onPaint(self, gfx)          */
} EventKind;

typedef struct FormsEvent {
    EventKind kind;
    int       slot;
    int       generation;
    int       i0, i1, i2;            /* general-purpose ints (button, key) */
    double    d0, d1;                /* general-purpose floats (mouse)     */
} FormsEvent;

#define FORMS_EV_QUEUE_CAP 512

/* Initialise the queue's mutex + condvar.  Called once from
 * sync_init_once() in forms_module.c. */
void event_queue_init(void);

/* Reset head/tail to zero.  Caller ensures single-threaded access.
 * Used by the C unit tests between cases. */
void event_queue_reset(void);

/* Predicates -- not lock-held.  Production callers that race with a
 * producer must take the queue lock themselves; the unit tests are
 * single-threaded and the production push/pop pair takes the lock
 * internally around the full check-then-act sequence. */
int event_queue_is_full(void);
int event_queue_is_empty(void);

/* Producer side -- locks, enqueues if space, drops the new event
 * otherwise (drop-newest policy, matches modules/window). */
void event_queue_push(FormsEvent ev);

/* Consumer side -- locks, pops one event into *out, returns 1 on
 * success or 0 if the queue is empty.  Non-blocking. */
int event_queue_try_pop(FormsEvent *out);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_EVENTS_H */
