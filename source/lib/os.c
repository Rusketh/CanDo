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

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#  include <lmcons.h>
#else
#  include <sys/utsname.h>
#  include <pwd.h>
#  include <sys/types.h>
#  if defined(__linux__)
#    include <sys/sysinfo.h>
#  elif defined(__APPLE__)
#    include <sys/sysctl.h>
#    include <mach/mach.h>
#  endif
#endif

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
 * System info accessors -- hostname, tmpdir, homedir, arch, uptime,
 * totalmem, freemem, cpus.
 * ======================================================================= */

static int os_hostname(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    char buf[256];
#if defined(_WIN32) || defined(_WIN64)
    DWORD n = sizeof(buf);
    if (!GetComputerNameA(buf, &n)) { libutil_push_str(vm, "", 0); return 1; }
    libutil_push_str(vm, buf, (u32)n);
#else
    if (gethostname(buf, sizeof(buf)) != 0) { libutil_push_str(vm, "", 0); return 1; }
    buf[sizeof(buf) - 1] = '\0';
    libutil_push_str(vm, buf, (u32)strlen(buf));
#endif
    return 1;
}

static int os_tmpdir(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(_WIN32) || defined(_WIN64)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0) { libutil_push_str(vm, "C:\\Windows\\Temp", 15); return 1; }
    /* Strip trailing slash to match POSIX convention. */
    while (n > 1 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[--n] = '\0';
    libutil_push_str(vm, buf, n);
#else
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    libutil_push_str(vm, t, (u32)strlen(t));
#endif
    return 1;
}

static int os_homedir(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(_WIN32) || defined(_WIN64)
    const char *h = getenv("USERPROFILE");
    if (!h || !*h) {
        const char *drive = getenv("HOMEDRIVE");
        const char *path  = getenv("HOMEPATH");
        if (drive && path) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s%s", drive, path);
            libutil_push_str(vm, buf, (u32)strlen(buf));
            return 1;
        }
        libutil_push_str(vm, "", 0);
        return 1;
    }
    libutil_push_str(vm, h, (u32)strlen(h));
#else
    const char *h = getenv("HOME");
    if (!h || !*h) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) h = pw->pw_dir;
    }
    if (!h) h = "";
    libutil_push_str(vm, h, (u32)strlen(h));
#endif
    return 1;
}

static int os_arch(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(__x86_64__) || defined(_M_X64)
    libutil_push_str(vm, "x86_64", 6);
#elif defined(__i386__) || defined(_M_IX86)
    libutil_push_str(vm, "x86", 3);
#elif defined(__aarch64__) || defined(_M_ARM64)
    libutil_push_str(vm, "aarch64", 7);
#elif defined(__arm__) || defined(_M_ARM)
    libutil_push_str(vm, "arm", 3);
#else
    libutil_push_str(vm, "unknown", 7);
#endif
    return 1;
}

static int os_platform(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(_WIN32) || defined(_WIN64)
    libutil_push_str(vm, "windows", 7);
#elif defined(__APPLE__)
    libutil_push_str(vm, "darwin", 6);
#elif defined(__linux__)
    libutil_push_str(vm, "linux", 5);
#elif defined(__FreeBSD__)
    libutil_push_str(vm, "freebsd", 7);
#else
    libutil_push_str(vm, "unix", 4);
#endif
    return 1;
}

static int os_uptime(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        cando_vm_push(vm, cando_number((f64)si.uptime));
        return 1;
    }
#elif defined(_WIN32) || defined(_WIN64)
    cando_vm_push(vm, cando_number((f64)GetTickCount64() / 1000.0));
    return 1;
#endif
    cando_vm_push(vm, cando_number(0));
    return 1;
}

static int os_totalmem(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        cando_vm_push(vm, cando_number((f64)si.totalram * (f64)si.mem_unit));
        return 1;
    }
#elif defined(_WIN32) || defined(_WIN64)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        cando_vm_push(vm, cando_number((f64)ms.ullTotalPhys));
        return 1;
    }
#endif
    cando_vm_push(vm, cando_number(0));
    return 1;
}

static int os_freemem(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        cando_vm_push(vm, cando_number((f64)si.freeram * (f64)si.mem_unit));
        return 1;
    }
#elif defined(_WIN32) || defined(_WIN64)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        cando_vm_push(vm, cando_number((f64)ms.ullAvailPhys));
        return 1;
    }
#endif
    cando_vm_push(vm, cando_number(0));
    return 1;
}

static int os_cpus(CandoVM *vm, int argc, CandoValue *args)
{
    /* Return an array of { model, speed } objects.  Falls back to a
     * single placeholder if the system info isn't reachable. */
    (void)argc; (void)args;
    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr_obj = cando_bridge_resolve(vm, cando_as_handle(arr_val));

#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
        CdoObject *e = cdo_object_new();
        CdoString *km = cdo_string_intern("model", 5);
        CdoString *ks = cdo_string_intern("speed", 5);
        CdoString *m  = cdo_string_intern("unknown", 7);
        cdo_object_rawset(e, km, cdo_string_value(m), FIELD_NONE);
        cdo_object_rawset(e, ks, cdo_number(0), FIELD_NONE);
        cdo_string_release(km);
        cdo_string_release(ks);
        cdo_array_push(arr_obj, cdo_object_value(e));
    }
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1;
    for (long i = 0; i < n; i++) {
        CdoObject *e = cdo_object_new();
        CdoString *km = cdo_string_intern("model", 5);
        CdoString *ks = cdo_string_intern("speed", 5);
        CdoString *m  = cdo_string_intern("unknown", 7);
        cdo_object_rawset(e, km, cdo_string_value(m), FIELD_NONE);
        cdo_object_rawset(e, ks, cdo_number(0), FIELD_NONE);
        cdo_string_release(km);
        cdo_string_release(ks);
        cdo_array_push(arr_obj, cdo_object_value(e));
    }
#endif
    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry os_methods[] = {
    { "getenv",   os_getenv   },
    { "setenv",   os_setenv   },
    { "execute",  os_execute  },
    { "exit",     os_exit     },
    { "time",     os_time     },
    { "clock",    os_clock    },

    /* New: system info. */
    { "hostname", os_hostname },
    { "tmpdir",   os_tmpdir   },
    { "homedir",  os_homedir  },
    { "arch",     os_arch     },
    { "platform", os_platform },
    { "uptime",   os_uptime   },
    { "totalmem", os_totalmem },
    { "freemem",  os_freemem  },
    { "cpus",     os_cpus     },
};

void cando_lib_os_register(CandoVM *vm)
{
    CandoValue os_val = cando_bridge_new_object(vm);
    CdoObject *os_obj = cando_bridge_resolve(vm, cando_as_handle(os_val));

    libutil_register_methods(vm, os_obj, os_methods,
                             CANDO_ARRAY_LEN(os_methods));

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
