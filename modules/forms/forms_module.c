/*
 * modules/forms/forms_module.c -- CanDo .NET-Forms-shaped Win32 binary module.
 *
 * Loaded into a script with:
 *
 *     VAR forms = include("./forms.dll");        // Windows
 *     VAR forms = include("./forms.so");         // Linux  (stub)
 *
 * The public API is shaped after System.Windows.Forms and styled after
 * Garry's Mod's Derma:
 *
 *     VAR f = forms.Form()
 *     f:setText("Hello")
 *     f:setSize(400, 300)
 *     f.onClose = function(self) { print("bye"); }
 *
 *     VAR b = forms.Button(f)
 *     b:setText("Click me")
 *     b:setLocation(20, 20)
 *     b:setSize(100, 30)
 *     b.onClick = function(self) { print("clicked"); }
 *
 *     f:show()           -- non-blocking; the script keeps running.
 *
 * Every form (and every control attached to it) lives in a slot in the
 * `g_slots` table, indexed by the `__forms_slot` field stamped onto the
 * script-side instance.  All Win32 work happens on a single dedicated
 * UI thread (`manager_thread_main`); script-side methods cross over to
 * it via SendMessageW (Win32 marshals automatically) or the Command
 * round-trip queue when the action requires running on the UI thread
 * itself (CreateWindow / DestroyWindow / message-loop teardown).
 *
 * Backend selection:
 *   CANDO_PLATFORM_WINDOWS    real Win32 backend (forms.dll)
 *   <otherwise>               stub backend; cando_module_init succeeds and
 *                             registers the public surface, but every
 *                             native errors with
 *                             "forms is only supported on Windows".
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#ifndef FORMS_MODULE_TEST_BUILD
#  include <cando.h>
#  include "vm/bridge.h"
#  include "object/object.h"
#  include "object/string.h"
#  include "object/value.h"
#  include "lib/libutil.h"
#  include "lib/meta.h"
#else
   /* Test build: stand-alone, no libcando headers.  Provide just enough
    * type aliases so the obj_set_* helpers below compile (they are
    * unused in the test path but kept in-tree to keep the file
    * self-contained). */
#  include <stdint.h>
#  include <stdbool.h>
   typedef double   f64;
   typedef uint32_t u32;
   typedef struct CandoVM     CandoVM;
   typedef struct CdoObject   CdoObject;
   typedef struct CdoString   CdoString;
   typedef struct CandoValue { int tag; union { double n; bool b; void *p; } as; } CandoValue;
   static inline CdoString *cdo_string_intern(const char *s, u32 n) { (void)s; (void)n; return (CdoString *)0; }
   static inline void cdo_string_release(CdoString *s) { (void)s; }
   static inline CandoValue cdo_string_value(CdoString *s) { (void)s; CandoValue v = {0,{0}}; return v; }
   static inline CandoValue cdo_number(double d) { CandoValue v = {0,{0}}; v.as.n = d; return v; }
   static inline CandoValue cdo_bool(bool b)     { CandoValue v = {0,{0}}; v.as.b = b; return v; }
   static inline bool cdo_object_rawset(CdoObject *o, CdoString *k, CandoValue v, int f) { (void)o; (void)k; (void)v; (void)f; return true; }
#  define FIELD_NONE 0
#endif

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  define FORMS_HAVE_WIN32 1
#else
#  define FORMS_HAVE_WIN32 0
#endif

#if FORMS_HAVE_WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <commctrl.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

#define FORMS_MODULE_VERSION "0.1.0"

/* =========================================================================
 * Tiny mutex / cond / thread wrapper.  libcando does not export its own
 * threading primitives to extension modules, so each binary module bundles
 * its own; this matches the pattern in modules/window.
 * ===================================================================== */

#if FORMS_HAVE_WIN32
   typedef CRITICAL_SECTION fm_mutex_t;
   typedef CONDITION_VARIABLE fm_cond_t;
   typedef HANDLE             fm_thread_t;
#  define FM_MUTEX_INIT(m)    InitializeCriticalSection(m)
#  define FM_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#  define FM_MUTEX_LOCK(m)    EnterCriticalSection(m)
#  define FM_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#  define FM_COND_INIT(c)     InitializeConditionVariable(c)
#  define FM_COND_DESTROY(c)  ((void)(c))
#  define FM_COND_WAIT(c,m)   SleepConditionVariableCS((c),(m),INFINITE)
#  define FM_COND_SIGNAL(c)   WakeConditionVariable(c)
#  define FM_COND_BROADCAST(c) WakeAllConditionVariable(c)
#else
   typedef pthread_mutex_t fm_mutex_t;
   typedef pthread_cond_t  fm_cond_t;
   typedef pthread_t       fm_thread_t;
#  define FM_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define FM_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define FM_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#  define FM_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#  define FM_COND_INIT(c)     pthread_cond_init((c), NULL)
#  define FM_COND_DESTROY(c)  pthread_cond_destroy(c)
#  define FM_COND_WAIT(c,m)   pthread_cond_wait((c),(m))
#  define FM_COND_SIGNAL(c)   pthread_cond_signal(c)
#  define FM_COND_BROADCAST(c) pthread_cond_broadcast(c)
#endif

/* =========================================================================
 * obj_set_* helpers (mirrors modules/window).
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

static void obj_set_number(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

static void obj_set_bool(CdoObject *obj, const char *key, bool value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(value), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Slot table.
 *
 * Every form and every control is one slot.  The script-side instance has
 * a `__forms_slot` field that indexes back here, plus a `__forms_gen`
 * generation counter so a recycled slot does not silently retarget an
 * old handle.
 * ===================================================================== */

typedef enum {
    KIND_NONE = 0,
    KIND_FORM,
    KIND_BUTTON,
    KIND_LABEL,
    KIND_TEXTBOX,
    KIND_CHECKBOX,
    KIND_RADIO,
    KIND_COMBOBOX,
    KIND_LISTBOX,
    KIND_PANEL,
    KIND_GROUPBOX,
    KIND_PROGRESS,
    KIND_TRACKBAR,
    KIND_NUMERIC,
    KIND_PICTUREBOX,
    KIND_KIND_COUNT
} ControlKind;

#define FORMS_MAX_SLOTS 256
#define FORMS_SLOT_KEY  "__forms_slot"
#define FORMS_GEN_KEY   "__forms_gen"
#define FORMS_KIND_KEY  "__forms_kind"

typedef struct FormsSlot {
    int          alive;
    int          generation;
    ControlKind  kind;
    int          parent_slot;       /* -1 for top-level forms             */
    /* Win32 handles -- NULL on stub builds.                              */
#if FORMS_HAVE_WIN32
    HWND         hwnd;
    WNDPROC      orig_proc;         /* for subclassed standard controls   */
#endif
    /* Cached desired geometry / text so getters can answer without a
     * round-trip when the manager thread has not yet created the HWND.  */
    int          x, y, w, h;
    int          visible;
    int          enabled;
    /* Retained handle to the script-side instance so callbacks survive
     * the script returning.                                              */
    CandoValue   inst_val;
    int          inst_val_held;
    int          has_lifeline;
} FormsSlot;

static FormsSlot g_slots[FORMS_MAX_SLOTS];
static fm_mutex_t g_slot_mutex;

/* =========================================================================
 * Event queue.
 *
 * Win32 callbacks (WndProc, control notifications) post Event records here.
 * The dispatcher thread drains them and calls user-supplied callbacks via
 * the child VM.  Single global queue, single producer (the manager thread)
 * + single consumer (the dispatcher thread).
 * ===================================================================== */

typedef enum {
    EV_NONE = 0,
    EV_CLICK,
    EV_CLOSE,
    EV_TEXT_CHANGED,
    EV_VALUE_CHANGED,
    EV_SELECTION_CHANGED,
    EV_KEY_DOWN,
    EV_KEY_UP,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_FOCUS,
    EV_BLUR,
    EV_RESIZE,
    EV_SHOWN,
} EventKind;

typedef struct FormsEvent {
    EventKind kind;
    int       slot;
    int       generation;
    int       i0, i1, i2;            /* general-purpose ints (button, key)*/
    double    d0, d1;                /* general-purpose floats (mouse)    */
} FormsEvent;

#define FORMS_EV_QUEUE_CAP 512

static FormsEvent g_ev_queue[FORMS_EV_QUEUE_CAP];
static int        g_ev_head = 0;     /* read index                        */
static int        g_ev_tail = 0;     /* write index                       */
static fm_mutex_t g_ev_mutex;
static fm_cond_t  g_ev_cond;

/* Public for unit tests: clear the queue between cases. */
static void event_queue_reset(void)
{
    g_ev_head = 0;
    g_ev_tail = 0;
}

/* True if pushing onto a full ring would step on the read cursor. */
static int event_queue_is_full(void)
{
    int next = (g_ev_tail + 1) % FORMS_EV_QUEUE_CAP;
    return next == g_ev_head;
}

static int event_queue_is_empty(void)
{
    return g_ev_head == g_ev_tail;
}

/* Producer side -- enqueues if space, drops the new event otherwise (the
 * window module uses the same drop-newest policy for input flooding). */
static void event_queue_push(FormsEvent ev)
{
    FM_MUTEX_LOCK(&g_ev_mutex);
    if (!event_queue_is_full()) {
        g_ev_queue[g_ev_tail] = ev;
        g_ev_tail = (g_ev_tail + 1) % FORMS_EV_QUEUE_CAP;
        FM_COND_SIGNAL(&g_ev_cond);
    }
    FM_MUTEX_UNLOCK(&g_ev_mutex);
}

/* Consumer side -- pops one event into *out, returns 1 on success or
 * 0 if the queue is empty.  Non-blocking. */
static int event_queue_try_pop(FormsEvent *out)
{
    int ok = 0;
    FM_MUTEX_LOCK(&g_ev_mutex);
    if (!event_queue_is_empty()) {
        *out = g_ev_queue[g_ev_head];
        g_ev_head = (g_ev_head + 1) % FORMS_EV_QUEUE_CAP;
        ok = 1;
    }
    FM_MUTEX_UNLOCK(&g_ev_mutex);
    return ok;
}

/* =========================================================================
 * Slot allocator.
 *
 * Generation counter advances on every allocation, so a script holding an
 * old handle whose slot has been recycled gets caught by the generation
 * mismatch in slot_resolve below.
 * ===================================================================== */

static int slot_alloc_locked(ControlKind kind, int parent_slot)
{
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {  /* index 0 reserved */
        if (!g_slots[i].alive) {
            FormsSlot *s = &g_slots[i];
            s->alive       = 1;
            s->generation += 1;
            s->kind        = kind;
            s->parent_slot = parent_slot;
            s->x = s->y = s->w = s->h = 0;
            s->visible = 0;
            s->enabled = 1;
            s->inst_val_held = 0;
            s->has_lifeline  = 0;
#if FORMS_HAVE_WIN32
            s->hwnd        = NULL;
            s->orig_proc   = NULL;
#endif
            return i;
        }
    }
    return -1;
}

static int slot_alloc(ControlKind kind, int parent_slot)
{
    FM_MUTEX_LOCK(&g_slot_mutex);
    int slot = slot_alloc_locked(kind, parent_slot);
    FM_MUTEX_UNLOCK(&g_slot_mutex);
    return slot;
}

/* =========================================================================
 * Manager-thread state machine (lazy-start on first form creation).
 * ===================================================================== */

typedef enum {
    MGR_UNSTARTED = 0,
    MGR_STARTING,
    MGR_RUNNING,
    MGR_INIT_FAILED,
    MGR_STOPPING,
    MGR_STOPPED,
} ManagerState;

static fm_mutex_t  g_mgr_mutex;
static fm_cond_t   g_mgr_cond;
static fm_thread_t g_mgr_thread;
static atomic_int  g_mgr_state       = MGR_UNSTARTED;
static atomic_int  g_mgr_should_stop = 0;
static atomic_int  g_sync_inited     = 0;
static int         g_atexit_registered = 0;

/* Sync object init -- lazy + idempotent.  Caller must call this before any
 * lock/cond use. */
static void sync_init_once(void)
{
    static atomic_int once = 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(&once, &expected, 1)) {
        FM_MUTEX_INIT(&g_slot_mutex);
        FM_MUTEX_INIT(&g_ev_mutex);
        FM_COND_INIT(&g_ev_cond);
        FM_MUTEX_INIT(&g_mgr_mutex);
        FM_COND_INIT(&g_mgr_cond);
        atomic_store(&g_sync_inited, 1);
    } else {
        while (!atomic_load(&g_sync_inited)) { /* spin */ }
    }
}

/* =========================================================================
 * Manager thread + Win32 backend.
 *
 * One dedicated manager thread owns ALL Win32 UI work: it creates the
 * hidden command-receiver window, every form window, every child
 * control, and runs the single shared message pump for the whole module.
 * This is the exact model WinForms' Application.Run uses internally and
 * matches the existing modules/window's GLFW manager.
 *
 * Cross-thread requests:
 *   - Property setters (SetText, MoveWindow, EnableWindow, ShowWindow,
 *     SendMessage(BM_*, EM_*, ...)) run from the script thread directly.
 *     Win32 marshals these to the owning thread automatically.
 *   - CreateWindow / DestroyWindow / Init MUST run on the owning UI
 *     thread, so callers post a Command via SendMessageW(g_mgr_hwnd,
 *     WM_FORMS_CMD, 0, &cmd) which blocks until the manager has
 *     processed it.
 * ===================================================================== */

#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)

#define WM_FORMS_CMD     (WM_USER + 100)
#define WM_FORMS_TICK    (WM_USER + 101)   /* drain event queue           */

static const wchar_t FORMS_WNDCLASS_FORM[] = L"CanDoForms_Form";
static const wchar_t FORMS_WNDCLASS_MGR[]  = L"CanDoForms_Manager";

typedef enum {
    CMD_CREATE_FORM = 1,
    CMD_CREATE_CONTROL,
    CMD_DESTROY,
} CmdOp;

typedef struct FormsCommand {
    CmdOp        op;
    int          slot;
    ControlKind  kind;
    int          parent_slot;
    wchar_t      text[512];
    int          x, y, w, h;
    int          style_extra;        /* per-kind style bits             */
    /* result */
    int          ok;
    char         err[200];
} FormsCommand;

static HWND        g_mgr_hwnd = NULL;
static DWORD       g_mgr_thread_id = 0;
static UINT        g_dispatch_timer_id = 0;

/* Forward declarations -- definitions follow. */
static LRESULT CALLBACK mgr_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK form_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l);
static void              dispatch_drain(void);

/* Convert a UTF-8 string to a freshly-allocated wide string.  Caller
 * frees with free().  Returns NULL on alloc failure. */
static wchar_t *utf8_to_wide(const char *s, int n)
{
    if (!s) { wchar_t *w = (wchar_t *)calloc(1, sizeof(wchar_t)); return w; }
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t *out = (wchar_t *)calloc((size_t)wn + 1, sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, n, out, wn);
    out[wn] = 0;
    return out;
}

/* Convert a wide string to a freshly-allocated UTF-8 string. */
static char *wide_to_utf8(const wchar_t *w)
{
    if (!w) { char *o = (char *)calloc(1, 1); return o; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *out = (char *)calloc((size_t)n + 1, 1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    return out;
}

/* Copy a UTF-8 source into a fixed-size wide buffer (truncates). */
static void utf8_into_wide_buf(const char *src, int srclen,
                               wchar_t *dst, int dstcap)
{
    if (dstcap <= 0) return;
    if (!src || srclen == 0) { dst[0] = 0; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, src, srclen,
                                dst, dstcap - 1);
    if (n < 0) n = 0;
    dst[n] = 0;
}

/* Push a FormsEvent onto the global queue from the UI thread. */
static void push_event(EventKind kind, int slot, int generation)
{
    FormsEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind       = kind;
    ev.slot       = slot;
    ev.generation = generation;
    event_queue_push(ev);
}

/* Look up a slot index from an HWND via GWLP_USERDATA (set at create). */
static int slot_from_hwnd(HWND h)
{
    if (!h) return 0;
    return (int)(intptr_t)GetWindowLongPtrW(h, GWLP_USERDATA);
}

/* =========================================================================
 * Manager hidden window.  Receives WM_FORMS_CMD from any thread.
 * ===================================================================== */

static int register_classes_once(void)
{
    static atomic_int once = 0;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&once, &expected, 1)) return 1;

    HINSTANCE hi = GetModuleHandleW(NULL);

    /* Form class -- top-level windows.  CS_OWNDC is fine for simple GDI;
     * background brush gives forms the default 3D button-face colour. */
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = form_wndproc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = FORMS_WNDCLASS_FORM;
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return 0;
    }

    /* Manager class -- hidden message-only window. */
    WNDCLASSEXW mc; memset(&mc, 0, sizeof(mc));
    mc.cbSize        = sizeof(mc);
    mc.lpfnWndProc   = mgr_wndproc;
    mc.hInstance     = hi;
    mc.lpszClassName = FORMS_WNDCLASS_MGR;
    if (!RegisterClassExW(&mc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return 0;
    }
    return 1;
}

/* Create and configure a top-level form window.  Runs on the manager
 * thread inside the WM_FORMS_CMD handler.  Returns 1 on success. */
static int do_create_form(FormsCommand *c)
{
    FormsSlot *s = &g_slots[c->slot];

    DWORD style    = WS_OVERLAPPEDWINDOW;
    DWORD ex_style = 0;

    int x = (c->x == 0 && c->y == 0) ? CW_USEDEFAULT : c->x;
    int y = (c->x == 0 && c->y == 0) ? CW_USEDEFAULT : c->y;
    int w = c->w > 0 ? c->w : 480;
    int h = c->h > 0 ? c->h : 360;

    HWND hwnd = CreateWindowExW(
        ex_style,
        FORMS_WNDCLASS_FORM,
        c->text[0] ? c->text : L"CanDo Form",
        style,
        x, y, w, h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        snprintf(c->err, sizeof(c->err),
                 "CreateWindowExW failed (GetLastError=%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c->slot);
    s->hwnd = hwnd;
    s->w = w; s->h = h;
    s->x = (x == CW_USEDEFAULT ? 0 : x);
    s->y = (y == CW_USEDEFAULT ? 0 : y);
    return 1;
}

/* Create a child control parented to an existing form/panel slot. */
static int do_create_control(FormsCommand *c)
{
    FormsSlot *parent = &g_slots[c->parent_slot];
    if (!parent->alive || !parent->hwnd) {
        snprintf(c->err, sizeof(c->err), "parent slot %d has no HWND",
                 c->parent_slot);
        return 0;
    }

    const wchar_t *cls = NULL;
    DWORD style = WS_CHILD | WS_VISIBLE;
    DWORD ex    = 0;

    switch (c->kind) {
    case KIND_BUTTON:
        cls = L"BUTTON";
        style |= BS_PUSHBUTTON | WS_TABSTOP;
        break;
    case KIND_LABEL:
        cls = L"STATIC";
        style |= SS_LEFT;
        break;
    case KIND_TEXTBOX:
        cls = L"EDIT";
        style |= WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP;
        if (c->style_extra & 1) style |= ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN;
        if (c->style_extra & 2) style |= ES_PASSWORD;
        ex = WS_EX_CLIENTEDGE;
        break;
    case KIND_CHECKBOX:
        cls = L"BUTTON";
        style |= BS_AUTOCHECKBOX | WS_TABSTOP;
        break;
    case KIND_RADIO:
        cls = L"BUTTON";
        style |= BS_AUTORADIOBUTTON | WS_TABSTOP;
        break;
    case KIND_GROUPBOX:
        cls = L"BUTTON";
        style |= BS_GROUPBOX;
        break;
    case KIND_COMBOBOX:
        cls = L"COMBOBOX";
        style |= CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP;
        if (c->style_extra & 1) {
            /* DropDownList style */
            style = (style & ~CBS_DROPDOWN) | CBS_DROPDOWNLIST;
        }
        break;
    case KIND_LISTBOX:
        cls = L"LISTBOX";
        style |= LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP;
        ex = WS_EX_CLIENTEDGE;
        break;
    case KIND_PANEL:
        cls = L"STATIC";
        style |= SS_NOTIFY;
        ex = WS_EX_CONTROLPARENT;
        break;
    case KIND_PROGRESS:
        cls = PROGRESS_CLASSW;
        break;
    case KIND_TRACKBAR:
        cls = TRACKBAR_CLASSW;
        style |= TBS_HORZ | TBS_AUTOTICKS;
        break;
    case KIND_NUMERIC:
        /* UpDown buddy + edit; for v0 we just ship the edit, plus a
         * NumericUpDown common control would need an extra HWND.  Use
         * a numeric-only EDIT for now -- the API surface is identical. */
        cls = L"EDIT";
        style |= WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP;
        ex = WS_EX_CLIENTEDGE;
        break;
    case KIND_PICTUREBOX:
        cls = L"STATIC";
        style |= SS_BITMAP;
        break;
    default:
        snprintf(c->err, sizeof(c->err), "unsupported control kind %d",
                 (int)c->kind);
        return 0;
    }

    int x = c->x, y = c->y;
    int w = c->w > 0 ? c->w : 80;
    int h = c->h > 0 ? c->h : 24;

    HWND hwnd = CreateWindowExW(
        ex, cls,
        c->text[0] ? c->text : L"",
        style,
        x, y, w, h,
        parent->hwnd, (HMENU)(LONG_PTR)c->slot,    /* slot doubles as control ID */
        GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        snprintf(c->err, sizeof(c->err),
                 "CreateWindowExW(child) failed (GetLastError=%lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c->slot);

    /* Inherit the parent's font so controls look native, not bitmap-Sys. */
    HFONT font = (HFONT)SendMessageW(parent->hwnd, WM_GETFONT, 0, 0);
    if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);

    FormsSlot *s = &g_slots[c->slot];
    s->hwnd = hwnd;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->visible = 1;
    return 1;
}

/* Destroy the HWND for a slot.  Runs on the manager thread. */
static int do_destroy(FormsCommand *c)
{
    FormsSlot *s = &g_slots[c->slot];
    if (s->hwnd) {
        DestroyWindow(s->hwnd);
        s->hwnd = NULL;
    }
    return 1;
}

/* Manager hidden-window WndProc.  Dispatches WM_FORMS_CMD to the right
 * do_* handler and answers WM_FORMS_TICK by draining the event queue. */
static LRESULT CALLBACK mgr_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_FORMS_CMD) {
        FormsCommand *c = (FormsCommand *)l;
        c->ok = 0;
        c->err[0] = 0;
        switch (c->op) {
        case CMD_CREATE_FORM:    c->ok = do_create_form(c);    break;
        case CMD_CREATE_CONTROL: c->ok = do_create_control(c); break;
        case CMD_DESTROY:        c->ok = do_destroy(c);        break;
        default:
            snprintf(c->err, sizeof(c->err), "unknown op %d", (int)c->op);
            break;
        }
        return 0;
    }
    if (msg == WM_FORMS_TICK) {
        dispatch_drain();
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* Top-level form WndProc.  Translates Win32 events into FormsEvents. */
static LRESULT CALLBACK form_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    int slot = slot_from_hwnd(h);
    int gen  = (slot > 0 && slot < FORMS_MAX_SLOTS) ?
               g_slots[slot].generation : 0;

    switch (msg) {
    case WM_CLOSE:
        push_event(EV_CLOSE, slot, gen);
        /* Don't auto-destroy; let the script call form:destroy() in the
         * onClose handler if it wants.  Hide instead, matching WinForms'
         * default FormClosing behaviour. */
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_LBUTTONDOWN: {
        FormsEvent ev = {0}; ev.kind = EV_MOUSE_DOWN; ev.slot = slot;
        ev.generation = gen; ev.i0 = 1;
        ev.d0 = (double)(short)LOWORD(l); ev.d1 = (double)(short)HIWORD(l);
        event_queue_push(ev);
        FormsEvent c = {0}; c.kind = EV_CLICK; c.slot = slot;
        c.generation = gen; c.i0 = 1;
        c.d0 = ev.d0; c.d1 = ev.d1;
        event_queue_push(c);
        return 0;
    }
    case WM_LBUTTONUP: {
        FormsEvent ev = {0}; ev.kind = EV_MOUSE_UP; ev.slot = slot;
        ev.generation = gen; ev.i0 = 1;
        ev.d0 = (double)(short)LOWORD(l); ev.d1 = (double)(short)HIWORD(l);
        event_queue_push(ev);
        return 0;
    }
    case WM_MOUSEMOVE: {
        FormsEvent ev = {0}; ev.kind = EV_MOUSE_MOVE; ev.slot = slot;
        ev.generation = gen;
        ev.d0 = (double)(short)LOWORD(l); ev.d1 = (double)(short)HIWORD(l);
        event_queue_push(ev);
        return 0;
    }
    case WM_KEYDOWN: {
        FormsEvent ev = {0}; ev.kind = EV_KEY_DOWN; ev.slot = slot;
        ev.generation = gen; ev.i0 = (int)w;
        event_queue_push(ev);
        return 0;
    }
    case WM_KEYUP: {
        FormsEvent ev = {0}; ev.kind = EV_KEY_UP; ev.slot = slot;
        ev.generation = gen; ev.i0 = (int)w;
        event_queue_push(ev);
        return 0;
    }
    case WM_SETFOCUS: push_event(EV_FOCUS, slot, gen); return 0;
    case WM_KILLFOCUS: push_event(EV_BLUR, slot, gen); return 0;
    case WM_SIZE: {
        int cw = LOWORD(l), ch = HIWORD(l);
        FormsEvent ev = {0}; ev.kind = EV_RESIZE; ev.slot = slot;
        ev.generation = gen; ev.i0 = cw; ev.i1 = ch;
        event_queue_push(ev);
        if (slot > 0 && slot < FORMS_MAX_SLOTS) {
            g_slots[slot].w = cw; g_slots[slot].h = ch;
        }
        return 0;
    }
    case WM_COMMAND: {
        /* Child control notifications.  HIWORD(w)=notify code,
         * LOWORD(w)=control ID (== slot), l=child HWND. */
        int code = HIWORD(w);
        int cid  = LOWORD(w);
        if (cid > 0 && cid < FORMS_MAX_SLOTS && g_slots[cid].alive) {
            int cgen = g_slots[cid].generation;
            ControlKind k = g_slots[cid].kind;
            if (k == KIND_BUTTON && code == BN_CLICKED) {
                push_event(EV_CLICK, cid, cgen);
            } else if ((k == KIND_CHECKBOX || k == KIND_RADIO) && code == BN_CLICKED) {
                push_event(EV_VALUE_CHANGED, cid, cgen);
                push_event(EV_CLICK, cid, cgen);
            } else if (k == KIND_TEXTBOX && code == EN_CHANGE) {
                push_event(EV_TEXT_CHANGED, cid, cgen);
            } else if (k == KIND_COMBOBOX && code == CBN_SELCHANGE) {
                push_event(EV_SELECTION_CHANGED, cid, cgen);
            } else if (k == KIND_LISTBOX && code == LBN_SELCHANGE) {
                push_event(EV_SELECTION_CHANGED, cid, cgen);
            }
        }
        return 0;
    }
    case WM_HSCROLL:
    case WM_VSCROLL: {
        HWND child = (HWND)l;
        if (child) {
            int cid = slot_from_hwnd(child);
            if (cid > 0 && cid < FORMS_MAX_SLOTS && g_slots[cid].alive) {
                push_event(EV_VALUE_CHANGED, cid, g_slots[cid].generation);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        /* Slot teardown is initiated by the script via destroy(); we
         * don't push an event here because WM_CLOSE already did. */
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* The manager thread main loop.  Creates the hidden window, runs the
 * standard Win32 message pump, drives event dispatch on a 16ms timer. */
static unsigned __stdcall manager_thread_main_win32(void *arg)
{
    (void)arg;
    if (!register_classes_once()) {
        atomic_store(&g_mgr_state, MGR_INIT_FAILED);
        FM_MUTEX_LOCK(&g_mgr_mutex);
        FM_COND_BROADCAST(&g_mgr_cond);
        FM_MUTEX_UNLOCK(&g_mgr_mutex);
        return 0;
    }
    InitCommonControls();

    g_mgr_thread_id = GetCurrentThreadId();
    g_mgr_hwnd = CreateWindowExW(0, FORMS_WNDCLASS_MGR, L"",
                                 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, NULL,
                                 GetModuleHandleW(NULL), NULL);
    if (!g_mgr_hwnd) {
        atomic_store(&g_mgr_state, MGR_INIT_FAILED);
        FM_MUTEX_LOCK(&g_mgr_mutex);
        FM_COND_BROADCAST(&g_mgr_cond);
        FM_MUTEX_UNLOCK(&g_mgr_mutex);
        return 0;
    }

    /* 16ms timer drives the event queue drain regardless of GUI activity
     * so callbacks fire even when no Win32 messages are pending. */
    g_dispatch_timer_id = (UINT)SetTimer(g_mgr_hwnd, 1, 16, NULL);

    atomic_store(&g_mgr_state, MGR_RUNNING);
    FM_MUTEX_LOCK(&g_mgr_mutex);
    FM_COND_BROADCAST(&g_mgr_cond);
    FM_MUTEX_UNLOCK(&g_mgr_mutex);

    MSG msg;
    while (!atomic_load(&g_mgr_should_stop)) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got == 0 || got == -1) break;
        if (msg.message == WM_TIMER && msg.hwnd == g_mgr_hwnd) {
            dispatch_drain();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        /* Drain after every dispatch so callbacks fire promptly. */
        dispatch_drain();
    }

    if (g_dispatch_timer_id) KillTimer(g_mgr_hwnd, 1);
    DestroyWindow(g_mgr_hwnd);
    g_mgr_hwnd = NULL;

    atomic_store(&g_mgr_state, MGR_STOPPED);
    return 0;
}

#else /* !FORMS_HAVE_WIN32 || FORMS_MODULE_TEST_BUILD -- stub backend */

#if !FORMS_HAVE_WIN32
static void *manager_thread_main_stub(void *arg)
{
    (void)arg;
    atomic_store(&g_mgr_state, MGR_RUNNING);
    FM_MUTEX_LOCK(&g_mgr_mutex);
    FM_COND_BROADCAST(&g_mgr_cond);
    FM_MUTEX_UNLOCK(&g_mgr_mutex);
    while (!atomic_load(&g_mgr_should_stop)) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    atomic_store(&g_mgr_state, MGR_STOPPED);
    return NULL;
}
#endif /* !FORMS_HAVE_WIN32 */

#endif

/* =========================================================================
 * Manager thread lifecycle.
 * ===================================================================== */

static void mgr_shutdown(void)
{
    int state = atomic_load(&g_mgr_state);
    if (state == MGR_UNSTARTED || state == MGR_STOPPED ||
        state == MGR_INIT_FAILED) return;

    atomic_store(&g_mgr_should_stop, 1);
#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
    if (g_mgr_hwnd) PostMessageW(g_mgr_hwnd, WM_QUIT, 0, 0);
    else if (g_mgr_thread_id) PostThreadMessageW(g_mgr_thread_id, WM_QUIT, 0, 0);
    if (g_mgr_thread) {
        WaitForSingleObject(g_mgr_thread, 2000);
        CloseHandle(g_mgr_thread);
    }
#else
    pthread_join(g_mgr_thread, NULL);
#endif
}

static int ensure_manager(void)
{
    sync_init_once();

    int state = atomic_load(&g_mgr_state);
    if (state == MGR_RUNNING)     return 1;
    if (state == MGR_INIT_FAILED) return 0;
    if (state == MGR_STOPPING || state == MGR_STOPPED) return 0;

    FM_MUTEX_LOCK(&g_mgr_mutex);
    state = atomic_load(&g_mgr_state);
    if (state == MGR_UNSTARTED) {
        atomic_store(&g_mgr_state, MGR_STARTING);
#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
        g_mgr_thread = (HANDLE)_beginthreadex(
            NULL, 0, manager_thread_main_win32, NULL, 0, NULL);
        if (!g_mgr_thread) {
            atomic_store(&g_mgr_state, MGR_INIT_FAILED);
            FM_MUTEX_UNLOCK(&g_mgr_mutex);
            return 0;
        }
#else
        if (pthread_create(&g_mgr_thread, NULL,
                           manager_thread_main_stub, NULL) != 0) {
            atomic_store(&g_mgr_state, MGR_INIT_FAILED);
            FM_MUTEX_UNLOCK(&g_mgr_mutex);
            return 0;
        }
#endif
        if (!g_atexit_registered) {
            atexit(mgr_shutdown);
            g_atexit_registered = 1;
        }
    }
    while ((state = atomic_load(&g_mgr_state)) == MGR_STARTING) {
        FM_COND_WAIT(&g_mgr_cond, &g_mgr_mutex);
    }
    FM_MUTEX_UNLOCK(&g_mgr_mutex);
    return state == MGR_RUNNING;
}

/* =========================================================================
 * Callback dispatch.
 *
 * The manager thread drains the global event queue and calls the
 * matching user-defined callback (`onClick`, `onClose`, ...) on the
 * slot's script-side instance.  Same shape as modules/window: per-call
 * VM state is reset after each dispatch so a faulty callback can't
 * poison the next one.
 * ===================================================================== */

#ifndef FORMS_MODULE_TEST_BUILD

/* The script thread captures the root VM on first form creation; the
 * manager thread uses g_dispatch_vm (a child of the root) to invoke
 * user-defined callbacks.  Same lifecycle as modules/window. */
static CandoVM   *g_root_vm = NULL;
static CandoVM    g_dispatch_vm;
static int        g_dispatch_vm_inited = 0;

/* Map an EventKind to its script-side property name. */
static const char *event_callback_name(EventKind k)
{
    switch (k) {
    case EV_CLICK:              return "onClick";
    case EV_CLOSE:              return "onClose";
    case EV_TEXT_CHANGED:       return "onTextChanged";
    case EV_VALUE_CHANGED:      return "onValueChanged";
    case EV_SELECTION_CHANGED:  return "onSelectionChanged";
    case EV_KEY_DOWN:           return "onKeyDown";
    case EV_KEY_UP:             return "onKeyUp";
    case EV_MOUSE_DOWN:         return "onMouseDown";
    case EV_MOUSE_UP:           return "onMouseUp";
    case EV_MOUSE_MOVE:         return "onMouseMove";
    case EV_FOCUS:              return "onFocus";
    case EV_BLUR:               return "onBlur";
    case EV_RESIZE:             return "onResize";
    case EV_SHOWN:              return "onShown";
    default:                    return NULL;
    }
}

static void dispatch_one(FormsEvent ev)
{
    if (!g_dispatch_vm_inited) return;
    if (ev.slot <= 0 || ev.slot >= FORMS_MAX_SLOTS) return;
    FormsSlot *s = &g_slots[ev.slot];
    if (!s->alive || !s->inst_val_held) return;
    if (s->generation != ev.generation) return;     /* stale event */

    const char *name = event_callback_name(ev.kind);
    if (!name) return;

    CdoObject *inst = cando_bridge_resolve(&g_dispatch_vm, s->inst_val.as.handle);
    if (!inst) return;

    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   field;
    bool       have = cdo_object_get(inst, key, &field);
    cdo_string_release(key);
    if (!have) return;

    bool callable = (field.tag == CDO_FUNCTION) ||
                    (field.tag == CDO_NATIVE)   ||
                    (field.tag == CDO_OBJECT &&
                     field.as.object &&
                     field.as.object->kind == OBJ_FUNCTION);
    if (!callable) return;

    CandoValue fn = cando_bridge_to_cando(&g_dispatch_vm, field);

    /* Build (self, ...payload).  We pack ints / doubles into args based
     * on the event kind so callbacks see WinForms-shaped signatures:
     *   onClick(self, button, x, y)
     *   onMouseDown(self, button, x, y)
     *   onMouseMove(self, x, y)
     *   onKeyDown(self, vk_code)
     *   onResize(self, w, h)
     *   onTextChanged(self), onValueChanged(self), ...
     */
    enum { MAX_ARGS = 6 };
    CandoValue argv[MAX_ARGS];
    int argc = 0;
    argv[argc++] = s->inst_val;
    switch (ev.kind) {
    case EV_CLICK:
    case EV_MOUSE_DOWN:
    case EV_MOUSE_UP:
        argv[argc++] = cando_number((f64)ev.i0);
        argv[argc++] = cando_number(ev.d0);
        argv[argc++] = cando_number(ev.d1);
        break;
    case EV_MOUSE_MOVE:
        argv[argc++] = cando_number(ev.d0);
        argv[argc++] = cando_number(ev.d1);
        break;
    case EV_KEY_DOWN:
    case EV_KEY_UP:
        argv[argc++] = cando_number((f64)ev.i0);
        break;
    case EV_RESIZE:
        argv[argc++] = cando_number((f64)ev.i0);
        argv[argc++] = cando_number((f64)ev.i1);
        break;
    default:
        break;
    }

    cando_vm_call_value(&g_dispatch_vm, fn, argv, argc);
    g_dispatch_vm.stack_top   = g_dispatch_vm.stack;
    g_dispatch_vm.frame_count = 0;
    g_dispatch_vm.has_error   = false;
    g_dispatch_vm.error_val_count = 0;
}

static void dispatch_drain(void)
{
    /* Cap the per-tick drain so a runaway flood (mouse-move spam) can't
     * starve the message pump for an entire tick.  Anything left waits
     * for the next WM_TIMER fire (16ms). */
    int budget = 64;
    FormsEvent ev;
    while (budget-- > 0 && event_queue_try_pop(&ev)) {
        dispatch_one(ev);
    }
}

/* =========================================================================
 * Slot resolution + teardown.
 * ===================================================================== */

static FormsSlot *slot_from_inst(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return NULL;
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    if (!o) return NULL;
    CdoString *ks = cdo_string_intern(FORMS_SLOT_KEY, (u32)strlen(FORMS_SLOT_KEY));
    CdoString *kg = cdo_string_intern(FORMS_GEN_KEY,  (u32)strlen(FORMS_GEN_KEY));
    CdoValue vs, vg;
    bool has_s = cdo_object_rawget(o, ks, &vs);
    bool has_g = cdo_object_rawget(o, kg, &vg);
    cdo_string_release(ks);
    cdo_string_release(kg);
    if (!has_s || !has_g) return NULL;
    if (vs.tag != CDO_NUMBER || vg.tag != CDO_NUMBER) return NULL;
    int slot = (int)vs.as.number;
    int gen  = (int)vg.as.number;
    if (slot <= 0 || slot >= FORMS_MAX_SLOTS) return NULL;
    FormsSlot *s = &g_slots[slot];
    if (!s->alive || s->generation != gen) return NULL;
    return s;
}

/* Internal teardown: destroy the HWND on the manager thread, drop the
 * retained instance handle, release the lifeline.  Idempotent. */
static void slot_teardown(FormsSlot *s)
{
    if (!s) return;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        FormsCommand cmd; memset(&cmd, 0, sizeof(cmd));
        cmd.op = CMD_DESTROY;
        cmd.slot = (int)(s - g_slots);
        if (g_mgr_hwnd) {
            SendMessageW(g_mgr_hwnd, WM_FORMS_CMD, 0, (LPARAM)&cmd);
        } else {
            DestroyWindow(s->hwnd);
            s->hwnd = NULL;
        }
    }
#endif
    if (s->inst_val_held) {
        cando_value_release(s->inst_val);
        s->inst_val_held = 0;
    }
    if (s->has_lifeline && g_root_vm) {
        cando_vm_lifeline_release(g_root_vm);
        s->has_lifeline = 0;
    }
    s->alive = 0;
}

/* printf-style throw -- mirrors modules/window's window_throw. */
static void forms_throw(CandoVM *vm, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cando_vm_error(vm, "%s", buf);
}

/* Errors with "forms is only supported on Windows" when invoked on the
 * stub build.  Returns 1 if the runtime supports forms, 0 otherwise
 * (caller should `return -1`). */
static int require_supported(CandoVM *vm, const char *who)
{
#if FORMS_HAVE_WIN32
    (void)vm; (void)who;
    return 1;
#else
    forms_throw(vm, "%s: forms is only supported on Windows "
                    "(loaded module is the stub build)", who);
    return 0;
#endif
}

/* Pull (slot) from args[0] or error.  Returns NULL on failure (caller
 * should `return -1` after checking has_error). */
static FormsSlot *arg_self(CandoVM *vm, int argc, CandoValue *args,
                           const char *who)
{
    if (argc < 1) {
        forms_throw(vm, "%s: missing receiver (call as obj:%s(...))", who, who);
        return NULL;
    }
    FormsSlot *s = slot_from_inst(vm, args[0]);
    if (!s) {
        forms_throw(vm, "%s: invalid or destroyed receiver", who);
        return NULL;
    }
    return s;
}

/* =========================================================================
 * Build a script-side instance for a freshly-created slot.  Stamps
 * __forms_slot / __forms_gen / __forms_kind, attaches the meta table,
 * mirrors initial geometry as plain fields, retains the value into the
 * slot, and acquires a VM lifeline so the script can return without
 * tearing the form down.
 * ===================================================================== */

static CandoValue build_instance(CandoVM *vm, int slot, ControlKind kind,
                                 const char *text,
                                 int x, int y, int w, int h)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);

    obj_set_number(o, FORMS_SLOT_KEY, (f64)slot);
    obj_set_number(o, FORMS_GEN_KEY,  (f64)g_slots[slot].generation);
    obj_set_number(o, FORMS_KIND_KEY, (f64)kind);
    if (text) obj_set_string(o, "text", text, (u32)strlen(text));
    obj_set_number(o, "x", (f64)x);
    obj_set_number(o, "y", (f64)y);
    obj_set_number(o, "width",  (f64)w);
    obj_set_number(o, "height", (f64)h);

    cando_lib_meta_attach(vm, o, "forms_control");

    g_slots[slot].inst_val      = cando_value_copy(v);
    g_slots[slot].inst_val_held = 1;

    cando_vm_lifeline_acquire(vm, "forms");
    g_slots[slot].has_lifeline = 1;
    return v;
}

/* Capture the root VM and lazily init the dispatch child VM.  Called
 * from every constructor before allocating a slot. */
static int prep_dispatch_vm(CandoVM *vm)
{
    if (!g_root_vm) g_root_vm = vm;
    if (!g_dispatch_vm_inited) {
        cando_vm_init_child(&g_dispatch_vm, vm);
        g_dispatch_vm_inited = 1;
    }
    return 1;
}

/* =========================================================================
 * Constructors -- forms.Form() and the generic control constructor used
 * by chunk 3 (Button, Label, ...).  build_create takes a kind + parent
 * so a single helper covers every control type.
 * ===================================================================== */

#if FORMS_HAVE_WIN32

static int post_create(FormsCommand *cmd, CandoVM *vm, const char *who)
{
    if (!g_mgr_hwnd) {
        forms_throw(vm, "%s: manager window not ready", who);
        return 0;
    }
    SendMessageW(g_mgr_hwnd, WM_FORMS_CMD, 0, (LPARAM)cmd);
    if (!cmd->ok) {
        forms_throw(vm, "%s: %s", who, cmd->err[0] ? cmd->err : "unknown");
        return 0;
    }
    return 1;
}

#endif

/* opts-or-positional argument pulling.  Always returns char* text in a
 * caller-owned buffer (utf-8) or NULL. */
static void parse_text_arg(CandoVM *vm, CandoValue v, char *out, int outcap)
{
    (void)vm;
    if (out == NULL || outcap <= 0) return;
    out[0] = 0;
    if (v.tag == CDO_STRING && v.as.string) {
        const char *s = v.as.string->data;
        u32 n = v.as.string->length;
        if ((int)n >= outcap) n = (u32)(outcap - 1);
        memcpy(out, s, n);
        out[n] = 0;
    }
}

static int generic_create(CandoVM *vm, ControlKind kind, int argc, CandoValue *args,
                          const char *who)
{
    if (!require_supported(vm, who)) return -1;
    if (!ensure_manager()) {
        forms_throw(vm, "%s: failed to start manager thread", who);
        return -1;
    }
    prep_dispatch_vm(vm);

    int parent_slot = -1;
    int x = 0, y = 0, w = 0, h = 0;
    char text[512] = {0};
    int style_extra = 0;

    /* Parent: if the first argument is an existing forms instance, use
     * it.  Forms constructed without a parent are top-level forms. */
    int arg0 = 0;
    if (argc >= 1 && cando_is_object(args[0])) {
        FormsSlot *p = slot_from_inst(vm, args[0]);
        if (p) {
            parent_slot = (int)(p - g_slots);
            arg0 = 1;
        }
    }
    /* Optional second-arg options object {x=, y=, width=, height=, text=}. */
    if (argc > arg0 && cando_is_object(args[arg0])) {
        CdoObject *opts = cando_bridge_resolve(vm, args[arg0].as.handle);
        struct { const char *k; int *v; } ints[] = {
            {"x", &x}, {"y", &y}, {"width", &w}, {"height", &h},
        };
        for (size_t i = 0; i < sizeof(ints)/sizeof(ints[0]); i++) {
            CdoString *kk = cdo_string_intern(ints[i].k, (u32)strlen(ints[i].k));
            CdoValue  vv;
            if (cdo_object_rawget(opts, kk, &vv) && vv.tag == CDO_NUMBER) {
                *ints[i].v = (int)vv.as.number;
            }
            cdo_string_release(kk);
        }
        CdoString *kt = cdo_string_intern("text", 4);
        CdoValue  vt;
        if (cdo_object_rawget(opts, kt, &vt) && vt.tag == CDO_STRING && vt.as.string) {
            u32 n = vt.as.string->length;
            if (n >= sizeof(text)) n = sizeof(text) - 1;
            memcpy(text, vt.as.string->data, n);
            text[n] = 0;
        }
        cdo_string_release(kt);
        CdoString *km = cdo_string_intern("multiline", 9);
        CdoValue  vm_;
        if (cdo_object_rawget(opts, km, &vm_) && vm_.tag == CDO_BOOL && vm_.as.boolean) {
            style_extra |= 1;
        }
        cdo_string_release(km);
        CdoString *kp = cdo_string_intern("password", 8);
        CdoValue  vp;
        if (cdo_object_rawget(opts, kp, &vp) && vp.tag == CDO_BOOL && vp.as.boolean) {
            style_extra |= 2;
        }
        cdo_string_release(kp);
    } else if (argc > arg0 && args[arg0].tag == CDO_STRING) {
        parse_text_arg(vm, args[arg0], text, sizeof(text));
    }

    if (kind != KIND_FORM && parent_slot < 0) {
        forms_throw(vm, "%s: a parent form is required for child controls", who);
        return -1;
    }

    int slot = slot_alloc(kind, parent_slot);
    if (slot < 0) {
        forms_throw(vm, "%s: too many forms/controls (max %d)", who, FORMS_MAX_SLOTS);
        return -1;
    }

#if FORMS_HAVE_WIN32
    FormsCommand cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.op   = (kind == KIND_FORM) ? CMD_CREATE_FORM : CMD_CREATE_CONTROL;
    cmd.slot = slot;
    cmd.kind = kind;
    cmd.parent_slot = parent_slot;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.style_extra = style_extra;
    utf8_into_wide_buf(text, (int)strlen(text), cmd.text,
                       (int)(sizeof(cmd.text)/sizeof(cmd.text[0])));

    if (!post_create(&cmd, vm, who)) {
        FM_MUTEX_LOCK(&g_slot_mutex);
        g_slots[slot].alive = 0;
        FM_MUTEX_UNLOCK(&g_slot_mutex);
        return -1;
    }
#endif

    CandoValue inst = build_instance(vm, slot, kind, text,
                                     g_slots[slot].x, g_slots[slot].y,
                                     g_slots[slot].w, g_slots[slot].h);
    cando_vm_push(vm, inst);
    return 1;
}

static int native_form_create(CandoVM *vm, int argc, CandoValue *args)
{
    return generic_create(vm, KIND_FORM, argc, args, "forms.Form");
}

#define FORMS_DEFINE_CTOR(NameStr, KindEnum, FnName)                       \
    static int FnName(CandoVM *vm, int argc, CandoValue *args)             \
    {                                                                      \
        return generic_create(vm, KindEnum, argc, args,                    \
                              "forms." NameStr);                           \
    }

FORMS_DEFINE_CTOR("Button",      KIND_BUTTON,      native_button_create)
FORMS_DEFINE_CTOR("Label",       KIND_LABEL,       native_label_create)
FORMS_DEFINE_CTOR("TextBox",     KIND_TEXTBOX,     native_textbox_create)
FORMS_DEFINE_CTOR("CheckBox",    KIND_CHECKBOX,    native_checkbox_create)
FORMS_DEFINE_CTOR("RadioButton", KIND_RADIO,       native_radio_create)
FORMS_DEFINE_CTOR("ComboBox",    KIND_COMBOBOX,    native_combobox_create)
FORMS_DEFINE_CTOR("ListBox",     KIND_LISTBOX,     native_listbox_create)
FORMS_DEFINE_CTOR("Panel",       KIND_PANEL,       native_panel_create)
FORMS_DEFINE_CTOR("GroupBox",    KIND_GROUPBOX,    native_groupbox_create)
FORMS_DEFINE_CTOR("ProgressBar", KIND_PROGRESS,    native_progress_create)
FORMS_DEFINE_CTOR("TrackBar",    KIND_TRACKBAR,    native_trackbar_create)
FORMS_DEFINE_CTOR("NumericUpDown", KIND_NUMERIC,   native_numeric_create)
FORMS_DEFINE_CTOR("PictureBox",  KIND_PICTUREBOX,  native_picturebox_create)

/* =========================================================================
 * Methods on every control instance.  Most setters cross threads via
 * SendMessageW; Win32 marshals to the owning UI thread automatically.
 * ===================================================================== */

static int native_set_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setText");
    if (!s) return -1;
    char buf[1024] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
#if FORMS_HAVE_WIN32
    wchar_t *w = utf8_to_wide(buf, -1);
    if (s->hwnd && w) SendMessageW(s->hwnd, WM_SETTEXT, 0, (LPARAM)w);
    free(w);
#endif
    /* Mirror onto the script-side instance so getText without a round-trip
     * still returns the latest value. */
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) obj_set_string(o, "text", buf, (u32)strlen(buf));
    }
    cando_vm_push(vm, args[0]);          /* return self for chaining */
    return 1;
}

static int native_get_text(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getText");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        int len = (int)SendMessageW(s->hwnd, WM_GETTEXTLENGTH, 0, 0);
        wchar_t *wbuf = (wchar_t *)calloc((size_t)len + 1, sizeof(wchar_t));
        if (wbuf) {
            SendMessageW(s->hwnd, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)wbuf);
            char *u8 = wide_to_utf8(wbuf);
            free(wbuf);
            if (u8) {
                cando_vm_push(vm,
                    cando_string_value(cando_string_new(u8, (u32)strlen(u8))));
                free(u8);
                return 1;
            }
        }
    }
#endif
    cando_vm_push(vm, cando_string_value(cando_string_new("", 0)));
    return 1;
}

static int native_set_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSize");
    if (!s) return -1;
    int w = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : s->w;
    int h = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : s->h;
    s->w = w; s->h = h;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, w, h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) { obj_set_number(o, "width", (f64)w); obj_set_number(o, "height", (f64)h); }
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_location(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setLocation");
    if (!s) return -1;
    int x = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : s->x;
    int y = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : s->y;
    s->x = x; s->y = y;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, x, y, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) { obj_set_number(o, "x", (f64)x); obj_set_number(o, "y", (f64)y); }
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSize");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        RECT r; GetClientRect(s->hwnd, &r);
        s->w = r.right - r.left;
        s->h = r.bottom - r.top;
    }
#endif
    cando_vm_push(vm, cando_number((f64)s->w));
    cando_vm_push(vm, cando_number((f64)s->h));
    return 2;
}

static int native_get_location(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getLocation");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        RECT r; GetWindowRect(s->hwnd, &r);
        if (s->parent_slot >= 0 && g_slots[s->parent_slot].hwnd) {
            POINT p = { r.left, r.top };
            ScreenToClient(g_slots[s->parent_slot].hwnd, &p);
            s->x = p.x; s->y = p.y;
        } else {
            s->x = r.left; s->y = r.top;
        }
    }
#endif
    cando_vm_push(vm, cando_number((f64)s->x));
    cando_vm_push(vm, cando_number((f64)s->y));
    return 2;
}

static int native_show(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "show");
    if (!s) return -1;
    s->visible = 1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) ShowWindow(s->hwnd, SW_SHOWNORMAL);
    if (s->kind == KIND_FORM && s->hwnd) {
        UpdateWindow(s->hwnd);
        push_event(EV_SHOWN, (int)(s - g_slots), s->generation);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_hide(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "hide");
    if (!s) return -1;
    s->visible = 0;
#if FORMS_HAVE_WIN32
    if (s->hwnd) ShowWindow(s->hwnd, SW_HIDE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_visible(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setVisible");
    if (!s) return -1;
    bool visible = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->visible = visible;
#if FORMS_HAVE_WIN32
    if (s->hwnd) ShowWindow(s->hwnd, visible ? SW_SHOWNORMAL : SW_HIDE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_enabled(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setEnabled");
    if (!s) return -1;
    bool enabled = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->enabled = enabled;
#if FORMS_HAVE_WIN32
    if (s->hwnd) EnableWindow(s->hwnd, enabled ? TRUE : FALSE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_focus(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "focus");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetFocus(s->hwnd);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_destroy(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "destroy");
    if (!s) return -1;
    slot_teardown(s);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_is_open(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_bool(false)); return 1; }
    FormsSlot *s = slot_from_inst(vm, args[0]);
    bool open = s != NULL && s->alive;
#if FORMS_HAVE_WIN32
    if (open && s->hwnd) open = IsWindow(s->hwnd) ? true : false;
#endif
    cando_vm_push(vm, cando_bool(open));
    return 1;
}

static int native_set_parent(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setParent");
    if (!s) return -1;
    FormsSlot *p = (argc >= 2 && cando_is_object(args[1])) ?
                   slot_from_inst(vm, args[1]) : NULL;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        SetParent(s->hwnd, p ? p->hwnd : NULL);
    }
#endif
    s->parent_slot = p ? (int)(p - g_slots) : -1;
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Control-specific methods.  These no-op silently when called on the
 * wrong control kind so user code can stay loose ("just call setChecked
 * on anything that has a check") without writing kind guards.
 * ===================================================================== */

static int native_set_checked(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setChecked");
    if (!s) return -1;
    bool checked = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
#if FORMS_HAVE_WIN32
    if (s->hwnd && (s->kind == KIND_CHECKBOX || s->kind == KIND_RADIO)) {
        SendMessageW(s->hwnd, BM_SETCHECK,
                     (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_checked(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getChecked");
    if (!s) return -1;
    bool checked = false;
#if FORMS_HAVE_WIN32
    if (s->hwnd && (s->kind == KIND_CHECKBOX || s->kind == KIND_RADIO)) {
        checked = (SendMessageW(s->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
#endif
    cando_vm_push(vm, cando_bool(checked));
    return 1;
}

static int native_add_item(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "addItem");
    if (!s) return -1;
    char buf[1024] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
    int index = -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        wchar_t *w = utf8_to_wide(buf, -1);
        if (s->kind == KIND_LISTBOX) {
            index = (int)SendMessageW(s->hwnd, LB_ADDSTRING, 0, (LPARAM)w);
        } else if (s->kind == KIND_COMBOBOX) {
            index = (int)SendMessageW(s->hwnd, CB_ADDSTRING, 0, (LPARAM)w);
        }
        free(w);
    }
#endif
    cando_vm_push(vm, cando_number((f64)index));
    return 1;
}

static int native_clear_items(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearItems");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)  SendMessageW(s->hwnd, LB_RESETCONTENT, 0, 0);
        if (s->kind == KIND_COMBOBOX) SendMessageW(s->hwnd, CB_RESETCONTENT, 0, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getSelectedIndex");
    if (!s) return -1;
    int idx = -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)
            idx = (int)SendMessageW(s->hwnd, LB_GETCURSEL, 0, 0);
        else if (s->kind == KIND_COMBOBOX)
            idx = (int)SendMessageW(s->hwnd, CB_GETCURSEL, 0, 0);
    }
#endif
    cando_vm_push(vm, cando_number((f64)idx));
    return 1;
}

static int native_set_selected_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setSelectedIndex");
    if (!s) return -1;
    int idx = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_LISTBOX)
            SendMessageW(s->hwnd, LB_SETCURSEL, (WPARAM)idx, 0);
        else if (s->kind == KIND_COMBOBOX)
            SendMessageW(s->hwnd, CB_SETCURSEL, (WPARAM)idx, 0);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_value(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setValue");
    if (!s) return -1;
    int v = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_PROGRESS) {
            SendMessageW(s->hwnd, PBM_SETPOS, (WPARAM)v, 0);
        } else if (s->kind == KIND_TRACKBAR) {
            SendMessageW(s->hwnd, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)v);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_value(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getValue");
    if (!s) return -1;
    int v = 0;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_PROGRESS)
            v = (int)SendMessageW(s->hwnd, PBM_GETPOS, 0, 0);
        else if (s->kind == KIND_TRACKBAR)
            v = (int)SendMessageW(s->hwnd, TBM_GETPOS, 0, 0);
        else if (s->kind == KIND_NUMERIC || s->kind == KIND_TEXTBOX) {
            int len = (int)SendMessageW(s->hwnd, WM_GETTEXTLENGTH, 0, 0);
            wchar_t *wbuf = (wchar_t *)calloc((size_t)len + 1, sizeof(wchar_t));
            if (wbuf) {
                SendMessageW(s->hwnd, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)wbuf);
                v = (int)wcstol(wbuf, NULL, 10);
                free(wbuf);
            }
        }
    }
#endif
    cando_vm_push(vm, cando_number((f64)v));
    return 1;
}

static int native_set_range(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setRange");
    if (!s) return -1;
    int lo = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
    int hi = (argc >= 3 && args[2].tag == CDO_NUMBER) ? (int)args[2].as.number : 100;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        if (s->kind == KIND_PROGRESS) {
            SendMessageW(s->hwnd, PBM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
        } else if (s->kind == KIND_TRACKBAR) {
            SendMessageW(s->hwnd, TBM_SETRANGEMIN, (WPARAM)TRUE, (LPARAM)lo);
            SendMessageW(s->hwnd, TBM_SETRANGEMAX, (WPARAM)TRUE, (LPARAM)hi);
        }
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#endif /* !FORMS_MODULE_TEST_BUILD */

/* =========================================================================
 * Module entry point (chunk 1: minimal -- VERSION + platform fields).
 * Constructors and method tables land in subsequent chunks.
 * ===================================================================== */

#ifndef FORMS_MODULE_TEST_BUILD
CandoValue cando_module_init(CandoVM *vm)
{
    sync_init_once();
    cando_lib_meta_register(vm);

    /* All forms instances share a single meta table -- methods like
     * setText / setSize work uniformly across forms and every control. */
    CdoObject *meta = cando_lib_meta_table(vm, "forms_control");
    cando_lib_meta_define(vm, meta, "setText",      native_set_text);
    cando_lib_meta_define(vm, meta, "getText",      native_get_text);
    cando_lib_meta_define(vm, meta, "setTitle",     native_set_text);     /* alias */
    cando_lib_meta_define(vm, meta, "setSize",      native_set_size);
    cando_lib_meta_define(vm, meta, "getSize",      native_get_size);
    cando_lib_meta_define(vm, meta, "setLocation",  native_set_location);
    cando_lib_meta_define(vm, meta, "getLocation",  native_get_location);
    cando_lib_meta_define(vm, meta, "setPosition",  native_set_location); /* alias */
    cando_lib_meta_define(vm, meta, "getPosition",  native_get_location); /* alias */
    cando_lib_meta_define(vm, meta, "show",         native_show);
    cando_lib_meta_define(vm, meta, "hide",         native_hide);
    cando_lib_meta_define(vm, meta, "setVisible",   native_set_visible);
    cando_lib_meta_define(vm, meta, "setEnabled",   native_set_enabled);
    cando_lib_meta_define(vm, meta, "focus",        native_focus);
    cando_lib_meta_define(vm, meta, "destroy",      native_destroy);
    cando_lib_meta_define(vm, meta, "isOpen",       native_is_open);
    cando_lib_meta_define(vm, meta, "setParent",    native_set_parent);
    cando_lib_meta_define(vm, meta, "setChecked",   native_set_checked);
    cando_lib_meta_define(vm, meta, "getChecked",   native_get_checked);
    cando_lib_meta_define(vm, meta, "addItem",      native_add_item);
    cando_lib_meta_define(vm, meta, "clearItems",   native_clear_items);
    cando_lib_meta_define(vm, meta, "getSelectedIndex", native_get_selected_index);
    cando_lib_meta_define(vm, meta, "setSelectedIndex", native_set_selected_index);
    cando_lib_meta_define(vm, meta, "setValue",     native_set_value);
    cando_lib_meta_define(vm, meta, "getValue",     native_get_value);
    cando_lib_meta_define(vm, meta, "setRange",     native_set_range);

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   FORMS_MODULE_VERSION,
                   (u32)sizeof(FORMS_MODULE_VERSION) - 1);
#if FORMS_HAVE_WIN32
    obj_set_string(obj, "platform", "windows", 7);
    obj_set_bool  (obj, "supported", true);
#else
    obj_set_string(obj, "platform", "stub", 4);
    obj_set_bool  (obj, "supported", false);
#endif

    libutil_set_method(vm, obj, "Form",          native_form_create);
    libutil_set_method(vm, obj, "Button",        native_button_create);
    libutil_set_method(vm, obj, "Label",         native_label_create);
    libutil_set_method(vm, obj, "TextBox",       native_textbox_create);
    libutil_set_method(vm, obj, "CheckBox",      native_checkbox_create);
    libutil_set_method(vm, obj, "RadioButton",   native_radio_create);
    libutil_set_method(vm, obj, "ComboBox",      native_combobox_create);
    libutil_set_method(vm, obj, "ListBox",       native_listbox_create);
    libutil_set_method(vm, obj, "Panel",         native_panel_create);
    libutil_set_method(vm, obj, "GroupBox",      native_groupbox_create);
    libutil_set_method(vm, obj, "ProgressBar",   native_progress_create);
    libutil_set_method(vm, obj, "TrackBar",      native_trackbar_create);
    libutil_set_method(vm, obj, "NumericUpDown", native_numeric_create);
    libutil_set_method(vm, obj, "PictureBox",    native_picturebox_create);

    return tbl;
}
#endif /* FORMS_MODULE_TEST_BUILD */

/* =========================================================================
 * Test hooks.  The C unit tests #include this file with
 *   -DFORMS_MODULE_TEST_BUILD
 * and exercise the pure-C helpers (event queue, slot allocator) without
 * pulling in libcando or Win32.
 * ===================================================================== */

#ifdef FORMS_MODULE_TEST_BUILD
void forms_test_init_sync(void)            { sync_init_once(); }
void forms_test_event_queue_reset(void)    { event_queue_reset(); }
int  forms_test_event_queue_is_empty(void) { return event_queue_is_empty(); }
int  forms_test_event_queue_is_full(void)  { return event_queue_is_full(); }
void forms_test_event_push(int kind, int slot)
{
    FormsEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind = (EventKind)kind;
    ev.slot = slot;
    event_queue_push(ev);
}
int forms_test_event_pop(int *kind_out, int *slot_out)
{
    FormsEvent ev;
    if (!event_queue_try_pop(&ev)) return 0;
    if (kind_out) *kind_out = (int)ev.kind;
    if (slot_out) *slot_out = ev.slot;
    return 1;
}
int forms_test_slot_alloc(int kind, int parent)
{
    return slot_alloc((ControlKind)kind, parent);
}
void forms_test_slot_free(int slot)
{
    if (slot < 0 || slot >= FORMS_MAX_SLOTS) return;
    FM_MUTEX_LOCK(&g_slot_mutex);
    g_slots[slot].alive = 0;
    FM_MUTEX_UNLOCK(&g_slot_mutex);
}
int forms_test_slot_kind(int slot)
{
    if (slot < 0 || slot >= FORMS_MAX_SLOTS) return KIND_NONE;
    return (int)g_slots[slot].kind;
}
int forms_test_slot_generation(int slot)
{
    if (slot < 0 || slot >= FORMS_MAX_SLOTS) return -1;
    return g_slots[slot].generation;
}
void forms_test_event_push_full(int kind, int slot, int gen,
                                int i0, int i1, int i2,
                                double d0, double d1)
{
    FormsEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind       = (EventKind)kind;
    ev.slot       = slot;
    ev.generation = gen;
    ev.i0         = i0;
    ev.i1         = i1;
    ev.i2         = i2;
    ev.d0         = d0;
    ev.d1         = d1;
    event_queue_push(ev);
}
int forms_test_event_pop_full(int *kind_out, int *slot_out, int *gen_out,
                              int *i0_out, int *i1_out, int *i2_out,
                              double *d0_out, double *d1_out)
{
    FormsEvent ev;
    if (!event_queue_try_pop(&ev)) return 0;
    if (kind_out) *kind_out = (int)ev.kind;
    if (slot_out) *slot_out = ev.slot;
    if (gen_out)  *gen_out  = ev.generation;
    if (i0_out)   *i0_out   = ev.i0;
    if (i1_out)   *i1_out   = ev.i1;
    if (i2_out)   *i2_out   = ev.i2;
    if (d0_out)   *d0_out   = ev.d0;
    if (d1_out)   *d1_out   = ev.d1;
    return 1;
}
#endif /* FORMS_MODULE_TEST_BUILD */
