/*
 * src/controls/ctl_trackbar.c -- TrackBar natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_trackbar.h"
#include "ctl_common.h"
#include "../core/sync.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
#endif

int native_set_tick_frequency(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTickFrequency");
    if (!s) return -1;
    int n = (argc >= 2 && cando_is_number(args[1])) ?
            (int)cando_as_number(args[1]) : 1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TRACKBAR) {
        SendMessageW(s->hwnd, TBM_SETTICFREQ, (WPARAM)n, 0);
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_small_step(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSmallStep");
    if (!s) return -1;
    int n = (argc >= 2 && cando_is_number(args[1])) ?
            (int)cando_as_number(args[1]) : 1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TRACKBAR) {
        SendMessageW(s->hwnd, TBM_SETLINESIZE, 0, (LPARAM)n);
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_large_step(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setLargeStep");
    if (!s) return -1;
    int n = (argc >= 2 && cando_is_number(args[1])) ?
            (int)cando_as_number(args[1]) : 5;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_TRACKBAR) {
        SendMessageW(s->hwnd, TBM_SETPAGESIZE, 0, (LPARAM)n);
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
