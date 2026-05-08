/*
 * src/controls/ctl_form.h -- Form-only native methods.
 *
 * Phase 1.2f of the rewrite.  Pulls out the top-level-window-only
 * surface: opacity, top-most, centre, min/max-size, icon, flash,
 * window state, resize / box toggles, taskbar visibility, and
 * accept/cancel button.  Phase 1.1 attached these methods only to
 * the forms_form meta table, so the runtime kind == KIND_FORM guards
 * inside each native are now belt-and-braces.
 */

#ifndef CANDO_FORMS_CONTROLS_FORM_H
#define CANDO_FORMS_CONTROLS_FORM_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_set_opacity         (CandoVM *vm, int argc, CandoValue *args);
int native_get_opacity         (CandoVM *vm, int argc, CandoValue *args);
int native_set_topmost         (CandoVM *vm, int argc, CandoValue *args);
int native_center              (CandoVM *vm, int argc, CandoValue *args);
int native_set_min_size        (CandoVM *vm, int argc, CandoValue *args);
int native_set_max_size        (CandoVM *vm, int argc, CandoValue *args);
int native_set_icon            (CandoVM *vm, int argc, CandoValue *args);
int native_flash               (CandoVM *vm, int argc, CandoValue *args);
int native_set_window_state    (CandoVM *vm, int argc, CandoValue *args);
int native_get_window_state    (CandoVM *vm, int argc, CandoValue *args);
int native_maximize            (CandoVM *vm, int argc, CandoValue *args);
int native_minimize            (CandoVM *vm, int argc, CandoValue *args);
int native_restore             (CandoVM *vm, int argc, CandoValue *args);
int native_set_resizable       (CandoVM *vm, int argc, CandoValue *args);
int native_set_minimize_box    (CandoVM *vm, int argc, CandoValue *args);
int native_set_maximize_box    (CandoVM *vm, int argc, CandoValue *args);
int native_set_show_in_taskbar (CandoVM *vm, int argc, CandoValue *args);
int native_set_accept_button   (CandoVM *vm, int argc, CandoValue *args);
int native_set_cancel_button   (CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_FORM_H */
