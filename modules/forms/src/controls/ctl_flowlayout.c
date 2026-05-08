/*
 * src/controls/ctl_flowlayout.c -- FlowLayoutPanel arrange + natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_flowlayout.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/slots.h"

#include <string.h>

/* ---------------- arrange algorithm ---------------- */

void flowlayout_arrange(int parent_slot, int client_w, int client_h)
{
    if (parent_slot <= 0 || parent_slot >= FORMS_MAX_SLOTS) return;
    FormsSlot *p = &g_slots[parent_slot];
    if (!p->alive) return;

    int pad_l = p->pad_l, pad_t = p->pad_t;
    int pad_r = p->pad_r, pad_b = p->pad_b;
    int avail_w = client_w - pad_l - pad_r;
    int avail_h = client_h - pad_t - pad_b;
    if (avail_w < 0) avail_w = 0;
    if (avail_h < 0) avail_h = 0;

    int dir       = p->flow_direction;   /* 0 LR, 1 RL, 2 TD, 3 BU */
    int wrap      = p->wrap_contents;
    int horizontal = (dir == 0 || dir == 1);

    /* Pen position (top-left for LR/TD, top-right for RL, bottom-left for BU). */
    int pen_x = (dir == 1) ? pad_l + avail_w : pad_l;
    int pen_y = (dir == 3) ? pad_t + avail_h : pad_t;
    int row_extent = 0;     /* tallest child in current row (horiz)
                             * or widest child in current column (vert) */

    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot) continue;
        if (!c->visible)   continue;
        if (c->dock != FORMS_DOCK_NONE) continue;   /* respect explicit dock */

        int mw = c->w + c->margin_l + c->margin_r;
        int mh = c->h + c->margin_t + c->margin_b;

        if (horizontal) {
            int x_advance = mw;
            int needs_wrap = (dir == 0)
                ? (pen_x + x_advance > pad_l + avail_w)
                : (pen_x - x_advance < pad_l);
            if (wrap && needs_wrap && row_extent > 0) {
                pen_y += row_extent;
                pen_x = (dir == 1) ? pad_l + avail_w : pad_l;
                row_extent = 0;
            }
            int cx = (dir == 0) ? pen_x + c->margin_l
                                : pen_x - mw + c->margin_l;
            int cy = pen_y + c->margin_t;
            c->x = cx;
            c->y = cy;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
            if (c->hwnd) {
                SetWindowPos(c->hwnd, NULL, cx, cy, c->w, c->h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
#endif
            if (mh > row_extent) row_extent = mh;
            pen_x = (dir == 0) ? pen_x + x_advance
                               : pen_x - x_advance;
        } else {
            int y_advance = mh;
            int needs_wrap = (dir == 2)
                ? (pen_y + y_advance > pad_t + avail_h)
                : (pen_y - y_advance < pad_t);
            if (wrap && needs_wrap && row_extent > 0) {
                pen_x += row_extent;
                pen_y = (dir == 3) ? pad_t + avail_h : pad_t;
                row_extent = 0;
            }
            int cx = pen_x + c->margin_l;
            int cy = (dir == 2) ? pen_y + c->margin_t
                                : pen_y - mh + c->margin_t;
            c->x = cx;
            c->y = cy;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
            if (c->hwnd) {
                SetWindowPos(c->hwnd, NULL, cx, cy, c->w, c->h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
#endif
            if (mw > row_extent) row_extent = mw;
            pen_y = (dir == 2) ? pen_y + y_advance
                               : pen_y - y_advance;
        }
    }
}

/* ---------------- natives ---------------- */

static int parse_flow_direction(CandoValue v)
{
    if (v.tag == CDO_NUMBER) {
        int n = (int)v.as.number;
        if (n < 0 || n > 3) return 0;
        return n;
    }
    if (v.tag == CDO_STRING && v.as.string) {
        const char *t = v.as.string->data;
        u32 n = v.as.string->length;
        if (n == 11 && memcmp(t, "leftToRight", 11) == 0) return 0;
        if (n == 11 && memcmp(t, "rightToLeft", 11) == 0) return 1;
        if (n == 7  && memcmp(t, "topDown",      7) == 0) return 2;
        if (n == 8  && memcmp(t, "bottomUp",     8) == 0) return 3;
    }
    return 0;
}

int native_set_flow_direction(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setFlowDirection");
    if (!s) return -1;
    if (argc >= 2) s->flow_direction = parse_flow_direction(args[1]);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FLOWLAYOUT) {
        RECT rc; GetClientRect(s->hwnd, &rc);
        flowlayout_arrange((int)(s - g_slots),
                           rc.right - rc.left, rc.bottom - rc.top);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_flow_direction(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getFlowDirection");
    if (!s) return -1;
    const char *t;
    switch (s->flow_direction) {
    case 1: t = "rightToLeft"; break;
    case 2: t = "topDown";     break;
    case 3: t = "bottomUp";    break;
    default: t = "leftToRight"; break;
    }
    cando_vm_push(vm,
        cando_string_value(cando_string_new(t, (u32)strlen(t))));
    return 1;
}

int native_set_wrap_contents(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setWrapContents");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->wrap_contents = on ? 1 : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FLOWLAYOUT) {
        RECT rc; GetClientRect(s->hwnd, &rc);
        flowlayout_arrange((int)(s - g_slots),
                           rc.right - rc.left, rc.bottom - rc.top);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_wrap_contents(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getWrapContents");
    if (!s) return -1;
    cando_vm_push(vm, cando_bool(s->wrap_contents ? true : false));
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
