/*
 * lib/console_events.c -- ring-buffer implementation.
 */

#include "console_events.h"

#include <string.h>

void console_eventq_init(ConsoleEventQueue *q)
{
    if (q->inited) return;
    memset(q->items, 0, sizeof(q->items));
    q->head = q->tail = q->count = 0;
    cando_os_mutex_init(&q->lock);
    cando_os_cond_init(&q->not_empty);
    q->inited = true;
}

void console_eventq_destroy(ConsoleEventQueue *q)
{
    if (!q->inited) return;
    cando_os_cond_destroy(&q->not_empty);
    cando_os_mutex_destroy(&q->lock);
    q->inited = false;
}

bool console_eventq_push(ConsoleEventQueue *q, const ConsoleEvent *ev)
{
    cando_os_mutex_lock(&q->lock);
    if (q->count >= CONSOLE_EVENT_QUEUE_CAP) {
        /* Drop NEWEST: just don't enqueue.  The oldest entries
         * (likely user keypresses) keep their slots. */
        cando_os_mutex_unlock(&q->lock);
        return false;
    }
    q->items[q->tail] = *ev;
    q->tail = (q->tail + 1) % CONSOLE_EVENT_QUEUE_CAP;
    q->count++;
    cando_os_cond_signal(&q->not_empty);
    cando_os_mutex_unlock(&q->lock);
    return true;
}

bool console_eventq_pop(ConsoleEventQueue *q, ConsoleEvent *out)
{
    cando_os_mutex_lock(&q->lock);
    bool ok = false;
    if (q->count > 0) {
        *out = q->items[q->head];
        q->head = (q->head + 1) % CONSOLE_EVENT_QUEUE_CAP;
        q->count--;
        ok = true;
    }
    cando_os_mutex_unlock(&q->lock);
    return ok;
}

bool console_eventq_pop_wait(ConsoleEventQueue *q, ConsoleEvent *out,
                              int timeout_ms)
{
    (void)timeout_ms;   /* portable wait_for would be nice; for now
                         * we don't expose timeouts to the dispatcher.
                         * Tests cover the non-waiting paths. */
    cando_os_mutex_lock(&q->lock);
    while (q->count == 0) {
        cando_os_cond_wait(&q->not_empty, &q->lock);
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % CONSOLE_EVENT_QUEUE_CAP;
    q->count--;
    cando_os_mutex_unlock(&q->lock);
    return true;
}

int console_eventq_count(const ConsoleEventQueue *q)
{
    return q->count;
}
