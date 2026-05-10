/*
 * src/controls/ctl_textbox.c -- EDIT-class natives for TextBox.
 *
 * Some natives (setTextAlign, clearText) are reachable from non-
 * TextBox meta tables too (Label/LinkLabel for alignment, ComboBox/
 * ListBox for clear), so the kind switch inside dispatches each call
 * to the right Win32 message.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_textbox.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"

#include <string.h>
#include <stdlib.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
/* Apply add/remove style flags to a control's GWL_STYLE and force a
 * frame-changed reflow so the new flags take effect immediately. */
static void edit_toggle_style(FormsSlot *s, LONG add, LONG remove)
{
    if (!s->hwnd) return;
    LONG st = GetWindowLongW(s->hwnd, GWL_STYLE);
    st &= ~remove;
    st |=  add;
    SetWindowLongW(s->hwnd, GWL_STYLE, st);
    SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

int native_set_multiline(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMultiline");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        if (on) edit_toggle_style(s,
                    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0);
        else    edit_toggle_style(s, 0,
                    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_readonly(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setReadOnly");
    if (!s) return -1;
    bool on = !(argc >= 2 && cando_is_bool(args[1]) && !cando_as_bool(args[1]));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        SendMessageW(s->hwnd, EM_SETREADONLY, (WPARAM)(on ? TRUE : FALSE), 0);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_placeholder(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setPlaceholder");
    if (!s) return -1;
    char buf[512] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            /* EM_SETCUEBANNER -- 0x1501 -- 2nd arg TRUE keeps the cue
             * visible while the edit has focus. */
            SendMessageW(s->hwnd, 0x1501, (WPARAM)TRUE, (LPARAM)w);
            free(w);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_password_char(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setPasswordChar");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    wchar_t pc = L'\0';
    if (argc >= 2) {
        if (cando_is_number(args[1])) pc = (wchar_t)(int)cando_as_number(args[1]);
        else if (cando_is_string(args[1]) && cando_as_string(args[1]) &&
                 cando_as_string(args[1])->length > 0) {
            wchar_t *w = utf8_to_wide(cando_as_string(args[1])->data,
                                      (int)cando_as_string(args[1])->length);
            if (w) { pc = w[0]; free(w); }
        }
        else if (cando_is_bool(args[1]) && cando_as_bool(args[1])) pc = L'*';
    }
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        SendMessageW(s->hwnd, EM_SETPASSWORDCHAR, (WPARAM)pc, 0);
        InvalidateRect(s->hwnd, NULL, TRUE);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_max_length(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMaxLength");
    if (!s) return -1;
    int n = (argc >= 2 && cando_is_number(args[1])) ? (int)cando_as_number(args[1]) : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        SendMessageW(s->hwnd, EM_SETLIMITTEXT, (WPARAM)(n > 0 ? n : 0), 0);
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_text_alignment(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTextAlign");
    if (!s) return -1;
    int align = 0;          /* 0 = left, 1 = center, 2 = right */
    if (argc >= 2) {
        if (cando_is_string(args[1]) && cando_as_string(args[1])) {
            const char *t = cando_as_string(args[1])->data;
            u32 n = cando_as_string(args[1])->length;
            if (n == 4 && memcmp(t, "left", 4) == 0) align = 0;
            else if (n == 6 && (memcmp(t, "center", 6) == 0 ||
                                memcmp(t, "centre", 6) == 0)) align = 1;
            else if (n == 5 && memcmp(t, "right", 5) == 0) align = 2;
        } else if (cando_is_number(args[1])) {
            int v = (int)cando_as_number(args[1]);
            if (v >= 0 && v <= 2) align = v;
        }
    }
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_TEXTBOX) {
            LONG add = (align == 1) ? ES_CENTER :
                       (align == 2) ? ES_RIGHT  : ES_LEFT;
            edit_toggle_style(s, add, ES_LEFT | ES_CENTER | ES_RIGHT);
            InvalidateRect(s->hwnd, NULL, TRUE);
        } else if (s->kind == KIND_LABEL || s->kind == KIND_LINKLABEL) {
            LONG add = (align == 1) ? SS_CENTER :
                       (align == 2) ? SS_RIGHT  : SS_LEFT;
            edit_toggle_style(s, add, SS_LEFT | SS_CENTER | SS_RIGHT);
            InvalidateRect(s->hwnd, NULL, TRUE);
        }
    }
#else
    (void)align;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_select_all(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "selectAll");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        SendMessageW(s->hwnd, EM_SETSEL, 0, -1);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_append_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "appendText");
    if (!s) return -1;
    char buf[1024] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TEXTBOX) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            int len = (int)SendMessageW(s->hwnd, WM_GETTEXTLENGTH, 0, 0);
            SendMessageW(s->hwnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            SendMessageW(s->hwnd, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)w);
            free(w);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_clear_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clear");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_TEXTBOX) {
            SetWindowTextW(s->hwnd, L"");
        } else if (s->kind == KIND_COMBOBOX) {
            SendMessageW(s->hwnd, CB_RESETCONTENT, 0, 0);
        } else if (s->kind == KIND_LISTBOX) {
            SendMessageW(s->hwnd, LB_RESETCONTENT, 0, 0);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
