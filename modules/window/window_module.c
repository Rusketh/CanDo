/*
 * modules/window/window_module.c -- CanDo window module entry point.
 *
 * Loaded into a script with:
 *
 *     VAR window = include("./window.so");      // Linux / macOS
 *     VAR window = include("./window.dll");     // Windows
 *
 * This chunk lazily spawns the GLFW manager thread on module load.
 * GLFW is not thread-safe -- glfwInit, glfwCreateWindow, and
 * glfwPollEvents must all happen on a single, stable thread.  We pin
 * those calls to the manager thread for the whole life of the
 * process; window creation in subsequent chunks will post commands
 * to it.  An atexit hook signals the thread to stop and joins it.
 *
 * Public surface in this chunk:
 *   window.VERSION       string
 *   window.glfwVersion   string  (from glfwGetVersionString)
 *   window._managerOk()  bool    (true if glfwInit succeeded)
 *
 * Window creation (window.create), close / isOpen / setTitle methods
 * on `_meta.window`, and event dispatch follow in subsequent chunks.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"
#include "lib/meta.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <GLFW/glfw3.h>

/* Tiny mutex / cond / thread wrapper -- libcando does not export its
 * cando_os_* sync helpers so binary modules cannot link them.  Same
 * trick used by modules/sqlite/sqlite_module.c. */
#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>
   typedef CRITICAL_SECTION   wm_mutex_t;
   typedef CONDITION_VARIABLE wm_cond_t;
   typedef HANDLE             wm_thread_t;
#  define WM_MUTEX_INIT(m)    InitializeCriticalSection(m)
#  define WM_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#  define WM_MUTEX_LOCK(m)    EnterCriticalSection(m)
#  define WM_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#  define WM_COND_INIT(c)     InitializeConditionVariable(c)
#  define WM_COND_DESTROY(c)  ((void)0)
#  define WM_COND_WAIT(c,m)   SleepConditionVariableCS((c),(m), INFINITE)
#  define WM_COND_SIGNAL(c)   WakeConditionVariable(c)
#  define WM_COND_BROADCAST(c) WakeAllConditionVariable(c)
#else
#  include <pthread.h>
   typedef pthread_mutex_t wm_mutex_t;
   typedef pthread_cond_t  wm_cond_t;
   typedef pthread_t       wm_thread_t;
#  define WM_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define WM_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define WM_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#  define WM_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#  define WM_COND_INIT(c)     pthread_cond_init((c), NULL)
#  define WM_COND_DESTROY(c)  pthread_cond_destroy(c)
#  define WM_COND_WAIT(c,m)   pthread_cond_wait((c),(m))
#  define WM_COND_SIGNAL(c)   pthread_cond_signal(c)
#  define WM_COND_BROADCAST(c) pthread_cond_broadcast(c)
#endif

#define WINDOW_MODULE_VERSION "0.0.3"

/* =========================================================================
 * obj_set_* helpers (mirrors modules/sqlite).
 * ===================================================================== */

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

/* =========================================================================
 * GLFW manager thread.
 *
 * Started lazily on the first call that needs GLFW (currently only
 * `window._managerOk` for testing; `window.create` will call into it
 * in the next chunk).  Runs glfwInit then loops on
 * glfwWaitEventsTimeout until signalled to stop.
 *
 * Lifecycle states:
 *   STATE_UNSTARTED       no thread spawned yet
 *   STATE_STARTING        thread created, init in progress
 *   STATE_RUNNING         glfwInit succeeded, event loop is running
 *   STATE_INIT_FAILED     glfwInit returned 0; thread has exited
 *   STATE_STOPPING        atexit hook has signalled stop
 *   STATE_STOPPED         thread joined, glfwTerminate called
 * ===================================================================== */

typedef enum {
    MGR_UNSTARTED = 0,
    MGR_STARTING,
    MGR_RUNNING,
    MGR_INIT_FAILED,
    MGR_STOPPING,
    MGR_STOPPED,
} ManagerState;

static wm_mutex_t  g_mgr_mutex;
static wm_cond_t   g_mgr_cond;
static wm_thread_t g_mgr_thread;
static atomic_int  g_mgr_state    = MGR_UNSTARTED;
static atomic_int  g_mgr_should_stop = 0;
static int         g_sync_inited  = 0;
static int         g_atexit_registered = 0;

static void mgr_init_sync_once(void)
{
    /* The very first call to ensure_manager() initialises the mutex /
     * condvar.  Guarded by a separate atomic so we don't race on the
     * mutex itself before it exists. */
    static atomic_int once = 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(&once, &expected, 1)) {
        WM_MUTEX_INIT(&g_mgr_mutex);
        WM_COND_INIT(&g_mgr_cond);
        g_sync_inited = 1;
    } else {
        /* Spin until the winner finishes init.  Cheap (microseconds). */
        while (!g_sync_inited) { /* memory_order_acquire via volatile read */ }
    }
}

#if defined(_WIN32) || defined(_WIN64)
static unsigned __stdcall manager_thread_main(void *arg)
#else
static void *manager_thread_main(void *arg)
#endif
{
    (void)arg;

    /* glfwInit happens on the manager thread itself, never on the caller. */
    int ok = glfwInit();
    WM_MUTEX_LOCK(&g_mgr_mutex);
    if (!ok) {
        atomic_store(&g_mgr_state, MGR_INIT_FAILED);
        WM_COND_BROADCAST(&g_mgr_cond);
        WM_MUTEX_UNLOCK(&g_mgr_mutex);
#if defined(_WIN32) || defined(_WIN64)
        return 0;
#else
        return NULL;
#endif
    }
    atomic_store(&g_mgr_state, MGR_RUNNING);
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);

    /* Idle event loop.  Once windows exist (next chunk) this same loop
     * will service their commands and dispatch their input events. */
    while (!atomic_load(&g_mgr_should_stop)) {
        glfwWaitEventsTimeout(0.05);
    }

    glfwTerminate();
    atomic_store(&g_mgr_state, MGR_STOPPED);
    WM_MUTEX_LOCK(&g_mgr_mutex);
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);

#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

/* Stop + join the manager thread.  Idempotent.  Called from atexit. */
static void mgr_shutdown(void)
{
    int state = atomic_load(&g_mgr_state);
    if (state == MGR_UNSTARTED || state == MGR_STOPPED ||
        state == MGR_INIT_FAILED) return;

    atomic_store(&g_mgr_should_stop, 1);
    /* Kick the manager out of glfwWaitEventsTimeout. */
    glfwPostEmptyEvent();

#if defined(_WIN32) || defined(_WIN64)
    WaitForSingleObject(g_mgr_thread, INFINITE);
    CloseHandle(g_mgr_thread);
#else
    pthread_join(g_mgr_thread, NULL);
#endif
}

/* Lazy start.  Returns true if the manager thread is in MGR_RUNNING.
 * Safe to call from any thread; idempotent. */
static int ensure_manager(void)
{
    mgr_init_sync_once();

    int state = atomic_load(&g_mgr_state);
    if (state == MGR_RUNNING)     return 1;
    if (state == MGR_INIT_FAILED) return 0;
    if (state == MGR_STOPPING || state == MGR_STOPPED) return 0;

    WM_MUTEX_LOCK(&g_mgr_mutex);
    /* Re-check under lock. */
    state = atomic_load(&g_mgr_state);
    if (state == MGR_UNSTARTED) {
        atomic_store(&g_mgr_state, MGR_STARTING);
#if defined(_WIN32) || defined(_WIN64)
        g_mgr_thread = (HANDLE)_beginthreadex(
            NULL, 0, manager_thread_main, NULL, 0, NULL);
        if (!g_mgr_thread) {
            atomic_store(&g_mgr_state, MGR_INIT_FAILED);
            WM_MUTEX_UNLOCK(&g_mgr_mutex);
            return 0;
        }
#else
        if (pthread_create(&g_mgr_thread, NULL, manager_thread_main, NULL)
            != 0) {
            atomic_store(&g_mgr_state, MGR_INIT_FAILED);
            WM_MUTEX_UNLOCK(&g_mgr_mutex);
            return 0;
        }
#endif
        if (!g_atexit_registered) {
            atexit(mgr_shutdown);
            g_atexit_registered = 1;
        }
    }
    while ((state = atomic_load(&g_mgr_state)) == MGR_STARTING) {
        WM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    WM_MUTEX_UNLOCK(&g_mgr_mutex);
    return state == MGR_RUNNING;
}

/* =========================================================================
 * Native: window._managerOk()  ->  bool
 * Diagnostic: triggers manager thread spawn (idempotent) and reports
 * whether glfwInit succeeded.  Tests use this to confirm GLFW boots
 * before window.create is wired up.
 * ===================================================================== */

static int native_window_manager_ok(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    int ok = ensure_manager();
    cando_vm_push(vm, cando_bool(ok != 0));
    return 1;
}

/* =========================================================================
 * Module entry point.
 * ===================================================================== */

CandoValue cando_module_init(CandoVM *vm)
{
    cando_lib_meta_register(vm);
    (void)cando_lib_meta_table(vm, "window");

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   WINDOW_MODULE_VERSION,
                   (u32)sizeof(WINDOW_MODULE_VERSION) - 1);

    const char *gv = glfwGetVersionString();
    if (gv) obj_set_string(obj, "glfwVersion", gv, (u32)strlen(gv));

    libutil_set_method(vm, obj, "_managerOk", native_window_manager_ok);

    return tbl;
}
