/*
 * src/controls/ctl_listview.h -- ListView native methods.
 *
 * Phase 3.2 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §7.4).
 * Wraps Win32's SysListView32 in report (details) mode by default,
 * with optional switch to list / smallIcon / largeIcon / tile via
 * setView.
 *
 * v1 surface uses (row index, column index) pairs rather than
 * first-class Item handles.  Multi-column rows are supplied as
 * arrays of strings.  Per-row check boxes / icons land in 3.2b.
 */

#ifndef CANDO_FORMS_CONTROLS_LISTVIEW_H
#define CANDO_FORMS_CONTROLS_LISTVIEW_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ListView.addColumn(title, [width]) -> index.  Defaults to width 100. */
int native_lv_add_column(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setColumnWidth(col, width). */
int native_lv_set_column_width(CandoVM *vm, int argc, CandoValue *args);

/* ListView.getColumnCount() -> int. */
int native_lv_get_column_count(CandoVM *vm, int argc, CandoValue *args);

/* ListView.addItem(stringOrArrayOfStrings) -> index.
 *   - string         creates a row with that string in column 0
 *   - array          places each element into the matching column.
 * Missing columns are left blank. */
int native_lv_add_item(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setSubItem(row, col, text). */
int native_lv_set_subitem(CandoVM *vm, int argc, CandoValue *args);

/* ListView.getItemText(row, col) -> string. */
int native_lv_get_item_text(CandoVM *vm, int argc, CandoValue *args);

/* ListView.removeItem(row). */
int native_lv_remove_item(CandoVM *vm, int argc, CandoValue *args);

/* ListView.clearItems(). */
int native_lv_clear_items(CandoVM *vm, int argc, CandoValue *args);

/* ListView.getItemCount() -> int. */
int native_lv_get_item_count(CandoVM *vm, int argc, CandoValue *args);

/* ListView.getSelectedIndex() -> int (-1 if none, the *first* selected
 * item when in multi-select mode -- use getSelectedIndices for the full set). */
int native_lv_get_selected_index(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setSelectedIndex(i). */
int native_lv_set_selected_index(CandoVM *vm, int argc, CandoValue *args);

/* ListView.getSelectedIndices() -> array of int. */
int native_lv_get_selected_indices(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setView("details" | "list" | "smallIcon" | "largeIcon" | "tile"). */
int native_lv_set_view(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setFullRowSelect(bool) — extended LVS_EX_FULLROWSELECT. */
int native_lv_set_full_row_select(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setGridLines(bool) — extended LVS_EX_GRIDLINES. */
int native_lv_set_grid_lines(CandoVM *vm, int argc, CandoValue *args);

/* ListView.setMultiSelect(bool) — toggles LVS_SINGLESEL. */
int native_lv_set_multi_select(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_LISTVIEW_H */
