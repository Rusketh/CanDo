/*
 * src/controls/ctl_checkbox.c -- CheckBox + RadioButton natives.
 *
 * The per-kind meta tables registered in forms_module.c attach
 * `setChecked` / `getChecked` only to forms_checkbox + forms_radiobutton
 * (Phase 1.1), so the runtime kind guards in native_set_checked /
 * native_get_checked are now belt-and-braces -- they catch a forged
 * handle that landed on a slot of the wrong kind.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_checkbox.h"
#include "ctl_common.h"
#include "../core/sync.h"     /* pulls <windows.h> on Win32, no-op elsewhere */

int native_set_checked(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setChecked");
    if (!s) return -1;
    bool checked = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && (s->kind == KIND_CHECKBOX || s->kind == KIND_RADIO)) {
        SendMessageW(s->hwnd, BM_SETCHECK,
                     (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
    }
#else
    (void)checked;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_checked(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getChecked");
    if (!s) return -1;
    bool checked = false;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && (s->kind == KIND_CHECKBOX || s->kind == KIND_RADIO)) {
        checked = (SendMessageW(s->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
#endif
    cando_vm_push(vm, cando_bool(checked));
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
