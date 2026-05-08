/*
 * src/core/layout.h -- pure layout / parsing helpers + vtable
 * interface for container-kind arrange functions.
 *
 * Phase 1.3 of the rewrite (REWRITE_PLAN.md).  The pure-C parsers
 * (dock / anchor / border-style / quad-arg LTRB) and the children
 * bounding-box scan move out of forms_module.c so future
 * container TUs (Phase 2 -- TabControl, ScrollPanel, SplitContainer,
 * FlowLayoutPanel, TableLayoutPanel) can call them directly.
 *
 * The vtable surface (forms_arrange_fn + register/lookup/arrange) is
 * forward-looking: today every container uses the same dock+anchor
 * pass inlined in forms_module.c; Phase 2 wires that pass behind
 * forms_layout_arrange() and lets each new container kind register
 * its own callback (table-grid, flow-wrap, scroll-viewport, etc.).
 *
 * No Win32 deps; the test build compiles this TU verbatim and runs
 * its parsers in unit tests.
 */

#ifndef CANDO_FORMS_CORE_LAYOUT_H
#define CANDO_FORMS_CORE_LAYOUT_H

#include "cando_compat.h"  /* CandoValue (real or test stub) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- pure parsers ---------- */

/* Parse 1, 2, or 4 numeric args into a left/top/right/bottom quad.
 * 1 value = "all four", 2 = "horizontal, vertical", 4 = explicit LTRB.
 * Non-number args yield 0 for that slot. */
void parse_quad_args(int argc, CandoValue *args,
                     int *l, int *t, int *r, int *b);

/* Parse a dock argument (number or "none"/"top"/"bottom"/"left"/
 * "right"/"fill") into a FORMS_DOCK_* constant.  Returns
 * FORMS_DOCK_NONE on unrecognised input. */
int parse_dock_arg(CandoValue v);

/* Parse an anchor argument: a single string ("left"/"right"/"top"/
 * "bottom"/"all"/"none"/"fill"), a space/pipe/comma/plus-separated
 * list ("left top right"), or a numeric bitmask.  Returns the bitmask
 * value (FORMS_ANCHOR_*).  An empty / unrecognised string yields the
 * default (LEFT|TOP). */
int parse_anchor_arg(CandoValue v);

/* Parse a border-style argument ("none" -> 1, "single" -> 2,
 * "3d"/"fixed3D" -> 3, or numeric 1..3).  Returns 0 for invalid. */
int parse_border_style(CandoValue v);

/* ---------- children scan ---------- */

/* Compute the bounding box of every alive child of `parent_slot` --
 * i.e. max(x+w) and max(y+h) over the children's cached geometry.
 * Writes the result via *out_w / *out_h.  Returns 1 if any child was
 * found, 0 if the parent is empty. */
int children_bbox(int parent_slot, int *out_w, int *out_h);

/* ---------- arrange vtable (future use, Phase 2) ---------- */

/* Container layout callback: arrange the children of `parent_slot`
 * within a `client_w` x `client_h` client area.  The callback walks
 * the slot table for live children and applies whatever positioning
 * its container kind requires.
 *
 * Today every container uses the same dock + anchor pass inlined in
 * forms_module.c; Phase 2 hooks the per-kind variants here. */
typedef void (*forms_arrange_fn)(int parent_slot, int client_w, int client_h);

/* Register an arrange function for a control kind.  Replaces any
 * previous registration.  Idempotent: calling with NULL clears it
 * (the default dock+anchor pass then runs).  No-op if `kind` is out
 * of range. */
void forms_layout_register(int kind, forms_arrange_fn fn);

/* Look up the arrange function for a control kind, or NULL if no
 * specific arrange has been registered (caller should fall back to
 * the default dock+anchor pass). */
forms_arrange_fn forms_layout_for(int kind);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_LAYOUT_H */
