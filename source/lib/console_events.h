/*
 * lib/console_events.h -- Single-producer/single-consumer ring buffer
 * for console events flowing from the dispatcher worker thread into
 * the VM-side callback drainer.
 *
 * Mirrors the forms module's events.{h,c} contract (drop-newest on
 * overflow) but stores ConsoleEvent records directly so we don't
 * have to dereference back through a control table.
 */

#ifndef CANDO_LIB_CONSOLE_EVENTS_H
#define CANDO_LIB_CONSOLE_EVENTS_H

#include "console_input.h"
#include "../core/thread_platform.h"

#define CONSOLE_EVENT_QUEUE_CAP 256

typedef struct ConsoleEventQueue {
    ConsoleEvent items[CONSOLE_EVENT_QUEUE_CAP];
    int          head;
    int          tail;
    int          count;
    cando_mutex_t lock;
    cando_cond_t  not_empty;
    bool         inited;
} ConsoleEventQueue;

void console_eventq_init(ConsoleEventQueue *q);
void console_eventq_destroy(ConsoleEventQueue *q);

/* Returns true if pushed.  Drop-newest on overflow: the most recent
 * event is discarded, the oldest survives.  Reasoning: a flood of
 * mouse-move events shouldn't starve out a queued keypress. */
bool console_eventq_push(ConsoleEventQueue *q, const ConsoleEvent *ev);

/* Try to pop one event.  Returns true if an event was dequeued. */
bool console_eventq_pop(ConsoleEventQueue *q, ConsoleEvent *out);

/* Block until an event is available (or timeout_ms elapses).  Returns
 * true if an event was popped.  timeout_ms = -1 means "block forever".  */
bool console_eventq_pop_wait(ConsoleEventQueue *q, ConsoleEvent *out,
                              int timeout_ms);

/* Snapshot count.  Approximate (no lock); for diagnostics only. */
int console_eventq_count(const ConsoleEventQueue *q);

#endif /* CANDO_LIB_CONSOLE_EVENTS_H */
