/*
 * src/core/geom.h -- pure geometry helpers for the forms module.
 *
 * Currently exposes the dock-rect peeling math used by the layout
 * pass.  Lives outside the Win32 backend so the C unit tests can
 * exercise it on any host.
 *
 * Future additions: rect/point structs, DPI scaling, padding/margin
 * arithmetic, anchor-edge resolution.  Each is a pure function with no
 * Win32 dependency, so they all live here.
 */

#ifndef CANDO_FORMS_CORE_GEOM_H
#define CANDO_FORMS_CORE_GEOM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Docking constants.  Mirror System.Windows.Forms.DockStyle so script
 * users see the same names. */
#define FORMS_DOCK_NONE   0
#define FORMS_DOCK_TOP    1
#define FORMS_DOCK_BOTTOM 2
#define FORMS_DOCK_LEFT   3
#define FORMS_DOCK_RIGHT  4
#define FORMS_DOCK_FILL   5

typedef struct DockRect { int x, y, w, h; } DockRect;

/* Peel a child's rectangle off the parent's remaining client area
 * according to `dock`.  *left, *top, *right, *bottom are read AND
 * written -- they shrink as each child consumes its strip.  Writes
 * the child's allocated rect to *out.
 *
 * FILL and NONE leave the remainder unchanged; FILL callers consume
 * whatever is left after every other child has docked.
 *
 * Pure: no Win32, no allocations. */
void compute_dock_rect(int dock, int child_w, int child_h,
                       int *left, int *top, int *right, int *bottom,
                       DockRect *out);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_GEOM_H */
