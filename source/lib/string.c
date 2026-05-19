/*
 * string.c -- Native string library for Cando.
 *
 * All methods take the string as args[0].  When invoked via colon syntax
 * (`s:method(...)`) the VM passes the receiver as args[0] automatically.
 * When invoked via the global module (`string.method(s, ...)`) the caller
 * passes the string explicitly.
 *
 * Must compile with gcc -std=c11.
 */

#include "string.h"
#include "libutil.h"
#include "meta.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/string.h"
#include "../object/array.h"
#include "../object/value.h"
#include "../object/object.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(_WIN32) || defined(_WIN64)
#  include "../compat/win_regex.h"
#else
#  include <regex.h>
#endif
#include <stdarg.h>

/* =========================================================================
 * string.length(s) → number
 * ======================================================================= */
static int str_length(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { cando_vm_push(vm, cando_number(0)); return 1; }
    cando_vm_push(vm, cando_number((f64)s->length));
    return 1;
}

/* =========================================================================
 * string.sub(s, start, end) → string
 *
 * Extracts bytes [start, end) from s (0-based, end exclusive).
 * Negative indices count from the end.  end defaults to s.length.
 * ======================================================================= */
static int str_sub(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }

    i32 len   = (i32)s->length;
    i32 start = (i32)libutil_arg_num_at(args, argc,1, 0);
    i32 end   = (i32)libutil_arg_num_at(args, argc,2, (f64)len);

    /* Normalise negative indices. */
    if (start < 0) start = len + start;
    if (end   < 0) end   = len + end;
    if (start < 0) start = 0;
    if (end   > len) end = len;
    if (start >= end) { libutil_push_str(vm,"", 0); return 1; }

    libutil_push_str(vm,s->data + start, (u32)(end - start));
    return 1;
}

/* =========================================================================
 * string.char(s, n) → string   (single character at 0-based index n)
 * ======================================================================= */
static int str_char(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    i32 n = (i32)libutil_arg_num_at(args, argc,1, 0);
    if (n < 0) n = (i32)s->length + n;
    if (n < 0 || n >= (i32)s->length) { libutil_push_str(vm,"", 0); return 1; }
    libutil_push_str(vm,s->data + n, 1);
    return 1;
}

/* =========================================================================
 * string.chars(s) → array of single-character strings
 * ======================================================================= */
static int str_chars(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(arr_val));
    if (s) {
        for (u32 i = 0; i < s->length; i++) {
            CdoString *cs = cdo_string_new(s->data + i, 1);
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
        }
    }
    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * string.toLower(s) / string.toUpper(s) → string
 * ======================================================================= */
static int str_toLower(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    char *buf = (char *)cando_alloc(s->length + 1);
    for (u32 i = 0; i < s->length; i++)
        buf[i] = (char)tolower((unsigned char)s->data[i]);
    buf[s->length] = '\0';
    CandoString *res = cando_string_new(buf, s->length);
    cando_free(buf);
    cando_vm_push(vm, cando_string_value(res));
    return 1;
}

static int str_toUpper(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    char *buf = (char *)cando_alloc(s->length + 1);
    for (u32 i = 0; i < s->length; i++)
        buf[i] = (char)toupper((unsigned char)s->data[i]);
    buf[s->length] = '\0';
    CandoString *res = cando_string_new(buf, s->length);
    cando_free(buf);
    cando_vm_push(vm, cando_string_value(res));
    return 1;
}

/* =========================================================================
 * string.trim(s) → string   (strips leading/trailing whitespace)
 * ======================================================================= */
static int str_trim(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    const char *p = s->data;
    const char *e = s->data + s->length;
    while (p < e && isspace((unsigned char)*p)) p++;
    while (e > p && isspace((unsigned char)*(e - 1))) e--;
    libutil_push_str(vm,p, (u32)(e - p));
    return 1;
}

/* =========================================================================
 * string.left(s, n) → string   (first n characters)
 * string.right(s, n) → string  (last n characters)
 * ======================================================================= */
static int str_left(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    u32 n = (u32)libutil_arg_num_at(args, argc,1, 0);
    if (n > s->length) n = s->length;
    libutil_push_str(vm,s->data, n);
    return 1;
}

static int str_right(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    u32 n = (u32)libutil_arg_num_at(args, argc,1, 0);
    if (n > s->length) n = s->length;
    libutil_push_str(vm,s->data + s->length - n, n);
    return 1;
}

/* =========================================================================
 * string.repeat(s, n) → string
 * ======================================================================= */
static int str_repeat(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc,0);
    if (!s) { libutil_push_str(vm,"", 0); return 1; }
    u32 n = (u32)libutil_arg_num_at(args, argc,1, 0);
    if (n == 0 || s->length == 0) { libutil_push_str(vm,"", 0); return 1; }
    u32   total = s->length * n;
    char *buf   = (char *)cando_alloc(total + 1);
    for (u32 i = 0; i < n; i++)
        memcpy(buf + i * s->length, s->data, s->length);
    buf[total] = '\0';
    CandoString *res = cando_string_new(buf, total);
    cando_free(buf);
    cando_vm_push(vm, cando_string_value(res));
    return 1;
}

/* =========================================================================
 * string.find(s, pattern, no_regex*) → number or null
 *
 * Returns the 0-based index of the first match, or null if not found.
 * If no_regex is truthy, performs a literal substring search.
 * ======================================================================= */
static int str_find(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s   = libutil_arg_str_at(args, argc,0);
    CandoString *pat = libutil_arg_str_at(args, argc,1);
    if (!s || !pat) { cando_vm_push(vm, cando_null()); return 1; }

    bool no_regex = (argc >= 3 && cando_is_bool(args[2]) && cando_as_bool(args[2]));

    if (no_regex) {
        /* Plain substring search. */
        const char *found = strstr(s->data, pat->data);
        if (!found) { cando_vm_push(vm, cando_null()); return 1; }
        cando_vm_push(vm, cando_number((f64)(found - s->data)));
        return 1;
    }

    /* Regex search. */
    regex_t re;
    if (regcomp(&re, pat->data, REG_EXTENDED) != 0) {
        cando_vm_error(vm, "string.find: invalid regex pattern '%s'",
                       pat->data);
        return -1;
    }
    regmatch_t m;
    int r = regexec(&re, s->data, 1, &m, 0);
    regfree(&re);
    if (r == REG_NOMATCH) { cando_vm_push(vm, cando_null()); return 1; }
    cando_vm_push(vm, cando_number((f64)m.rm_so));
    return 1;
}

/* =========================================================================
 * string.split(s, pattern, no_regex*) → array
 * ======================================================================= */
static int str_split(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s   = libutil_arg_str_at(args, argc,0);
    CandoString *pat = libutil_arg_str_at(args, argc,1);

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    if (!s || !pat || pat->length == 0) {
        if (s) {
            CdoString *cs = cdo_string_new(s->data, s->length);
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
        }
        cando_vm_push(vm, arr_val);
        return 1;
    }

    bool no_regex = (argc >= 3 && cando_is_bool(args[2]) && cando_as_bool(args[2]));

    if (no_regex) {
        /* Plain literal split. */
        const char *cur = s->data;
        const char *end = s->data + s->length;
        u32 plen = pat->length;
        while (cur <= end) {
            const char *found = strstr(cur, pat->data);
            if (!found) found = end;
            CdoString *cs = cdo_string_new(cur, (u32)(found - cur));
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
            if (found >= end) break;
            cur = found + plen;
        }
        cando_vm_push(vm, arr_val);
        return 1;
    }

    /* Regex split. */
    regex_t re;
    if (regcomp(&re, pat->data, REG_EXTENDED) != 0) {
        cando_vm_error(vm, "string.split: invalid regex pattern '%s'",
                       pat->data);
        cando_value_release(arr_val);
        return -1;
    }
    const char *cur = s->data;
    while (*cur) {
        regmatch_t m;
        if (regexec(&re, cur, 1, &m, 0) == REG_NOMATCH || m.rm_so < 0) {
            /* No more matches: push the remainder. */
            CdoString *cs = cdo_string_new(cur, (u32)strlen(cur));
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
            break;
        }
        /* Push the part before the match. */
        CdoString *cs = cdo_string_new(cur, (u32)m.rm_so);
        cdo_array_push(arr, cdo_string_value(cs));
        cdo_string_release(cs);
        cur += m.rm_eo;
        /* Avoid infinite loop on zero-length match. */
        if (m.rm_eo == m.rm_so) {
            if (*cur) cur++;
            else break;
        }
    }
    regfree(&re);
    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * string.replace(s, pattern, repl, no_regex*) → string
 * ======================================================================= */
static int str_replace(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s    = libutil_arg_str_at(args, argc, 0);
    CandoString *pat  = libutil_arg_str_at(args, argc, 1);
    CandoString *repl = libutil_arg_str_at(args, argc, 2);

    if (!s || !pat || !repl) {
        if (s) libutil_push_str(vm, s->data, s->length);
        else libutil_push_str(vm, "", 0);
        return 1;
    }

    bool no_regex = (argc >= 4 && cando_is_bool(args[3]) && cando_as_bool(args[3]));

    if (no_regex) {
        /* Plain literal replacement (replaces all occurrences). */
        if (pat->length == 0) {
            libutil_push_str(vm, s->data, s->length);
            return 1;
        }

        /* First pass: count occurrences to calculate buffer size. */
        u32 count = 0;
        const char *p = s->data;
        while ((p = strstr(p, pat->data)) != NULL) {
            count++;
            p += pat->length;
        }

        if (count == 0) {
            libutil_push_str(vm, s->data, s->length);
            return 1;
        }

        u32 new_len = s->length + count * (repl->length - pat->length);
        char *buf = (char *)cando_alloc(new_len + 1);
        char *dst = buf;
        const char *src = s->data;
        while ((p = strstr(src, pat->data)) != NULL) {
            u32 prefix_len = (u32)(p - src);
            memcpy(dst, src, prefix_len);
            dst += prefix_len;
            memcpy(dst, repl->data, repl->length);
            dst += repl->length;
            src = p + pat->length;
        }
        strcpy(dst, src);

        CandoString *res = cando_string_new(buf, new_len);
        cando_free(buf);
        cando_vm_push(vm, cando_string_value(res));
        return 1;
    }

    /* Regex replacement (replaces all occurrences). */
    regex_t re;
    if (regcomp(&re, pat->data, REG_EXTENDED) != 0) {
        cando_vm_error(vm, "string.replace: invalid regex pattern '%s'", pat->data);
        return -1;
    }

    /* Rough upper bound for result size. In a real system, we'd use a dynamic buffer. */
    u32 buf_cap = s->length * 2 + repl->length * 2 + 1024;
    char *buf = (char *)cando_alloc(buf_cap);
    buf[0] = '\0';
    u32 buf_len = 0;

    const char *p = s->data;
    regmatch_t m;
    while (regexec(&re, p, 1, &m, 0) == 0) {
        u32 prefix_len = (u32)m.rm_so;
        u32 match_len  = (u32)(m.rm_eo - m.rm_so);

        /* Ensure capacity. */
        if (buf_len + prefix_len + repl->length >= buf_cap - 1) {
             buf_cap = (buf_len + prefix_len + repl->length) * 2;
             buf = cando_realloc(buf, buf_cap);
        }

        memcpy(buf + buf_len, p, prefix_len);
        buf_len += prefix_len;
        memcpy(buf + buf_len, repl->data, repl->length);
        buf_len += repl->length;

        p += m.rm_eo;
        if (match_len == 0) {
            if (*p) {
                buf[buf_len++] = *p++;
            } else break;
        }
    }
    /* Append remainder. */
    u32 rem_len = (u32)strlen(p);
    if (buf_len + rem_len >= buf_cap) {
        buf = cando_realloc(buf, buf_len + rem_len + 1);
    }
    memcpy(buf + buf_len, p, rem_len);
    buf_len += rem_len;
    buf[buf_len] = '\0';

    regfree(&re);
    CandoString *res = cando_string_new(buf, buf_len);
    cando_free(buf);
    cando_vm_push(vm, cando_string_value(res));
    return 1;
}

/* =========================================================================
 * string.startsWith(s, prefix) → bool
 * ======================================================================= */
static int str_startsWith(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s      = libutil_arg_str_at(args, argc, 0);
    CandoString *prefix = libutil_arg_str_at(args, argc, 1);
    if (!s || !prefix) { cando_vm_push(vm, cando_bool(false)); return 1; }
    if (prefix->length > s->length) { cando_vm_push(vm, cando_bool(false)); return 1; }
    bool res = (memcmp(s->data, prefix->data, prefix->length) == 0);
    cando_vm_push(vm, cando_bool(res));
    return 1;
}

/* =========================================================================
 * string.endsWith(s, suffix) → bool
 * ======================================================================= */
static int str_endsWith(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s      = libutil_arg_str_at(args, argc, 0);
    CandoString *suffix = libutil_arg_str_at(args, argc, 1);
    if (!s || !suffix) { cando_vm_push(vm, cando_bool(false)); return 1; }
    if (suffix->length > s->length) { cando_vm_push(vm, cando_bool(false)); return 1; }
    bool res = (memcmp(s->data + s->length - suffix->length, suffix->data, suffix->length) == 0);
    cando_vm_push(vm, cando_bool(res));
    return 1;
}

/* =========================================================================
 * string.format(fmt, ...) → string
 * ======================================================================= */
static int str_format(CandoVM *vm, int argc, CandoValue *args) {
    const char *fmt = libutil_arg_cstr_at(args, argc, 0);
    if (!fmt) { libutil_push_str(vm, "", 0); return 1; }

    /* Simple implementation supporting only %s and %d for now. */
    char buf[4096];
    char *dst = buf;
    char * const end = buf + sizeof(buf) - 1; /* keep one byte for NUL */
    int arg_idx = 1;
    const char *p = fmt;

    while (*p && dst < end) {
        usize remain = (usize)(end - dst);
        if (*p == '%' && *(p+1)) {
            p++;
            if (*p == 's') {
                const char *val = libutil_arg_cstr_at(args, argc, arg_idx++);
                if (val) {
                    usize vlen = strlen(val);
                    if (vlen > remain) vlen = remain;
                    memcpy(dst, val, vlen);
                    dst += vlen;
                }
            } else if (*p == 'd' || *p == 'f') {
                f64 val = libutil_arg_num_at(args, argc, arg_idx++, 0.0);
                int n = snprintf(dst, remain + 1,
                                 (*p == 'd') ? "%.0f" : "%f", val);
                if (n < 0) n = 0;
                if ((usize)n > remain) n = (int)remain;
                dst += n;
            } else if (*p == '%') {
                *dst++ = '%';
            } else {
                *dst++ = '%';
                if (dst < end) *dst++ = *p;
            }
            p++;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    libutil_push_str(vm, buf, (u32)(dst - buf));
    return 1;
}

/* =========================================================================
 * string.match(s, pattern, start*, end*) → bool, array
 *
 * Returns two values: whether the pattern matched, and an array of all
 * captured groups (or the full match if no groups).
 * ======================================================================= */
static int str_match(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s   = libutil_arg_str_at(args, argc,0);
    CandoString *pat = libutil_arg_str_at(args, argc,1);

    if (!s || !pat) {
        cando_vm_push(vm, cando_bool(false));
        CandoValue arr_val = cando_bridge_new_array(vm);
        cando_vm_push(vm, arr_val);
        return 2;
    }

    u32 start = (u32)libutil_arg_num_at(args, argc,2, 0);
    u32 end   = (u32)libutil_arg_num_at(args, argc,3, (f64)s->length);
    if (start > s->length) start = s->length;
    if (end   > s->length) end   = s->length;

    /* Compile regex with up to 16 capture groups. */
    regex_t re;
    if (regcomp(&re, pat->data, REG_EXTENDED) != 0) {
        cando_vm_error(vm, "string.match: invalid regex pattern '%s'",
                       pat->data);
        return -1;
    }

#define MAX_GROUPS 16
    regmatch_t matches[MAX_GROUPS];
    /* Apply the sub-string window by temporarily using a slice. */
    char  *slice   = NULL;
    u32    slen    = end - start;
    bool   own_buf = false;
    if (start == 0 && end == s->length) {
        slice = s->data;
    } else {
        slice = (char *)cando_alloc(slen + 1);
        memcpy(slice, s->data + start, slen);
        slice[slen] = '\0';
        own_buf = true;
    }

    int r = regexec(&re, slice, MAX_GROUPS, matches, 0);
    regfree(&re);

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    if (r == REG_NOMATCH) {
        if (own_buf) cando_free(slice);
        cando_vm_push(vm, cando_bool(false));
        cando_vm_push(vm, arr_val);
        return 2;
    }

    /* Determine number of captured groups (skip [0] which is the full match). */
    int ngroups = 0;
    for (int i = 1; i < MAX_GROUPS; i++) {
        if (matches[i].rm_so < 0) break;
        ngroups++;
    }

    if (ngroups == 0) {
        /* No capture groups: return the full match as the single element. */
        u32 mlen = (u32)(matches[0].rm_eo - matches[0].rm_so);
        CdoString *cs = cdo_string_new(slice + matches[0].rm_so, mlen);
        cdo_array_push(arr, cdo_string_value(cs));
        cdo_string_release(cs);
    } else {
        for (int i = 1; i <= ngroups; i++) {
            u32 mlen = (u32)(matches[i].rm_eo - matches[i].rm_so);
            CdoString *cs = cdo_string_new(slice + matches[i].rm_so, mlen);
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
        }
    }

    if (own_buf) cando_free(slice);
    cando_vm_push(vm, cando_bool(true));
    cando_vm_push(vm, arr_val);
    return 2;
}

/* =========================================================================
 * indexOf, lastIndexOf, includes
 * ======================================================================= */

static int str_indexOf(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    CandoString *n = libutil_arg_str_at(args, argc, 1);
    if (!s || !n) { cando_vm_push(vm, cando_number(-1)); return 1; }
    i32 from = (i32)libutil_arg_num_at(args, argc, 2, 0);
    if (from < 0) from = 0;
    if ((u32)from > s->length) { cando_vm_push(vm, cando_number(-1)); return 1; }
    if (n->length == 0) { cando_vm_push(vm, cando_number((f64)from)); return 1; }
    if (n->length > s->length) { cando_vm_push(vm, cando_number(-1)); return 1; }
    for (u32 i = (u32)from; i + n->length <= s->length; i++) {
        if (memcmp(s->data + i, n->data, n->length) == 0) {
            cando_vm_push(vm, cando_number((f64)i));
            return 1;
        }
    }
    cando_vm_push(vm, cando_number(-1));
    return 1;
}

static int str_lastIndexOf(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    CandoString *n = libutil_arg_str_at(args, argc, 1);
    if (!s || !n) { cando_vm_push(vm, cando_number(-1)); return 1; }
    if (n->length > s->length) { cando_vm_push(vm, cando_number(-1)); return 1; }
    i32 from = (i32)libutil_arg_num_at(args, argc, 2, (f64)(s->length - n->length));
    if (from < 0) { cando_vm_push(vm, cando_number(-1)); return 1; }
    if ((u32)from + n->length > s->length) from = (i32)(s->length - n->length);
    for (i32 i = from; i >= 0; i--) {
        if (memcmp(s->data + i, n->data, n->length) == 0) {
            cando_vm_push(vm, cando_number((f64)i));
            return 1;
        }
    }
    cando_vm_push(vm, cando_number(-1));
    return 1;
}

static int str_includes(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    CandoString *n = libutil_arg_str_at(args, argc, 1);
    if (!s || !n) { cando_vm_push(vm, cando_bool(false)); return 1; }
    if (n->length == 0) { cando_vm_push(vm, cando_bool(true)); return 1; }
    if (n->length > s->length) { cando_vm_push(vm, cando_bool(false)); return 1; }
    for (u32 i = 0; i + n->length <= s->length; i++) {
        if (memcmp(s->data + i, n->data, n->length) == 0) {
            cando_vm_push(vm, cando_bool(true));
            return 1;
        }
    }
    cando_vm_push(vm, cando_bool(false));
    return 1;
}

/* =========================================================================
 * padStart, padEnd
 * ======================================================================= */

static void pad_impl(CandoVM *vm, CandoString *s, u32 target_len,
                     const char *pad, u32 pad_len, bool at_start)
{
    if (s->length >= target_len || pad_len == 0) {
        libutil_push_str(vm, s->data, s->length);
        return;
    }
    u32 fill = target_len - s->length;
    char *out = (char *)cando_alloc(target_len + 1);
    if (at_start) {
        for (u32 i = 0; i < fill; i++) out[i] = pad[i % pad_len];
        memcpy(out + fill, s->data, s->length);
    } else {
        memcpy(out, s->data, s->length);
        for (u32 i = 0; i < fill; i++) out[s->length + i] = pad[i % pad_len];
    }
    out[target_len] = '\0';
    libutil_push_str(vm, out, target_len);
    cando_free(out);
}

static int str_padStart(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    u32 target = (u32)libutil_arg_num_at(args, argc, 1, (f64)s->length);
    CandoString *p = libutil_arg_str_at(args, argc, 2);
    const char *pad = p ? p->data : " ";
    u32 pad_len = p ? p->length : 1;
    pad_impl(vm, s, target, pad, pad_len, true);
    return 1;
}

static int str_padEnd(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    u32 target = (u32)libutil_arg_num_at(args, argc, 1, (f64)s->length);
    CandoString *p = libutil_arg_str_at(args, argc, 2);
    const char *pad = p ? p->data : " ";
    u32 pad_len = p ? p->length : 1;
    pad_impl(vm, s, target, pad, pad_len, false);
    return 1;
}

/* =========================================================================
 * trimStart, trimEnd
 * ======================================================================= */

static int is_trim_char(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int str_trimStart(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    u32 i = 0;
    while (i < s->length && is_trim_char((unsigned char)s->data[i])) i++;
    libutil_push_str(vm, s->data + i, s->length - i);
    return 1;
}

static int str_trimEnd(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    u32 e = s->length;
    while (e > 0 && is_trim_char((unsigned char)s->data[e - 1])) e--;
    libutil_push_str(vm, s->data, e);
    return 1;
}

/* =========================================================================
 * codePointAt, fromCodePoint
 *
 * UTF-8 aware -- decode the codepoint starting at byte index `i` of
 * the string.  Byte index, not character index, by convention with
 * CanDo's other indexing operations.
 * ======================================================================= */

static int str_codePointAt(CandoVM *vm, int argc, CandoValue *args) {
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { cando_vm_push(vm, cando_null()); return 1; }
    i32 idx = (i32)libutil_arg_num_at(args, argc, 1, 0);
    if (idx < 0 || (u32)idx >= s->length) { cando_vm_push(vm, cando_null()); return 1; }
    unsigned char c0 = (unsigned char)s->data[idx];
    u32 cp = 0;
    if      (c0 < 0x80) { cp = c0; }
    else if ((c0 & 0xE0) == 0xC0 && (u32)idx + 1 < s->length) {
        cp = ((u32)(c0 & 0x1F) << 6)
           |  (u32)((unsigned char)s->data[idx + 1] & 0x3F);
    }
    else if ((c0 & 0xF0) == 0xE0 && (u32)idx + 2 < s->length) {
        cp = ((u32)(c0 & 0x0F) << 12)
           | ((u32)((unsigned char)s->data[idx + 1] & 0x3F) << 6)
           |  (u32)((unsigned char)s->data[idx + 2] & 0x3F);
    }
    else if ((c0 & 0xF8) == 0xF0 && (u32)idx + 3 < s->length) {
        cp = ((u32)(c0 & 0x07) << 18)
           | ((u32)((unsigned char)s->data[idx + 1] & 0x3F) << 12)
           | ((u32)((unsigned char)s->data[idx + 2] & 0x3F) << 6)
           |  (u32)((unsigned char)s->data[idx + 3] & 0x3F);
    }
    else { cp = c0; }     /* malformed byte -- return as-is */
    cando_vm_push(vm, cando_number((f64)cp));
    return 1;
}

static int str_fromCodePoint(CandoVM *vm, int argc, CandoValue *args) {
    /* Build a UTF-8 string from a series of codepoint integers. */
    char  *buf = (char *)cando_alloc((size_t)argc * 4 + 1);
    u32 off = 0;
    for (int i = 0; i < argc; i++) {
        if (!cando_is_number(args[i])) continue;
        u32 cp = (u32)cando_as_number(args[i]);
        if (cp < 0x80) {
            buf[off++] = (char)cp;
        } else if (cp < 0x800) {
            buf[off++] = (char)(0xC0 | (cp >> 6));
            buf[off++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[off++] = (char)(0xE0 | (cp >> 12));
            buf[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[off++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x110000) {
            buf[off++] = (char)(0xF0 | (cp >> 18));
            buf[off++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[off++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    buf[off] = '\0';
    libutil_push_str(vm, buf, off);
    cando_free(buf);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry string_methods[] = {
    { "length",        str_length        },
    { "sub",           str_sub           },
    { "char",          str_char          },
    { "chars",         str_chars         },
    { "toLower",       str_toLower       },
    { "toUpper",       str_toUpper       },
    { "trim",          str_trim          },
    { "left",          str_left          },
    { "right",         str_right         },
    { "repeat",        str_repeat        },
    { "find",          str_find          },
    { "split",         str_split         },
    { "replace",       str_replace       },
    { "startsWith",    str_startsWith    },
    { "endsWith",      str_endsWith      },
    { "format",        str_format        },
    { "match",         str_match         },

    /* Querying */
    { "indexOf",       str_indexOf       },
    { "lastIndexOf",   str_lastIndexOf   },
    { "includes",      str_includes      },
    { "contains",      str_includes      },  /* alias */

    /* Padding */
    { "padStart",      str_padStart      },
    { "padEnd",        str_padEnd        },

    /* Trim variants */
    { "trimStart",     str_trimStart     },
    { "trimEnd",       str_trimEnd       },

    /* UTF-8 codepoint helpers */
    { "codePointAt",   str_codePointAt   },
    { "fromCodePoint", str_fromCodePoint },
};

void cando_lib_string_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, cando_as_handle(proto_val));

    libutil_register_methods(vm, proto, string_methods,
                             CANDO_ARRAY_LEN(string_methods));

    /* Expose as both a global module and the default string prototype. */
    cando_vm_set_global(vm, "string", proto_val, true);
    vm->string_proto = proto_val;

    /* Mirror onto `_meta.string` so user scripts can extend the prototype
     * via either name (`string.foo = ...` or `_meta.string.foo = ...`). */
    cando_lib_meta_register(vm);
    cando_lib_meta_set(vm, "string", proto);
}
