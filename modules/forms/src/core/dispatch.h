/*
 * src/core/dispatch.h -- event-to-callback name routing for the
 * forms module.
 *
 * The dispatcher thread pops FormsEvent records off the event queue
 * and looks up the matching script-side property name (`onClick`,
 * `onTextChanged`, ...) before invoking the callback.  Only the pure
 * mapping lives here; the libcando-bound dispatch loop (dispatch_one
 * / dispatch_drain) stays in forms_module.c until Phase 1 splits the
 * controls + backend.
 *
 * Stable mapping: scripts and tests expect the names listed here.
 * When a new EventKind is added in events.h, add the corresponding
 * "onX" string in dispatch.c.
 */

#ifndef CANDO_FORMS_CORE_DISPATCH_H
#define CANDO_FORMS_CORE_DISPATCH_H

#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map an EventKind to its script-side property name (e.g.
 * `EV_CLICK -> "onClick"`).  Returns NULL for unknown kinds (never
 * fires from production code, but the dispatcher guards against it
 * in case the queue is corrupted by a stale producer). */
const char *event_callback_name(EventKind k);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_DISPATCH_H */
