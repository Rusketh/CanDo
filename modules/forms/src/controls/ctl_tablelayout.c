/*
 * src/controls/ctl_tablelayout.c -- TableLayoutPanel arrange + natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_tablelayout.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/slots.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- arrange algorithm ---------------- */

#define TBL_MAX_AXIS 64    /* practical cap on cols + rows for v1 */

void tablelayout_arrange(int parent_slot, int client_w, int client_h)
{
    if (parent_slot <= 0 || parent_slot >= FORMS_MAX_SLOTS) return;
    FormsSlot *p = &g_slots[parent_slot];
    if (!p->alive) return;

    int cols = p->table_cols > 0 ? p->table_cols : 1;
    int rows = p->table_rows > 0 ? p->table_rows : 1;
    if (cols > TBL_MAX_AXIS) cols = TBL_MAX_AXIS;
    if (rows > TBL_MAX_AXIS) rows = TBL_MAX_AXIS;

    int gap = p->cell_padding;
    int pad_l = p->pad_l, pad_t = p->pad_t;
    int pad_r = p->pad_r, pad_b = p->pad_b;
    int avail_w = client_w - pad_l - pad_r;
    int avail_h = client_h - pad_t - pad_b;
    if (avail_w < 0) avail_w = 0;
    if (avail_h < 0) avail_h = 0;

    /* Auto-measure column widths and row heights from single-cell
     * children.  Multi-span children are placed but not measured
     * (matches WinForms TableLayoutPanel behaviour). */
    int col_w[TBL_MAX_AXIS] = {0};
    int row_h[TBL_MAX_AXIS] = {0};

    /* First pass: fill in -1 cells row-major + measure. */
    int next_cell = 0;
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot) continue;
        if (!c->visible) continue;

        int cc, cr;
        if (c->cell_col < 0 || c->cell_row < 0) {
            /* Auto-place at the next free cell in row-major order. */
            cc = next_cell % cols;
            cr = next_cell / cols;
            next_cell++;
        } else {
            cc = c->cell_col;
            cr = c->cell_row;
        }
        if (cc >= cols) cc = cols - 1;
        if (cr >= rows) cr = rows - 1;

        int cs = c->cell_col_span > 0 ? c->cell_col_span : 1;
        int rs = c->cell_row_span > 0 ? c->cell_row_span : 1;
        if (cc + cs > cols) cs = cols - cc;
        if (cr + rs > rows) rs = rows - cr;

        int mw = c->w + c->margin_l + c->margin_r;
        int mh = c->h + c->margin_t + c->margin_b;

        if (cs == 1 && mw > col_w[cc]) col_w[cc] = mw;
        if (rs == 1 && mh > row_h[cr]) row_h[cr] = mh;
    }

    /* Distribute leftover space evenly across columns / rows that
     * came back zero-width or zero-height (or all of them, when the
     * grid is empty).  Total auto-allocated width = sum + gaps. */
    int total_auto_w = 0;
    int empty_cols = 0;
    for (int x = 0; x < cols; x++) {
        total_auto_w += col_w[x];
        if (col_w[x] == 0) empty_cols++;
    }
    int gap_w = (cols > 1) ? (cols - 1) * gap : 0;
    int slack_w = avail_w - total_auto_w - gap_w;
    if (slack_w > 0) {
        if (empty_cols > 0) {
            int per = slack_w / empty_cols;
            int rem = slack_w - per * empty_cols;
            for (int x = 0; x < cols; x++) {
                if (col_w[x] == 0) {
                    col_w[x] = per + (rem > 0 ? 1 : 0);
                    if (rem > 0) rem--;
                }
            }
        } else {
            /* Every column already auto-sized: distribute evenly. */
            int per = slack_w / cols;
            int rem = slack_w - per * cols;
            for (int x = 0; x < cols; x++) {
                col_w[x] += per + (rem > 0 ? 1 : 0);
                if (rem > 0) rem--;
            }
        }
    }
    int total_auto_h = 0;
    int empty_rows = 0;
    for (int y = 0; y < rows; y++) {
        total_auto_h += row_h[y];
        if (row_h[y] == 0) empty_rows++;
    }
    int gap_h = (rows > 1) ? (rows - 1) * gap : 0;
    int slack_h = avail_h - total_auto_h - gap_h;
    if (slack_h > 0) {
        if (empty_rows > 0) {
            int per = slack_h / empty_rows;
            int rem = slack_h - per * empty_rows;
            for (int y = 0; y < rows; y++) {
                if (row_h[y] == 0) {
                    row_h[y] = per + (rem > 0 ? 1 : 0);
                    if (rem > 0) rem--;
                }
            }
        } else {
            int per = slack_h / rows;
            int rem = slack_h - per * rows;
            for (int y = 0; y < rows; y++) {
                row_h[y] += per + (rem > 0 ? 1 : 0);
                if (rem > 0) rem--;
            }
        }
    }

    /* Cumulative offsets. */
    int col_x[TBL_MAX_AXIS + 1] = {0};
    int row_y[TBL_MAX_AXIS + 1] = {0};
    col_x[0] = pad_l;
    for (int x = 0; x < cols; x++)
        col_x[x + 1] = col_x[x] + col_w[x] + (x + 1 < cols ? gap : 0);
    row_y[0] = pad_t;
    for (int y = 0; y < rows; y++)
        row_y[y + 1] = row_y[y] + row_h[y] + (y + 1 < rows ? gap : 0);

    /* Second pass: place the children. */
    next_cell = 0;
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot) continue;
        if (!c->visible) continue;

        int cc, cr;
        if (c->cell_col < 0 || c->cell_row < 0) {
            cc = next_cell % cols;
            cr = next_cell / cols;
            next_cell++;
        } else {
            cc = c->cell_col;
            cr = c->cell_row;
        }
        if (cc >= cols) cc = cols - 1;
        if (cr >= rows) cr = rows - 1;

        int cs = c->cell_col_span > 0 ? c->cell_col_span : 1;
        int rs = c->cell_row_span > 0 ? c->cell_row_span : 1;
        if (cc + cs > cols) cs = cols - cc;
        if (cr + rs > rows) rs = rows - cr;

        int cell_x = col_x[cc];
        int cell_y = row_y[cr];
        int cell_w = col_x[cc + cs] - col_x[cc] - (cs > 0 ? gap : 0);
        int cell_h = row_y[cr + rs] - row_y[cr] - (rs > 0 ? gap : 0);
        /* When the span ends at the last column / row, no trailing
         * gap was added so don't subtract it. */
        if (cc + cs == cols) cell_w += gap;
        if (cr + rs == rows) cell_h += gap;

        int x = cell_x + c->margin_l;
        int y = cell_y + c->margin_t;
        int w = cell_w - c->margin_l - c->margin_r;
        int h = cell_h - c->margin_t - c->margin_b;
        if (w < 0) w = 0;
        if (h < 0) h = 0;

        c->x = x; c->y = y; c->w = w; c->h = h;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
        if (c->hwnd) {
            SetWindowPos(c->hwnd, NULL, x, y, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
#endif
    }
}

/* ---------------- natives ---------------- */

static void tablelayout_relayout_now(FormsSlot *s)
{
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABLELAYOUT) {
        RECT rc; GetClientRect(s->hwnd, &rc);
        tablelayout_arrange((int)(s - g_slots),
                            rc.right - rc.left, rc.bottom - rc.top);
    }
#else
    (void)s;
#endif
}

int native_set_columns(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setColumns");
    if (!s) return -1;
    int n = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
            (int)args[1].as.number : 1;
    if (n < 1) n = 1;
    s->table_cols = n;
    tablelayout_relayout_now(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_rows(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setRows");
    if (!s) return -1;
    int n = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
            (int)args[1].as.number : 1;
    if (n < 1) n = 1;
    s->table_rows = n;
    tablelayout_relayout_now(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_cell_padding(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setCellPadding");
    if (!s) return -1;
    int n = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
            (int)args[1].as.number : 0;
    if (n < 0) n = 0;
    s->cell_padding = n;
    tablelayout_relayout_now(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_table_add(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "add");
    if (!s) return -1;
    if (argc < 4) {
        forms_throw(vm, "add: needs (child, col, row[, colSpan, rowSpan])");
        return -1;
    }
    FormsSlot *child = slot_from_inst(vm, args[1]);
    if (!child) {
        forms_throw(vm, "add: invalid child instance");
        return -1;
    }
    int col = (args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 0;
    int row = (args[3].tag == CDO_NUMBER) ? (int)args[3].as.number : 0;
    int cs  = (argc >= 5 && args[4].tag == CDO_NUMBER) ? (int)args[4].as.number : 1;
    int rs  = (argc >= 6 && args[5].tag == CDO_NUMBER) ? (int)args[5].as.number : 1;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (cs  < 1) cs  = 1;
    if (rs  < 1) rs  = 1;
    child->cell_col      = col;
    child->cell_row      = row;
    child->cell_col_span = cs;
    child->cell_row_span = rs;
    tablelayout_relayout_now(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
