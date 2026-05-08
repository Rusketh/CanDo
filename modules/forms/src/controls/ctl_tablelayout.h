/*
 * src/controls/ctl_tablelayout.h -- TableLayoutPanel native methods
 * + arrange callback.
 *
 * Phase 2.5 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §6.6).
 * Second consumer of the layout vtable from Phase 1.3.
 *
 * Algorithm (v1):
 *   - Container has a fixed grid (table_cols x table_rows).
 *   - Each child carries its (col, row) coordinates plus optional
 *     col / row span.  Children with cell_col == -1 fall back to
 *     "fill the next free cell in row-major order".
 *   - Column widths are auto-sized: each column = max preferred
 *     width of children that occupy a single column there.
 *   - Row heights similarly.
 *   - Multi-cell-spanning children are placed but don't drive auto
 *     measurement (mirrors the WinForms TableLayoutPanel rule).
 *   - cell_padding is honoured as gap between adjacent cells.
 *   - Each child fills its cell minus (margin + cell_padding).
 *
 * Future: per-axis column / row styles ("auto" / "absolute,N" /
 * "percent,N") land when scripts ask for proportional cells.
 */

#ifndef CANDO_FORMS_CONTROLS_TABLELAYOUT_H
#define CANDO_FORMS_CONTROLS_TABLELAYOUT_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Walk every alive child of `parent_slot` and place it within a
 * client_w x client_h grid according to its cell coordinates.
 * Auto-measures column widths and row heights from children.
 * Win32-only inside; no-op stub on non-Windows so the registration
 * still links. */
void tablelayout_arrange(int parent_slot, int client_w, int client_h);

/* TableLayoutPanel.setColumns(n).  Auto-clamps to >= 1. */
int native_set_columns(CandoVM *vm, int argc, CandoValue *args);

/* TableLayoutPanel.setRows(n).  Auto-clamps to >= 1. */
int native_set_rows(CandoVM *vm, int argc, CandoValue *args);

/* TableLayoutPanel.setCellPadding(p).  Gap between adjacent cells. */
int native_set_cell_padding(CandoVM *vm, int argc, CandoValue *args);

/* TableLayoutPanel.add(child, col, row, [colSpan], [rowSpan]).
 * Stamps the cell coordinates onto the child's slot.  No-op if child
 * isn't actually a descendant of this layout. */
int native_table_add(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_TABLELAYOUT_H */
