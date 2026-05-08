/*
 * src/controls/ctl_checkbox.h -- CheckBox + RadioButton native methods.
 *
 * Phase 1.2b proof-of-concept extraction (REWRITE_PLAN.md): pulls the
 * tiny self-contained `setChecked` / `getChecked` natives out of
 * forms_module.c into their own TU.  Kind-specific dispatch is the
 * domain of the per-kind meta tables (Phase 1.1) so calling these on
 * something that isn't a CheckBox or RadioButton is now a hard VM
 * error rather than a silent no-op.
 *
 * Both natives are libcando-bound; the entire header gates on
 * FORMS_MODULE_TEST_BUILD so the test build never sees them.
 */

#ifndef CANDO_FORMS_CONTROLS_CHECKBOX_H
#define CANDO_FORMS_CONTROLS_CHECKBOX_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CheckBox / RadioButton.setChecked(bool) -- BM_SETCHECK + return self. */
int native_set_checked(CandoVM *vm, int argc, CandoValue *args);

/* CheckBox / RadioButton.getChecked() -> bool. */
int native_get_checked(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_CHECKBOX_H */
