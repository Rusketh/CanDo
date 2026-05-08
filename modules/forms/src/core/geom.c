/*
 * src/core/geom.c -- pure geometry helpers.  See geom.h for rationale.
 */

#include "geom.h"

void compute_dock_rect(int dock, int child_w, int child_h,
                       int *left, int *top, int *right, int *bottom,
                       DockRect *out)
{
    int x = *left, y = *top;
    int w = *right - *left, h = *bottom - *top;

    switch (dock) {
    case FORMS_DOCK_TOP:
        h = child_h; *top    += child_h;
        break;
    case FORMS_DOCK_BOTTOM:
        y = *bottom - child_h; h = child_h; *bottom -= child_h;
        break;
    case FORMS_DOCK_LEFT:
        w = child_w; *left   += child_w;
        break;
    case FORMS_DOCK_RIGHT:
        x = *right  - child_w; w = child_w; *right  -= child_w;
        break;
    case FORMS_DOCK_FILL:
    case FORMS_DOCK_NONE:
    default:
        /* Leave remainder untouched.  FILL callers get whatever's
         * left after every other dock has peeled its strip. */
        break;
    }

    out->x = x; out->y = y; out->w = w; out->h = h;
}
