/*
 * src/core/layout.c -- pure layout helpers + arrange-vtable storage.
 *
 * The parsers and children_bbox are entirely pure (no Win32, no
 * libcando) and run in the test build verbatim.  The arrange vtable
 * is just a small static array indexed by ControlKind -- no dynamic
 * allocation.
 */

#include "layout.h"
#include "geom.h"          /* FORMS_DOCK_* */
#include "slots.h"         /* FORMS_ANCHOR_*, FORMS_MAX_SLOTS, g_slots */

#include <stddef.h>
#include <string.h>

/* ---------------- pure parsers ---------------- */

void parse_quad_args(int argc, CandoValue *args,
                     int *l, int *t, int *r, int *b)
{
    int v0 = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
    if (argc < 3 || args[2].tag != CDO_NUMBER) {
        *l = *t = *r = *b = v0;
        return;
    }
    int v1 = (int)args[2].as.number;
    if (argc < 4 || args[3].tag != CDO_NUMBER) {
        *l = *r = v0; *t = *b = v1;
        return;
    }
    int v2 = (int)args[3].as.number;
    int v3 = (argc >= 5 && args[4].tag == CDO_NUMBER) ? (int)args[4].as.number : v2;
    *l = v0; *t = v1; *r = v2; *b = v3;
}

int parse_dock_arg(CandoValue v)
{
    if (v.tag == CDO_NUMBER) {
        int n = (int)v.as.number;
        if (n < FORMS_DOCK_NONE || n > FORMS_DOCK_FILL) return FORMS_DOCK_NONE;
        return n;
    }
    if (v.tag == CDO_STRING && v.as.string) {
        const char *s = v.as.string->data;
        u32 n = v.as.string->length;
        #define CHECK(name, val) \
            if (n == sizeof(name)-1 && memcmp(s, name, sizeof(name)-1) == 0) return val
        CHECK("none",   FORMS_DOCK_NONE);
        CHECK("top",    FORMS_DOCK_TOP);
        CHECK("bottom", FORMS_DOCK_BOTTOM);
        CHECK("left",   FORMS_DOCK_LEFT);
        CHECK("right",  FORMS_DOCK_RIGHT);
        CHECK("fill",   FORMS_DOCK_FILL);
        #undef CHECK
    }
    return FORMS_DOCK_NONE;
}

int parse_anchor_arg(CandoValue v)
{
    if (v.tag == CDO_NUMBER) return (int)v.as.number;
    if (v.tag != CDO_STRING || !v.as.string) return FORMS_ANCHOR_DEFAULT;
    const char *str = v.as.string->data;
    u32 n = v.as.string->length;
    int mask = 0;
    u32 i = 0;
    while (i < n) {
        while (i < n && (str[i] == ' ' || str[i] == '|' ||
                         str[i] == ',' || str[i] == '+')) i++;
        u32 start = i;
        while (i < n && str[i] != ' ' && str[i] != '|' &&
               str[i] != ',' && str[i] != '+') i++;
        u32 len = i - start;
        const char *t = str + start;
        if      (len == 4 && memcmp(t, "left",   4) == 0) mask |= FORMS_ANCHOR_LEFT;
        else if (len == 3 && memcmp(t, "top",    3) == 0) mask |= FORMS_ANCHOR_TOP;
        else if (len == 5 && memcmp(t, "right",  5) == 0) mask |= FORMS_ANCHOR_RIGHT;
        else if (len == 6 && memcmp(t, "bottom", 6) == 0) mask |= FORMS_ANCHOR_BOTTOM;
        else if (len == 3 && memcmp(t, "all",    3) == 0) mask |= FORMS_ANCHOR_ALL;
        else if (len == 4 && memcmp(t, "fill",   4) == 0) mask |= FORMS_ANCHOR_ALL;
        else if (len == 4 && memcmp(t, "none",   4) == 0) mask  = FORMS_ANCHOR_NONE;
    }
    return mask ? mask : FORMS_ANCHOR_DEFAULT;
}

int parse_border_style(CandoValue v)
{
    if (v.tag == CDO_NUMBER) {
        int n = (int)v.as.number;
        if (n < 1 || n > 3) return 0;
        return n;
    }
    if (v.tag == CDO_STRING && v.as.string) {
        const char *s = v.as.string->data;
        u32 n = v.as.string->length;
        if (n == 4 && memcmp(s, "none",   4) == 0) return 1;
        if (n == 6 && memcmp(s, "single", 6) == 0) return 2;
        if (n == 2 && memcmp(s, "3d",     2) == 0) return 3;
        if (n == 5 && memcmp(s, "fixed3d", 5) == 0) return 3;
    }
    return 0;
}

/* ---------------- children scan ---------------- */

int children_bbox(int parent_slot, int *out_w, int *out_h)
{
    int max_right = 0, max_bottom = 0, any = 0;
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot) continue;
        any = 1;
        int right  = c->x + c->w;
        int bottom = c->y + c->h;
        if (right  > max_right ) max_right  = right;
        if (bottom > max_bottom) max_bottom = bottom;
    }
    if (out_w) *out_w = max_right;
    if (out_h) *out_h = max_bottom;
    return any;
}

/* ---------------- arrange vtable ---------------- */

/* One slot per ControlKind.  KIND_KIND_COUNT is the array bound (it
 * sits one past the last real kind in slots.h). */
static forms_arrange_fn g_arrange[KIND_KIND_COUNT];

void forms_layout_register(int kind, forms_arrange_fn fn)
{
    if (kind < 0 || kind >= (int)KIND_KIND_COUNT) return;
    g_arrange[kind] = fn;
}

forms_arrange_fn forms_layout_for(int kind)
{
    if (kind < 0 || kind >= (int)KIND_KIND_COUNT) return NULL;
    return g_arrange[kind];
}
