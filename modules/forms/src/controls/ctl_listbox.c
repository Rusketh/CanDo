/*
 * src/controls/ctl_listbox.c -- ListBox + ComboBox item natives.
 *
 * Each native dispatches on s->kind to the matching Win32 message
 * (LB_* for ListBox, CB_* for ComboBox).  list_item_text is a helper
 * shared by getItem and getItems.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_listbox.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"

#include <stdlib.h>
#include <string.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
/* Pull the i-th item text out of a list/combo as UTF-8.  Caller frees.
 * Returns NULL on out-of-range / wrong-kind / OOM. */
static char *list_item_text(FormsSlot *s, int idx)
{
    if (!s || !s->hwnd) return NULL;
    LRESULT len_msg = (s->kind == KIND_LISTBOX)  ? LB_GETTEXTLEN :
                      (s->kind == KIND_COMBOBOX) ? CB_GETLBTEXTLEN : -1;
    LRESULT get_msg = (s->kind == KIND_LISTBOX)  ? LB_GETTEXT     :
                      (s->kind == KIND_COMBOBOX) ? CB_GETLBTEXT   : -1;
    if (len_msg < 0) return NULL;
    LRESULT len = SendMessageW(s->hwnd, (UINT)len_msg, (WPARAM)idx, 0);
    if (len < 0) return NULL;
    wchar_t *wbuf = (wchar_t *)calloc((size_t)len + 1, sizeof(wchar_t));
    if (!wbuf) return NULL;
    SendMessageW(s->hwnd, (UINT)get_msg, (WPARAM)idx, (LPARAM)wbuf);
    char *u8 = wide_to_utf8(wbuf);
    free(wbuf);
    return u8;
}
#endif

int native_add_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addItem");
    if (!s) return -1;
    char buf[1024] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
    int index = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (s->kind == KIND_LISTBOX) {
            index = (int)SendMessageW(s->hwnd, LB_ADDSTRING, 0, (LPARAM)w);
        } else if (s->kind == KIND_COMBOBOX) {
            index = (int)SendMessageW(s->hwnd, CB_ADDSTRING, 0, (LPARAM)w);
        }
        free(w);
    }
#endif
    cando_vm_push(vm, cando_number((f64)index));
    return 1;
}

int native_clear_items(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearItems");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)  SendMessageW(s->hwnd, LB_RESETCONTENT, 0, 0);
        if (s->kind == KIND_COMBOBOX) SendMessageW(s->hwnd, CB_RESETCONTENT, 0, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_item_count(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getItemCount");
    if (!s) return -1;
    int n = 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)
            n = (int)SendMessageW(s->hwnd, LB_GETCOUNT, 0, 0);
        else if (s->kind == KIND_COMBOBOX)
            n = (int)SendMessageW(s->hwnd, CB_GETCOUNT, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

int native_get_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getItem");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
    if (idx < 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    char *u8 = list_item_text(s, idx);
    if (u8) {
        cando_vm_push(vm,
            cando_string_value(cando_string_new(u8, (u32)strlen(u8))));
        free(u8);
        return 1;
    }
#endif
    cando_vm_push(vm, cando_null());
    return 1;
}

int native_get_items(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getItems");
    if (!s) return -1;
    CandoValue arr = cando_bridge_new_array(vm);
    CdoObject *a   = cando_bridge_resolve(vm, arr.as.handle);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        int n = 0;
        if (s->kind == KIND_LISTBOX)
            n = (int)SendMessageW(s->hwnd, LB_GETCOUNT, 0, 0);
        else if (s->kind == KIND_COMBOBOX)
            n = (int)SendMessageW(s->hwnd, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            char *u8 = list_item_text(s, i);
            if (!u8) continue;
            CdoString *cs = cdo_string_intern(u8, (u32)strlen(u8));
            cdo_array_push(a, cdo_string_value(cs));
            cdo_string_release(cs);
            free(u8);
        }
    }
#else
    (void)a;
#endif
    cando_vm_push(vm, arr);
    return 1;
}

int native_remove_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "removeItem");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && idx >= 0) {
        if (s->kind == KIND_LISTBOX)
            SendMessageW(s->hwnd, LB_DELETESTRING, (WPARAM)idx, 0);
        else if (s->kind == KIND_COMBOBOX)
            SendMessageW(s->hwnd, CB_DELETESTRING, (WPARAM)idx, 0);
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedIndex");
    if (!s) return -1;
    int idx = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)
            idx = (int)SendMessageW(s->hwnd, LB_GETCURSEL, 0, 0);
        else if (s->kind == KIND_COMBOBOX)
            idx = (int)SendMessageW(s->hwnd, CB_GETCURSEL, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)idx));
    return 1;
}

int native_set_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSelectedIndex");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)
            SendMessageW(s->hwnd, LB_SETCURSEL, (WPARAM)idx, 0);
        else if (s->kind == KIND_COMBOBOX)
            SendMessageW(s->hwnd, CB_SETCURSEL, (WPARAM)idx, 0);
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
