/*
 * src/controls/ctl_treeview.c -- TreeView natives wrapping SysTreeView32.
 *
 * Handles are opaque numbers in script land.  Internally we cast
 * HTREEITEM <-> uintptr_t <-> double; HTREEITEM values fit in a
 * double's 53-bit mantissa on every Windows pointer model so the
 * round-trip is exact.  A NULL / 0 handle means "root" or "none".
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_treeview.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/textconv.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
static inline double tv_handle_to_number(HTREEITEM h)
{
    return (double)(uintptr_t)h;
}
static inline HTREEITEM tv_number_to_handle(double n)
{
    return (HTREEITEM)(uintptr_t)n;
}

/* Recursively count the items in a subtree rooted at `start`. */
static int tv_count_subtree(HWND hwnd, HTREEITEM start)
{
    int n = 0;
    HTREEITEM it = start;
    while (it) {
        n++;
        HTREEITEM child = (HTREEITEM)SendMessageW(hwnd, TVM_GETNEXTITEM,
                                                  TVGN_CHILD, (LPARAM)it);
        if (child) n += tv_count_subtree(hwnd, child);
        it = (HTREEITEM)SendMessageW(hwnd, TVM_GETNEXTITEM,
                                     TVGN_NEXT, (LPARAM)it);
    }
    return n;
}
#endif

int native_tree_add_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addNode");
    if (!s) return -1;

    /* args[1] = parent handle (number) or NULL/0; args[2] = text. */
    double parent_n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
    int    text_arg = (argc >= 3) ? 2 : (argc >= 2 && args[1].tag == CDO_STRING ? 1 : -1);
    char buf[512] = {0};
    if (text_arg >= 0) parse_text_arg(vm, args[text_arg], buf, sizeof(buf));

    double out = 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            TVINSERTSTRUCTW ins; memset(&ins, 0, sizeof(ins));
            HTREEITEM parent = (text_arg == 2) ? tv_number_to_handle(parent_n)
                                               : NULL;
            ins.hParent       = parent ? parent : TVI_ROOT;
            ins.hInsertAfter  = TVI_LAST;
            ins.item.mask     = TVIF_TEXT;
            ins.item.pszText  = w;
            HTREEITEM h = (HTREEITEM)SendMessageW(s->hwnd, TVM_INSERTITEMW,
                                                  0, (LPARAM)&ins);
            if (h) out = tv_handle_to_number(h);
            free(w);
        }
    }
#else
    (void)parent_n;
#endif
    cando_vm_push(vm, cando_number(out));
    return 1;
}

int native_tree_remove_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "removeNode");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW && n != 0.0) {
        SendMessageW(s->hwnd, TVM_DELETEITEM, 0, (LPARAM)tv_number_to_handle(n));
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_clear_nodes(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearNodes");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW) {
        SendMessageW(s->hwnd, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_get_selected_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedNode");
    if (!s) return -1;
    double out = 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW) {
        HTREEITEM h = (HTREEITEM)SendMessageW(s->hwnd, TVM_GETNEXTITEM,
                                              TVGN_CARET, 0);
        if (h) out = tv_handle_to_number(h);
    }
#endif
    cando_vm_push(vm, cando_number(out));
    return 1;
}

int native_tree_set_selected_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSelectedNode");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW) {
        SendMessageW(s->hwnd, TVM_SELECTITEM, TVGN_CARET,
                     (LPARAM)tv_number_to_handle(n));
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_expand_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "expandNode");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW && n != 0.0) {
        SendMessageW(s->hwnd, TVM_EXPAND, TVE_EXPAND,
                     (LPARAM)tv_number_to_handle(n));
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_collapse_node(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "collapseNode");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW && n != 0.0) {
        SendMessageW(s->hwnd, TVM_EXPAND, TVE_COLLAPSE,
                     (LPARAM)tv_number_to_handle(n));
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_get_node_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getNodeText");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW && n != 0.0) {
        wchar_t buf[512] = {0};
        TVITEMW it; memset(&it, 0, sizeof(it));
        it.mask       = TVIF_TEXT;
        it.hItem      = tv_number_to_handle(n);
        it.pszText    = buf;
        it.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
        if (SendMessageW(s->hwnd, TVM_GETITEMW, 0, (LPARAM)&it)) {
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
    (void)n;
#endif
    cando_vm_push(vm, cando_null());
    return 1;
}

int native_tree_set_node_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setNodeText");
    if (!s) return -1;
    double n = (argc >= 2 && args[1].tag == CDO_NUMBER) ? args[1].as.number : 0.0;
    char buf[512] = {0};
    if (argc >= 3) parse_text_arg(vm, args[2], buf, sizeof(buf));
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW && n != 0.0) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (w) {
            TVITEMW it; memset(&it, 0, sizeof(it));
            it.mask    = TVIF_TEXT;
            it.hItem   = tv_number_to_handle(n);
            it.pszText = w;
            SendMessageW(s->hwnd, TVM_SETITEMW, 0, (LPARAM)&it);
            free(w);
        }
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_tree_get_node_count(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getNodeCount");
    if (!s) return -1;
    int n = 0;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TREEVIEW) {
        n = (int)SendMessageW(s->hwnd, TVM_GETCOUNT, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
