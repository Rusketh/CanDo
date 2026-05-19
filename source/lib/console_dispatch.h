/*
 * lib/console_dispatch.h -- Console event dispatcher thread.
 *
 * Spawns a worker that reads input bytes, decodes them via
 * console_input, optionally pushes them onto the event queue, and
 * drains the queue by invoking the script's onKey / onMouse /
 * onResize handlers on a child VM.
 *
 * Pattern mirrors modules/forms/src/core/dispatch.{h,c}: parent VM
 * is shared by handle / global tables (cando_vm_init_child); child
 * runs callbacks on its own stack so the script's main thread can
 * sit in console.wait() without re-entry races.
 */

#ifndef CANDO_LIB_CONSOLE_DISPATCH_H
#define CANDO_LIB_CONSOLE_DISPATCH_H

#include "../vm/vm.h"
#include "console_events.h"

#include <stdbool.h>

typedef struct ConsoleDispatch ConsoleDispatch;

/* Create a dispatcher bound to the parent VM and the console object
 * handle (used to look up `onKey` / `onMouse` / `onResize` fields).
 * Does not spawn the worker yet -- call console_dispatch_start. */
ConsoleDispatch *console_dispatch_create(CandoVM *parent,
                                          HandleIndex console_handle);

/* Spawn the worker thread.  Idempotent. */
void console_dispatch_start(ConsoleDispatch *d);

/* Signal the worker to stop on its next wake.  Returns immediately. */
void console_dispatch_stop(ConsoleDispatch *d);

/* Block until the worker has actually exited. */
void console_dispatch_wait(ConsoleDispatch *d);

bool console_dispatch_running(const ConsoleDispatch *d);

/* Tear down (joins the worker first if still alive). */
void console_dispatch_destroy(ConsoleDispatch *d);

#endif /* CANDO_LIB_CONSOLE_DISPATCH_H */
