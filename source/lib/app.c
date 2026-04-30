/*
 * lib/app.c -- Application lifecycle global for Cando.
 *
 * See app.h for the surface.  Backed by the VM's lifeline registry
 * (cando_vm_request_quit / cando_vm_quit_requested / ...).
 *
 * Must compile with gcc -std=c11.
 */

#include "app.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"

#include <stdlib.h>
#if defined(_WIN32) || defined(_WIN64)
#  include <process.h>
#  define cando_app_exit(code) _exit(code)
#else
#  include <unistd.h>
#  define cando_app_exit(code) _exit(code)
#endif

/* =========================================================================
 * app.quit([code])  -- request graceful shutdown.
 *
 * Sets the VM's quit-requested flag and stores the requested exit
 * code.  Subsystems that want to react (window module's manager loop,
 * future http accept loops, etc.) poll cando_vm_quit_requested and
 * release their lifelines so cando_vm_wait_all_lifelines can return.
 *
 * The script may keep running -- this call returns immediately.
 * ===================================================================== */

static int app_quit(CandoVM *vm, int argc, CandoValue *args)
{
    int code = (argc >= 1 && args[0].tag == CDO_NUMBER)
               ? (int)args[0].as.number : 0;
    cando_vm_request_quit(vm, code);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * app.exit([code])  -- hard stop.
 *
 * Equivalent to POSIX _exit() after setting the exit code.  Use only
 * when you cannot wait for cando_vm_wait_all_lifelines (typically
 * unnecessary -- prefer app.quit()).
 * ===================================================================== */

static int app_exit(CandoVM *vm, int argc, CandoValue *args)
{
    int code = (argc >= 1 && args[0].tag == CDO_NUMBER)
               ? (int)args[0].as.number : 0;
    cando_vm_request_quit(vm, code);
    /* Skip atexit hooks and stdio flushes -- there's no portable way
     * to guarantee a child render thread isn't mid-GL call.  exit(3)
     * could deadlock the way glfwTerminate does on X11. */
    cando_app_exit(code);
    return 0;  /* unreachable */
}

/* =========================================================================
 * app.isQuitting()  -- has app.quit() been called?
 * ===================================================================== */

static int app_is_quitting(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_bool(cando_vm_quit_requested(vm)));
    return 1;
}

/* =========================================================================
 * app.holds()  -- current lifeline count (number of subsystems
 * currently holding the process alive).
 * ===================================================================== */

static int app_holds(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)cando_vm_lifeline_count(vm)));
    return 1;
}

/* =========================================================================
 * app.exitCode([code])  -- get or set the process exit code.
 * ===================================================================== */

static int app_exit_code(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc >= 1 && args[0].tag == CDO_NUMBER) {
        cando_vm_set_exit_code(vm, (int)args[0].as.number);
    }
    cando_vm_push(vm, cando_number((f64)cando_vm_get_exit_code(vm)));
    return 1;
}

/* =========================================================================
 * Registration
 * ===================================================================== */

void cando_lib_app_register(CandoVM *vm)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, val.as.handle);

    libutil_set_method(vm, obj, "quit",       app_quit);
    libutil_set_method(vm, obj, "exit",       app_exit);
    libutil_set_method(vm, obj, "isQuitting", app_is_quitting);
    libutil_set_method(vm, obj, "holds",      app_holds);
    libutil_set_method(vm, obj, "exitCode",   app_exit_code);

    cando_vm_set_global(vm, "app", val, true);
}
