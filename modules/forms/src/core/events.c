/*
 * src/core/events.c -- event-queue implementation.  See events.h for
 * the contract.  Globals are file-scope so the buffer's storage stays
 * in this TU.
 */

#include "events.h"
#include "sync.h"

static FormsEvent g_ev_queue[FORMS_EV_QUEUE_CAP];
static int        g_ev_head = 0;     /* read index  */
static int        g_ev_tail = 0;     /* write index */
static fm_mutex_t g_ev_mutex;
static fm_cond_t  g_ev_cond;

void event_queue_init(void)
{
    FM_MUTEX_INIT(&g_ev_mutex);
    FM_COND_INIT(&g_ev_cond);
}

void event_queue_reset(void)
{
    g_ev_head = 0;
    g_ev_tail = 0;
}

int event_queue_is_full(void)
{
    int next = (g_ev_tail + 1) % FORMS_EV_QUEUE_CAP;
    return next == g_ev_head;
}

int event_queue_is_empty(void)
{
    return g_ev_head == g_ev_tail;
}

void event_queue_push(FormsEvent ev)
{
    FM_MUTEX_LOCK(&g_ev_mutex);
    if (!event_queue_is_full()) {
        g_ev_queue[g_ev_tail] = ev;
        g_ev_tail = (g_ev_tail + 1) % FORMS_EV_QUEUE_CAP;
        FM_COND_SIGNAL(&g_ev_cond);
    }
    FM_MUTEX_UNLOCK(&g_ev_mutex);
}

int event_queue_try_pop(FormsEvent *out)
{
    int ok = 0;
    FM_MUTEX_LOCK(&g_ev_mutex);
    if (!event_queue_is_empty()) {
        *out = g_ev_queue[g_ev_head];
        g_ev_head = (g_ev_head + 1) % FORMS_EV_QUEUE_CAP;
        ok = 1;
    }
    FM_MUTEX_UNLOCK(&g_ev_mutex);
    return ok;
}
