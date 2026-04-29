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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <GLFW/glfw3.h>
#if defined(_WIN32) || defined(_WIN64)
#  include <GL/gl.h>
#else
#  include <GL/gl.h>
#endif

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

#define WINDOW_MODULE_VERSION "0.0.5"

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

/* =========================================================================
 * Window slot table + command queue.
 *
 * Window state lives in a fixed-size static array indexed by the
 * `__window_slot` field stamped onto each script-side instance.  All
 * GLFW operations on that state must happen on the manager thread,
 * so callers post commands via g_pending_cmd and wait for the manager
 * to fulfill them.  A generation counter invalidates handles whose
 * slot has been recycled.
 * ===================================================================== */

#define WINDOW_MAX_SLOTS 32

typedef struct WindowSlot {
    int          alive;          /* 0 if free, 1 if in use            */
    int          generation;     /* incremented each time slot is recycled */
    GLFWwindow  *handle;         /* NULL until manager fulfills CREATE  */
    char         title[128];
    int          width;
    int          height;
} WindowSlot;

static WindowSlot g_slots[WINDOW_MAX_SLOTS];

typedef enum {
    CMD_NONE = 0,
    CMD_CREATE,
    CMD_DESTROY,
} CmdType;

typedef struct Command {
    CmdType      type;
    int          slot;           /* slot index for CREATE / DESTROY */
    /* CREATE inputs (read by manager): */
    char         title[128];
    int          width;
    int          height;
    /* outputs (written by manager): */
    int          ok;             /* 1 = success, 0 = failure         */
    char         err[160];
} Command;

static Command g_pending_cmd;
static int     g_cmd_present = 0;   /* 1 = command waiting for manager */
static int     g_cmd_done    = 0;   /* 1 = manager has finished it     */

/* Allocate a fresh slot.  Caller must hold g_mgr_mutex.  Returns -1 if
 * the table is full. */
static int slot_alloc_locked(void)
{
    for (int i = 0; i < WINDOW_MAX_SLOTS; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation++;
            g_slots[i].handle = NULL;
            g_slots[i].title[0] = '\0';
            g_slots[i].width  = 0;
            g_slots[i].height = 0;
            return i;
        }
    }
    return -1;
}

/* Manager-thread helper: drain the single pending command slot. */
static void mgr_drain_commands(void)
{
    WM_MUTEX_LOCK(&g_mgr_mutex);
    if (!g_cmd_present) {
        WM_MUTEX_UNLOCK(&g_mgr_mutex);
        return;
    }
    Command cmd = g_pending_cmd;
    g_cmd_present = 0;
    /* Release the mutex while we run GLFW so other threads can post the
     * next command (it will block on g_cmd_present, but won't deadlock
     * with anything we do here). */
    WM_MUTEX_UNLOCK(&g_mgr_mutex);

    switch (cmd.type) {
    case CMD_CREATE: {
        WindowSlot *s = &g_slots[cmd.slot];
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        GLFWwindow *w = glfwCreateWindow(cmd.width, cmd.height,
                                         cmd.title, NULL, NULL);
        if (!w) {
            cmd.ok = 0;
            const char *gd = NULL;
            int gc = glfwGetError(&gd);
            snprintf(cmd.err, sizeof(cmd.err),
                     "glfwCreateWindow failed (code=%d): %s",
                     gc, gd ? gd : "(no detail)");
        } else {
            s->handle = w;
            memcpy(s->title, cmd.title, sizeof(s->title));
            s->width  = cmd.width;
            s->height = cmd.height;
            cmd.ok    = 1;
        }
        break;
    }
    case CMD_DESTROY: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            glfwDestroyWindow(s->handle);
            s->handle = NULL;
        }
        s->alive = 0;
        cmd.ok   = 1;
        break;
    }
    default:
        cmd.ok = 0;
        snprintf(cmd.err, sizeof(cmd.err), "unknown command type %d",
                 (int)cmd.type);
        break;
    }

    WM_MUTEX_LOCK(&g_mgr_mutex);
    g_pending_cmd = cmd;     /* propagate ok / err back to the caller */
    g_cmd_done    = 1;
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);
}

/* Manager-thread helper: render one frame for every live window.
 * For the moment this is just clear-to-black + swap; the next chunk
 * will run user-supplied `w.draw` callbacks here.  Also reaps windows
 * the user closed via the close button. */
static void mgr_render_frame(void)
{
    for (int i = 0; i < WINDOW_MAX_SLOTS; i++) {
        WindowSlot *s = &g_slots[i];
        if (!s->alive || !s->handle) continue;

        /* Honour the close button.  Don't free the slot here -- the
         * script-side handle still holds a generation, and freeing
         * would race with mgr_drain_commands; just destroy the GLFW
         * window and mark the slot dormant.  isOpen() will then
         * return FALSE. */
        if (glfwWindowShouldClose(s->handle)) {
            glfwDestroyWindow(s->handle);
            s->handle = NULL;
            s->alive  = 0;
            continue;
        }

        glfwMakeContextCurrent(s->handle);

        int fbw, fbh;
        glfwGetFramebufferSize(s->handle, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            glViewport(0, 0, fbw, fbh);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        glfwSwapBuffers(s->handle);
        glfwMakeContextCurrent(NULL);
    }
}

/* Tear down every still-live window.  Called from the manager thread
 * during shutdown so we never leak GLFW state. */
static void mgr_destroy_all_windows(void)
{
    for (int i = 0; i < WINDOW_MAX_SLOTS; i++) {
        if (g_slots[i].alive && g_slots[i].handle) {
            glfwDestroyWindow(g_slots[i].handle);
            g_slots[i].handle = NULL;
            g_slots[i].alive  = 0;
        }
    }
}

/* Caller-side: post a command to the manager and wait for completion.
 * Must be called with the manager already running.  Returns the command
 * by value (with `ok` and `err` filled in). */
static Command mgr_post_command(Command cmd)
{
    WM_MUTEX_LOCK(&g_mgr_mutex);
    /* Wait if a previous command is still in flight. */
    while (g_cmd_present || g_cmd_done) {
        WM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    g_pending_cmd = cmd;
    g_cmd_present = 1;
    g_cmd_done    = 0;
    WM_MUTEX_UNLOCK(&g_mgr_mutex);

    /* Kick the manager out of glfwWaitEventsTimeout so it picks up the
     * command immediately. */
    glfwPostEmptyEvent();

    WM_MUTEX_LOCK(&g_mgr_mutex);
    while (!g_cmd_done) {
        WM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    Command result = g_pending_cmd;
    g_cmd_done = 0;
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);
    return result;
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

    /* Event loop -- drain commands, render every alive window, poll. */
    while (!atomic_load(&g_mgr_should_stop)) {
        mgr_drain_commands();
        mgr_render_frame();
        glfwWaitEventsTimeout(0.016); /* ~60 Hz wakeup */
    }

    /* Final drain so any in-flight DESTROY commands complete cleanly. */
    mgr_drain_commands();
    /* Close any windows the user forgot to close before exit. */
    mgr_destroy_all_windows();

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
 * Native helpers
 * ===================================================================== */

/* Fire a runtime error from a native function.  Mirrors sqlite_throw. */
static void window_throw(CandoVM *vm, const char *fmt, ...)
{
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cando_vm_error(vm, "%s", buf);
}

static void obj_set_number(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

static bool obj_get_number(CdoObject *obj, const char *key, f64 *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = v.as.number;
    return true;
}

#define WINDOW_SLOT_KEY "__window_slot"
#define WINDOW_GEN_KEY  "__window_gen"

/* Resolve a window-instance argument to a live slot.  Throws and
 * returns NULL if the argument is not a window or has been closed. */
static WindowSlot *resolve_window(CandoVM *vm, CandoValue val,
                                  const char *fn_name)
{
    if (!cando_is_object(val)) {
        window_throw(vm, "%s: expected window instance", fn_name);
        return NULL;
    }
    CdoObject *obj = cando_bridge_resolve(vm, val.as.handle);
    f64 fslot = -1.0, fgen = -1.0;
    if (!obj_get_number(obj, WINDOW_SLOT_KEY, &fslot) ||
        !obj_get_number(obj, WINDOW_GEN_KEY,  &fgen)) {
        window_throw(vm, "%s: not a window instance", fn_name);
        return NULL;
    }
    int idx = (int)fslot;
    int gen = (int)fgen;
    if (idx < 0 || idx >= WINDOW_MAX_SLOTS) {
        window_throw(vm, "%s: window slot out of range", fn_name);
        return NULL;
    }
    WindowSlot *s = &g_slots[idx];
    if (!s->alive || s->generation != gen) {
        window_throw(vm, "%s: window has been closed", fn_name);
        return NULL;
    }
    return s;
}

/* =========================================================================
 * Native: window._managerOk()  ->  bool
 * ===================================================================== */

static int native_window_manager_ok(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    int ok = ensure_manager();
    cando_vm_push(vm, cando_bool(ok != 0));
    return 1;
}

/* =========================================================================
 * Native: window.create(title, width, height) -> window
 *         window.create(opts)                  -> window
 * ===================================================================== */

static int native_window_create(CandoVM *vm, int argc, CandoValue *args)
{
    /* Defaults match LOVE2D's love.window.setMode without arguments. */
    char title[128]; snprintf(title, sizeof(title), "%s", "CanDo");
    int width  = 800;
    int height = 600;

    /* Two call shapes: positional or single-options-table. */
    if (argc >= 1 && cando_is_object(args[0])) {
        CdoObject *opts = cando_bridge_resolve(vm, args[0].as.handle);
        CdoString *k_title  = cdo_string_intern("title",  5);
        CdoString *k_width  = cdo_string_intern("width",  5);
        CdoString *k_height = cdo_string_intern("height", 6);
        CdoValue v;
        if (cdo_object_rawget(opts, k_title, &v) && v.tag == CDO_STRING &&
            v.as.string) {
            const char *s = v.as.string->data;
            u32 n = v.as.string->length;
            if (n >= sizeof(title)) n = sizeof(title) - 1;
            memcpy(title, s, n);
            title[n] = '\0';
        }
        if (cdo_object_rawget(opts, k_width, &v) && v.tag == CDO_NUMBER) {
            width = (int)v.as.number;
        }
        if (cdo_object_rawget(opts, k_height, &v) && v.tag == CDO_NUMBER) {
            height = (int)v.as.number;
        }
        cdo_string_release(k_title);
        cdo_string_release(k_width);
        cdo_string_release(k_height);
    } else {
        if (argc >= 1 && args[0].tag == CDO_STRING && args[0].as.string) {
            const char *s = args[0].as.string->data;
            u32 n = args[0].as.string->length;
            if (n >= sizeof(title)) n = sizeof(title) - 1;
            memcpy(title, s, n);
            title[n] = '\0';
        }
        if (argc >= 2 && args[1].tag == CDO_NUMBER) {
            width = (int)args[1].as.number;
        }
        if (argc >= 3 && args[2].tag == CDO_NUMBER) {
            height = (int)args[2].as.number;
        }
    }
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    if (!ensure_manager()) {
        window_throw(vm, "window.create: GLFW failed to initialise "
                     "(missing display or X server?)");
        return -1;
    }

    /* Allocate a slot before posting -- the manager fills in handle,
     * but the slot index must be stable so the script-side instance
     * can reference it. */
    WM_MUTEX_LOCK(&g_mgr_mutex);
    int slot = slot_alloc_locked();
    int generation = slot >= 0 ? g_slots[slot].generation : -1;
    WM_MUTEX_UNLOCK(&g_mgr_mutex);
    if (slot < 0) {
        window_throw(vm, "window.create: too many windows (max %d)",
                     WINDOW_MAX_SLOTS);
        return -1;
    }

    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_CREATE;
    cmd.slot   = slot;
    cmd.width  = width;
    cmd.height = height;
    memcpy(cmd.title, title, sizeof(cmd.title));

    Command result = mgr_post_command(cmd);
    if (!result.ok) {
        /* Free the slot so it can be retried.  Slot's generation
         * already advanced; nothing else to do. */
        WM_MUTEX_LOCK(&g_mgr_mutex);
        g_slots[slot].alive = 0;
        WM_MUTEX_UNLOCK(&g_mgr_mutex);
        window_throw(vm, "window.create: %s", result.err);
        return -1;
    }

    /* Build the script-side instance.  Stamp the slot index + generation
     * so subsequent method calls can locate the C-side state. */
    CandoValue inst_val = cando_bridge_new_object(vm);
    CdoObject *inst = cando_bridge_resolve(vm, inst_val.as.handle);

    obj_set_number(inst, WINDOW_SLOT_KEY, (f64)slot);
    obj_set_number(inst, WINDOW_GEN_KEY,  (f64)generation);
    obj_set_string(inst, "title", title, (u32)strlen(title));
    obj_set_number(inst, "width",  (f64)width);
    obj_set_number(inst, "height", (f64)height);

    /* Chain to the _meta.window prototype so colon methods resolve. */
    cando_lib_meta_attach(vm, inst, "window");

    cando_vm_push(vm, inst_val);
    return 1;
}

/* =========================================================================
 * Native: w:close()
 * ===================================================================== */

static int native_window_close(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.close: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.close");
    if (!s) return -1;

    int slot = (int)(s - g_slots);
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_DESTROY;
    cmd.slot = slot;
    (void)mgr_post_command(cmd);

    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Native: w:isOpen()
 * ===================================================================== */

static int native_window_is_open(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    f64 fslot = -1.0, fgen = -1.0;
    if (!obj_get_number(obj, WINDOW_SLOT_KEY, &fslot) ||
        !obj_get_number(obj, WINDOW_GEN_KEY,  &fgen)) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    int idx = (int)fslot;
    int gen = (int)fgen;
    bool open = (idx >= 0 && idx < WINDOW_MAX_SLOTS &&
                 g_slots[idx].alive && g_slots[idx].generation == gen);
    cando_vm_push(vm, cando_bool(open));
    return 1;
}

/* =========================================================================
 * Module entry point.
 * ===================================================================== */

CandoValue cando_module_init(CandoVM *vm)
{
    cando_lib_meta_register(vm);
    CdoObject *meta = cando_lib_meta_table(vm, "window");

    /* _meta.window methods: callable as w:close(), w:isOpen(), ... */
    cando_lib_meta_define(vm, meta, "close",  native_window_close);
    cando_lib_meta_define(vm, meta, "isOpen", native_window_is_open);

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   WINDOW_MODULE_VERSION,
                   (u32)sizeof(WINDOW_MODULE_VERSION) - 1);

    const char *gv = glfwGetVersionString();
    if (gv) obj_set_string(obj, "glfwVersion", gv, (u32)strlen(gv));

    libutil_set_method(vm, obj, "create",     native_window_create);
    libutil_set_method(vm, obj, "_managerOk", native_window_manager_ok);

    return tbl;
}
