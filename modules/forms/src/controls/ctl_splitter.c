/*
 * src/controls/ctl_splitter.c -- Splitter natives.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_splitter.h"
#include "ctl_common.h"
#include "../core/sync.h"
#include "../core/slots.h"

#include <string.h>

static int parse_splitter_orientation(CandoValue v)
{
    if (v.tag == CDO_NUMBER) {
        int n = (int)v.as.number;
        return (n == 1) ? 1 : 0;
    }
    if (v.tag == CDO_STRING && v.as.string) {
        const char *t = v.as.string->data;
        u32 n = v.as.string->length;
        if (n == 8 && memcmp(t, "vertical",   8) == 0) return 0;
        if (n == 10 && memcmp(t, "horizontal", 10) == 0) return 1;
    }
    return 0;
}

int native_set_orientation(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setOrientation");
    if (!s) return -1;
    if (argc >= 2) s->splitter_orientation = parse_splitter_orientation(args[1]);
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    /* Force a SETCURSOR refresh so the new resize cursor shows
     * immediately even if the mouse hasn't moved. */
    if (s->hwnd && s->kind == KIND_SPLITTER) {
        POINT p; GetCursorPos(&p);
        SetCursorPos(p.x, p.y);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

int native_get_orientation(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getOrientation");
    if (!s) return -1;
    const char *t = (s->splitter_orientation == 1) ? "horizontal" : "vertical";
    cando_vm_push(vm,
        cando_string_value(cando_string_new(t, (u32)strlen(t))));
    return 1;
}

int native_set_splitter_target(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTarget");
    if (!s) return -1;
    int target = -1;
    if (argc >= 2) {
        FormsSlot *t = slot_from_inst(vm, args[1]);
        if (t) target = (int)(t - g_slots);
    }
    s->splitter_target_slot = target;
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */
