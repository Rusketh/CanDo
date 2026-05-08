/*
 * src/controls/ctl_progress.c -- ProgressBar natives.
 *
 * The runtime kind guard inside each native is belt-and-braces: the
 * per-kind meta table dispatch in Phase 1.1 already ensures only
 * forms_progress instances reach here.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_progress.h"
#include "ctl_common.h"
#include "../core/sync.h"
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  include <commctrl.h>
#endif
#include <string.h>

int native_set_step(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setStep");
    if (!s) return -1;
    int step = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
               (int)args[1].as.number : 1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_PROGRESS) {
        SendMessageW(s->hwnd, PBM_SETSTEP, (WPARAM)step, 0);
    }
#else
    (void)step;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_step_it(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "stepIt");
    if (!s) return -1;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_PROGRESS) {
        SendMessageW(s->hwnd, PBM_STEPIT, 0, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_marquee(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMarquee");
    if (!s) return -1;
    bool active = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    int  speed  = (argc >= 3 && args[2].tag == CDO_NUMBER) ?
                  (int)args[2].as.number : 30;
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_PROGRESS) {
        SendMessageW(s->hwnd, PBM_SETMARQUEE,
                     (WPARAM)(active ? TRUE : FALSE), (LPARAM)speed);
    }
#else
    (void)active; (void)speed;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_set_state(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setState");
    if (!s) return -1;
    int state = 1;  /* PBST_NORMAL */
    if (argc >= 2) {
        if (args[1].tag == CDO_NUMBER) {
            state = (int)args[1].as.number;
        } else if (args[1].tag == CDO_STRING && args[1].as.string) {
            const char *t = args[1].as.string->data;
            u32 n = args[1].as.string->length;
            #define MATCH(name, val) \
                if (n == sizeof(name)-1 && memcmp(t, name, sizeof(name)-1) == 0) state = val
            MATCH("normal",  1);  /* PBST_NORMAL */
            MATCH("error",   2);  /* PBST_ERROR  */
            MATCH("paused",  3);  /* PBST_PAUSED */
            MATCH("warning", 2);  /* alias -> error (red) for ergonomics */
            MATCH("green",   1);
            MATCH("yellow",  3);
            MATCH("red",     2);
            #undef MATCH
        }
    }
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    if (s->hwnd && s->kind == KIND_PROGRESS) {
        SendMessageW(s->hwnd, PBM_SETSTATE, (WPARAM)state, 0);
    }
#else
    (void)state;
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
