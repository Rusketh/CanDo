/*
 * src/controls/ctl_scrollpanel.h -- ScrollPanel native methods.
 *
 * Phase 2.1 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §6.2).
 * ScrollPanel is a Panel variant with WS_VSCROLL | WS_HSCROLL and a
 * virtual scrollable area (set via setScrollSize); the panel_wndproc
 * subclass installed in forms_module.c consumes WM_VSCROLL /
 * WM_HSCROLL locally and calls ScrollWindowEx to slide the children.
 *
 * The natives here update the slot's scroll state and (on Win32)
 * push the change to the HWND via SetScrollInfo.
 */

#ifndef CANDO_FORMS_CONTROLS_SCROLLPANEL_H
#define CANDO_FORMS_CONTROLS_SCROLLPANEL_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ScrollPanel.setAutoScroll(bool) — toggle scroll-bar engagement.
 * When OFF the bars are present (because of WS_VSCROLL | WS_HSCROLL
 * in the create style) but the panel ignores scroll events. */
int native_set_auto_scroll(CandoVM *vm, int argc, CandoValue *args);

/* ScrollPanel.setScrollSize(w, h) — set the virtual viewport size in
 * logical pixels.  Configures the SCROLLINFO ranges based on the
 * delta between virtual size and current client size. */
int native_set_scroll_size(CandoVM *vm, int argc, CandoValue *args);

/* ScrollPanel.scrollTo(x, y) — programmatically scroll to (x, y) in
 * virtual coordinates.  Clamps to the legal range. */
int native_scroll_to(CandoVM *vm, int argc, CandoValue *args);

/* ScrollPanel.getScrollPosition() -> x, y — multi-value return of
 * the current virtual top-left in logical pixels. */
int native_get_scroll_position(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_SCROLLPANEL_H */
