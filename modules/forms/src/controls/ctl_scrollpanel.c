/*
 * src/controls/ctl_scrollpanel.c -- ScrollPanel natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_scrollpanel.h"
#include "ctl_common.h"
#include "../core/sync.h"

#include <string.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
/* Push the slot's scroll state to the SCROLLINFO of its HWND.
 * Called whenever auto_scroll, scroll_w/h, or the panel's own size
 * changes -- the SCROLLINFO range depends on (virtual - client). */
static void scrollpanel_apply(FormsSlot *s)
{
    if (!s->hwnd) return;

    RECT rc; GetClientRect(s->hwnd, &rc);
    int client_w = rc.right - rc.left;
    int client_h = rc.bottom - rc.top;

    SCROLLINFO si; memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;

    /* Vertical. */
    si.nMin  = 0;
    si.nMax  = (s->auto_scroll && s->scroll_h > 0) ? s->scroll_h - 1 : 0;
    si.nPage = (UINT)(client_h > 0 ? client_h : 1);
    si.nPos  = s->scroll_y;
    SetScrollInfo(s->hwnd, SB_VERT, &si, TRUE);

    /* Horizontal. */
    si.nMin  = 0;
    si.nMax  = (s->auto_scroll && s->scroll_w > 0) ? s->scroll_w - 1 : 0;
    si.nPage = (UINT)(client_w > 0 ? client_w : 1);
    si.nPos  = s->scroll_x;
    SetScrollInfo(s->hwnd, SB_HORZ, &si, TRUE);
}
#endif

int native_set_auto_scroll(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setAutoScroll");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->auto_scroll = on ? 1 : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->kind == KIND_SCROLLPANEL) {
        if (!on) {
            /* Reset the viewport to (0, 0) when disabling so children
             * don't get stuck off-screen. */
            s->scroll_x = s->scroll_y = 0;
        }
        scrollpanel_apply(s);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_scroll_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setScrollSize");
    if (!s) return -1;
    int w = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
    int h = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    s->scroll_w = w;
    s->scroll_h = h;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->kind == KIND_SCROLLPANEL) scrollpanel_apply(s);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_scroll_to(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "scrollTo");
    if (!s) return -1;
    int x = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : s->scroll_x;
    int y = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : s->scroll_y;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_SCROLLPANEL) {
        RECT rc; GetClientRect(s->hwnd, &rc);
        int client_w = rc.right - rc.left;
        int client_h = rc.bottom - rc.top;
        int max_x = s->scroll_w - client_w;
        if (max_x < 0) max_x = 0;
        int max_y = s->scroll_h - client_h;
        if (max_y < 0) max_y = 0;
        if (x < 0)     x = 0;
        if (x > max_x) x = max_x;
        if (y < 0)     y = 0;
        if (y > max_y) y = max_y;
        int dx = s->scroll_x - x;
        int dy = s->scroll_y - y;
        s->scroll_x = x;
        s->scroll_y = y;
        SCROLLINFO si; memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = x;
        SetScrollInfo(s->hwnd, SB_HORZ, &si, TRUE);
        si.nPos   = y;
        SetScrollInfo(s->hwnd, SB_VERT, &si, TRUE);
        if (dx || dy) {
            ScrollWindowEx(s->hwnd, dx, dy, NULL, NULL, NULL, NULL,
                           SW_SCROLLCHILDREN | SW_INVALIDATE);
        }
    } else
#endif
    {
        s->scroll_x = x;
        s->scroll_y = y;
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_scroll_position(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getScrollPosition");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->scroll_x));
    cando_vm_push(vm, cando_number((f64)s->scroll_y));
    return 2;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
