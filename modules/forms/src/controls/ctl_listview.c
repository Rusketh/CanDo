/*
 * src/controls/ctl_listview.c -- ListView natives wrapping SysListView32.
 *
 * Multi-column rows; addItem accepts either a single string (column 0
 * only) or an array of strings (one per column).
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_listview.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
   /* LVM_GETCOLUMNCOUNT only exposes itself when _WIN32_WINNT >= 0x0501
    * with certain SDK versions; the cross-compile uses 0x0600 but
    * older MinGW commctrl.h still hides it.  Define the canonical
    * value so we don't depend on the SDK shipping it. */
#  ifndef LVM_GETCOLUMNCOUNT
#    define LVM_GETCOLUMNCOUNT  (LVM_FIRST + 100)
#  endif
#endif

#include <stdlib.h>
#include <string.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
/* Helper: send LVM_SETITEMTEXT for a single subitem with UTF-8 input. */
static void lv_set_text(HWND hwnd, int row, int col, const char *utf8)
{
    wchar_t *w = utf8_to_wide(utf8 ? utf8 : "", -1);
    if (!w) return;
    LVITEMW it; memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText  = w;
    SendMessageW(hwnd, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    free(w);
}
#endif

int native_lv_add_column(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addColumn");
    if (!s) return -1;
    char buf[256] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
    int width = (argc >= 3 && args[2].tag == CDO_NUMBER) ?
                (int)args[2].as.number : 100;
    int index = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            int existing = (int)SendMessageW(s->hwnd, LVM_GETCOLUMNCOUNT, 0, 0);
            LVCOLUMNW col; memset(&col, 0, sizeof(col));
            col.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.fmt     = LVCFMT_LEFT;
            col.cx      = width;
            col.pszText = w;
            index = (int)SendMessageW(s->hwnd, LVM_INSERTCOLUMNW,
                                      (WPARAM)existing, (LPARAM)&col);
            free(w);
        }
    }
#else
    (void)width;
#endif
    cando_vm_push(vm, cando_number((f64)index));
    return 1;
}

int native_lv_set_column_width(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setColumnWidth");
    if (!s) return -1;
    int col = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
    int w   = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 100;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        SendMessageW(s->hwnd, LVM_SETCOLUMNWIDTH, (WPARAM)col, (LPARAM)w);
    }
#else
    (void)col; (void)w;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_get_column_count(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getColumnCount");
    if (!s) return -1;
    int n = 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        n = (int)SendMessageW(s->hwnd, LVM_GETCOLUMNCOUNT, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

int native_lv_add_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addItem");
    if (!s) return -1;
    int index = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && argc >= 2) {
        int existing = (int)SendMessageW(s->hwnd, LVM_GETITEMCOUNT, 0, 0);
        wchar_t empty[1] = {0};
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask     = LVIF_TEXT;
        it.iItem    = existing;
        it.iSubItem = 0;
        it.pszText  = empty;
        index = (int)SendMessageW(s->hwnd, LVM_INSERTITEMW, 0, (LPARAM)&it);

        if (index >= 0) {
            if (args[1].tag == CDO_STRING && args[1].as.string) {
                /* Single-string form: column 0 only. */
                char tmp[1024] = {0};
                u32 n = args[1].as.string->length;
                if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
                memcpy(tmp, args[1].as.string->data, n);
                tmp[n] = 0;
                lv_set_text(s->hwnd, index, 0, tmp);
            } else if (args[1].tag == CDO_ARRAY && args[1].as.handle) {
                /* Array form: each element -> matching column. */
                CdoObject *arr = cando_bridge_resolve(vm, args[1].as.handle);
                if (arr) {
                    u32 len  = cdo_array_len(arr);
                    u32 cols = (u32)SendMessageW(s->hwnd, LVM_GETCOLUMNCOUNT, 0, 0);
                    for (u32 i = 0; i < len && i < cols; i++) {
                        CdoValue cv;
                        if (!cdo_array_rawget_idx(arr, i, &cv)) continue;
                        if (cv.tag == CDO_STRING && cv.as.string) {
                            char tmp[1024] = {0};
                            u32 n = cv.as.string->length;
                            if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
                            memcpy(tmp, cv.as.string->data, n);
                            tmp[n] = 0;
                            lv_set_text(s->hwnd, index, (int)i, tmp);
                        }
                    }
                }
            }
        }
    }
#endif
    cando_vm_push(vm, cando_number((f64)index));
    return 1;
}

int native_lv_set_subitem(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSubItem");
    if (!s) return -1;
    int row = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
    int col = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 0;
    char buf[1024] = {0};
    if (argc >= 4) parse_text_arg(vm, args[3], buf, sizeof(buf));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && row >= 0) {
        lv_set_text(s->hwnd, row, col, buf);
    }
#else
    (void)row; (void)col;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_get_item_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getItemText");
    if (!s) return -1;
    int row = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
    int col = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && row >= 0) {
        wchar_t buf[512] = {0};
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.iSubItem   = col;
        it.pszText    = buf;
        it.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
        SendMessageW(s->hwnd, LVM_GETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
        char *u8 = wide_to_utf8(buf);
        if (u8) {
            cando_vm_push(vm,
                cando_string_value(cando_string_new(u8, (u32)strlen(u8))));
            free(u8);
            return 1;
        }
    }
#else
    (void)row; (void)col;
#endif
    cando_vm_push(vm, cando_null());
    return 1;
}

int native_lv_remove_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "removeItem");
    if (!s) return -1;
    int row = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && row >= 0) {
        SendMessageW(s->hwnd, LVM_DELETEITEM, (WPARAM)row, 0);
    }
#else
    (void)row;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_clear_items(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearItems");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        SendMessageW(s->hwnd, LVM_DELETEALLITEMS, 0, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_get_item_count(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getItemCount");
    if (!s) return -1;
    int n = 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        n = (int)SendMessageW(s->hwnd, LVM_GETITEMCOUNT, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

int native_lv_get_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedIndex");
    if (!s) return -1;
    int idx = -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        idx = (int)SendMessageW(s->hwnd, LVM_GETNEXTITEM,
                                (WPARAM)-1, MAKELPARAM(LVNI_SELECTED, 0));
    }
#endif
    cando_vm_push(vm, cando_number((f64)idx));
    return 1;
}

int native_lv_set_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSelectedIndex");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && idx >= 0) {
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        it.state     = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(s->hwnd, LVM_SETITEMSTATE, (WPARAM)idx, (LPARAM)&it);
        SendMessageW(s->hwnd, LVM_ENSUREVISIBLE, (WPARAM)idx, FALSE);
    }
#else
    (void)idx;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_get_selected_indices(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedIndices");
    if (!s) return -1;
    CandoValue arr = cando_bridge_new_array(vm);
    CdoObject *a   = cando_bridge_resolve(vm, arr.as.handle);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW && a) {
        int idx = -1;
        for (;;) {
            idx = (int)SendMessageW(s->hwnd, LVM_GETNEXTITEM,
                                    (WPARAM)idx, MAKELPARAM(LVNI_SELECTED, 0));
            if (idx < 0) break;
            cdo_array_push(a, cdo_number((f64)idx));
        }
    }
#else
    (void)a;
#endif
    cando_vm_push(vm, arr);
    return 1;
}

int native_lv_set_view(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setView");
    if (!s) return -1;
    int view = 1;   /* LV_VIEW_DETAILS */
    if (argc >= 2 && args[1].tag == CDO_STRING && args[1].as.string) {
        const char *t = args[1].as.string->data;
        u32 n = args[1].as.string->length;
        if      (n == 7 && memcmp(t, "details",   7) == 0) view = 1;
        else if (n == 4 && memcmp(t, "list",      4) == 0) view = 2;
        else if (n == 9 && memcmp(t, "smallIcon", 9) == 0) view = 3;
        else if (n == 9 && memcmp(t, "largeIcon", 9) == 0) view = 0;
        else if (n == 4 && memcmp(t, "tile",      4) == 0) view = 4;
    } else if (argc >= 2 && args[1].tag == CDO_NUMBER) {
        view = (int)args[1].as.number;
    }
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        SendMessageW(s->hwnd, LVM_SETVIEW, (WPARAM)view, 0);
    }
#else
    (void)view;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
static void lv_toggle_ex_style(HWND hwnd, DWORD bit, bool on)
{
    DWORD ex = (DWORD)SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0);
    DWORD next = on ? (ex | bit) : (ex & ~bit);
    SendMessageW(hwnd, LVM_SETEXTENDEDLISTVIEWSTYLE, (WPARAM)bit, (LPARAM)next);
}
#endif

int native_lv_set_full_row_select(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setFullRowSelect");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW)
        lv_toggle_ex_style(s->hwnd, LVS_EX_FULLROWSELECT, on);
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_set_grid_lines(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setGridLines");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW)
        lv_toggle_ex_style(s->hwnd, LVS_EX_GRIDLINES, on);
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_lv_set_multi_select(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMultiSelect");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_LISTVIEW) {
        LONG st = GetWindowLongW(s->hwnd, GWL_STYLE);
        if (on) st &= ~LVS_SINGLESEL;
        else    st |=  LVS_SINGLESEL;
        SetWindowLongW(s->hwnd, GWL_STYLE, st);
    }
#else
    (void)on;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
