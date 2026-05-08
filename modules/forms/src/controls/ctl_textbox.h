/*
 * src/controls/ctl_textbox.h -- TextBox native methods.
 *
 * Phase 1.2 of the rewrite (REWRITE_PLAN.md).  Pulls the
 * EDIT-control-specific natives out of forms_module.c.  The Phase 1.1
 * meta tables attach these to forms_textbox; setTextAlign and
 * clearText are also reachable from forms_label / forms_linklabel
 * (setTextAlign) and forms_combobox / forms_listbox (the "clear"
 * alias) by registering the same native under a different name.
 *
 * The runtime kind guards inside each native handle the cross-kind
 * cases (e.g. clearText resets ListBox via LB_RESETCONTENT) so each
 * function knows what to do regardless of which meta dispatched it.
 */

#ifndef CANDO_FORMS_CONTROLS_TEXTBOX_H
#define CANDO_FORMS_CONTROLS_TEXTBOX_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_set_multiline      (CandoVM *vm, int argc, CandoValue *args);
int native_set_readonly       (CandoVM *vm, int argc, CandoValue *args);
int native_set_placeholder    (CandoVM *vm, int argc, CandoValue *args);
int native_set_password_char  (CandoVM *vm, int argc, CandoValue *args);
int native_set_max_length     (CandoVM *vm, int argc, CandoValue *args);
int native_set_text_alignment (CandoVM *vm, int argc, CandoValue *args);
int native_select_all         (CandoVM *vm, int argc, CandoValue *args);
int native_append_text        (CandoVM *vm, int argc, CandoValue *args);
int native_clear_text         (CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_TEXTBOX_H */
