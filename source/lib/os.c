/*
 * lib/os.c -- Operating System interface standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "os.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* =========================================================================
 * os.getenv(name) → string | null
 * ======================================================================= */

static int os_getenv(CandoVM *vm, int argc, CandoValue *args)
{
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    if (!name) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    const char *val = getenv(name);
    if (!val) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    libutil_push_cstr(vm, val);
    return 1;
}

/* =========================================================================
 * os.setenv(name, value, overwrite?) → bool
 * ======================================================================= */

static int os_setenv(CandoVM *vm, int argc, CandoValue *args)
{
    const char *name  = libutil_arg_cstr_at(args, argc, 0);
    const char *value = libutil_arg_cstr_at(args, argc, 1);
    bool overwrite = (argc >= 3) ? (bool)libutil_arg_num_at(args, argc, 2, 1.0) : true;

    if (!name || !value) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }

#if defined(_WIN32) || defined(_WIN64)
    if (!overwrite) {
        if (getenv(name)) {
            cando_vm_push(vm, cando_bool(true));
            return 1;
        }
    }
    int res = _putenv_s(name, value);
#else
    int res = setenv(name, value, overwrite ? 1 : 0);
#endif
    cando_vm_push(vm, cando_bool(res == 0));
    return 1;
}

/* =========================================================================
 * os.execute(command) → number
 * ======================================================================= */

static int os_execute(CandoVM *vm, int argc, CandoValue *args)
{
    const char *cmd = libutil_arg_cstr_at(args, argc, 0);
    if (!cmd) {
        cando_vm_push(vm, cando_number(-1));
        return 1;
    }

    int status = system(cmd);
    cando_vm_push(vm, cando_number((f64)status));
    return 1;
}

/* =========================================================================
 * os.exit(code?) → (terminates)
 * ======================================================================= */

static int os_exit(CandoVM *vm, int argc, CandoValue *args)
{
    (void)vm;
    int code = (int)libutil_arg_num_at(args, argc, 0, 0.0);
    exit(code);
    return 0; /* unreachable */
}

/* =========================================================================
 * os.time() → number
 * ======================================================================= */

static int os_time(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)time(NULL)));
    return 1;
}

/* =========================================================================
 * os.clock() → number
 * ======================================================================= */

static int os_clock(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)clock() / CLOCKS_PER_SEC));
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_os_register(CandoVM *vm)
{
    CandoValue os_val = cando_bridge_new_object(vm);
    CdoObject *os_obj = cando_bridge_resolve(vm, os_val.as.handle);

    libutil_set_method(vm, os_obj, "getenv",  os_getenv);
    libutil_set_method(vm, os_obj, "setenv",  os_setenv);
    libutil_set_method(vm, os_obj, "execute", os_execute);
    libutil_set_method(vm, os_obj, "exit",    os_exit);
    libutil_set_method(vm, os_obj, "time",    os_time);
    libutil_set_method(vm, os_obj, "clock",   os_clock);

    /* Platform constants */
#if defined(_WIN32) || defined(_WIN64)
    const char *name = "windows";
#else
    const char *name = "unix";
#endif

    CdoString *k = cdo_string_intern("name", 4);
    CdoString *v = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(os_obj, k, cdo_string_value(v), FIELD_STATIC);
    cdo_string_release(k);
    cdo_string_release(v);

    cando_vm_set_global(vm, "os", os_val, true);
}
