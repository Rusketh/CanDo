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
#if !(defined(_WIN32) || defined(_WIN64))
#  include <time.h>
#endif

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

#define WINDOW_MODULE_VERSION "0.0.11"

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
    int          has_lifeline;   /* 1 while we're holding a VM lifeline */
    /* Retained handle to the script-side instance.  Holds a reference
     * count on the CdoObject so the manager thread can call user
     * methods (`w.draw`, `w.keypressed`, ...) without the GC reaping
     * the instance behind our back.  Set in window.create after the
     * GLFW window is built; released by slot_teardown. */
    CandoValue   inst_val;
    int          inst_val_held;
    /* High-resolution timestamp of the last frame for dt computation. */
    double       last_frame_time;
    /* Per-window event queue.  Single-producer (GLFW callbacks fire on
     * the manager thread inside glfwPollEvents) and single-consumer
     * (the manager dispatches in mgr_render_frame), so no locking is
     * needed.  If the queue overflows we drop the new event rather
     * than the old one -- a runaway flood (e.g. 1000 mouse_moved/sec)
     * shouldn't starve out an earlier keypressed. */
    struct Event *events;        /* heap-allocated circular buffer    */
    int          ev_capacity;
    int          ev_head;        /* read index                        */
    int          ev_tail;        /* write index (head==tail => empty) */
} WindowSlot;

static WindowSlot g_slots[WINDOW_MAX_SLOTS];

/* Captured on first window.create and used by any thread that needs to
 * touch the VM lifeline counter.  All windows in a process share the
 * same root VM, so this is safe to cache. */
static CandoVM   *g_root_vm = NULL;

/* Child VM the manager thread uses to invoke user-supplied callbacks
 * (w.draw, w.update, w.keypressed, ...).  Initialised lazily on the
 * first successful window.create so it can chain off the captured
 * root VM.  Per cando_vm_init_child semantics it shares globals,
 * handles, strings, and the lifeline registry with the root VM. */
static CandoVM   g_dispatch_vm;
static int       g_dispatch_vm_inited = 0;

/* Forward decl -- definition lives in the event-queue section below. */
static void slot_clear_event_queue(WindowSlot *s);

/* Slot teardown shared by CMD_DESTROY, the close-button reaper, and the
 * shutdown sweep.  Closes the GLFW window if still open, drops the
 * retained instance handle, and releases any lifeline the slot was
 * holding.  Caller is responsible for the GL context being safe to
 * destroy on this thread. */
static void slot_teardown(WindowSlot *s)
{
    if (!s) return;
    if (s->handle) {
        glfwDestroyWindow(s->handle);
        s->handle = NULL;
    }
    if (s->inst_val_held) {
        cando_value_release(s->inst_val);
        s->inst_val_held = 0;
    }
    if (s->has_lifeline && g_root_vm) {
        cando_vm_lifeline_release(g_root_vm);
        s->has_lifeline = 0;
    }
    /* Reset queue indices but keep the buffer allocated so slot
     * recycling avoids the malloc churn.  free() at shutdown is
     * skipped -- the OS reclaims everything. */
    slot_clear_event_queue(s);
    s->alive = 0;
}

/* =========================================================================
 * Callback dispatch -- look up `name` on the slot's instance, call it
 * with `self` already pushed as args[0].  Silent no-op if the field is
 * missing or not callable.  Runs on the manager thread, in the child VM.
 * ===================================================================== */

static void dispatch_call(WindowSlot *s, const char *name,
                          CandoValue *extra_args, u32 extra_argc)
{
    if (!g_dispatch_vm_inited || !s->inst_val_held) return;

    CdoObject *inst = cando_bridge_resolve(&g_dispatch_vm, s->inst_val.as.handle);
    if (!inst) {
        fprintf(stderr, "[wnd] dispatch %s: no inst\n", name);
        return;
    }

    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   field_cdo;
    bool       have = cdo_object_get(inst, key, &field_cdo);
    cdo_string_release(key);
    if (!have) return;
    /* Accept any callable tag: a script function (CDO_FUNCTION), a
     * native function (CDO_NATIVE), or a plain object whose kind is
     * OBJ_FUNCTION (some paths produce CDO_OBJECT-tagged closures). */
    bool callable = (field_cdo.tag == CDO_FUNCTION) ||
                    (field_cdo.tag == CDO_NATIVE)   ||
                    (field_cdo.tag == CDO_OBJECT &&
                     field_cdo.as.object &&
                     field_cdo.as.object->kind == OBJ_FUNCTION);
    if (!callable) return;

    /* Convert CdoValue -> CandoValue (allocates / re-uses a handle). */
    CandoValue fn = cando_bridge_to_cando(&g_dispatch_vm, field_cdo);

    /* Build (self, ...extra_args) and dispatch. */
    enum { MAX_ARGS = 8 };
    if (extra_argc > MAX_ARGS - 1) extra_argc = MAX_ARGS - 1;
    CandoValue argv[MAX_ARGS];
    argv[0] = s->inst_val;
    for (u32 i = 0; i < extra_argc; i++) argv[1 + i] = extra_args[i];

    /* The dispatch VM is single-threaded (only the manager touches it),
     * so no locking is needed around this call. */
    cando_vm_call_value(&g_dispatch_vm, fn, argv, 1 + extra_argc);
    /* Discard any return values left on the child VM's stack and
     * clear any per-call error state so the next dispatch starts
     * clean.  Don't release `fn` here: cando_bridge_to_cando produced
     * a borrowed handle that the parent VM owns through the instance
     * field, so releasing would over-decref. */
    g_dispatch_vm.stack_top   = g_dispatch_vm.stack;
    g_dispatch_vm.frame_count = 0;
    g_dispatch_vm.has_error   = false;
    g_dispatch_vm.error_val_count = 0;
}

typedef enum {
    CMD_NONE = 0,
    CMD_CREATE,
    CMD_DESTROY,
    CMD_SET_TITLE,
    CMD_SET_SIZE,
    CMD_GET_SIZE,
    CMD_SET_POSITION,
    CMD_GET_POSITION,
    CMD_FOCUS,
    CMD_SET_VISIBLE,
    CMD_GET_ATTRIB,        /* read a GLFW_* window attribute */
    CMD_GET_MOUSE,
    CMD_GET_DPI_SCALE,
    CMD_GET_FRAMEBUFFER,
    CMD_SET_VSYNC,
} CmdType;

typedef struct Command {
    CmdType      type;
    int          slot;
    /* General-purpose inputs (semantics depend on type): */
    char         title[128];
    int          width;
    int          height;
    int          ix;             /* attrib id, vsync int, etc.       */
    int          iy;
    /* outputs: */
    int          ok;
    char         err[160];
    int          ox;             /* int outputs (size, pos, attribs) */
    int          oy;
    double       fx;             /* float outputs (mouse, scale)     */
    double       fy;
} Command;

/* Command-queue state machine.  Each post is a 3-step round-trip:
 *
 *   IDLE  -- caller-side: post a request, transition to PENDING
 *   PENDING -- manager-side: pick up the request, run it, transition to RESULT
 *   RESULT -- caller-side: collect the result, transition back to IDLE
 *
 * The manager holds the slot at PENDING until its work is finished; this
 * blocks a second caller from clobbering the in-flight command. */
typedef enum { CMD_IDLE = 0, CMD_PENDING, CMD_RESULT } CmdState;

static Command  g_pending_cmd;
static CmdState g_cmd_state = CMD_IDLE;

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

/* =========================================================================
 * Event queue + GLFW input callbacks.
 *
 * GLFW callbacks fire on the manager thread (inside glfwPollEvents)
 * and enqueue Event records into the destination slot's ring buffer.
 * mgr_render_frame drains the queue at the start of every frame and
 * dispatches each event to the matching user-defined callback (LOVE-
 * style names: keypressed, keyreleased, textinput, mousepressed, ...)
 * via the dispatch VM.
 * ===================================================================== */

typedef enum {
    EV_KEY_PRESS = 0,
    EV_KEY_RELEASE,
    EV_TEXT,
    EV_MOUSE_PRESS,
    EV_MOUSE_RELEASE,
    EV_MOUSE_MOVE,
    EV_WHEEL,
    EV_RESIZE,
    EV_FOCUS,
} EvType;

typedef struct Event {
    EvType type;
    int    i0, i1, i2;     /* general-purpose int args */
    double d0, d1;         /* general-purpose double args */
    char   text[8];        /* UTF-8 codepoint for EV_TEXT, NUL terminated */
} Event;

#define WINDOW_EV_QUEUE_CAP 128

static void slot_init_event_queue(WindowSlot *s)
{
    if (s->events) return;  /* already allocated, slot recycle */
    s->events     = (Event *)calloc(WINDOW_EV_QUEUE_CAP, sizeof(Event));
    s->ev_capacity = WINDOW_EV_QUEUE_CAP;
    s->ev_head    = 0;
    s->ev_tail    = 0;
}

static void slot_clear_event_queue(WindowSlot *s)
{
    if (!s->events) return;
    s->ev_head = 0;
    s->ev_tail = 0;
}

static void event_push(WindowSlot *s, Event ev)
{
    if (!s || !s->events) return;
    int next = (s->ev_tail + 1) % s->ev_capacity;
    if (next == s->ev_head) return;  /* queue full -- drop the new event */
    s->events[s->ev_tail] = ev;
    s->ev_tail = next;
}

/* Resolve a GLFWwindow* back to its WindowSlot via the user pointer
 * we set at create time.  Returns NULL if the slot has been torn down
 * underneath us (shouldn't happen: GLFW callbacks fire only during
 * glfwPollEvents on the manager thread, and slot_teardown also runs
 * on that thread). */
static WindowSlot *slot_from_glfw(GLFWwindow *w)
{
    if (!w) return NULL;
    return (WindowSlot *)glfwGetWindowUserPointer(w);
}

/* Encode a Unicode codepoint as UTF-8 into `out` (max 4 bytes + NUL).
 * Used by EV_TEXT so user code receives strings, like LOVE's love.textinput. */
static void utf8_encode(unsigned cp, char out[8])
{
    int n = 0;
    if (cp < 0x80) {
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out[n++] = (char)(0xF0 | (cp >> 18));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
}

/* GLFW callbacks ---------------------------------------------------------- */

static void cb_key(GLFWwindow *w, int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    Event ev = (Event){0};
    if (action == GLFW_RELEASE) {
        ev.type = EV_KEY_RELEASE;
        ev.i0   = key;
    } else {
        ev.type = EV_KEY_PRESS;
        ev.i0   = key;
        ev.i1   = (action == GLFW_REPEAT) ? 1 : 0;
    }
    event_push(s, ev);
}

static void cb_char(GLFWwindow *w, unsigned int codepoint)
{
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    Event ev = (Event){0};
    ev.type = EV_TEXT;
    utf8_encode(codepoint, ev.text);
    event_push(s, ev);
}

static void cb_mouse_button(GLFWwindow *w, int button, int action, int mods)
{
    (void)mods;
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(w, &cx, &cy);
    Event ev = (Event){0};
    ev.type = (action == GLFW_PRESS) ? EV_MOUSE_PRESS : EV_MOUSE_RELEASE;
    ev.i0   = button;
    ev.d0   = cx;
    ev.d1   = cy;
    event_push(s, ev);
}

static void cb_cursor_pos(GLFWwindow *w, double xpos, double ypos)
{
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    Event ev = (Event){0};
    ev.type = EV_MOUSE_MOVE;
    ev.d0   = xpos;
    ev.d1   = ypos;
    event_push(s, ev);
}

static void cb_scroll(GLFWwindow *w, double dx, double dy)
{
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    Event ev = (Event){0};
    ev.type = EV_WHEEL;
    ev.d0   = dx;
    ev.d1   = dy;
    event_push(s, ev);
}

static void cb_framebuffer_size(GLFWwindow *w, int width, int height)
{
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    s->width  = width;
    s->height = height;
    Event ev = (Event){0};
    ev.type = EV_RESIZE;
    ev.i0   = width;
    ev.i1   = height;
    event_push(s, ev);
}

static void cb_window_focus(GLFWwindow *w, int focused)
{
    WindowSlot *s = slot_from_glfw(w);
    if (!s) return;
    Event ev = (Event){0};
    ev.type = EV_FOCUS;
    ev.i0   = focused ? 1 : 0;
    event_push(s, ev);
}

/* Wire up every callback on a freshly-created window.  Runs on the
 * manager thread during CMD_CREATE. */
static void slot_install_callbacks(WindowSlot *s)
{
    if (!s || !s->handle) return;
    glfwSetWindowUserPointer(s->handle, s);
    glfwSetKeyCallback             (s->handle, cb_key);
    glfwSetCharCallback            (s->handle, cb_char);
    glfwSetMouseButtonCallback     (s->handle, cb_mouse_button);
    glfwSetCursorPosCallback       (s->handle, cb_cursor_pos);
    glfwSetScrollCallback          (s->handle, cb_scroll);
    glfwSetFramebufferSizeCallback (s->handle, cb_framebuffer_size);
    glfwSetWindowFocusCallback     (s->handle, cb_window_focus);
}

/* Dispatch every queued event to the matching user-defined callback.
 * Called from mgr_render_frame at the start of each frame, before
 * w.update / w.draw fire. */
static void slot_dispatch_events(WindowSlot *s)
{
    while (s->ev_head != s->ev_tail) {
        Event ev = s->events[s->ev_head];
        s->ev_head = (s->ev_head + 1) % s->ev_capacity;
        if (!s->alive) break;  /* w:close() during dispatch */

        switch (ev.type) {
        case EV_KEY_PRESS: {
            CandoValue args[2] = { cando_number((f64)ev.i0),
                                    cando_bool(ev.i1 != 0) };
            dispatch_call(s, "keypressed", args, 2);
            break;
        }
        case EV_KEY_RELEASE: {
            CandoValue args[1] = { cando_number((f64)ev.i0) };
            dispatch_call(s, "keyreleased", args, 1);
            break;
        }
        case EV_TEXT: {
            CandoValue args[1] = { cando_string_value(
                cando_string_new(ev.text, (u32)strlen(ev.text))) };
            dispatch_call(s, "textinput", args, 1);
            cando_value_release(args[0]);
            break;
        }
        case EV_MOUSE_PRESS: {
            CandoValue args[3] = { cando_number(ev.d0),
                                    cando_number(ev.d1),
                                    cando_number((f64)ev.i0) };
            dispatch_call(s, "mousepressed", args, 3);
            break;
        }
        case EV_MOUSE_RELEASE: {
            CandoValue args[3] = { cando_number(ev.d0),
                                    cando_number(ev.d1),
                                    cando_number((f64)ev.i0) };
            dispatch_call(s, "mousereleased", args, 3);
            break;
        }
        case EV_MOUSE_MOVE: {
            CandoValue args[2] = { cando_number(ev.d0),
                                    cando_number(ev.d1) };
            dispatch_call(s, "mousemoved", args, 2);
            break;
        }
        case EV_WHEEL: {
            CandoValue args[2] = { cando_number(ev.d0),
                                    cando_number(ev.d1) };
            dispatch_call(s, "wheelmoved", args, 2);
            break;
        }
        case EV_RESIZE: {
            CandoValue args[2] = { cando_number((f64)ev.i0),
                                    cando_number((f64)ev.i1) };
            dispatch_call(s, "resize", args, 2);
            break;
        }
        case EV_FOCUS: {
            CandoValue args[1] = { cando_bool(ev.i0 != 0) };
            dispatch_call(s, "focus", args, 1);
            break;
        }
        }
    }
}

/* Manager-thread helper: drain the single pending command slot. */
static void mgr_drain_commands(void)
{
    WM_MUTEX_LOCK(&g_mgr_mutex);
    if (g_cmd_state != CMD_PENDING) {
        WM_MUTEX_UNLOCK(&g_mgr_mutex);
        return;
    }
    Command cmd = g_pending_cmd;
    /* Stay at CMD_PENDING through the GLFW work so a fast caller can't
     * post a second command before this one's result is consumed. */
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
            /* Disable vsync by default so the manager's render loop
             * doesn't block on a refresh that may not exist (xvfb has
             * no real vblank).  Users opt back in via w:setVSync(true). */
            glfwMakeContextCurrent(w);
            glfwSwapInterval(0);
            glfwMakeContextCurrent(NULL);

            /* Clear any should-close flag set at construction (xvfb
             * sometimes leaves one set when there is no WM). */
            glfwSetWindowShouldClose(w, GLFW_FALSE);

            s->handle = w;
            memcpy(s->title, cmd.title, sizeof(s->title));
            s->width  = cmd.width;
            s->height = cmd.height;

            /* Set up the per-slot event queue and wire up GLFW input
             * callbacks so user-defined keypressed / mousemoved /
             * etc. fire on subsequent frames. */
            slot_init_event_queue(s);
            slot_clear_event_queue(s);
            slot_install_callbacks(s);

            cmd.ok    = 1;
        }
        break;
    }
    case CMD_DESTROY: {
        slot_teardown(&g_slots[cmd.slot]);
        cmd.ok = 1;
        break;
    }
    case CMD_SET_TITLE: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            glfwSetWindowTitle(s->handle, cmd.title);
            memcpy(s->title, cmd.title, sizeof(s->title));
        }
        cmd.ok = 1;
        break;
    }
    case CMD_SET_SIZE: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            glfwSetWindowSize(s->handle, cmd.width, cmd.height);
            s->width  = cmd.width;
            s->height = cmd.height;
        }
        cmd.ok = 1;
        break;
    }
    case CMD_GET_SIZE: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            int w = 0, h = 0;
            glfwGetWindowSize(s->handle, &w, &h);
            cmd.ox = w; cmd.oy = h;
            s->width  = w;
            s->height = h;
        } else {
            cmd.ox = s->width; cmd.oy = s->height;
        }
        cmd.ok = 1;
        break;
    }
    case CMD_SET_POSITION: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) glfwSetWindowPos(s->handle, cmd.ix, cmd.iy);
        cmd.ok = 1;
        break;
    }
    case CMD_GET_POSITION: {
        WindowSlot *s = &g_slots[cmd.slot];
        int x = 0, y = 0;
        if (s->handle) glfwGetWindowPos(s->handle, &x, &y);
        cmd.ox = x; cmd.oy = y;
        cmd.ok = 1;
        break;
    }
    case CMD_FOCUS: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) glfwFocusWindow(s->handle);
        cmd.ok = 1;
        break;
    }
    case CMD_SET_VISIBLE: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            if (cmd.ix) glfwShowWindow(s->handle);
            else        glfwHideWindow(s->handle);
        }
        cmd.ok = 1;
        break;
    }
    case CMD_GET_ATTRIB: {
        WindowSlot *s = &g_slots[cmd.slot];
        cmd.ox = s->handle ? glfwGetWindowAttrib(s->handle, cmd.ix) : 0;
        cmd.ok = 1;
        break;
    }
    case CMD_GET_MOUSE: {
        WindowSlot *s = &g_slots[cmd.slot];
        double x = 0.0, y = 0.0;
        if (s->handle) glfwGetCursorPos(s->handle, &x, &y);
        cmd.fx = x; cmd.fy = y;
        cmd.ok = 1;
        break;
    }
    case CMD_GET_DPI_SCALE: {
        WindowSlot *s = &g_slots[cmd.slot];
        float sx = 1.0f, sy = 1.0f;
        if (s->handle) glfwGetWindowContentScale(s->handle, &sx, &sy);
        cmd.fx = sx; cmd.fy = sy;
        cmd.ok = 1;
        break;
    }
    case CMD_GET_FRAMEBUFFER: {
        WindowSlot *s = &g_slots[cmd.slot];
        int fbw = 0, fbh = 0;
        if (s->handle) glfwGetFramebufferSize(s->handle, &fbw, &fbh);
        cmd.ox = fbw; cmd.oy = fbh;
        cmd.ok = 1;
        break;
    }
    case CMD_SET_VSYNC: {
        WindowSlot *s = &g_slots[cmd.slot];
        if (s->handle) {
            glfwMakeContextCurrent(s->handle);
            glfwSwapInterval(cmd.ix);
            glfwMakeContextCurrent(NULL);
        }
        cmd.ok = 1;
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
    g_cmd_state   = CMD_RESULT;
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);
}

/* Manager-thread helper: drive one frame for every live window.
 *
 *   1. Reap windows whose close button was pressed.
 *   2. Compute dt, dispatch w.update(dt).
 *   3. Make the window's GL context current, clear, dispatch w.draw(),
 *      glfwSwapBuffers.
 *
 * GLFW input callbacks fire during glfwPollEvents (called by the
 * manager loop after this); the next chunk extends mgr_render_frame
 * to drain those events here too. */
static void mgr_render_frame(void)
{
    double now = glfwGetTime();

    for (int i = 0; i < WINDOW_MAX_SLOTS; i++) {
        WindowSlot *s = &g_slots[i];
        if (!s->alive || !s->handle) continue;

        /* Note: we deliberately do NOT auto-tear-down on
         * glfwWindowShouldClose yet.  The close-button -> w.quit ->
         * w:close() chain is wired up alongside the GLFW input
         * callbacks in the next chunk.  Keeping this off avoids xvfb
         * spuriously reporting should-close at startup when there is
         * no WM to handle WM_PROTOCOLS / WM_DELETE_WINDOW. */
        glfwSetWindowShouldClose(s->handle, GLFW_FALSE);

        double dt = (s->last_frame_time > 0.0)
                    ? (now - s->last_frame_time)
                    : 0.0;
        s->last_frame_time = now;

        /* Drain queued GLFW input events first so the user's update()
         * sees current state and can react to fresh keys / clicks. */
        slot_dispatch_events(s);
        if (!s->alive || !s->handle) continue;

        /* update(dt) -- runs before any GL work so user code can mutate
         * state for `draw` to render. */
        if (g_dispatch_vm_inited && s->inst_val_held) {
            CandoValue dt_arg = cando_number(dt);
            dispatch_call(s, "update", &dt_arg, 1);
        }

        glfwMakeContextCurrent(s->handle);

        int fbw, fbh;
        glfwGetFramebufferSize(s->handle, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            glViewport(0, 0, fbw, fbh);
            /* Set up an LOVE-style coordinate system: pixels, origin at
             * top-left, Y axis pointing down.  This lets the draw module's
             * primitives accept the same numbers a user would expect. */
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, (double)fbw, (double)fbh, 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            /* Sane default raster state -- the draw module assumes these
             * are set when its primitives run inside a `draw` callback. */
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        /* draw() -- user runs draw.* primitives here (when the draw
         * module lands).  Until then this is a clean place to call
         * any direct GL the user wants. */
        if (g_dispatch_vm_inited && s->inst_val_held) {
            dispatch_call(s, "draw", NULL, 0);
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
        slot_teardown(&g_slots[i]);
    }
}

/* Caller-side: post a command to the manager and wait for completion.
 * Must be called with the manager already running.  Returns the command
 * by value (with `ok` and `err` filled in). */
static Command mgr_post_command(Command cmd)
{
    WM_MUTEX_LOCK(&g_mgr_mutex);
    while (g_cmd_state != CMD_IDLE) {
        WM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    g_pending_cmd = cmd;
    g_cmd_state   = CMD_PENDING;
    WM_COND_BROADCAST(&g_mgr_cond);
    WM_MUTEX_UNLOCK(&g_mgr_mutex);

    /* Kick the manager out of glfwWaitEventsTimeout so it picks up the
     * command immediately. */
    glfwPostEmptyEvent();

    WM_MUTEX_LOCK(&g_mgr_mutex);
    while (g_cmd_state != CMD_RESULT) {
        WM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    Command result = g_pending_cmd;
    g_cmd_state    = CMD_IDLE;
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

    /* Event loop -- drain commands, render every alive window, poll.
     *
     * We use glfwPollEvents (non-blocking) + a short sleep instead of
     * glfwWaitEventsTimeout because the latter does not reliably wake
     * on glfwPostEmptyEvent when no windows remain (observed on X11),
     * which strands the manager thread inside an unwakeable wait at
     * shutdown.  Polling also keeps the shutdown latency bounded by
     * the sleep interval. */
    while (!atomic_load(&g_mgr_should_stop)) {
        mgr_drain_commands();
        /* Honour app.quit() -- once requested, tear down every alive
         * window so their lifelines release and the script's
         * cando_vm_wait_all_lifelines can return.  Idempotent: once
         * a slot is dead this is a no-op. */
        if (g_root_vm && cando_vm_quit_requested(g_root_vm)) {
            for (int i = 0; i < WINDOW_MAX_SLOTS; i++) {
                if (g_slots[i].alive) slot_teardown(&g_slots[i]);
            }
        }
        mgr_render_frame();
        glfwPollEvents();
#if defined(_WIN32) || defined(_WIN64)
        Sleep(8);
#else
        struct timespec ts = { 0, 8 * 1000 * 1000 };
        nanosleep(&ts, NULL);
#endif
    }

    /* Final drain so any in-flight DESTROY commands complete cleanly. */
    mgr_drain_commands();
    /* Close any windows the user forgot to close before exit. */
    mgr_destroy_all_windows();

    /* Skip glfwTerminate -- on Linux/X11, especially under xvfb, it has
     * been observed to deadlock during atexit when other libraries (like
     * libcando's cleanup running first) have already torn down state
     * GLFW reaches into.  All windows have been destroyed and the OS
     * reclaims the X connection on process exit, so skipping the
     * orderly terminate is safe in practice. */
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

    /* Capture the VM the first time we're called.  All windows in a
     * process share the same root VM, so a single reference is enough
     * for slot_teardown to release lifelines from any thread. */
    if (!g_root_vm) g_root_vm = vm;

    /* Lazily set up the child VM the manager thread uses for user
     * callbacks.  This must happen on the script thread, before any
     * window exists, so that cando_vm_init_child sees a fully-set-up
     * parent.  After this point, the manager may call
     * cando_vm_call_value(&g_dispatch_vm, ...) from any frame. */
    if (!g_dispatch_vm_inited) {
        cando_vm_init_child(&g_dispatch_vm, vm);
        g_dispatch_vm_inited = 1;
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

    /* Pin the VM so the script can `return` immediately and the process
     * stays alive until the user (or the close button) tears the
     * window down.  slot_teardown releases the matching lifeline. */
    cando_vm_lifeline_acquire(vm, "window");
    g_slots[slot].has_lifeline = 1;

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

    /* Retain a reference for the manager thread so user callbacks
     * (dispatched via the child VM) can find this instance. */
    g_slots[slot].inst_val      = cando_value_copy(inst_val);
    g_slots[slot].inst_val_held = 1;

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
 * _meta.window accessor methods.
 * ===================================================================== */

/* Helpers ----------------------------------------------------------------- */

/* Update an existing string field on the script-side instance. */
static void inst_update_string(CdoObject *inst, const char *key,
                               const char *data, u32 len)
{
    obj_set_string(inst, key, data, len);
}

static void inst_update_number(CdoObject *inst, const char *key, f64 value)
{
    obj_set_number(inst, key, value);
}

/* Common epilogue: post `cmd` to the manager and throw on failure. */
static int post_or_throw(CandoVM *vm, Command cmd, const char *fn,
                         Command *out)
{
    Command r = mgr_post_command(cmd);
    if (!r.ok) {
        window_throw(vm, "%s: %s", fn,
                     r.err[0] ? r.err : "manager command failed");
        return 0;
    }
    if (out) *out = r;
    return 1;
}

/* w:setTitle(str) ---------------------------------------------------------- */

static int native_window_set_title(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        window_throw(vm, "window.setTitle: (window, str) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.setTitle");
    if (!s) return -1;
    if (args[1].tag != CDO_STRING || !args[1].as.string) {
        window_throw(vm, "window.setTitle: title must be a string");
        return -1;
    }
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_TITLE;
    cmd.slot = (int)(s - g_slots);
    u32 n = args[1].as.string->length;
    if (n >= sizeof(cmd.title)) n = sizeof(cmd.title) - 1;
    memcpy(cmd.title, args[1].as.string->data, n);
    cmd.title[n] = '\0';
    if (!post_or_throw(vm, cmd, "window.setTitle", NULL)) return -1;

    /* Mirror onto the script-side instance so `w.title` stays current. */
    CdoObject *inst = cando_bridge_resolve(vm, args[0].as.handle);
    inst_update_string(inst, "title", cmd.title, (u32)strlen(cmd.title));

    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* w:getTitle() ------------------------------------------------------------- */

static int native_window_get_title(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getTitle: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getTitle");
    if (!s) return -1;
    libutil_push_cstr(vm, s->title);
    return 1;
}

/* w:setSize(w, h) ---------------------------------------------------------- */

static int native_window_set_size(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3 || args[1].tag != CDO_NUMBER || args[2].tag != CDO_NUMBER) {
        window_throw(vm, "window.setSize: (window, width, height) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.setSize");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SIZE;
    cmd.slot   = (int)(s - g_slots);
    cmd.width  = (int)args[1].as.number;
    cmd.height = (int)args[2].as.number;
    if (cmd.width  < 1) cmd.width  = 1;
    if (cmd.height < 1) cmd.height = 1;
    if (!post_or_throw(vm, cmd, "window.setSize", NULL)) return -1;

    CdoObject *inst = cando_bridge_resolve(vm, args[0].as.handle);
    inst_update_number(inst, "width",  (f64)cmd.width);
    inst_update_number(inst, "height", (f64)cmd.height);

    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* w:getSize() -- multi-return (width, height) ----------------------------- */

static int native_window_get_size(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getSize: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getSize");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_SIZE;
    cmd.slot = (int)(s - g_slots);
    Command r;
    if (!post_or_throw(vm, cmd, "window.getSize", &r)) return -1;
    cando_vm_push(vm, cando_number((f64)r.ox));
    cando_vm_push(vm, cando_number((f64)r.oy));
    return 2;
}

/* w:setPosition(x, y) / w:getPosition() ----------------------------------- */

static int native_window_set_position(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3 || args[1].tag != CDO_NUMBER || args[2].tag != CDO_NUMBER) {
        window_throw(vm, "window.setPosition: (window, x, y) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.setPosition");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_POSITION;
    cmd.slot = (int)(s - g_slots);
    cmd.ix   = (int)args[1].as.number;
    cmd.iy   = (int)args[2].as.number;
    if (!post_or_throw(vm, cmd, "window.setPosition", NULL)) return -1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_window_get_position(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getPosition: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getPosition");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_POSITION;
    cmd.slot = (int)(s - g_slots);
    Command r;
    if (!post_or_throw(vm, cmd, "window.getPosition", &r)) return -1;
    cando_vm_push(vm, cando_number((f64)r.ox));
    cando_vm_push(vm, cando_number((f64)r.oy));
    return 2;
}

/* w:focus() / w:hasFocus() ------------------------------------------------ */

static int native_window_focus(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.focus: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.focus");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_FOCUS;
    cmd.slot = (int)(s - g_slots);
    if (!post_or_throw(vm, cmd, "window.focus", NULL)) return -1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_window_has_focus(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.hasFocus: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.hasFocus");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_ATTRIB;
    cmd.slot = (int)(s - g_slots);
    cmd.ix   = GLFW_FOCUSED;
    Command r;
    if (!post_or_throw(vm, cmd, "window.hasFocus", &r)) return -1;
    cando_vm_push(vm, cando_bool(r.ox != 0));
    return 1;
}

/* w:setVisible(bool) / w:isVisible() -------------------------------------- */

static int native_window_set_visible(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        window_throw(vm, "window.setVisible: (window, bool) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.setVisible");
    if (!s) return -1;
    bool show = (args[1].tag == CDO_BOOL) ? args[1].as.boolean : true;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_VISIBLE;
    cmd.slot = (int)(s - g_slots);
    cmd.ix   = show ? 1 : 0;
    if (!post_or_throw(vm, cmd, "window.setVisible", NULL)) return -1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_window_is_visible(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.isVisible: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.isVisible");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_ATTRIB;
    cmd.slot = (int)(s - g_slots);
    cmd.ix   = GLFW_VISIBLE;
    Command r;
    if (!post_or_throw(vm, cmd, "window.isVisible", &r)) return -1;
    cando_vm_push(vm, cando_bool(r.ox != 0));
    return 1;
}

/* w:getMouse() -- multi-return (x, y) ------------------------------------- */

static int native_window_get_mouse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getMouse: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getMouse");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_MOUSE;
    cmd.slot = (int)(s - g_slots);
    Command r;
    if (!post_or_throw(vm, cmd, "window.getMouse", &r)) return -1;
    cando_vm_push(vm, cando_number(r.fx));
    cando_vm_push(vm, cando_number(r.fy));
    return 2;
}

/* w:getDPIScale() -- multi-return (sx, sy) -------------------------------- */

static int native_window_get_dpi_scale(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getDPIScale: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getDPIScale");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_DPI_SCALE;
    cmd.slot = (int)(s - g_slots);
    Command r;
    if (!post_or_throw(vm, cmd, "window.getDPIScale", &r)) return -1;
    cando_vm_push(vm, cando_number(r.fx));
    cando_vm_push(vm, cando_number(r.fy));
    return 2;
}

/* w:getFramebufferSize() -- multi-return (w, h) --------------------------- */

static int native_window_get_framebuffer_size(CandoVM *vm, int argc,
                                              CandoValue *args)
{
    if (argc < 1) {
        window_throw(vm, "window.getFramebufferSize: (window) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.getFramebufferSize");
    if (!s) return -1;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_GET_FRAMEBUFFER;
    cmd.slot = (int)(s - g_slots);
    Command r;
    if (!post_or_throw(vm, cmd, "window.getFramebufferSize", &r)) return -1;
    cando_vm_push(vm, cando_number((f64)r.ox));
    cando_vm_push(vm, cando_number((f64)r.oy));
    return 2;
}

/* w:setVSync(bool) -------------------------------------------------------- */

static int native_window_set_vsync(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        window_throw(vm, "window.setVSync: (window, bool) required");
        return -1;
    }
    WindowSlot *s = resolve_window(vm, args[0], "window.setVSync");
    if (!s) return -1;
    bool on = (args[1].tag == CDO_BOOL) ? args[1].as.boolean : true;
    Command cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_VSYNC;
    cmd.slot = (int)(s - g_slots);
    cmd.ix   = on ? 1 : 0;
    if (!post_or_throw(vm, cmd, "window.setVSync", NULL)) return -1;
    cando_vm_push(vm, cando_bool(true));
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
    cando_lib_meta_define(vm, meta, "close",              native_window_close);
    cando_lib_meta_define(vm, meta, "isOpen",             native_window_is_open);
    cando_lib_meta_define(vm, meta, "setTitle",           native_window_set_title);
    cando_lib_meta_define(vm, meta, "getTitle",           native_window_get_title);
    cando_lib_meta_define(vm, meta, "setSize",            native_window_set_size);
    cando_lib_meta_define(vm, meta, "getSize",            native_window_get_size);
    cando_lib_meta_define(vm, meta, "setPosition",        native_window_set_position);
    cando_lib_meta_define(vm, meta, "getPosition",        native_window_get_position);
    cando_lib_meta_define(vm, meta, "focus",              native_window_focus);
    cando_lib_meta_define(vm, meta, "hasFocus",           native_window_has_focus);
    cando_lib_meta_define(vm, meta, "setVisible",         native_window_set_visible);
    cando_lib_meta_define(vm, meta, "isVisible",          native_window_is_visible);
    cando_lib_meta_define(vm, meta, "getMouse",           native_window_get_mouse);
    cando_lib_meta_define(vm, meta, "getDPIScale",        native_window_get_dpi_scale);
    cando_lib_meta_define(vm, meta, "getFramebufferSize", native_window_get_framebuffer_size);
    cando_lib_meta_define(vm, meta, "setVSync",           native_window_set_vsync);

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   WINDOW_MODULE_VERSION,
                   (u32)sizeof(WINDOW_MODULE_VERSION) - 1);

    const char *gv = glfwGetVersionString();
    if (gv) obj_set_string(obj, "glfwVersion", gv, (u32)strlen(gv));

    libutil_set_method(vm, obj, "create",     native_window_create);
    libutil_set_method(vm, obj, "_managerOk", native_window_manager_ok);

    /* window.keys -- LOVE-style key-code constants.  Keys arrive at
     * w.keypressed / w.keyreleased as integers; users compare against
     * `window.keys.escape`, `window.keys.left`, etc. */
    {
        CandoValue keys_val = cando_bridge_new_object(vm);
        CdoObject *keys     = cando_bridge_resolve(vm, keys_val.as.handle);

        /* Letters */
        for (int c = 0; c < 26; c++) {
            char name[2] = { (char)('a' + c), '\0' };
            obj_set_number(keys, name, (f64)(GLFW_KEY_A + c));
        }
        /* Digits */
        for (int d = 0; d < 10; d++) {
            char name[3] = { '_', (char)('0' + d), '\0' };
            /* Cando identifiers can't start with a digit, so prefix
             * digit keys with `_`: window.keys._0 .. window.keys._9. */
            obj_set_number(keys, name, (f64)(GLFW_KEY_0 + d));
        }
        /* Function keys */
        for (int f = 1; f <= 12; f++) {
            char name[4]; snprintf(name, sizeof(name), "f%d", f);
            obj_set_number(keys, name, (f64)(GLFW_KEY_F1 + (f - 1)));
        }
        /* Named keys (LOVE love.keyboard.isDown spellings) */
        obj_set_number(keys, "space",      (f64)GLFW_KEY_SPACE);
        obj_set_number(keys, "escape",     (f64)GLFW_KEY_ESCAPE);
        obj_set_number(keys, "return",     (f64)GLFW_KEY_ENTER);
        obj_set_number(keys, "enter",      (f64)GLFW_KEY_ENTER);
        obj_set_number(keys, "tab",        (f64)GLFW_KEY_TAB);
        obj_set_number(keys, "backspace",  (f64)GLFW_KEY_BACKSPACE);
        obj_set_number(keys, "delete",     (f64)GLFW_KEY_DELETE);
        obj_set_number(keys, "insert",     (f64)GLFW_KEY_INSERT);
        obj_set_number(keys, "home",       (f64)GLFW_KEY_HOME);
        obj_set_number(keys, "end",        (f64)GLFW_KEY_END);
        obj_set_number(keys, "pageup",     (f64)GLFW_KEY_PAGE_UP);
        obj_set_number(keys, "pagedown",   (f64)GLFW_KEY_PAGE_DOWN);
        obj_set_number(keys, "left",       (f64)GLFW_KEY_LEFT);
        obj_set_number(keys, "right",      (f64)GLFW_KEY_RIGHT);
        obj_set_number(keys, "up",         (f64)GLFW_KEY_UP);
        obj_set_number(keys, "down",       (f64)GLFW_KEY_DOWN);
        obj_set_number(keys, "lshift",     (f64)GLFW_KEY_LEFT_SHIFT);
        obj_set_number(keys, "rshift",     (f64)GLFW_KEY_RIGHT_SHIFT);
        obj_set_number(keys, "lctrl",      (f64)GLFW_KEY_LEFT_CONTROL);
        obj_set_number(keys, "rctrl",      (f64)GLFW_KEY_RIGHT_CONTROL);
        obj_set_number(keys, "lalt",       (f64)GLFW_KEY_LEFT_ALT);
        obj_set_number(keys, "ralt",       (f64)GLFW_KEY_RIGHT_ALT);
        obj_set_number(keys, "lsuper",     (f64)GLFW_KEY_LEFT_SUPER);
        obj_set_number(keys, "rsuper",     (f64)GLFW_KEY_RIGHT_SUPER);
        obj_set_number(keys, "capslock",   (f64)GLFW_KEY_CAPS_LOCK);
        obj_set_number(keys, "minus",      (f64)GLFW_KEY_MINUS);
        obj_set_number(keys, "equals",     (f64)GLFW_KEY_EQUAL);
        obj_set_number(keys, "comma",      (f64)GLFW_KEY_COMMA);
        obj_set_number(keys, "period",     (f64)GLFW_KEY_PERIOD);
        obj_set_number(keys, "slash",      (f64)GLFW_KEY_SLASH);
        obj_set_number(keys, "backslash",  (f64)GLFW_KEY_BACKSLASH);
        obj_set_number(keys, "semicolon",  (f64)GLFW_KEY_SEMICOLON);
        obj_set_number(keys, "apostrophe", (f64)GLFW_KEY_APOSTROPHE);
        obj_set_number(keys, "lbracket",   (f64)GLFW_KEY_LEFT_BRACKET);
        obj_set_number(keys, "rbracket",   (f64)GLFW_KEY_RIGHT_BRACKET);

        CdoString *kkeys = cdo_string_intern("keys", 4);
        cdo_object_rawset(obj, kkeys, cdo_object_value(keys), FIELD_NONE);
        cdo_string_release(kkeys);
    }

    /* window.mouse -- mouse-button constants. */
    {
        CandoValue mouse_val = cando_bridge_new_object(vm);
        CdoObject *mouse     = cando_bridge_resolve(vm, mouse_val.as.handle);
        obj_set_number(mouse, "left",   (f64)GLFW_MOUSE_BUTTON_LEFT);
        obj_set_number(mouse, "right",  (f64)GLFW_MOUSE_BUTTON_RIGHT);
        obj_set_number(mouse, "middle", (f64)GLFW_MOUSE_BUTTON_MIDDLE);
        CdoString *kmouse = cdo_string_intern("mouse", 5);
        cdo_object_rawset(obj, kmouse, cdo_object_value(mouse), FIELD_NONE);
        cdo_string_release(kmouse);
    }

    return tbl;
}
