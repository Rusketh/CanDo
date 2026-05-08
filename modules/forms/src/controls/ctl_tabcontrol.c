/*
 * src/controls/ctl_tabcontrol.c -- TabControl natives (SysTabControl32).
 *
 * Phase 2.2: tab-strip operations (addTab / removeTab / clearTabs /
 * getTabCount / get/setSelectedIndex / get/setTabText).  TabPage
 * (the per-tab content panel) lands in Phase 2.2b alongside
 * TCN_SELCHANGE event routing.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_tabcontrol.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
#endif

#include <stdlib.h>
#include <string.h>

int native_add_tab(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addTab");
    if (!s) return -1;
    char buf[256] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
    int index = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL) {
        int count = (int)SendMessageW(s->hwnd, TCM_GETITEMCOUNT, 0, 0);
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            TCITEMW it; memset(&it, 0, sizeof(it));
            it.mask    = TCIF_TEXT;
            it.pszText = w;
            index = (int)SendMessageW(s->hwnd, TCM_INSERTITEMW,
                                      (WPARAM)count, (LPARAM)&it);
            free(w);
        }
    }
#endif
    cando_vm_push(vm, cando_number((f64)index));
    return 1;
}

int native_remove_tab(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "removeTab");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
              (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL && idx >= 0) {
        SendMessageW(s->hwnd, TCM_DELETEITEM, (WPARAM)idx, 0);
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_clear_tabs(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearTabs");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL) {
        SendMessageW(s->hwnd, TCM_DELETEALLITEMS, 0, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_tab_count(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getTabCount");
    if (!s) return -1;
    int n = 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL) {
        n = (int)SendMessageW(s->hwnd, TCM_GETITEMCOUNT, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

int native_get_selected_tab(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedIndex");
    if (!s) return -1;
    int idx = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL) {
        idx = (int)SendMessageW(s->hwnd, TCM_GETCURSEL, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)idx));
    return 1;
}

int native_set_selected_tab(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSelectedIndex");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
              (int)args[1].as.number : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL) {
        SendMessageW(s->hwnd, TCM_SETCURSEL, (WPARAM)idx, 0);
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_tab_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getTabText");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
              (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL && idx >= 0) {
        wchar_t buf[512] = {0};
        TCITEMW it; memset(&it, 0, sizeof(it));
        it.mask       = TCIF_TEXT;
        it.pszText    = buf;
        it.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
        if (SendMessageW(s->hwnd, TCM_GETITEMW, (WPARAM)idx, (LPARAM)&it)) {
            char *u8 = wide_to_utf8(buf);
            if (u8) {
                cando_vm_push(vm,
                    cando_string_value(cando_string_new(u8, (u32)strlen(u8))));
                free(u8);
                return 1;
            }
        }
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, cando_null());
    return 1;
}

int native_set_tab_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTabText");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
              (int)args[1].as.number : -1;
    char buf[256] = {0};
    if (argc >= 3) parse_text_arg(vm, args[2], buf, sizeof(buf));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TABCONTROL && idx >= 0) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            TCITEMW it; memset(&it, 0, sizeof(it));
            it.mask    = TCIF_TEXT;
            it.pszText = w;
            SendMessageW(s->hwnd, TCM_SETITEMW, (WPARAM)idx, (LPARAM)&it);
            free(w);
        }
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
