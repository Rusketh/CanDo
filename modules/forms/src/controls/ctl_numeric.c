/*
 * src/controls/ctl_numeric.c -- NumericUpDown natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_numeric.h"
#include "ctl_common.h"
#include "../core/sync.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
#endif

int native_set_increment(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setIncrement");
    if (!s) return -1;
    int n = (argc >= 2 && cando_is_number(args[1])) ?
            (int)cando_as_number(args[1]) : 1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_NUMERIC) {
        /* For NumericUpDown the spinner is a sibling msctls_updown32; the
         * UDM_SETACCEL message tunes how fast its repeat-arrow climbs. */
        UDACCEL acc; acc.nSec = 0; acc.nInc = (UINT)n;
        SendMessageW(s->hwnd, UDM_SETACCEL, 1, (LPARAM)&acc);
    }
#else
    (void)n;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
