/*
 * src/controls/ctl_form.c -- top-level-window-only natives.
 *
 * The form_toggle_style helper applies WS_* and WS_EX_* style
 * adds/removes with SWP_FRAMECHANGED so the new flags take effect
 * without a manual repaint.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_form.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"

#include <string.h>
#include <stdlib.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
static void form_toggle_style(FormsSlot *s, LONG add, LONG remove,
                              LONG add_ex, LONG remove_ex)
{
    if (!s->hwnd) return;
    LONG st = GetWindowLongW(s->hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(s->hwnd, GWL_EXSTYLE);
    st &= ~remove;    st |= add;
    ex &= ~remove_ex; ex |= add_ex;
    SetWindowLongW(s->hwnd, GWL_STYLE,   st);
    SetWindowLongW(s->hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

int native_set_opacity(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setOpacity");
    if (!s) return -1;
    /* Accept 0..1 (float) or 0..255 (integer).  WinForms uses 0.0-1.0;
     * we honour both for ergonomic reasons.  >=1 is opaque. */
    int alpha = 255;
    if (argc >= 2 && cando_is_number(args[1])) {
        double v = cando_as_number(args[1]);
        if (v <= 1.0) alpha = (int)(v * 255.0 + 0.5);
        else          alpha = (int)v;
        if (alpha < 0)   alpha = 0;
        if (alpha > 255) alpha = 255;
    }
    s->opacity     = alpha;
    s->has_opacity = 1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        LONG ex = GetWindowLongW(s->hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_LAYERED))
            SetWindowLongW(s->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(s->hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_opacity(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getOpacity");
    if (!s) return -1;
    int alpha = s->has_opacity ? s->opacity : 255;
    cando_vm_push(vm, cando_number((f64)alpha / 255.0));
    return 1;
}

int native_set_topmost(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTopMost");
    if (!s) return -1;
    bool top = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
    s->topmost = top ? 1 : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        SetWindowPos(s->hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_center(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "center");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        RECT wr; GetWindowRect(s->hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        HMONITOR mon = MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            int mw = mi.rcWork.right - mi.rcWork.left;
            int mh = mi.rcWork.bottom - mi.rcWork.top;
            int x = mi.rcWork.left + (mw - w) / 2;
            int y = mi.rcWork.top  + (mh - h) / 2;
            SetWindowPos(s->hwnd, NULL, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            s->x = x; s->y = y;
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_min_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMinSize");
    if (!s) return -1;
    int w = (argc >= 2 && cando_is_number(args[1])) ? (int)cando_as_number(args[1]) : 0;
    int h = (argc >= 3 && cando_is_number(args[2])) ? (int)cando_as_number(args[2]) : 0;
    s->min_w = w; s->min_h = h;
    s->has_min_size = (w > 0 || h > 0) ? 1 : 0;
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_max_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMaxSize");
    if (!s) return -1;
    int w = (argc >= 2 && cando_is_number(args[1])) ? (int)cando_as_number(args[1]) : 0;
    int h = (argc >= 3 && cando_is_number(args[2])) ? (int)cando_as_number(args[2]) : 0;
    s->max_w = w; s->max_h = h;
    s->has_max_size = (w > 0 || h > 0) ? 1 : 0;
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_icon(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setIcon");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM && argc >= 2 &&
        cando_is_string(args[1]) && cando_as_string(args[1])) {
        wchar_t *w = utf8_to_wide(cando_as_string(args[1])->data,
                                  (int)cando_as_string(args[1])->length);
        if (w) {
            HICON small = (HICON)LoadImageW(NULL, w, IMAGE_ICON, 16, 16,
                                            LR_LOADFROMFILE | LR_DEFAULTSIZE);
            HICON big   = (HICON)LoadImageW(NULL, w, IMAGE_ICON, 32, 32,
                                            LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (small) {
                if (s->hicon_small) DestroyIcon(s->hicon_small);
                s->hicon_small = small;
                SendMessageW(s->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
            }
            if (big) {
                if (s->hicon_big) DestroyIcon(s->hicon_big);
                s->hicon_big = big;
                SendMessageW(s->hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
            }
            free(w);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_flash(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "flash");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        FLASHWINFO fi; memset(&fi, 0, sizeof(fi));
        fi.cbSize = sizeof(fi);
        fi.hwnd   = s->hwnd;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fi.uCount  = (argc >= 2 && cando_is_number(args[1])) ?
                     (UINT)(int)cando_as_number(args[1]) : 3;
        fi.dwTimeout = 0;
        FlashWindowEx(&fi);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_window_state(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setWindowState");
    if (!s) return -1;
    int cmd = -1;
    if (argc >= 2) {
        if (cando_is_string(args[1]) && cando_as_string(args[1])) {
            const char *t = cando_as_string(args[1])->data;
            u32 n = cando_as_string(args[1])->length;
            if      (n == 6 && memcmp(t, "normal",   6) == 0) cmd = 0;
            else if (n == 8 && memcmp(t, "maximize", 8) == 0) cmd = 1;
            else if (n == 9 && memcmp(t, "maximized",9) == 0) cmd = 1;
            else if (n == 8 && memcmp(t, "minimize", 8) == 0) cmd = 2;
            else if (n == 9 && memcmp(t, "minimized",9) == 0) cmd = 2;
            else if (n == 8 && memcmp(t, "restored", 8) == 0) cmd = 0;
        } else if (cando_is_number(args[1])) {
            cmd = (int)cando_as_number(args[1]);
        }
    }
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        switch (cmd) {
        case 0: ShowWindow(s->hwnd, SW_RESTORE);  break;
        case 1: ShowWindow(s->hwnd, SW_MAXIMIZE); break;
        case 2: ShowWindow(s->hwnd, SW_MINIMIZE); break;
        default: break;
        }
    }
#else
    (void)cmd;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_window_state(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getWindowState");
    if (!s) return -1;
    const char *st = "normal";
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (IsIconic(s->hwnd))      st = "minimized";
        else if (IsZoomed(s->hwnd)) st = "maximized";
    }
#endif
    cando_vm_push(vm, cando_string_value(cando_string_new(st, (u32)strlen(st))));
    return 1;
}

int native_maximize(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "maximize");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) ShowWindow(s->hwnd, SW_MAXIMIZE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_minimize(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "minimize");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) ShowWindow(s->hwnd, SW_MINIMIZE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_restore(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "restore");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) ShowWindow(s->hwnd, SW_RESTORE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_resizable(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setResizable");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        if (on) form_toggle_style(s, WS_THICKFRAME | WS_MAXIMIZEBOX, 0, 0, 0);
        else    form_toggle_style(s, 0, WS_THICKFRAME | WS_MAXIMIZEBOX, 0, 0);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_minimize_box(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMinimizeBox");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        if (on) form_toggle_style(s, WS_MINIMIZEBOX, 0, 0, 0);
        else    form_toggle_style(s, 0, WS_MINIMIZEBOX, 0, 0);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_maximize_box(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMaximizeBox");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        if (on) form_toggle_style(s, WS_MAXIMIZEBOX, 0, 0, 0);
        else    form_toggle_style(s, 0, WS_MAXIMIZEBOX, 0, 0);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_show_in_taskbar(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setShowInTaskbar");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_FORM) {
        if (on) form_toggle_style(s, 0, 0, WS_EX_APPWINDOW, WS_EX_TOOLWINDOW);
        else    form_toggle_style(s, 0, 0, WS_EX_TOOLWINDOW, WS_EX_APPWINDOW);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_accept_button(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setAcceptButton");
    if (!s) return -1;
    int btn_slot = -1;
    if (argc >= 2) {
        FormsSlot *b = slot_from_inst(vm, args[1]);
        if (b) btn_slot = (int)(b - g_slots);
    }
    s->accept_btn_slot = btn_slot;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    /* Mark the button as the default-push button so Win32 paints the
     * extra emphasis ring.  (Triggering Enter as a click requires a
     * dialog-style message loop -- not yet wired up.) */
    if (btn_slot > 0 && g_slots[btn_slot].alive && g_slots[btn_slot].hwnd &&
        g_slots[btn_slot].kind == KIND_BUTTON) {
        LONG st = GetWindowLongW(g_slots[btn_slot].hwnd, GWL_STYLE);
        st = (st & ~BS_TYPEMASK) | BS_DEFPUSHBUTTON;
        SetWindowLongW(g_slots[btn_slot].hwnd, GWL_STYLE, st);
        InvalidateRect(g_slots[btn_slot].hwnd, NULL, TRUE);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_cancel_button(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setCancelButton");
    if (!s) return -1;
    int btn_slot = -1;
    if (argc >= 2) {
        FormsSlot *b = slot_from_inst(vm, args[1]);
        if (b) btn_slot = (int)(b - g_slots);
    }
    s->cancel_btn_slot = btn_slot;
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
