/*
 * src/controls/ctl_flowlayout.h -- FlowLayoutPanel native methods +
 * arrange callback registration.
 *
 * Phase 2.4 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §6.5).
 * FlowLayoutPanel is the first real consumer of the layout vtable
 * defined in src/core/layout.h: it registers a per-kind arrange
 * function so panel_wndproc's WM_SIZE handler can dispatch to the
 * flow algorithm instead of the default dock+anchor pass.
 */

#ifndef CANDO_FORMS_CONTROLS_FLOWLAYOUT_H
#define CANDO_FORMS_CONTROLS_FLOWLAYOUT_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arrange every alive child of `parent_slot` into a flow within a
 * client_w x client_h rectangle, honouring the slot's
 * flow_direction (0 = LR, 1 = RL, 2 = TD, 3 = BU) and
 * wrap_contents flag.  Each child consumes its stored geometry plus
 * margin; padding on the parent is honoured as a top-left inset.
 *
 * Win32-only at the body level; on non-Windows builds the function
 * compiles to a no-op so registration still links. */
void flowlayout_arrange(int parent_slot, int client_w, int client_h);

/* FlowLayoutPanel.setFlowDirection(string|number).  String form:
 *   "leftToRight" (0), "rightToLeft" (1), "topDown" (2), "bottomUp" (3). */
int native_set_flow_direction(CandoVM *vm, int argc, CandoValue *args);

/* FlowLayoutPanel.getFlowDirection() -> string. */
int native_get_flow_direction(CandoVM *vm, int argc, CandoValue *args);

/* FlowLayoutPanel.setWrapContents(bool). */
int native_set_wrap_contents(CandoVM *vm, int argc, CandoValue *args);

/* FlowLayoutPanel.getWrapContents() -> bool. */
int native_get_wrap_contents(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_FLOWLAYOUT_H */
