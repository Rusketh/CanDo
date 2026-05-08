/*
 * src/controls/ctl_tabcontrol.h -- TabControl native methods.
 *
 * Phase 2.2 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §6.3).
 * Backed by Win32's SysTabControl32 (WC_TABCONTROL).  This first cut
 * implements the tab-strip surface (addTab / removeTab / count /
 * selected index); per-tab content panels (TabPage) and the
 * TCN_SELCHANGE event wiring land in a follow-on commit so scripts
 * can compose tab-switching UI.
 *
 * The runtime kind guard inside each native is belt-and-braces:
 * Phase 1.1's per-kind meta tables already gate dispatch.
 */

#ifndef CANDO_FORMS_CONTROLS_TABCONTROL_H
#define CANDO_FORMS_CONTROLS_TABCONTROL_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TabControl.addTab(title) -> index.  Inserts a tab at the end. */
int native_add_tab(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.removeTab(index).  No-op if index out of range. */
int native_remove_tab(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.clearTabs().  Removes every tab. */
int native_clear_tabs(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.getTabCount() -> number. */
int native_get_tab_count(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.getSelectedIndex() -> number.  -1 if no tab selected. */
int native_get_selected_tab(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.setSelectedIndex(i). */
int native_set_selected_tab(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.getTabText(index) -> string.  NULL if index out of range. */
int native_get_tab_text(CandoVM *vm, int argc, CandoValue *args);

/* TabControl.setTabText(index, title). */
int native_set_tab_text(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_TABCONTROL_H */
