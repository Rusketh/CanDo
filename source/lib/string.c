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
#include <regex.h>

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
    CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);
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

    bool no_regex = (argc >= 3 && cando_is_bool(args[2]) && args[2].as.boolean);

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
    CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);

    if (!s || !pat || pat->length == 0) {
        if (s) {
            CdoString *cs = cdo_string_new(s->data, s->length);
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
        }
        cando_vm_push(vm, arr_val);
        return 1;
    }

    bool no_regex = (argc >= 3 && cando_is_bool(args[2]) && args[2].as.boolean);

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
    CdoObject *arr = cando_bridge_resolve(vm, arr_val.as.handle);

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
 * Registration
 * ======================================================================= */

void cando_lib_string_register(CandoVM *vm)
{
    CandoValue proto_val = cando_bridge_new_object(vm);
    CdoObject *proto     = cando_bridge_resolve(vm, proto_val.as.handle);

    libutil_set_method(vm, proto, "length",  str_length);
    libutil_set_method(vm, proto, "sub",     str_sub);
    libutil_set_method(vm, proto, "char",    str_char);
    libutil_set_method(vm, proto, "chars",   str_chars);
    libutil_set_method(vm, proto, "toLower", str_toLower);
    libutil_set_method(vm, proto, "toUpper", str_toUpper);
    libutil_set_method(vm, proto, "trim",    str_trim);
    libutil_set_method(vm, proto, "left",    str_left);
    libutil_set_method(vm, proto, "right",   str_right);
    libutil_set_method(vm, proto, "repeat",  str_repeat);
    libutil_set_method(vm, proto, "find",    str_find);
    libutil_set_method(vm, proto, "split",   str_split);
    libutil_set_method(vm, proto, "match",   str_match);

    /* Expose as both a global module and the default string prototype. */
    cando_vm_set_global(vm, "string", proto_val, true);
    vm->string_proto = proto_val;
}
