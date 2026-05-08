/*
 * src/controls/ctl_listbox.h -- ListBox + ComboBox item natives.
 *
 * Both controls share the same item-management surface (addItem,
 * removeItem, clearItems, getItem, getItems, getItemCount,
 * getSelectedIndex, setSelectedIndex).  The runtime kind switch
 * inside dispatches to LB_* (ListBox) or CB_* (ComboBox) Win32
 * messages.  Phase 1.1 attached these natives only to the
 * forms_listbox / forms_combobox meta tables.
 */

#ifndef CANDO_FORMS_CONTROLS_LISTBOX_H
#define CANDO_FORMS_CONTROLS_LISTBOX_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_add_item            (CandoVM *vm, int argc, CandoValue *args);
int native_clear_items         (CandoVM *vm, int argc, CandoValue *args);
int native_get_item_count      (CandoVM *vm, int argc, CandoValue *args);
int native_get_item            (CandoVM *vm, int argc, CandoValue *args);
int native_get_items           (CandoVM *vm, int argc, CandoValue *args);
int native_remove_item         (CandoVM *vm, int argc, CandoValue *args);
int native_get_selected_index  (CandoVM *vm, int argc, CandoValue *args);
int native_set_selected_index  (CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_LISTBOX_H */
