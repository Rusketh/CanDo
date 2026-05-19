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

#if defined(_WIN32) || defined(_WIN64)
/* Minimal strptime for Windows (or just fallback) */
static char *strptime_fallback(const char *buf, const char *fmt, struct tm *tm) {
    (void)buf; (void)fmt; (void)tm;
    return NULL;
}
#define strptime strptime_fallback
#endif

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
 * Component accessors -- year / month / day / hour / minute / second
 * / millisecond / dayOfWeek / dayOfYear.
 *
 * All accept a Unix-epoch timestamp (seconds since 1970-01-01 UTC, the
 * unit `datetime.now()` returns) and use UTC for the breakdown.
 * Callers wanting local-time fields should subtract the offset
 * themselves or use a future datetime.local() helper.
 * ======================================================================= */

static struct tm tm_from_ts(f64 ts)
{
    time_t t = (time_t)ts;
    struct tm tm;
#if defined(_WIN32) || defined(_WIN64)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

static int dt_year(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)(tm.tm_year + 1900))); return 1; }

static int dt_month(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)(tm.tm_mon + 1))); return 1; }

static int dt_day(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)tm.tm_mday)); return 1; }

static int dt_hour(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)tm.tm_hour)); return 1; }

static int dt_minute(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)tm.tm_min)); return 1; }

static int dt_second(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)tm.tm_sec)); return 1; }

static int dt_millisecond(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, (f64)time(NULL));
    cando_vm_push(vm, cando_number((ts - (f64)(int64_t)ts) * 1000.0));
    return 1;
}

static int dt_dayOfWeek(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)tm.tm_wday)); return 1; }

static int dt_dayOfYear(CandoVM *vm, int argc, CandoValue *args)
{ struct tm tm = tm_from_ts(libutil_arg_num_at(args, argc, 0, (f64)time(NULL)));
  cando_vm_push(vm, cando_number((f64)(tm.tm_yday + 1))); return 1; }

/* =========================================================================
 * Date math -- addSeconds, addMinutes, addHours, addDays, addMonths,
 * addYears.  Seconds/minutes/hours are trivial arithmetic on the
 * timestamp; the calendar units fan out through gmtime/timegm.
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
static time_t timegm_portable(struct tm *tm)
{
    /* Windows lacks timegm; emulate it via _mkgmtime. */
    return _mkgmtime(tm);
}
#define TIMEGM timegm_portable
#else
#define TIMEGM timegm
#endif

static int dt_addSeconds(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number(ts + n));
    return 1;
}
static int dt_addMinutes(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number(ts + n * 60.0));
    return 1;
}
static int dt_addHours(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number(ts + n * 3600.0));
    return 1;
}
static int dt_addDays(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number(ts + n * 86400.0));
    return 1;
}
static int dt_addMonths(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    struct tm tm = tm_from_ts(ts);
    tm.tm_mon += (int)n;
    cando_vm_push(vm, cando_number((f64)TIMEGM(&tm)));
    return 1;
}
static int dt_addYears(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, 0);
    f64 n  = libutil_arg_num_at(args, argc, 1, 0);
    struct tm tm = tm_from_ts(ts);
    tm.tm_year += (int)n;
    cando_vm_push(vm, cando_number((f64)TIMEGM(&tm)));
    return 1;
}

/* =========================================================================
 * Diffs
 * ======================================================================= */

static int dt_diff(CandoVM *vm, int argc, CandoValue *args)
{
    f64 a = libutil_arg_num_at(args, argc, 0, 0);
    f64 b = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number(a - b));
    return 1;
}
static int dt_diffDays(CandoVM *vm, int argc, CandoValue *args)
{
    f64 a = libutil_arg_num_at(args, argc, 0, 0);
    f64 b = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number((a - b) / 86400.0));
    return 1;
}
static int dt_diffHours(CandoVM *vm, int argc, CandoValue *args)
{
    f64 a = libutil_arg_num_at(args, argc, 0, 0);
    f64 b = libutil_arg_num_at(args, argc, 1, 0);
    cando_vm_push(vm, cando_number((a - b) / 3600.0));
    return 1;
}

/* =========================================================================
 * Calendar helpers
 * ======================================================================= */

static int dt_isLeapYear(CandoVM *vm, int argc, CandoValue *args)
{
    int y = (int)libutil_arg_num_at(args, argc, 0, 0);
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    cando_vm_push(vm, cando_bool(leap));
    return 1;
}

static int dt_daysInMonth(CandoVM *vm, int argc, CandoValue *args)
{
    int y = (int)libutil_arg_num_at(args, argc, 0, 0);
    int m = (int)libutil_arg_num_at(args, argc, 1, 1);
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) { cando_vm_push(vm, cando_number(0)); return 1; }
    int days = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days = 29;
    cando_vm_push(vm, cando_number((f64)days));
    return 1;
}

static int dt_startOfDay(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, (f64)time(NULL));
    int64_t day = (int64_t)ts / 86400;
    cando_vm_push(vm, cando_number((f64)(day * 86400)));
    return 1;
}
static int dt_endOfDay(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ts = libutil_arg_num_at(args, argc, 0, (f64)time(NULL));
    int64_t day = (int64_t)ts / 86400;
    cando_vm_push(vm, cando_number((f64)((day + 1) * 86400 - 1)));
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_datetime_register(CandoVM *vm)
{
    CandoValue dt_val = cando_bridge_new_object(vm);
    CdoObject *dt_obj = cando_bridge_resolve(vm, cando_as_handle(dt_val));

    libutil_set_method(vm, dt_obj, "now",     dt_now);
    libutil_set_method(vm, dt_obj, "format",  dt_format);
    libutil_set_method(vm, dt_obj, "parse",   dt_parse);

    /* Component accessors. */
    libutil_set_method(vm, dt_obj, "year",        dt_year);
    libutil_set_method(vm, dt_obj, "month",       dt_month);
    libutil_set_method(vm, dt_obj, "day",         dt_day);
    libutil_set_method(vm, dt_obj, "hour",        dt_hour);
    libutil_set_method(vm, dt_obj, "minute",      dt_minute);
    libutil_set_method(vm, dt_obj, "second",      dt_second);
    libutil_set_method(vm, dt_obj, "millisecond", dt_millisecond);
    libutil_set_method(vm, dt_obj, "dayOfWeek",   dt_dayOfWeek);
    libutil_set_method(vm, dt_obj, "dayOfYear",   dt_dayOfYear);

    /* Date math. */
    libutil_set_method(vm, dt_obj, "addSeconds",  dt_addSeconds);
    libutil_set_method(vm, dt_obj, "addMinutes",  dt_addMinutes);
    libutil_set_method(vm, dt_obj, "addHours",    dt_addHours);
    libutil_set_method(vm, dt_obj, "addDays",     dt_addDays);
    libutil_set_method(vm, dt_obj, "addMonths",   dt_addMonths);
    libutil_set_method(vm, dt_obj, "addYears",    dt_addYears);

    libutil_set_method(vm, dt_obj, "diff",        dt_diff);
    libutil_set_method(vm, dt_obj, "diffDays",    dt_diffDays);
    libutil_set_method(vm, dt_obj, "diffHours",   dt_diffHours);

    libutil_set_method(vm, dt_obj, "isLeapYear",   dt_isLeapYear);
    libutil_set_method(vm, dt_obj, "daysInMonth",  dt_daysInMonth);
    libutil_set_method(vm, dt_obj, "startOfDay",   dt_startOfDay);
    libutil_set_method(vm, dt_obj, "endOfDay",     dt_endOfDay);

    cando_vm_set_global(vm, "datetime", dt_val, true);
}
