/*
 * src/controls/ctl_splitter.h -- Splitter native methods.
 *
 * Phase 2.3 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §6.7).
 * Splitter is a thin draggable bar that resizes a target sibling.
 * The drag tracking lives in splitter_wndproc inside forms_module.c
 * (mouse capture + SetWindowPos + parent relayout).  The natives
 * here just configure the slot's orientation and explicit drag
 * target.
 */

#ifndef CANDO_FORMS_CONTROLS_SPLITTER_H
#define CANDO_FORMS_CONTROLS_SPLITTER_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Splitter.setOrientation(string|number).
 *   "vertical"  / 0  — vertical bar, drags horizontally (default).
 *   "horizontal" / 1 — horizontal bar, drags vertically.
 * Adjusts the cached cursor so SETCURSOR shows the right resize arrow. */
int native_set_orientation(CandoVM *vm, int argc, CandoValue *args);

/* Splitter.getOrientation() -> "vertical" | "horizontal". */
int native_get_orientation(CandoVM *vm, int argc, CandoValue *args);

/* Splitter.setTarget(otherControl) — explicit drag target.
 * Without this, the splitter resizes the previous alive sibling
 * (i.e. whichever control was created just before it under the same
 * parent).  Pass NULL / a non-control to clear. */
int native_set_splitter_target(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_SPLITTER_H */
