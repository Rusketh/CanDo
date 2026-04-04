/*
 * lib/datetime.c -- Date and time standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif

#include "datetime.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * datetime.now() → number
 * ======================================================================= */

static int dt_now(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)time(NULL)));
    return 1;
}

/* =========================================================================
 * datetime.format(timestamp, format_string) → string
 * ======================================================================= */

static int dt_format(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts_f          = libutil_arg_num_at(args, argc, 0, (f64)time(NULL));
    const char *fmt   = libutil_arg_cstr_at(args, argc, 1);
    if (!fmt) fmt = "%Y-%m-%d %H:%M:%S";

    time_t ts = (time_t)ts_f;
    struct tm *tm_info = localtime(&ts);
    if (!tm_info) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    char buf[256];
    if (strftime(buf, sizeof(buf), fmt, tm_info) == 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    libutil_push_cstr(vm, buf);
    return 1;
}

/* =========================================================================
 * datetime.parse(date_string, format_string) → number | null
 * ======================================================================= */

static int dt_parse(CandoVM *vm, int argc, CandoValue *args)
{
    const char *s   = libutil_arg_cstr_at(args, argc, 0);
    const char *fmt = libutil_arg_cstr_at(args, argc, 1);

    if (!s || !fmt) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    struct tm tm_info;
    memset(&tm_info, 0, sizeof(struct tm));
    if (strptime(s, fmt, &tm_info) == NULL) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    /* Important: mktime uses the local timezone. */
    time_t ts = mktime(&tm_info);
    if (ts == -1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    cando_vm_push(vm, cando_number((f64)ts));
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_datetime_register(CandoVM *vm)
{
    CandoValue dt_val = cando_bridge_new_object(vm);
    CdoObject *dt_obj = cando_bridge_resolve(vm, dt_val.as.handle);

    libutil_set_method(vm, dt_obj, "now",    dt_now);
    libutil_set_method(vm, dt_obj, "format", dt_format);
    libutil_set_method(vm, dt_obj, "parse",  dt_parse);

    cando_vm_set_global(vm, "datetime", dt_val, true);
}
