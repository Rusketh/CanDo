/*
 * src/controls/ctl_common.c -- shared helpers used by every per-control
 * native TU.  See ctl_common.h for the contract.
 *
 * Entire file gates on FORMS_MODULE_TEST_BUILD: in the test build the
 * libcando bridge is unavailable so this TU compiles to nothing.  The
 * production build links the standalone object next to forms_module.o.
 */

#ifndef FORMS_MODULE_TEST_BUILD

#include "ctl_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

FormsSlot *slot_from_inst(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return NULL;
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    if (!o) return NULL;
    CdoString *ks = cdo_string_intern(FORMS_SLOT_KEY, (u32)strlen(FORMS_SLOT_KEY));
    CdoString *kg = cdo_string_intern(FORMS_GEN_KEY,  (u32)strlen(FORMS_GEN_KEY));
    CdoString *kk = cdo_string_intern(FORMS_KIND_KEY, (u32)strlen(FORMS_KIND_KEY));
    CdoValue vs, vg, vk;
    bool has_s = cdo_object_rawget(o, ks, &vs);
    bool has_g = cdo_object_rawget(o, kg, &vg);
    bool has_k = cdo_object_rawget(o, kk, &vk);
    cdo_string_release(ks);
    cdo_string_release(kg);
    cdo_string_release(kk);
    if (!has_s || !has_g || !has_k) return NULL;
    if (vs.tag != CDO_NUMBER ||
        vg.tag != CDO_NUMBER ||
        vk.tag != CDO_NUMBER) return NULL;
    int slot = (int)vs.as.number;
    int gen  = (int)vg.as.number;
    int kind = (int)vk.as.number;
    if (slot <= 0 || slot >= FORMS_MAX_SLOTS) return NULL;
    FormsSlot *s = &g_slots[slot];
    if (!s->alive || s->generation != gen) return NULL;
    /* Phase 0.5 cross-check: forged handle landing on a live slot
     * still has to match the kind that build_instance stamped. */
    if ((int)s->kind != kind) return NULL;
    return s;
}

void forms_throw(CandoVM *vm, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cando_vm_error(vm, "%s", buf);
}

int require_supported(CandoVM *vm, const char *who)
{
#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
    (void)vm; (void)who;
    return 1;
#else
    forms_throw(vm, "%s: forms is only supported on Windows "
                    "(loaded module is the stub build)", who);
    return 0;
#endif
}

FormsSlot *arg_self(CandoVM *vm, int argc, CandoValue *args,
                    const char *who)
{
    if (argc < 1) {
        forms_throw(vm, "%s: missing receiver (call as obj:%s(...))", who, who);
        return NULL;
    }
    FormsSlot *s = slot_from_inst(vm, args[0]);
    if (!s) {
        forms_throw(vm, "%s: invalid or destroyed receiver", who);
        return NULL;
    }
    return s;
}

void parse_text_arg(CandoVM *vm, CandoValue v, char *out, int outcap)
{
    (void)vm;
    if (out == NULL || outcap <= 0) return;
    out[0] = 0;
    if (v.tag == CDO_STRING && v.as.string) {
        const char *s = v.as.string->data;
        u32 n = v.as.string->length;
        if ((int)n >= outcap) n = (u32)(outcap - 1);
        memcpy(out, s, n);
        out[n] = 0;
    }
}

#endif /* !FORMS_MODULE_TEST_BUILD */
