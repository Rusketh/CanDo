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

/* Pull in nanosleep / clock_gettime from <time.h> on POSIX. */
#if !defined(_WIN32) && !defined(_WIN64)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

/* libcando includes (production) / test stubs are now in
 * src/core/cando_compat.h so every src/core TU shares one source of
 * truth.  Falling back to local stubs in the test build keeps this
 * file self-contained for the existing `#include "forms_module.c"`
 * test pattern. */
#include "src/core/cando_compat.h"

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
 * its own; this matches the pattern in modules/window.  Now lives in
 * src/core/sync.h so every core TU shares a single source of truth.
 * ===================================================================== */

#include "src/core/sync.h"

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

/* Like obj_set_number but stamps the slot as FIELD_STATIC so script
 * code can't overwrite the value after construction.  Used for the
 * native identity fields (__forms_slot, __forms_gen, __forms_kind)
 * to harden slot_from_inst against a script that clobbers its own
 * handle by accident or design.  Phase 0.5 protection step.  When
 * libcando grows a true opaque-native-handle slot the identity will
 * move there entirely; for now FIELD_STATIC is the cleanest fix that
 * doesn't require a libcando ABI change. */
static void obj_set_number_static(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_STATIC);
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
 * Slot table + supporting helpers.
 *
 * Every form and every control is one slot.  Implementation now lives
 * in src/core/slots.{c,h}: the ControlKind enum, FormsSlot struct,
 * the FORMS_MAX_SLOTS / FORMS_*_KEY / FORMS_ANCHOR_* / FORMS_AUTOSIZE_*
 * / FORMS_CURSOR_* constants, the g_slots / g_slot_mutex storage, and
 * the slot allocator (slot_alloc / slot_alloc_locked).  Phase 0.5 will
 * replace the script-visible __forms_slot field with an opaque handle.
 *
 * Geometry helpers (DockRect, FORMS_DOCK_*, compute_dock_rect) live
 * in src/core/geom.h; colour helpers (NamedColor, g_named_colors,
 * ci_strneq, parse_hex_color, lookup_named_color, rgb_to_colorref,
 * colorref_to_rgb) live in src/core/color.h.
 * ===================================================================== */

#include "src/core/geom.h"
#include "src/core/color.h"
#include "src/core/slots.h"
#include "src/core/layout.h"

/* Forward decl: autosize_apply lives near the layout code far below;
 * the setText / setFont setters call it from anywhere in the file. */
static void autosize_apply(FormsSlot *s);

/* Forward decl: meta_name_for_kind is defined just before
 * cando_module_init (where the meta tables are also built); used by
 * build_instance to attach the right per-kind meta. */
#ifndef FORMS_MODULE_TEST_BUILD
static const char *meta_name_for_kind(ControlKind k);
#endif

/* =========================================================================
 * Event queue.
 *
 * Win32 callbacks post FormsEvent records into a global ring buffer;
 * the dispatcher thread drains it and calls user-supplied callbacks
 * via the child VM.  Implementation lives in src/core/events.{c,h}.
 * ===================================================================== */

#include "src/core/events.h"

/* =========================================================================
 * Manager-thread state machine (lazy-start on first form creation).
 *
 * The ManagerState enum lives in src/core/manager.h.  The Win32-bound
 * manager body (window class, message-only HWND, command queue,
 * thread bootstrap) stays inlined here until Phase 1 of the rewrite
 * splits the backend.
 * ===================================================================== */

#include "src/core/manager.h"
#include "src/core/dispatch.h"

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
        event_queue_init();
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
static void              layout_dock_children(int parent_slot);
static void              layout_anchor_children(int parent_slot);
static LPCWSTR           cursor_kind_to_idc(int kind);

/* utf8_to_wide / wide_to_utf8 / utf8_into_wide_buf moved to
 * src/core/textconv.{c,h}. */
#include "src/core/textconv.h"

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

/* Panel subclass.  STATIC's default wndproc swallows WM_COMMAND /
 * WM_HSCROLL / WM_VSCROLL — meaning a button (or any notifying child)
 * parented to a Panel never delivers BN_CLICKED to the top-level form,
 * so user-supplied onClick handlers silently never fire.  We work
 * around this by subclassing every panel: notification messages from
 * children walk up the parent chain via SendMessageW until they reach
 * a real form_wndproc that can translate them into FormsEvents.       */
static LRESULT CALLBACK panel_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    int slot = slot_from_hwnd(h);
    WNDPROC orig = (slot > 0 && slot < FORMS_MAX_SLOTS)
                   ? g_slots[slot].orig_proc : NULL;
    switch (msg) {
    case WM_HSCROLL:
    case WM_VSCROLL: {
        /* ScrollPanel owns its scroll bars and consumes scroll events
         * locally; plain Panel forwards them to its parent so e.g. a
         * TextBox child whose scroll bar was clicked still works. */
        if (slot > 0 && slot < FORMS_MAX_SLOTS &&
            g_slots[slot].kind == KIND_SCROLLPANEL &&
            g_slots[slot].auto_scroll) {
            int is_vert = (msg == WM_VSCROLL);
            int bar  = is_vert ? SB_VERT : SB_HORZ;
            SCROLLINFO si; memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask  = SIF_ALL;
            GetScrollInfo(h, bar, &si);
            int old_pos = si.nPos;
            int new_pos = old_pos;
            switch (LOWORD(w)) {
            case SB_LINEUP:        new_pos -= 16; break;
            case SB_LINEDOWN:      new_pos += 16; break;
            case SB_PAGEUP:        new_pos -= (int)si.nPage; break;
            case SB_PAGEDOWN:      new_pos += (int)si.nPage; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: new_pos = si.nTrackPos; break;
            case SB_TOP:           new_pos = si.nMin; break;
            case SB_BOTTOM:        new_pos = si.nMax; break;
            }
            int max_pos = si.nMax - (int)si.nPage + 1;
            if (max_pos < si.nMin) max_pos = si.nMin;
            if (new_pos < si.nMin) new_pos = si.nMin;
            if (new_pos > max_pos) new_pos = max_pos;
            if (new_pos != old_pos) {
                si.fMask = SIF_POS;
                si.nPos  = new_pos;
                SetScrollInfo(h, bar, &si, TRUE);
                int dx = is_vert ? 0 : (old_pos - new_pos);
                int dy = is_vert ? (old_pos - new_pos) : 0;
                ScrollWindowEx(h, dx, dy, NULL, NULL, NULL, NULL,
                               SW_SCROLLCHILDREN | SW_INVALIDATE);
                if (is_vert) g_slots[slot].scroll_y = new_pos;
                else         g_slots[slot].scroll_x = new_pos;
            }
            return 0;
        }
    } /* fall through to the parent-forwarding case */
    /* fall through */
    case WM_COMMAND:
    case WM_NOTIFY: {
        HWND parent = GetAncestor(h, GA_PARENT);
        if (parent) return SendMessageW(parent, msg, w, l);
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR: {
        /* Same forwarding for child colour overrides so panel children
         * pick up custom fore/back colours stored on the form. */
        HWND parent = GetAncestor(h, GA_PARENT);
        if (parent) return SendMessageW(parent, msg, w, l);
        break;
    }
    case WM_SIZE: {
        /* When a panel changes size -- because its own parent's dock
         * pass moved it, or the script called setSize/setDock on it --
         * its docked / anchored children need the same re-layout pass
         * the form's WndProc runs on its top-level resize.  Without
         * this, children that were docked while the panel was 0x0
         * would never grow into the panel's eventual real size.       */
        if (slot > 0 && slot < FORMS_MAX_SLOTS) {
            int cw = LOWORD(l), ch = HIWORD(l);
            g_slots[slot].w = cw;
            g_slots[slot].h = ch;
            /* Phase 2.4: dispatch via the layout vtable.  If a
             * container kind has registered a custom arrange (e.g.
             * FlowLayoutPanel, future Table/Split), use it
             * exclusively -- those layouts replace the default
             * dock+anchor pass.  Otherwise fall back to the standard
             * anchor + dock chain. */
            forms_arrange_fn arrange =
                forms_layout_for((int)g_slots[slot].kind);
            if (arrange) {
                arrange(slot, cw, ch);
            } else {
                layout_anchor_children(slot);
                layout_dock_children(slot);
            }
        }
        break;
    }
    }
    if (orig) return CallWindowProcW(orig, h, msg, w, l);
    return DefWindowProcW(h, msg, w, l);
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
    case KIND_SCROLLPANEL:
        /* Same backing class as Panel (STATIC SS_NOTIFY) plus WS_VSCROLL |
         * WS_HSCROLL so the scroll bars are present.  The script has to
         * call setAutoScroll(true) / setScrollSize(w, h) before they
         * actually become functional -- see panel_wndproc's WM_VSCROLL /
         * WM_HSCROLL handler for the scroll behaviour. */
        cls = L"STATIC";
        style |= SS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN;
        ex = WS_EX_CONTROLPARENT;
        break;
    case KIND_PROGRESS:
        cls = PROGRESS_CLASSW;
        if (c->style_extra & 4) style |= PBS_MARQUEE;
        if (c->style_extra & 8) style |= PBS_VERTICAL;
        if (c->style_extra & 16) style |= PBS_SMOOTH;
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
    case KIND_LINKLABEL:
        /* SysLink ships with comctl32 v6 (Windows XP+).  Falls back to a
         * regular STATIC if the control isn't registered, so the script
         * still gets a label even on bare-bones systems.               */
        cls = L"SysLink";
        style |= 0x00000001;        /* LWS_TRANSPARENT */
        break;
    case KIND_DATETIMEPICKER:
        cls = DATETIMEPICK_CLASSW;
        style |= WS_TABSTOP;
        if (c->style_extra & 1) style |= 0x0008;   /* DTS_SHOWNONE */
        if (c->style_extra & 2) style |= 0x0009;   /* DTS_TIMEFORMAT (clock) */
        break;
    case KIND_MONTHCALENDAR:
        cls = MONTHCAL_CLASSW;
        style |= WS_TABSTOP;
        break;
    case KIND_STATUSBAR:
        cls = STATUSCLASSNAMEW;
        /* Status bars dock to the parent's bottom by default (CCS_BOTTOM)
         * and resize themselves on the parent's WM_SIZE -- the geometry
         * we pass to CreateWindowExW is effectively ignored. */
        style |= SBARS_SIZEGRIP;
        break;
    case KIND_SPINNER:
        cls = UPDOWN_CLASSW;
        style |= UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_HOTTRACK |
                 UDS_ALIGNRIGHT | UDS_NOTHOUSANDS;
        break;
    case KIND_TABCONTROL:
        /* SysTabControl32 -- comctl32-provided tab strip.  TCS_TABS is
         * the default; WS_CLIPSIBLINGS keeps page repaints clean when
         * tabs are switched.  WS_EX_CONTROLPARENT lets Tab key
         * traverse children that live inside tab-page panels. */
        cls = WC_TABCONTROLW;
        style |= WS_CLIPSIBLINGS | WS_TABSTOP;
        ex = WS_EX_CONTROLPARENT;
        break;
    case KIND_FLOWLAYOUT:
        /* FlowLayoutPanel: same backing class as Panel (STATIC SS_NOTIFY).
         * The flow arrangement (wrap, direction) is purely a script-side
         * concern -- panel_wndproc consults forms_layout_for(KIND_FLOWLAYOUT)
         * on WM_SIZE and routes children through the registered arrange
         * function instead of the default dock+anchor pass. */
        cls = L"STATIC";
        style |= SS_NOTIFY;
        ex = WS_EX_CONTROLPARENT;
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

    /* Subclass panels so notifications from their children (BN_CLICKED,
     * EN_CHANGE, ...) reach the form's WndProc -- the default STATIC
     * wndproc would otherwise swallow them.  ScrollPanel rides the same
     * subclass (it routes WM_VSCROLL / WM_HSCROLL through there too).
     * FlowLayoutPanel uses the same subclass plus the layout vtable
     * (forms_layout_for) on WM_SIZE. */
    if (c->kind == KIND_PANEL || c->kind == KIND_SCROLLPANEL ||
        c->kind == KIND_FLOWLAYOUT) {
        WNDPROC prev = (WNDPROC)SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC, (LONG_PTR)panel_wndproc);
        s->orig_proc = prev;
    }
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
            /* Phase 2.4 -- per-kind arrange via the layout vtable.
             * Falls back to the default anchor + dock pass when no
             * specific arrange has been registered for this kind. */
            forms_arrange_fn arrange =
                forms_layout_for((int)g_slots[slot].kind);
            if (arrange) {
                arrange(slot, cw, ch);
            } else {
                layout_anchor_children(slot);
                layout_dock_children(slot);
            }
        }
        return 0;
    }
    case WM_SETCURSOR: {
        /* w = HWND of window receiving the cursor message.  When the mouse
         * is over a child, look up its slot and apply any cursor override. */
        HWND target = (HWND)w;
        int  cid    = slot_from_hwnd(target);
        if (cid > 0 && cid < FORMS_MAX_SLOTS && g_slots[cid].alive) {
            int ck = g_slots[cid].cursor_kind;
            if (ck != FORMS_CURSOR_DEFAULT) {
                HCURSOR hc = LoadCursorW(NULL, cursor_kind_to_idc(ck));
                if (hc) { SetCursor(hc); return TRUE; }
            }
        }
        if (slot > 0 && slot < FORMS_MAX_SLOTS &&
            g_slots[slot].cursor_kind != FORMS_CURSOR_DEFAULT) {
            HCURSOR hc = LoadCursorW(NULL,
                cursor_kind_to_idc(g_slots[slot].cursor_kind));
            if (hc) { SetCursor(hc); return TRUE; }
        }
        return DefWindowProcW(h, msg, w, l);
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
    case WM_NOTIFY: {
        /* Common-control notifications: SysLink clicks, DateTimePicker
         * changes, MonthCalendar selection changes, ListView/TreeView
         * (future). Notification code lives in NMHDR.code. */
        NMHDR *nh = (NMHDR *)l;
        if (nh && nh->idFrom > 0 && nh->idFrom < FORMS_MAX_SLOTS) {
            int cid = (int)nh->idFrom;
            if (g_slots[cid].alive) {
                int cgen = g_slots[cid].generation;
                ControlKind k = g_slots[cid].kind;
                /* The notification codes are defined as `(NM_FIRST - n)`,
                 * which the Win32 headers expand to a wrap-around UINT
                 * (e.g. NM_CLICK == 0xFFFFFFFE).  Compare on the raw UINT
                 * value to avoid sign-compare warnings under -Wextra. */
                UINT code = nh->code;
                if (k == KIND_LINKLABEL &&
                    (code == (UINT)NM_CLICK || code == (UINT)NM_RETURN)) {
                    push_event(EV_CLICK, cid, cgen);
                }
                /* DateTimePicker: DTN_DATETIMECHANGE = (DTN_FIRST - 19) */
                else if (k == KIND_DATETIMEPICKER &&
                         code == (UINT)(DTN_FIRST - 19)) {
                    push_event(EV_VALUE_CHANGED, cid, cgen);
                }
                /* MonthCalendar: MCN_SELCHANGE = (MCN_FIRST - 3) */
                else if (k == KIND_MONTHCALENDAR &&
                         code == (UINT)(MCN_FIRST - 3)) {
                    push_event(EV_SELECTION_CHANGED, cid, cgen);
                }
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR: {
        /* w = HDC of the control, l = child HWND.  If the child slot
         * has a custom fore/back colour, apply it and return the
         * matching brush.  Otherwise let the default proc paint. */
        HWND child = (HWND)l;
        int  cid   = slot_from_hwnd(child);
        if (cid > 0 && cid < FORMS_MAX_SLOTS && g_slots[cid].alive) {
            FormsSlot *cs = &g_slots[cid];
            HDC hdc = (HDC)w;
            if (cs->has_fore) SetTextColor(hdc, (COLORREF)cs->fore_color);
            if (cs->has_back) {
                SetBkColor(hdc, (COLORREF)cs->back_color);
                if (!cs->back_brush)
                    cs->back_brush = CreateSolidBrush((COLORREF)cs->back_color);
                if (cs->back_brush) return (LRESULT)cs->back_brush;
            }
            if (cs->has_fore && !cs->has_back) {
                /* Make the bg transparent so only the fore colour
                 * differs from the system default -- otherwise text
                 * gets a coloured rectangle behind it. */
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetStockObject(NULL_BRUSH);
            }
        }
        break;
    }
    case WM_ERASEBKGND: {
        /* Paint the form's own background colour if set. */
        if (slot > 0 && slot < FORMS_MAX_SLOTS && g_slots[slot].has_back) {
            FormsSlot *fs = &g_slots[slot];
            if (!fs->back_brush)
                fs->back_brush = CreateSolidBrush((COLORREF)fs->back_color);
            if (fs->back_brush) {
                RECT r; GetClientRect(h, &r);
                FillRect((HDC)w, &r, fs->back_brush);
                return 1;
            }
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        if (slot > 0 && slot < FORMS_MAX_SLOTS) {
            FormsSlot *fs = &g_slots[slot];
            MINMAXINFO *mmi = (MINMAXINFO *)l;
            if (fs->has_min_size) {
                if (fs->min_w > 0) mmi->ptMinTrackSize.x = fs->min_w;
                if (fs->min_h > 0) mmi->ptMinTrackSize.y = fs->min_h;
            }
            if (fs->has_max_size) {
                if (fs->max_w > 0) mmi->ptMaxTrackSize.x = fs->max_w;
                if (fs->max_h > 0) mmi->ptMaxTrackSize.y = fs->max_h;
            }
            return 0;
        }
        break;
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

/* Exported module shutdown hook.  cando_vm_destroy looks this symbol up
 * via dlsym and calls it before unloading the module so the manager
 * thread is stopped while the .so is still mapped.  Idempotent. */
void cando_module_shutdown(void)
{
    mgr_shutdown();
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

/* event_callback_name moved to src/core/dispatch.{c,h}. */

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
    if (!inst) {
        fprintf(stderr, "[forms] dispatch %s: no inst for slot %d\n",
                name, ev.slot);
        return;
    }

    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   field;
    bool       have = cdo_object_get(inst, key, &field);
    cdo_string_release(key);
    if (!have) return;          /* no handler installed -- silent */

    bool callable = (field.tag == CDO_FUNCTION) ||
                    (field.tag == CDO_NATIVE)   ||
                    (field.tag == CDO_OBJECT &&
                     field.as.object &&
                     field.as.object->kind == OBJ_FUNCTION);
    if (!callable) {
        fprintf(stderr, "[forms] dispatch %s: handler is not callable "
                "(tag=%d)\n", name, (int)field.tag);
        return;
    }

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
    /* Surface uncaught errors from the user handler in the canonical
     * "cando: uncaught error in <ctx>: <msg>" form.  The message already
     * carries the stack trace produced by the VM, and log_uncaught
     * clears has_error / error_vals so the next dispatch starts clean. */
    if (g_dispatch_vm.has_error) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "forms %s handler", name);
        cando_vm_log_uncaught(&g_dispatch_vm, ctx);
    }
    g_dispatch_vm.stack_top   = g_dispatch_vm.stack;
    g_dispatch_vm.frame_count = 0;
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
 *
 * Phase 1.2a: slot_from_inst (and the shared error / arg helpers
 * forms_throw, require_supported, arg_self, parse_text_arg) now live
 * in src/controls/ctl_common.{c,h}.  slot_teardown stays here for
 * the moment -- it's Win32-bound (HWND destroy + manager command
 * round-trip) and moves with the backend split in Phase 1+.
 * ===================================================================== */

#include "src/controls/ctl_common.h"

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
    if (s->back_brush) { DeleteObject(s->back_brush); s->back_brush = NULL; }
    if (s->hfont)      { DeleteObject(s->hfont);      s->hfont      = NULL; }
    if (s->hicon_small) { DestroyIcon(s->hicon_small); s->hicon_small = NULL; }
    if (s->hicon_big)   { DestroyIcon(s->hicon_big);   s->hicon_big   = NULL; }
    if (s->tooltip_hwnd) { DestroyWindow(s->tooltip_hwnd); s->tooltip_hwnd = NULL; }
#endif
    if (s->tooltip) { free(s->tooltip); s->tooltip = NULL; }
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

/* forms_throw, require_supported, arg_self moved to
 * src/controls/ctl_common.{c,h}. */

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

    /* Native identity fields -- FIELD_STATIC so script code cannot
     * silently overwrite them and de-sync from the slot table.  Phase
     * 0.5 hardening step (REWRITE_PLAN.md). */
    obj_set_number_static(o, FORMS_SLOT_KEY, (f64)slot);
    obj_set_number_static(o, FORMS_GEN_KEY,  (f64)g_slots[slot].generation);
    obj_set_number_static(o, FORMS_KIND_KEY, (f64)kind);
    if (text) obj_set_string(o, "text", text, (u32)strlen(text));
    obj_set_number(o, "x", (f64)x);
    obj_set_number(o, "y", (f64)y);
    obj_set_number(o, "width",  (f64)w);
    obj_set_number(o, "height", (f64)h);

    /* Phase 1.1: per-kind meta table.  cando_lib_meta_attach wires
     * `o.__index = _meta.<name>`; the per-kind tables chain their
     * own __index to forms_control_base in cando_module_init. */
    cando_lib_meta_attach(vm, o, meta_name_for_kind(kind));

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

/* parse_text_arg moved to src/controls/ctl_common.{c,h}. */

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
        CdoString *kmar = cdo_string_intern("marquee", 7);
        CdoValue  vmar;
        if (cdo_object_rawget(opts, kmar, &vmar) && vmar.tag == CDO_BOOL && vmar.as.boolean) {
            style_extra |= 4;
        }
        cdo_string_release(kmar);
        CdoString *kvert = cdo_string_intern("vertical", 8);
        CdoValue  vvert;
        if (cdo_object_rawget(opts, kvert, &vvert) && vvert.tag == CDO_BOOL && vvert.as.boolean) {
            style_extra |= 8;
        }
        cdo_string_release(kvert);
        CdoString *ksm = cdo_string_intern("smooth", 6);
        CdoValue  vsm;
        if (cdo_object_rawget(opts, ksm, &vsm) && vsm.tag == CDO_BOOL && vsm.as.boolean) {
            style_extra |= 16;
        }
        cdo_string_release(ksm);
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
FORMS_DEFINE_CTOR("LinkLabel",   KIND_LINKLABEL,   native_linklabel_create)
FORMS_DEFINE_CTOR("DateTimePicker", KIND_DATETIMEPICKER, native_datetime_create)
FORMS_DEFINE_CTOR("MonthCalendar",  KIND_MONTHCALENDAR,  native_calendar_create)
FORMS_DEFINE_CTOR("StatusBar",   KIND_STATUSBAR,   native_statusbar_create)
FORMS_DEFINE_CTOR("Spinner",     KIND_SPINNER,     native_spinner_create)
/* Phase 2 container constructors. */
FORMS_DEFINE_CTOR("ScrollPanel",     KIND_SCROLLPANEL, native_scrollpanel_create)
FORMS_DEFINE_CTOR("TabControl",      KIND_TABCONTROL,  native_tabcontrol_create)
FORMS_DEFINE_CTOR("FlowLayoutPanel", KIND_FLOWLAYOUT,  native_flowlayout_create)

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
    autosize_apply(s);
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

static int native_set_width(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setWidth");
    if (!s) return -1;
    int w = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : s->w;
    s->w = w;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, w, s->h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) obj_set_number(o, "width", (f64)w);
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_height(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setHeight");
    if (!s) return -1;
    int h = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : s->h;
    s->h = h;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, s->w, h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) obj_set_number(o, "height", (f64)h);
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_width(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getWidth");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        RECT r; GetClientRect(s->hwnd, &r);
        s->w = r.right - r.left;
        s->h = r.bottom - r.top;
    }
#endif
    cando_vm_push(vm, cando_number((f64)s->w));
    return 1;
}

static int native_get_height(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getHeight");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        RECT r; GetClientRect(s->hwnd, &r);
        s->w = r.right - r.left;
        s->h = r.bottom - r.top;
    }
#endif
    cando_vm_push(vm, cando_number((f64)s->h));
    return 1;
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

/* native_set_checked + native_get_checked moved to
 * src/controls/ctl_checkbox.{c,h}. */
#include "src/controls/ctl_checkbox.h"

/* ListBox + ComboBox item natives (addItem, removeItem, clearItems,
 * getItem, getItems, getItemCount, getSelectedIndex, setSelectedIndex)
 * plus the list_item_text helper now live in
 * src/controls/ctl_listbox.{c,h}. */
#include "src/controls/ctl_listbox.h"

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

/* =========================================================================
 * Colours (script-facing).  setForeColor / setBackColor accept any of:
 *   - three numbers (r, g, b)            -- 0..255 each
 *   - one packed integer 0xRRGGBB
 *   - a hex string ("#RRGGBB", "#RGB", or "#AARRGGBB" -- alpha ignored)
 *   - a CSS-style named colour ("red", "cornflowerblue", ...)
 *
 * The pure-C parsing helpers (parse_hex_color, lookup_named_color, ...)
 * live above, near compute_dock_rect; they're shared with the C unit
 * test build that strips libcando + Win32.
 * ===================================================================== */

static unsigned int parse_color_args(CandoValue *args, int argc, int start,
                                     unsigned int default_colorref)
{
    if (argc <= start) return default_colorref;
    /* Single string-arg: "#RRGGBB", "#RGB" or named colour. */
    if (argc == start + 1 && args[start].tag == CDO_STRING && args[start].as.string) {
        const char *s = args[start].as.string->data;
        u32 n = args[start].as.string->length;
        unsigned int rgb;
        if (parse_hex_color(s, n, &rgb))   return rgb_to_colorref(rgb);
        if (lookup_named_color(s, n, &rgb)) return rgb_to_colorref(rgb);
        return default_colorref;
    }
    /* Single-arg packed 0xRRGGBB */
    if (argc == start + 1 && args[start].tag == CDO_NUMBER) {
        unsigned int rgb = (unsigned int)args[start].as.number & 0xFFFFFFu;
        return rgb_to_colorref(rgb);
    }
    /* Three-arg (r, g, b) -- match WinForms Color.FromArgb. */
    int r = (argc > start     && args[start].tag     == CDO_NUMBER) ? (int)args[start].as.number     : 0;
    int g = (argc > start + 1 && args[start + 1].tag == CDO_NUMBER) ? (int)args[start + 1].as.number : 0;
    int b = (argc > start + 2 && args[start + 2].tag == CDO_NUMBER) ? (int)args[start + 2].as.number : 0;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (unsigned int)(((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r);
}

static int native_set_fore_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setForeColor");
    if (!s) return -1;
    s->fore_color = parse_color_args(args, argc, 1, 0);
    s->has_fore   = 1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) InvalidateRect(s->hwnd, NULL, TRUE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_back_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setBackColor");
    if (!s) return -1;
    s->back_color = parse_color_args(args, argc, 1, 0);
    s->has_back   = 1;
#if FORMS_HAVE_WIN32
    if (s->back_brush) { DeleteObject(s->back_brush); s->back_brush = NULL; }
    if (s->hwnd) InvalidateRect(s->hwnd, NULL, TRUE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_clear_fore_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearForeColor");
    if (!s) return -1;
    s->has_fore = 0;
#if FORMS_HAVE_WIN32
    if (s->hwnd) InvalidateRect(s->hwnd, NULL, TRUE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_clear_back_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearBackColor");
    if (!s) return -1;
    s->has_back = 0;
#if FORMS_HAVE_WIN32
    if (s->back_brush) { DeleteObject(s->back_brush); s->back_brush = NULL; }
    if (s->hwnd) InvalidateRect(s->hwnd, NULL, TRUE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* Return the current fore/back colour as 0xRRGGBB, or NULL when no
 * override is in effect.  Useful for round-tripping ("save & restore"
 * theming) and for adoption-by-children patterns. */
static int native_get_fore_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getForeColor");
    if (!s) return -1;
    if (!s->has_fore) { cando_vm_push(vm, cando_null()); return 1; }
    cando_vm_push(vm, cando_number((f64)colorref_to_rgb(s->fore_color)));
    return 1;
}

static int native_get_back_color(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getBackColor");
    if (!s) return -1;
    if (!s->has_back) { cando_vm_push(vm, cando_null()); return 1; }
    cando_vm_push(vm, cando_number((f64)colorref_to_rgb(s->back_color)));
    return 1;
}

/* =========================================================================
 * Fonts.
 *
 * Font state is stored on the slot: face name (UTF-8, max 63 chars),
 * point size, and four boolean style flags (bold / italic / underline /
 * strikeout).  When any field changes we destroy the cached HFONT and
 * rebuild it on the next setter call before sending WM_SETFONT.
 *
 * Setters are forgiving: setFont("Segoe UI"), setFont("Segoe UI", 14),
 * setFont({face="Segoe UI", size=14, bold=true, italic=true}) all work.
 * Calling setBold(true) without a prior setFont takes the platform
 * default face/size and just toggles weight on top.
 * ===================================================================== */

#if FORMS_HAVE_WIN32
/* Pull the current default-GUI-font face/size into out_face/out_size so
 * setBold/setItalic can build on top of them when the script never
 * specified a face.  Falls back to "Segoe UI" 9pt if anything fails. */
static void default_gui_font_metrics(char *out_face, int out_face_cap,
                                     int *out_size)
{
    HFONT def = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf; memset(&lf, 0, sizeof(lf));
    int ok = (def && GetObjectW(def, sizeof(lf), &lf) > 0);
    if (ok) {
        char *u8 = wide_to_utf8(lf.lfFaceName);
        if (u8) {
            int n = (int)strlen(u8);
            if (n >= out_face_cap) n = out_face_cap - 1;
            memcpy(out_face, u8, (size_t)n);
            out_face[n] = 0;
            free(u8);
        } else {
            snprintf(out_face, (size_t)out_face_cap, "Segoe UI");
        }
        /* lfHeight is in logical units; convert to a positive point size.
         * Negative => character cell height in pixels. */
        HDC hdc = GetDC(NULL);
        int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) ReleaseDC(NULL, hdc);
        int height_px = (int)(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight);
        if (height_px <= 0) height_px = 12;
        *out_size = (int)((height_px * 72 + dpi / 2) / dpi);
        if (*out_size <= 0) *out_size = 9;
    } else {
        snprintf(out_face, (size_t)out_face_cap, "Segoe UI");
        *out_size = 9;
    }
}

/* Recreate s->hfont from the slot's current font fields and apply it via
 * WM_SETFONT so the control redraws with the new typeface. */
static void apply_font(FormsSlot *s)
{
    if (!s) return;
    if (s->hfont) { DeleteObject(s->hfont); s->hfont = NULL; }
    if (!s->has_font) return;

    LOGFONTW lf; memset(&lf, 0, sizeof(lf));
    HDC hdc = GetDC(NULL);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
    int size = s->font_size > 0 ? s->font_size : 9;
    /* Win32 wants a negative lfHeight to specify character height in
     * pixels (rather than cell height -- the practical font-size you
     * see in office apps).  -MulDiv(points, dpi, 72). */
    lf.lfHeight  = -((size * dpi + 36) / 72);
    lf.lfWeight  = s->font_bold      ? FW_BOLD : FW_NORMAL;
    lf.lfItalic  = s->font_italic    ? TRUE : FALSE;
    lf.lfUnderline = s->font_underline ? TRUE : FALSE;
    lf.lfStrikeOut = s->font_strikeout ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

    const char *face = s->font_face[0] ? s->font_face : "Segoe UI";
    wchar_t *wface = utf8_to_wide(face, -1);
    if (wface) {
        size_t copy = wcslen(wface);
        if (copy > LF_FACESIZE - 1) copy = LF_FACESIZE - 1;
        memcpy(lf.lfFaceName, wface, copy * sizeof(wchar_t));
        lf.lfFaceName[copy] = 0;
        free(wface);
    }
    s->hfont = CreateFontIndirectW(&lf);
    if (s->hwnd && s->hfont) {
        SendMessageW(s->hwnd, WM_SETFONT, (WPARAM)s->hfont, TRUE);
        InvalidateRect(s->hwnd, NULL, TRUE);
    }
}
#else
static void apply_font(FormsSlot *s) { (void)s; }
#endif

/* Common helper: parse a setFont options object and write into slot. */
static void font_options_into_slot(CandoVM *vm, CdoObject *opts, FormsSlot *s)
{
    (void)vm;
    if (!opts) return;
    /* face / family: string face name */
    {
        CdoString *k = cdo_string_intern("face", 4);
        CdoValue v;
        if (cdo_object_rawget(opts, k, &v) && v.tag == CDO_STRING && v.as.string) {
            u32 n = v.as.string->length;
            if (n >= sizeof(s->font_face)) n = sizeof(s->font_face) - 1;
            memcpy(s->font_face, v.as.string->data, n);
            s->font_face[n] = 0;
        }
        cdo_string_release(k);
    }
    {
        CdoString *k = cdo_string_intern("family", 6);
        CdoValue v;
        if (cdo_object_rawget(opts, k, &v) && v.tag == CDO_STRING && v.as.string) {
            u32 n = v.as.string->length;
            if (n >= sizeof(s->font_face)) n = sizeof(s->font_face) - 1;
            memcpy(s->font_face, v.as.string->data, n);
            s->font_face[n] = 0;
        }
        cdo_string_release(k);
    }
    /* size: numeric point size */
    {
        CdoString *k = cdo_string_intern("size", 4);
        CdoValue v;
        if (cdo_object_rawget(opts, k, &v) && v.tag == CDO_NUMBER) {
            int sz = (int)v.as.number;
            if (sz > 0) s->font_size = sz;
        }
        cdo_string_release(k);
    }
    struct { const char *k; int *v; } flags[] = {
        { "bold",      &s->font_bold      },
        { "italic",    &s->font_italic    },
        { "underline", &s->font_underline },
        { "strikeout", &s->font_strikeout },
    };
    for (size_t i = 0; i < sizeof(flags)/sizeof(flags[0]); i++) {
        CdoString *k = cdo_string_intern(flags[i].k, (u32)strlen(flags[i].k));
        CdoValue v;
        if (cdo_object_rawget(opts, k, &v) && v.tag == CDO_BOOL) {
            *flags[i].v = v.as.boolean ? 1 : 0;
        }
        cdo_string_release(k);
    }
}

/* Make sure font_face / font_size are populated.  When the script has
 * never set a face we fall back to the platform default GUI font so the
 * resulting HFONT looks native. */
static void font_ensure_defaults(FormsSlot *s)
{
    if (!s) return;
    if (s->font_face[0] && s->font_size > 0) return;
#if FORMS_HAVE_WIN32
    char  face[64] = {0};
    int   size = 0;
    default_gui_font_metrics(face, (int)sizeof(face), &size);
    if (!s->font_face[0]) {
        size_t n = strlen(face);
        if (n >= sizeof(s->font_face)) n = sizeof(s->font_face) - 1;
        memcpy(s->font_face, face, n); s->font_face[n] = 0;
    }
    if (s->font_size <= 0) s->font_size = size > 0 ? size : 9;
#else
    if (!s->font_face[0]) {
        const char *fallback = "Segoe UI";
        size_t n = strlen(fallback);
        memcpy(s->font_face, fallback, n); s->font_face[n] = 0;
    }
    if (s->font_size <= 0) s->font_size = 9;
#endif
}

static int native_set_font(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setFont");
    if (!s) return -1;
    /* Argument forms:
     *   setFont("Segoe UI")
     *   setFont("Segoe UI", 12)
     *   setFont("Segoe UI", 12, "bold")
     *   setFont(12)                    -- size only, keep current face
     *   setFont({face="...", size=12, bold=true, italic=true})
     */
    if (argc >= 2) {
        if (cando_is_object(args[1])) {
            CdoObject *opts = cando_bridge_resolve(vm, args[1].as.handle);
            font_options_into_slot(vm, opts, s);
        } else {
            int idx = 1;
            if (args[idx].tag == CDO_STRING && args[idx].as.string) {
                u32 n = args[idx].as.string->length;
                if (n >= sizeof(s->font_face)) n = sizeof(s->font_face) - 1;
                memcpy(s->font_face, args[idx].as.string->data, n);
                s->font_face[n] = 0;
                idx++;
            }
            if (argc > idx && args[idx].tag == CDO_NUMBER) {
                int sz = (int)args[idx].as.number;
                if (sz > 0) s->font_size = sz;
                idx++;
            }
            /* Optional trailing style string: "bold", "italic", "bold italic", ... */
            if (argc > idx && args[idx].tag == CDO_STRING && args[idx].as.string) {
                const char *t = args[idx].as.string->data;
                u32 n = args[idx].as.string->length;
                s->font_bold = s->font_italic = s->font_underline = 0;
                if (n >= 4 && strstr((const char *)t, "bold"))      s->font_bold      = 1;
                if (n >= 6 && strstr((const char *)t, "italic"))    s->font_italic    = 1;
                if (n >= 9 && strstr((const char *)t, "underline")) s->font_underline = 1;
                (void)n;
            }
        }
    }
    s->has_font = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_font_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setFontSize");
    if (!s) return -1;
    int sz = (argc >= 2 && args[1].tag == CDO_NUMBER) ? (int)args[1].as.number : 0;
    if (sz > 0) s->font_size = sz;
    s->has_font = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_bold(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setBold");
    if (!s) return -1;
    bool b = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->font_bold = b ? 1 : 0;
    s->has_font  = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_italic(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setItalic");
    if (!s) return -1;
    bool b = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->font_italic = b ? 1 : 0;
    s->has_font    = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_underline(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setUnderline");
    if (!s) return -1;
    bool b = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->font_underline = b ? 1 : 0;
    s->has_font       = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_set_strikeout(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setStrikeout");
    if (!s) return -1;
    bool b = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->font_strikeout = b ? 1 : 0;
    s->has_font       = 1;
    font_ensure_defaults(s);
    apply_font(s);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* Drop the slot's font override -- the control reverts to the parent
 * form's font (set up via WM_SETFONT at creation time). */
static int native_clear_font(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "clearFont");
    if (!s) return -1;
    s->has_font       = 0;
    s->font_face[0]   = 0;
    s->font_size      = 0;
    s->font_bold      = 0;
    s->font_italic    = 0;
    s->font_underline = 0;
    s->font_strikeout = 0;
#if FORMS_HAVE_WIN32
    if (s->hfont) { DeleteObject(s->hfont); s->hfont = NULL; }
    if (s->hwnd) {
        /* Reapply the parent's font so the control looks native again. */
        HFONT pf = NULL;
        if (s->parent_slot > 0 && s->parent_slot < FORMS_MAX_SLOTS &&
            g_slots[s->parent_slot].hwnd) {
            pf = (HFONT)SendMessageW(g_slots[s->parent_slot].hwnd,
                                     WM_GETFONT, 0, 0);
        }
        if (!pf) pf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(s->hwnd, WM_SETFONT, (WPARAM)pf, TRUE);
        InvalidateRect(s->hwnd, NULL, TRUE);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* Return the current font as an object {face, size, bold, italic,
 * underline, strikeout}, or NULL if no override is set. */
static int native_get_font(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getFont");
    if (!s) return -1;
    if (!s->has_font) { cando_vm_push(vm, cando_null()); return 1; }
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    obj_set_string(o, "face", s->font_face,
                   (u32)strlen(s->font_face[0] ? s->font_face : ""));
    obj_set_number(o, "size", (f64)s->font_size);
    obj_set_bool  (o, "bold",      s->font_bold      ? true : false);
    obj_set_bool  (o, "italic",    s->font_italic    ? true : false);
    obj_set_bool  (o, "underline", s->font_underline ? true : false);
    obj_set_bool  (o, "strikeout", s->font_strikeout ? true : false);
    cando_vm_push(vm, v);
    return 1;
}

/* =========================================================================
 * Form-only extras: opacity, top-most, centering, min/max sizes,
 * border style.  Most of these are no-ops on non-forms (silent).
 * ===================================================================== */

/* Form-only natives setOpacity/getOpacity/setTopMost/center/
 * setMinSize/setMaxSize moved to src/controls/ctl_form.{c,h}. */
#include "src/controls/ctl_form.h"

/* Phase 2 container natives. */
#include "src/controls/ctl_scrollpanel.h"
#include "src/controls/ctl_tabcontrol.h"
#include "src/controls/ctl_flowlayout.h"

/* parse_border_style moved to src/core/layout.{c,h}. */

static int native_set_border_style(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setBorderStyle");
    if (!s) return -1;
    int style = (argc >= 2) ? parse_border_style(args[1]) : 0;
    if (style == 0) {
        cando_vm_push(vm, args[0]);
        return 1;
    }
    s->border_style_set = 1;
    s->border_style     = style;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        LONG st = GetWindowLongW(s->hwnd, GWL_STYLE);
        LONG ex = GetWindowLongW(s->hwnd, GWL_EXSTYLE);
        st &= ~WS_BORDER;
        ex &= ~WS_EX_CLIENTEDGE;
        ex &= ~WS_EX_STATICEDGE;
        if (style == 2) st |= WS_BORDER;
        if (style == 3) ex |= WS_EX_CLIENTEDGE;
        SetWindowLongW(s->hwnd, GWL_STYLE,   st);
        SetWindowLongW(s->hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * General extras shared by every control: z-order helpers, refresh,
 * getEnabled / getVisible accessors.
 * ===================================================================== */

static int native_bring_to_front(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "bringToFront");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) BringWindowToTop(s->hwnd);
    if (s->hwnd) SetWindowPos(s->hwnd, HWND_TOP, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_send_to_back(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "sendToBack");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) SetWindowPos(s->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* Force the control to redraw, mirroring System.Windows.Forms.Control.Refresh. */
static int native_refresh(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "refresh");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        InvalidateRect(s->hwnd, NULL, TRUE);
        UpdateWindow(s->hwnd);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_enabled(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getEnabled");
    if (!s) return -1;
    bool enabled = s->enabled ? true : false;
#if FORMS_HAVE_WIN32
    if (s->hwnd) enabled = IsWindowEnabled(s->hwnd) ? true : false;
#endif
    cando_vm_push(vm, cando_bool(enabled));
    return 1;
}

static int native_get_visible(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getVisible");
    if (!s) return -1;
    bool visible = s->visible ? true : false;
#if FORMS_HAVE_WIN32
    if (s->hwnd) visible = IsWindowVisible(s->hwnd) ? true : false;
#endif
    cando_vm_push(vm, cando_bool(visible));
    return 1;
}

#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
static void layout_dock_children(int parent_slot)
{
    if (parent_slot <= 0 || parent_slot >= FORMS_MAX_SLOTS) return;
    FormsSlot *p = &g_slots[parent_slot];
    if (!p->alive || !p->hwnd) return;
    RECT r;
    if (!GetClientRect(p->hwnd, &r)) return;

    int left = r.left, top = r.top, right = r.right, bottom = r.bottom;
    int fill_slot = -1;

    /* Two-pass: first non-fill in declaration order, then the lone fill
     * (if any) gets whatever's left. */
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot || !c->hwnd) continue;
        if (c->dock == FORMS_DOCK_NONE) continue;
        if (c->dock == FORMS_DOCK_FILL) { fill_slot = i; continue; }

        DockRect out;
        compute_dock_rect(c->dock, c->w, c->h,
                          &left, &top, &right, &bottom, &out);
        SetWindowPos(c->hwnd, NULL, out.x, out.y, out.w, out.h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        c->x = out.x; c->y = out.y; c->w = out.w; c->h = out.h;
    }
    if (fill_slot > 0) {
        FormsSlot *c = &g_slots[fill_slot];
        int w = right - left, h = bottom - top;
        SetWindowPos(c->hwnd, NULL, left, top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        c->x = left; c->y = top; c->w = w; c->h = h;
    }
}
#endif

/* =========================================================================
 * Preferred-size measurement.  Picks a sensible width/height that "fits"
 * the control's contents -- text + padding for buttons / labels / etc.,
 * the bounding box of children for containers (forms, panels, group
 * boxes), the widest item for ListBox / ComboBox.
 * ===================================================================== */

#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
static int measure_text_extent(FormsSlot *s, const wchar_t *text,
                               int *out_w, int *out_h)
{
    *out_w = 0; *out_h = 0;
    if (!s->hwnd) return 0;
    HDC hdc = GetDC(s->hwnd);
    if (!hdc) return 0;
    HFONT hf = s->has_font ? s->hfont : NULL;
    if (!hf) hf = (HFONT)SendMessageW(s->hwnd, WM_GETFONT, 0, 0);
    HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
    RECT r = { 0, 0, 0, 0 };
    UINT flags = DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS;
    /* Multi-line edits / labels: respect line breaks. */
    if (s->kind == KIND_TEXTBOX || s->kind == KIND_LABEL ||
        s->kind == KIND_LINKLABEL) {
        flags |= DT_WORDBREAK;
    } else {
        flags |= DT_SINGLELINE;
    }
    DrawTextW(hdc, text && text[0] ? text : L"M", -1, &r, flags);
    *out_w = r.right - r.left;
    *out_h = r.bottom - r.top;
    if (hf && old) SelectObject(hdc, old);
    ReleaseDC(s->hwnd, hdc);
    return 1;
}

/* children_bbox moved to src/core/layout.{c,h}. */

static void compute_preferred_size(FormsSlot *s, int *out_w, int *out_h)
{
    int w = s->w, h = s->h;
    if (!s->hwnd) { *out_w = w; *out_h = h; return; }
    int slot = (int)(s - g_slots);

    /* Containers: bounding box of children + a small margin. */
    if (s->kind == KIND_FORM   || s->kind == KIND_PANEL ||
        s->kind == KIND_GROUPBOX) {
        int cw = 0, ch = 0;
        int has_children = children_bbox(slot, &cw, &ch);
        /* Per-kind defaults; the script-side padding fields add on top. */
        int default_pad_x = 8, default_pad_y = 8;
        int extra_top = 0;
        if (s->kind == KIND_GROUPBOX) extra_top = 16;     /* title area      */
        if (has_children) {
            cw += default_pad_x + s->pad_l + s->pad_r;
            ch += default_pad_y + s->pad_t + s->pad_b + extra_top;
        } else if (s->kind == KIND_GROUPBOX) {
            /* Empty groupbox: at least show the title. */
            int len = (int)SendMessageW(s->hwnd, WM_GETTEXTLENGTH, 0, 0);
            wchar_t *wbuf = (wchar_t *)calloc((size_t)len + 1, sizeof(wchar_t));
            if (wbuf) {
                if (len > 0) SendMessageW(s->hwnd, WM_GETTEXT, len + 1, (LPARAM)wbuf);
                int tw, th;
                measure_text_extent(s, wbuf, &tw, &th);
                free(wbuf);
                cw = tw + 24 + s->pad_l + s->pad_r;
                ch = th + 16 + s->pad_t + s->pad_b;
            }
        } else {
            *out_w = w; *out_h = h; return;
        }
        if (s->kind == KIND_FORM) {
            /* Forms also need to account for the non-client frame: the
             * children sit in the client rect, but setSize takes the
             * outer window rect. */
            RECT rc = { 0, 0, cw, ch };
            DWORD style = (DWORD)GetWindowLongW(s->hwnd, GWL_STYLE);
            DWORD ex    = (DWORD)GetWindowLongW(s->hwnd, GWL_EXSTYLE);
            BOOL has_menu = (GetMenu(s->hwnd) != NULL);
            AdjustWindowRectEx(&rc, style, has_menu, ex);
            cw = rc.right - rc.left;
            ch = rc.bottom - rc.top;
            if (s->has_min_size) {
                if (cw < s->min_w) cw = s->min_w;
                if (ch < s->min_h) ch = s->min_h;
            }
            if (s->has_max_size) {
                if (s->max_w > 0 && cw > s->max_w) cw = s->max_w;
                if (s->max_h > 0 && ch > s->max_h) ch = s->max_h;
            }
        }
        *out_w = cw; *out_h = ch;
        return;
    }

    /* Buttons: ask Win32 for the ideal size first (XP+); if that fails,
     * fall back to the text-based measurement below. */
    if (s->kind == KIND_BUTTON) {
        SIZE sz; sz.cx = 0; sz.cy = 0;
        if (SendMessageW(s->hwnd, BCM_GETIDEALSIZE, 0, (LPARAM)&sz)
            && sz.cx > 0 && sz.cy > 0) {
            *out_w = sz.cx;
            *out_h = sz.cy;
            return;
        }
    }

    /* Text-based controls: measure the caption with the current font. */
    int len = (int)SendMessageW(s->hwnd, WM_GETTEXTLENGTH, 0, 0);
    wchar_t *wbuf = (wchar_t *)calloc((size_t)len + 1, sizeof(wchar_t));
    int tw = 0, th = 0;
    if (wbuf) {
        if (len > 0) SendMessageW(s->hwnd, WM_GETTEXT, len + 1, (LPARAM)wbuf);
        measure_text_extent(s, len > 0 ? wbuf : L"M", &tw, &th);
        if (len <= 0) tw = 0;
    }
    free(wbuf);

    int pad_w = 0, pad_h = 0;
    int min_w = 0, min_h = 0;
    switch (s->kind) {
    case KIND_BUTTON:
        pad_w = 24; pad_h = 14; min_w = 75; min_h = 23;
        break;
    case KIND_LABEL:
    case KIND_LINKLABEL:
        pad_w = 0; pad_h = 0;
        break;
    case KIND_CHECKBOX:
    case KIND_RADIO:
        /* 13px box + 4px gap from text. */
        pad_w = 17 + 4;
        pad_h = 4;
        if (th < 17) th = 17;
        break;
    case KIND_TEXTBOX:
        pad_w = 8; pad_h = 6; min_w = 60; min_h = 20;
        break;
    case KIND_NUMERIC:
        pad_w = 24; pad_h = 6; min_w = 60; min_h = 20;
        break;
    case KIND_COMBOBOX:
    case KIND_LISTBOX: {
        UINT msg_count   = (s->kind == KIND_COMBOBOX) ? CB_GETCOUNT     : LB_GETCOUNT;
        UINT msg_textlen = (s->kind == KIND_COMBOBOX) ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
        UINT msg_text    = (s->kind == KIND_COMBOBOX) ? CB_GETLBTEXT    : LB_GETTEXT;
        int n = (int)SendMessageW(s->hwnd, msg_count, 0, 0);
        int item_h = th > 0 ? th : 16;
        for (int i = 0; i < n; i++) {
            int ilen = (int)SendMessageW(s->hwnd, msg_textlen, (WPARAM)i, 0);
            if (ilen <= 0) continue;
            wchar_t *ib = (wchar_t *)calloc((size_t)ilen + 1, sizeof(wchar_t));
            if (!ib) continue;
            SendMessageW(s->hwnd, msg_text, (WPARAM)i, (LPARAM)ib);
            int iw, ih;
            measure_text_extent(s, ib, &iw, &ih);
            if (iw > tw) tw = iw;
            if (ih > item_h) item_h = ih;
            free(ib);
        }
        if (s->kind == KIND_COMBOBOX) {
            pad_w = 28;     /* dropdown arrow width */
            pad_h = 8;
            th = item_h;
            min_w = 80;
        } else {
            /* ListBox: stack all items vertically. */
            pad_w = 16;     /* room for scrollbar */
            pad_h = 4;
            th = (n > 0 ? n : 1) * item_h;
            min_w = 80;
        }
        break;
    }
    case KIND_PROGRESS:
        pad_w = 0; pad_h = 0;
        if (tw < 100) tw = 100;
        th = 16;
        break;
    case KIND_TRACKBAR:
        pad_w = 0; pad_h = 0;
        if (tw < 100) tw = 100;
        th = 24;
        break;
    case KIND_DATETIMEPICKER:
        pad_w = 28; pad_h = 6; min_w = 100; min_h = 20;
        break;
    case KIND_MONTHCALENDAR: {
        /* The month-calendar control supplies its own ideal size. */
        RECT mr;
        if (SendMessageW(s->hwnd, MCM_GETMINREQRECT, 0, (LPARAM)&mr)) {
            tw = mr.right - mr.left;
            th = mr.bottom - mr.top;
        }
        pad_w = 0; pad_h = 0;
        break;
    }
    case KIND_STATUSBAR:
        /* Status bars own their geometry via WM_SIZE on the parent. */
        *out_w = s->w; *out_h = s->h;
        return;
    case KIND_PICTUREBOX:
    case KIND_SPINNER:
    default:
        pad_w = 4; pad_h = 4;
        break;
    }

    int rw = tw + pad_w + s->pad_l + s->pad_r;
    int rh = th + pad_h + s->pad_t + s->pad_b;
    if (rw < min_w) rw = min_w;
    if (rh < min_h) rh = min_h;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    *out_w = rw;
    *out_h = rh;
}

/* Re-fit the control to its preferred size when AutoSize is on.  Called
 * from setText / setFont / addItem / setPadding etc. to keep the control
 * sized to its current content.                                          */
static void autosize_apply(FormsSlot *s)
{
    if (!s || !s->autosize) return;
    int w = s->w, h = s->h;
    compute_preferred_size(s, &w, &h);
    if (s->autosize_mode == FORMS_AUTOSIZE_GROW) {
        if (w < s->w) w = s->w;
        if (h < s->h) h = s->h;
    }
    if (w == s->w && h == s->h) return;
    s->w = w; s->h = h;
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, w, h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s->parent_slot > 0) layout_dock_children(s->parent_slot);
}
#else
static void compute_preferred_size(FormsSlot *s, int *out_w, int *out_h)
{
    *out_w = s->w; *out_h = s->h;
}
static void autosize_apply(FormsSlot *s) { (void)s; }
#endif

static int native_get_preferred_size(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getPreferredSize");
    if (!s) return -1;
    int w = s->w, h = s->h;
    compute_preferred_size(s, &w, &h);
    cando_vm_push(vm, cando_number((f64)w));
    cando_vm_push(vm, cando_number((f64)h));
    return 2;
}

static int native_size_to_content(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "sizeToContent");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    int w = s->w, h = s->h;
    compute_preferred_size(s, &w, &h);
    s->w = w; s->h = h;
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, w, h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) {
            obj_set_number(o, "width",  (f64)w);
            obj_set_number(o, "height", (f64)h);
        }
    }
    if (s->parent_slot > 0) layout_dock_children(s->parent_slot);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_size_to_content_width(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "sizeToContentWidth");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    int w = s->w, h = s->h;
    compute_preferred_size(s, &w, &h);
    s->w = w;
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, w, s->h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) obj_set_number(o, "width", (f64)w);
    }
    if (s->parent_slot > 0) layout_dock_children(s->parent_slot);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_size_to_content_height(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "sizeToContentHeight");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    int w = s->w, h = s->h;
    compute_preferred_size(s, &w, &h);
    s->h = h;
    if (s->hwnd) SetWindowPos(s->hwnd, NULL, 0, 0, s->w, h,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s->inst_val_held) {
        CdoObject *o = cando_bridge_resolve(vm, s->inst_val.as.handle);
        if (o) obj_set_number(o, "height", (f64)h);
    }
    if (s->parent_slot > 0) layout_dock_children(s->parent_slot);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Padding / Margin / AutoSize / Anchor.
 *
 * Padding is the inner gap between a control and its contents (consumed
 * by sizeToContent / autosize, and by the docking pass when the control
 * is acting as a parent).  Margin is the outer gap reserved around the
 * control for layout managers.  Anchor describes how a child's edges
 * track the parent's edges on resize.
 * ===================================================================== */

/* parse_quad_args moved to src/core/layout.{c,h}. */

static int native_set_padding(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setPadding");
    if (!s) return -1;
    parse_quad_args(argc, args, &s->pad_l, &s->pad_t, &s->pad_r, &s->pad_b);
#if FORMS_HAVE_WIN32
    autosize_apply(s);
    layout_dock_children((int)(s - g_slots));
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_padding(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getPadding");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->pad_l));
    cando_vm_push(vm, cando_number((f64)s->pad_t));
    cando_vm_push(vm, cando_number((f64)s->pad_r));
    cando_vm_push(vm, cando_number((f64)s->pad_b));
    return 4;
}

static int native_set_margin(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setMargin");
    if (!s) return -1;
    parse_quad_args(argc, args, &s->margin_l, &s->margin_t,
                    &s->margin_r, &s->margin_b);
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_margin(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getMargin");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->margin_l));
    cando_vm_push(vm, cando_number((f64)s->margin_t));
    cando_vm_push(vm, cando_number((f64)s->margin_r));
    cando_vm_push(vm, cando_number((f64)s->margin_b));
    return 4;
}

static int native_set_autosize(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setAutoSize");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->autosize = on ? 1 : 0;
#if FORMS_HAVE_WIN32
    autosize_apply(s);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_autosize(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getAutoSize");
    if (!s) return -1;
    cando_vm_push(vm, cando_bool(s->autosize ? true : false));
    return 1;
}

static int native_set_autosize_mode(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setAutoSizeMode");
    if (!s) return -1;
    int mode = FORMS_AUTOSIZE_GROW_SHRINK;
    if (argc >= 2) {
        if (args[1].tag == CDO_NUMBER) {
            int n = (int)args[1].as.number;
            if (n == FORMS_AUTOSIZE_GROW || n == FORMS_AUTOSIZE_GROW_SHRINK)
                mode = n;
        } else if (args[1].tag == CDO_STRING && args[1].as.string) {
            const char *t = args[1].as.string->data;
            u32 n = args[1].as.string->length;
            if (n == 4 && memcmp(t, "grow", 4) == 0) mode = FORMS_AUTOSIZE_GROW;
            else if (n == 10 && memcmp(t, "growshrink", 10) == 0)
                mode = FORMS_AUTOSIZE_GROW_SHRINK;
            else if (n == 11 && memcmp(t, "growandshrink", 11) == 0)
                mode = FORMS_AUTOSIZE_GROW_SHRINK;
        }
    }
    s->autosize_mode = mode;
#if FORMS_HAVE_WIN32
    autosize_apply(s);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* parse_anchor_arg moved to src/core/layout.{c,h}. */

/* Capture the current gaps so subsequent resizes can reproduce them. */
#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
static void anchor_capture(FormsSlot *s)
{
    if (s->parent_slot <= 0 || !g_slots[s->parent_slot].hwnd) return;
    RECT pr;
    GetClientRect(g_slots[s->parent_slot].hwnd, &pr);
    int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    s->anchor_l = s->x;
    s->anchor_t = s->y;
    s->anchor_r = pw - (s->x + s->w);
    s->anchor_b = ph - (s->y + s->h);
    s->anchor_w = pw;
    s->anchor_h = ph;
}

/* Apply anchors for every direct child of `parent_slot`.  Called from the
 * parent's WM_SIZE handler before the dock pass. */
static void layout_anchor_children(int parent_slot)
{
    if (parent_slot <= 0 || parent_slot >= FORMS_MAX_SLOTS) return;
    FormsSlot *p = &g_slots[parent_slot];
    if (!p->alive || !p->hwnd) return;
    RECT pr;
    if (!GetClientRect(p->hwnd, &pr)) return;
    int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != parent_slot || !c->hwnd) continue;
        if (c->dock != FORMS_DOCK_NONE) continue;
        int a = c->anchor;
        if (a == FORMS_ANCHOR_DEFAULT) continue;  /* default = stay put */
        if (c->anchor_w == 0 && c->anchor_h == 0) continue;  /* not captured */
        int x = c->x, y = c->y, w = c->w, h = c->h;
        int right_anchor  = (a & FORMS_ANCHOR_RIGHT)  != 0;
        int left_anchor   = (a & FORMS_ANCHOR_LEFT)   != 0;
        int top_anchor    = (a & FORMS_ANCHOR_TOP)    != 0;
        int bottom_anchor = (a & FORMS_ANCHOR_BOTTOM) != 0;
        if (left_anchor && right_anchor) {
            x = c->anchor_l;
            w = pw - c->anchor_l - c->anchor_r;
        } else if (right_anchor) {
            x = pw - c->anchor_r - c->w;
        } else if (left_anchor) {
            x = c->anchor_l;
        } else {
            /* No horizontal anchor -- track parent centre. */
            x = c->anchor_l + (pw - c->anchor_w) / 2;
        }
        if (top_anchor && bottom_anchor) {
            y = c->anchor_t;
            h = ph - c->anchor_t - c->anchor_b;
        } else if (bottom_anchor) {
            y = ph - c->anchor_b - c->h;
        } else if (top_anchor) {
            y = c->anchor_t;
        } else {
            y = c->anchor_t + (ph - c->anchor_h) / 2;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        SetWindowPos(c->hwnd, NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        c->x = x; c->y = y; c->w = w; c->h = h;
    }
}
#endif

static int native_set_anchor(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setAnchor");
    if (!s) return -1;
    int mask = (argc >= 2) ? parse_anchor_arg(args[1]) : FORMS_ANCHOR_DEFAULT;
    s->anchor = mask;
#if FORMS_HAVE_WIN32
    anchor_capture(s);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_anchor(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getAnchor");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->anchor));
    return 1;
}

/* =========================================================================
 * TextBox enrichments.  All silent no-ops on non-edit kinds so script
 * code can stay loose.
 * ===================================================================== */

/* TextBox / Edit-class natives (setMultiline, setReadOnly, setPlaceholder,
 * setPasswordChar, setMaxLength, setTextAlign, selectAll, appendText,
 * clearText) plus the edit_toggle_style helper now live in
 * src/controls/ctl_textbox.{c,h}. */
#include "src/controls/ctl_textbox.h"

/* =========================================================================
 * Tooltip + Cursor.
 * ===================================================================== */

#if FORMS_HAVE_WIN32 && !defined(FORMS_MODULE_TEST_BUILD)
/* Walk up to the form that owns this slot.  Returns NULL if the slot is
 * a top-level form with no parent. */
static FormsSlot *slot_owning_form(FormsSlot *s)
{
    while (s && s->parent_slot > 0) s = &g_slots[s->parent_slot];
    return (s && s->kind == KIND_FORM) ? s : NULL;
}

/* Lazily create a tooltip HWND owned by the form, then add or update the
 * tool entry that targets `s->hwnd`. */
static void tooltip_attach(FormsSlot *s, const char *text_utf8)
{
    if (!s || !s->hwnd) return;
    FormsSlot *form = (s->kind == KIND_FORM) ? s : slot_owning_form(s);
    if (!form) return;
    if (!form->tooltip_hwnd) {
        form->tooltip_hwnd = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            form->hwnd, NULL, NULL, NULL);
    }
    if (!form->tooltip_hwnd) return;

    wchar_t *w = (text_utf8 && text_utf8[0]) ? utf8_to_wide(text_utf8, -1)
                                             : NULL;
    TOOLINFOW ti; memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd   = form->hwnd;
    ti.uId    = (UINT_PTR)s->hwnd;
    /* Try update first; if no entry exists, add one. */
    if (!SendMessageW(form->tooltip_hwnd, TTM_GETTOOLINFOW, 0, (LPARAM)&ti)) {
        ti.lpszText = w ? w : (wchar_t *)L"";
        SendMessageW(form->tooltip_hwnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    } else {
        ti.lpszText = w ? w : (wchar_t *)L"";
        SendMessageW(form->tooltip_hwnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    if (w) free(w);
    /* Empty text -> remove the tool entirely. */
    if (!text_utf8 || !text_utf8[0]) {
        SendMessageW(form->tooltip_hwnd, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
}
#endif

static int native_set_tooltip(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setToolTip");
    if (!s) return -1;
    char buf[1024] = {0};
    if (argc >= 2) parse_text_arg(vm, args[1], buf, sizeof(buf));
    if (s->tooltip) { free(s->tooltip); s->tooltip = NULL; }
    if (buf[0]) s->tooltip = strdup(buf);
#if FORMS_HAVE_WIN32
    tooltip_attach(s, buf);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

#if FORMS_HAVE_WIN32
static LPCWSTR cursor_kind_to_idc(int kind)
{
    switch (kind) {
    case FORMS_CURSOR_HAND:      return IDC_HAND;
    case FORMS_CURSOR_IBEAM:     return IDC_IBEAM;
    case FORMS_CURSOR_WAIT:      return IDC_WAIT;
    case FORMS_CURSOR_CROSS:     return IDC_CROSS;
    case FORMS_CURSOR_SIZE_NS:   return IDC_SIZENS;
    case FORMS_CURSOR_SIZE_WE:   return IDC_SIZEWE;
    case FORMS_CURSOR_SIZE_NWSE: return IDC_SIZENWSE;
    case FORMS_CURSOR_SIZE_NESW: return IDC_SIZENESW;
    case FORMS_CURSOR_SIZE_ALL:  return IDC_SIZEALL;
    case FORMS_CURSOR_NO:        return IDC_NO;
    case FORMS_CURSOR_HELP:      return IDC_HELP;
    case FORMS_CURSOR_APPSTART:  return IDC_APPSTARTING;
    case FORMS_CURSOR_ARROW:     return IDC_ARROW;
    default:                     return IDC_ARROW;
    }
}
#endif

static int native_set_cursor(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setCursor");
    if (!s) return -1;
    int kind = FORMS_CURSOR_DEFAULT;
    if (argc >= 2) {
        if (args[1].tag == CDO_NUMBER) {
            kind = (int)args[1].as.number;
        } else if (args[1].tag == CDO_STRING && args[1].as.string) {
            const char *t = args[1].as.string->data;
            u32 n = args[1].as.string->length;
            #define CCURSOR(name, val) \
                if (n == sizeof(name)-1 && memcmp(t, name, sizeof(name)-1) == 0) kind = val
            CCURSOR("default",     FORMS_CURSOR_DEFAULT);
            CCURSOR("arrow",       FORMS_CURSOR_ARROW);
            CCURSOR("hand",        FORMS_CURSOR_HAND);
            CCURSOR("pointer",     FORMS_CURSOR_HAND);
            CCURSOR("ibeam",       FORMS_CURSOR_IBEAM);
            CCURSOR("text",        FORMS_CURSOR_IBEAM);
            CCURSOR("wait",        FORMS_CURSOR_WAIT);
            CCURSOR("cross",       FORMS_CURSOR_CROSS);
            CCURSOR("crosshair",   FORMS_CURSOR_CROSS);
            CCURSOR("size-ns",     FORMS_CURSOR_SIZE_NS);
            CCURSOR("size-we",     FORMS_CURSOR_SIZE_WE);
            CCURSOR("size-nwse",   FORMS_CURSOR_SIZE_NWSE);
            CCURSOR("size-nesw",   FORMS_CURSOR_SIZE_NESW);
            CCURSOR("size-all",    FORMS_CURSOR_SIZE_ALL);
            CCURSOR("no",          FORMS_CURSOR_NO);
            CCURSOR("forbidden",   FORMS_CURSOR_NO);
            CCURSOR("help",        FORMS_CURSOR_HELP);
            CCURSOR("appstarting", FORMS_CURSOR_APPSTART);
            #undef CCURSOR
        }
    }
    s->cursor_kind = kind;
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Form-level state: icon, flash, window state, resizable, taskbar...
 * ===================================================================== */

/* Remaining Form-only natives (setIcon, flash, setWindowState/
 * getWindowState, maximize/minimize/restore, setResizable,
 * setMinimizeBox, setMaximizeBox, setShowInTaskbar, setAcceptButton,
 * setCancelButton) plus the form_toggle_style helper now live in
 * src/controls/ctl_form.{c,h}. */

/* =========================================================================
 * Numeric / Progress / TrackBar enrichments.
 * ===================================================================== */

/* setStep / stepIt           moved to src/controls/ctl_progress.{c,h}
 * setTickFrequency / setSmall/LargeStep
 *                            moved to src/controls/ctl_trackbar.{c,h}
 * setIncrement               moved to src/controls/ctl_numeric.{c,h}  */
#include "src/controls/ctl_progress.h"
#include "src/controls/ctl_trackbar.h"
#include "src/controls/ctl_numeric.h"

/* =========================================================================
 * Convenience helpers: remove / getParent / getChildren / hasFocus /
 * setName / contains / tab order accessors.
 * ===================================================================== */

static int native_remove(CandoVM *vm, int argc, CandoValue *args)
{
    /* Alias for destroy -- Derma uses :Remove(). */
    return native_destroy(vm, argc, args);
}

static int native_get_parent(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getParent");
    if (!s) return -1;
    if (s->parent_slot > 0 && s->parent_slot < FORMS_MAX_SLOTS &&
        g_slots[s->parent_slot].alive &&
        g_slots[s->parent_slot].inst_val_held) {
        cando_vm_push(vm, g_slots[s->parent_slot].inst_val);
    } else {
        cando_vm_push(vm, cando_null());
    }
    return 1;
}

static int native_get_children(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getChildren");
    if (!s) return -1;
    int slot = (int)(s - g_slots);
    CandoValue arr = cando_bridge_new_array(vm);
    CdoObject *a   = cando_bridge_resolve(vm, arr.as.handle);
    for (int i = 1; i < FORMS_MAX_SLOTS; i++) {
        FormsSlot *c = &g_slots[i];
        if (!c->alive || c->parent_slot != slot) continue;
        if (!c->inst_val_held) continue;
        CdoObject *co = cando_bridge_resolve(vm, c->inst_val.as.handle);
        if (co) cdo_array_push(a, cdo_object_value(co));
    }
    cando_vm_push(vm, arr);
    return 1;
}

static int native_has_focus(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "hasFocus");
    if (!s) return -1;
    bool focused = false;
#if FORMS_HAVE_WIN32
    if (s->hwnd) focused = (GetFocus() == s->hwnd);
#endif
    cando_vm_push(vm, cando_bool(focused));
    return 1;
}

static int native_contains(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "contains");
    if (!s) return -1;
    bool yes = false;
    if (argc >= 2) {
        FormsSlot *c = slot_from_inst(vm, args[1]);
        while (c && c->parent_slot > 0) {
            if (c->parent_slot == (int)(s - g_slots)) { yes = true; break; }
            c = &g_slots[c->parent_slot];
        }
    }
    cando_vm_push(vm, cando_bool(yes));
    return 1;
}

static int native_set_tab_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTabIndex");
    if (!s) return -1;
    s->tab_index = (argc >= 2 && args[1].tag == CDO_NUMBER) ?
                   (int)args[1].as.number : -1;
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_tab_index(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getTabIndex");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->tab_index));
    return 1;
}

static int native_set_tab_stop(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setTabStop");
    if (!s) return -1;
    bool on = !(argc >= 2 && args[1].tag == CDO_BOOL && !args[1].as.boolean);
    s->tab_stop = on ? 1 : 0;
#if FORMS_HAVE_WIN32
    if (s->hwnd) {
        LONG st = GetWindowLongW(s->hwnd, GWL_STYLE);
        if (on) st |=  WS_TABSTOP; else st &= ~WS_TABSTOP;
        SetWindowLongW(s->hwnd, GWL_STYLE, st);
    }
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

/* parse_dock_arg moved to src/core/layout.{c,h}.  This stub keeps the
 * old function visible so the in-file callers keep compiling until
 * their migration to layout.h's public name lands.  Phase 1.3a. */
#if 0
static int parse_dock_arg(CandoValue v)
{
    if (v.tag == CDO_NUMBER) {
        int n = (int)v.as.number;
        if (n < FORMS_DOCK_NONE || n > FORMS_DOCK_FILL) return FORMS_DOCK_NONE;
        return n;
    }
    if (v.tag == CDO_STRING && v.as.string) {
        const char *s = v.as.string->data;
        u32 n = v.as.string->length;
        #define CHECK(name, val) \
            if (n == sizeof(name)-1 && memcmp(s, name, sizeof(name)-1) == 0) return val
        CHECK("none",   FORMS_DOCK_NONE);
        CHECK("top",    FORMS_DOCK_TOP);
        CHECK("bottom", FORMS_DOCK_BOTTOM);
        CHECK("left",   FORMS_DOCK_LEFT);
        CHECK("right",  FORMS_DOCK_RIGHT);
        CHECK("fill",   FORMS_DOCK_FILL);
        #undef CHECK
    }
    return FORMS_DOCK_NONE;
}
#endif /* parse_dock_arg moved -- see src/core/layout.h */

static int native_set_dock(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "setDock");
    if (!s) return -1;
    s->dock = (argc >= 2) ? parse_dock_arg(args[1]) : FORMS_DOCK_NONE;
#if FORMS_HAVE_WIN32
    if (s->parent_slot > 0) layout_dock_children(s->parent_slot);
#endif
    cando_vm_push(vm, args[0]);
    return 1;
}

static int native_get_dock(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "getDock");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->dock));
    return 1;
}

/* =========================================================================
 * ProgressBar extras: marquee animation + colour state.
 * ===================================================================== */

/* setMarquee / setState moved to src/controls/ctl_progress.{c,h}. */

/* Force-relayout helper -- script can call form:relayout() if it has
 * mutated child sizes manually and wants the dock pass to re-run. */
static int native_relayout(CandoVM *vm, int argc, CandoValue *args)
{
    FormsSlot *s = arg_self(vm, argc, args, "relayout");
    if (!s) return -1;
#if FORMS_HAVE_WIN32
    layout_dock_children((int)(s - g_slots));
#endif
    cando_vm_push(vm, args[0]);
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
 * Meta-table inheritance + per-kind name lookup.
 *
 * Phase 1.1 replaces the single shared "forms_control" meta with a base
 * table plus per-control-kind tables that prototype-chain to it.
 * Methods land on the most specific table possible -- e.g. addItem
 * lives only on forms_listbox / forms_combobox, so calling it on a
 * Button is a hard error from the VM rather than a silent no-op.
 * ===================================================================== */

#ifndef FORMS_MODULE_TEST_BUILD
/* Wire `child.__index = parent` so dispatch on instances stamped with
 * the child meta falls through to the parent for any name the child
 * hasn't overridden. */
static void meta_inherit(CdoObject *child, CdoObject *parent)
{
    if (!child || !parent) return;
    cdo_object_rawset(child, g_meta_index,
                      cdo_object_value(parent), FIELD_NONE);
}

/* Map a ControlKind to its meta-table name.  Kept in step with
 * register_forms_meta_tables below. */
static const char *meta_name_for_kind(ControlKind k)
{
    switch (k) {
    case KIND_FORM:            return "forms_form";
    case KIND_BUTTON:          return "forms_button";
    case KIND_LABEL:           return "forms_label";
    case KIND_LINKLABEL:       return "forms_linklabel";
    case KIND_TEXTBOX:         return "forms_textbox";
    case KIND_CHECKBOX:        return "forms_checkbox";
    case KIND_RADIO:           return "forms_radiobutton";
    case KIND_COMBOBOX:        return "forms_combobox";
    case KIND_LISTBOX:         return "forms_listbox";
    case KIND_PANEL:           return "forms_panel";
    case KIND_GROUPBOX:        return "forms_groupbox";
    case KIND_PROGRESS:        return "forms_progress";
    case KIND_TRACKBAR:        return "forms_trackbar";
    case KIND_NUMERIC:         return "forms_numeric";
    case KIND_PICTUREBOX:      return "forms_picturebox";
    case KIND_DATETIMEPICKER:  return "forms_datetimepicker";
    case KIND_MONTHCALENDAR:   return "forms_monthcalendar";
    case KIND_STATUSBAR:       return "forms_statusbar";
    case KIND_SPINNER:         return "forms_spinner";
    /* Phase 2 container kinds. */
    case KIND_SCROLLPANEL:     return "forms_scrollpanel";
    case KIND_TABCONTROL:      return "forms_tabcontrol";
    case KIND_TABPAGE:         return "forms_tabpage";
    case KIND_SPLITCONTAINER:  return "forms_splitcontainer";
    case KIND_SPLITTER:        return "forms_splitter";
    case KIND_FLOWLAYOUT:      return "forms_flowlayout";
    case KIND_TABLELAYOUT:     return "forms_tablelayout";
    case KIND_NONE:
    case KIND_KIND_COUNT:
        break;
    }
    return "forms_control_base";    /* unreachable from valid slots */
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

    /* ------------------------------------------------------------
     * Phase 1.1: per-kind meta tables.
     *
     * `forms_control_base` carries the methods every control inherits.
     * Each per-kind table chains its __index to the base and adds its
     * specific methods on top.  build_instance() looks up the right
     * name via meta_name_for_kind(kind) so type-incorrect calls (e.g.
     * `addItem` on a Button) become hard errors rather than silent
     * no-ops.
     *
     * Clean break from forms 1.x: the Derma PascalCase aliases and the
     * cross-kind silent-no-op coverage are dropped.  Old scripts must
     * port to the canonical camelCase / per-kind surface. */
    CdoObject *base = cando_lib_meta_table(vm, "forms_control_base");

    /* Text. */
    cando_lib_meta_define(vm, base, "setText",        native_set_text);
    cando_lib_meta_define(vm, base, "getText",        native_get_text);

    /* Geometry. */
    cando_lib_meta_define(vm, base, "setSize",        native_set_size);
    cando_lib_meta_define(vm, base, "getSize",        native_get_size);
    cando_lib_meta_define(vm, base, "setWidth",       native_set_width);
    cando_lib_meta_define(vm, base, "setHeight",      native_set_height);
    cando_lib_meta_define(vm, base, "getWidth",       native_get_width);
    cando_lib_meta_define(vm, base, "getHeight",      native_get_height);
    cando_lib_meta_define(vm, base, "setLocation",    native_set_location);
    cando_lib_meta_define(vm, base, "getLocation",    native_get_location);
    cando_lib_meta_define(vm, base, "sizeToContent",       native_size_to_content);
    cando_lib_meta_define(vm, base, "sizeToContentWidth",  native_size_to_content_width);
    cando_lib_meta_define(vm, base, "sizeToContentHeight", native_size_to_content_height);
    cando_lib_meta_define(vm, base, "getPreferredSize",    native_get_preferred_size);

    /* Visibility / state / lifetime. */
    cando_lib_meta_define(vm, base, "show",           native_show);
    cando_lib_meta_define(vm, base, "hide",           native_hide);
    cando_lib_meta_define(vm, base, "setVisible",     native_set_visible);
    cando_lib_meta_define(vm, base, "getVisible",     native_get_visible);
    cando_lib_meta_define(vm, base, "setEnabled",     native_set_enabled);
    cando_lib_meta_define(vm, base, "getEnabled",     native_get_enabled);
    cando_lib_meta_define(vm, base, "focus",          native_focus);
    cando_lib_meta_define(vm, base, "hasFocus",       native_has_focus);
    cando_lib_meta_define(vm, base, "destroy",        native_destroy);
    cando_lib_meta_define(vm, base, "isOpen",         native_is_open);

    /* Hierarchy. */
    cando_lib_meta_define(vm, base, "setParent",      native_set_parent);
    cando_lib_meta_define(vm, base, "getParent",      native_get_parent);
    cando_lib_meta_define(vm, base, "getChildren",    native_get_children);
    cando_lib_meta_define(vm, base, "contains",       native_contains);
    cando_lib_meta_define(vm, base, "bringToFront",   native_bring_to_front);
    cando_lib_meta_define(vm, base, "sendToBack",     native_send_to_back);

    /* Look. */
    cando_lib_meta_define(vm, base, "setForeColor",   native_set_fore_color);
    cando_lib_meta_define(vm, base, "setBackColor",   native_set_back_color);
    cando_lib_meta_define(vm, base, "getForeColor",   native_get_fore_color);
    cando_lib_meta_define(vm, base, "getBackColor",   native_get_back_color);
    cando_lib_meta_define(vm, base, "clearForeColor", native_clear_fore_color);
    cando_lib_meta_define(vm, base, "clearBackColor", native_clear_back_color);
    cando_lib_meta_define(vm, base, "setFont",        native_set_font);
    cando_lib_meta_define(vm, base, "setFontSize",    native_set_font_size);
    cando_lib_meta_define(vm, base, "setBold",        native_set_bold);
    cando_lib_meta_define(vm, base, "setItalic",      native_set_italic);
    cando_lib_meta_define(vm, base, "setUnderline",   native_set_underline);
    cando_lib_meta_define(vm, base, "setStrikeout",   native_set_strikeout);
    cando_lib_meta_define(vm, base, "clearFont",      native_clear_font);
    cando_lib_meta_define(vm, base, "getFont",        native_get_font);
    cando_lib_meta_define(vm, base, "setBorderStyle", native_set_border_style);
    cando_lib_meta_define(vm, base, "setCursor",      native_set_cursor);
    cando_lib_meta_define(vm, base, "setToolTip",     native_set_tooltip);

    /* Layout. */
    cando_lib_meta_define(vm, base, "setPadding",      native_set_padding);
    cando_lib_meta_define(vm, base, "getPadding",      native_get_padding);
    cando_lib_meta_define(vm, base, "setMargin",       native_set_margin);
    cando_lib_meta_define(vm, base, "getMargin",       native_get_margin);
    cando_lib_meta_define(vm, base, "setAnchor",       native_set_anchor);
    cando_lib_meta_define(vm, base, "getAnchor",       native_get_anchor);
    cando_lib_meta_define(vm, base, "setDock",         native_set_dock);
    cando_lib_meta_define(vm, base, "getDock",         native_get_dock);
    cando_lib_meta_define(vm, base, "setAutoSize",     native_set_autosize);
    cando_lib_meta_define(vm, base, "getAutoSize",     native_get_autosize);
    cando_lib_meta_define(vm, base, "setAutoSizeMode", native_set_autosize_mode);
    cando_lib_meta_define(vm, base, "relayout",        native_relayout);
    cando_lib_meta_define(vm, base, "refresh",         native_refresh);

    /* Tab order. */
    cando_lib_meta_define(vm, base, "setTabIndex",    native_set_tab_index);
    cando_lib_meta_define(vm, base, "getTabIndex",    native_get_tab_index);
    cando_lib_meta_define(vm, base, "setTabStop",     native_set_tab_stop);

    /* ----- Per-kind meta tables (each chains __index to base) ----- */

    /* Form: top-level window extras. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_form");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setOpacity",        native_set_opacity);
        cando_lib_meta_define(vm, m, "getOpacity",        native_get_opacity);
        cando_lib_meta_define(vm, m, "setTopMost",        native_set_topmost);
        cando_lib_meta_define(vm, m, "center",            native_center);
        cando_lib_meta_define(vm, m, "setMinSize",        native_set_min_size);
        cando_lib_meta_define(vm, m, "setMaxSize",        native_set_max_size);
        cando_lib_meta_define(vm, m, "setIcon",           native_set_icon);
        cando_lib_meta_define(vm, m, "flash",             native_flash);
        cando_lib_meta_define(vm, m, "setWindowState",    native_set_window_state);
        cando_lib_meta_define(vm, m, "getWindowState",    native_get_window_state);
        cando_lib_meta_define(vm, m, "maximize",          native_maximize);
        cando_lib_meta_define(vm, m, "minimize",          native_minimize);
        cando_lib_meta_define(vm, m, "restore",           native_restore);
        cando_lib_meta_define(vm, m, "setResizable",      native_set_resizable);
        cando_lib_meta_define(vm, m, "setMinimizeBox",    native_set_minimize_box);
        cando_lib_meta_define(vm, m, "setMaximizeBox",    native_set_maximize_box);
        cando_lib_meta_define(vm, m, "setShowInTaskbar",  native_set_show_in_taskbar);
        cando_lib_meta_define(vm, m, "setAcceptButton",   native_set_accept_button);
        cando_lib_meta_define(vm, m, "setCancelButton",   native_set_cancel_button);
    }

    /* Button -- pure base; placeholder kept so script-side type() reads
     * "forms_button" and per-kind extensions can land here later. */
    meta_inherit(cando_lib_meta_table(vm, "forms_button"), base);

    /* Label -- pure base for now. */
    meta_inherit(cando_lib_meta_table(vm, "forms_label"), base);

    /* LinkLabel -- pure base for now. */
    meta_inherit(cando_lib_meta_table(vm, "forms_linklabel"), base);

    /* TextBox -- text manipulation. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_textbox");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setMultiline",     native_set_multiline);
        cando_lib_meta_define(vm, m, "setReadOnly",      native_set_readonly);
        cando_lib_meta_define(vm, m, "setPlaceholder",   native_set_placeholder);
        cando_lib_meta_define(vm, m, "setPasswordChar",  native_set_password_char);
        cando_lib_meta_define(vm, m, "setMaxLength",     native_set_max_length);
        cando_lib_meta_define(vm, m, "setTextAlign",     native_set_text_alignment);
        cando_lib_meta_define(vm, m, "selectAll",        native_select_all);
        cando_lib_meta_define(vm, m, "appendText",       native_append_text);
        cando_lib_meta_define(vm, m, "clearText",        native_clear_text);
        cando_lib_meta_define(vm, m, "setValue",         native_set_value);
        cando_lib_meta_define(vm, m, "getValue",         native_get_value);
    }

    /* CheckBox + RadioButton -- check state. */
    {
        CdoObject *cb = cando_lib_meta_table(vm, "forms_checkbox");
        meta_inherit(cb, base);
        cando_lib_meta_define(vm, cb, "setChecked",      native_set_checked);
        cando_lib_meta_define(vm, cb, "getChecked",      native_get_checked);

        CdoObject *rb = cando_lib_meta_table(vm, "forms_radiobutton");
        meta_inherit(rb, base);
        cando_lib_meta_define(vm, rb, "setChecked",      native_set_checked);
        cando_lib_meta_define(vm, rb, "getChecked",      native_get_checked);
    }

    /* ComboBox + ListBox -- item lists. */
    {
        const char *list_metas[] = { "forms_combobox", "forms_listbox" };
        for (size_t i = 0; i < sizeof(list_metas) / sizeof(list_metas[0]); i++) {
            CdoObject *m = cando_lib_meta_table(vm, list_metas[i]);
            meta_inherit(m, base);
            cando_lib_meta_define(vm, m, "addItem",          native_add_item);
            cando_lib_meta_define(vm, m, "removeItem",       native_remove_item);
            cando_lib_meta_define(vm, m, "clearItems",       native_clear_items);
            cando_lib_meta_define(vm, m, "getItem",          native_get_item);
            cando_lib_meta_define(vm, m, "getItems",         native_get_items);
            cando_lib_meta_define(vm, m, "getItemCount",     native_get_item_count);
            cando_lib_meta_define(vm, m, "getSelectedIndex", native_get_selected_index);
            cando_lib_meta_define(vm, m, "setSelectedIndex", native_set_selected_index);
            cando_lib_meta_define(vm, m, "clear",            native_clear_text);
        }
    }

    /* Panel + GroupBox -- containers; pure base for now. */
    meta_inherit(cando_lib_meta_table(vm, "forms_panel"),    base);
    meta_inherit(cando_lib_meta_table(vm, "forms_groupbox"), base);

    /* ProgressBar -- value/range/state. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_progress");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setValue",         native_set_value);
        cando_lib_meta_define(vm, m, "getValue",         native_get_value);
        cando_lib_meta_define(vm, m, "setRange",         native_set_range);
        cando_lib_meta_define(vm, m, "setMarquee",       native_set_marquee);
        cando_lib_meta_define(vm, m, "setState",         native_set_state);
        cando_lib_meta_define(vm, m, "setStep",          native_set_step);
        cando_lib_meta_define(vm, m, "stepIt",           native_step_it);
    }

    /* TrackBar -- value/range/tick. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_trackbar");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setValue",         native_set_value);
        cando_lib_meta_define(vm, m, "getValue",         native_get_value);
        cando_lib_meta_define(vm, m, "setRange",         native_set_range);
        cando_lib_meta_define(vm, m, "setTickFrequency", native_set_tick_frequency);
        cando_lib_meta_define(vm, m, "setSmallStep",     native_set_small_step);
        cando_lib_meta_define(vm, m, "setLargeStep",     native_set_large_step);
    }

    /* NumericUpDown -- value/range/increment. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_numeric");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setValue",         native_set_value);
        cando_lib_meta_define(vm, m, "getValue",         native_get_value);
        cando_lib_meta_define(vm, m, "setRange",         native_set_range);
        cando_lib_meta_define(vm, m, "setIncrement",     native_set_increment);
    }

    /* PictureBox -- pure base for now. */
    meta_inherit(cando_lib_meta_table(vm, "forms_picturebox"), base);

    /* DateTimePicker / MonthCalendar -- pure base; future setValue/getValue
     * land here once the new API spec is implemented. */
    meta_inherit(cando_lib_meta_table(vm, "forms_datetimepicker"), base);
    meta_inherit(cando_lib_meta_table(vm, "forms_monthcalendar"),  base);

    /* StatusBar -- pure base for now. */
    meta_inherit(cando_lib_meta_table(vm, "forms_statusbar"), base);

    /* Spinner -- value/range. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_spinner");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setValue",         native_set_value);
        cando_lib_meta_define(vm, m, "getValue",         native_get_value);
        cando_lib_meta_define(vm, m, "setRange",         native_set_range);
    }

    /* ----- Phase 2 container kinds ----- *
     * Each meta table chains __index to base so every common method
     * (setText, setSize, setLocation, setEnabled, setForeColor,
     * setFont, setPadding, setAnchor, setDock, etc.) is available on
     * instances of these kinds even before the container-specific
     * natives (addTab, scrollTo, setSplitterDistance, etc.) land.
     * Constructors land alongside the per-kind native files. */
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_scrollpanel");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setAutoScroll",      native_set_auto_scroll);
        cando_lib_meta_define(vm, m, "setScrollSize",      native_set_scroll_size);
        cando_lib_meta_define(vm, m, "scrollTo",           native_scroll_to);
        cando_lib_meta_define(vm, m, "getScrollPosition",  native_get_scroll_position);
    }
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_tabcontrol");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "addTab",             native_add_tab);
        cando_lib_meta_define(vm, m, "removeTab",          native_remove_tab);
        cando_lib_meta_define(vm, m, "clearTabs",          native_clear_tabs);
        cando_lib_meta_define(vm, m, "getTabCount",        native_get_tab_count);
        cando_lib_meta_define(vm, m, "getSelectedIndex",   native_get_selected_tab);
        cando_lib_meta_define(vm, m, "setSelectedIndex",   native_set_selected_tab);
        cando_lib_meta_define(vm, m, "getTabText",         native_get_tab_text);
        cando_lib_meta_define(vm, m, "setTabText",         native_set_tab_text);
    }
    meta_inherit(cando_lib_meta_table(vm, "forms_tabpage"),        base);
    meta_inherit(cando_lib_meta_table(vm, "forms_splitcontainer"), base);
    meta_inherit(cando_lib_meta_table(vm, "forms_splitter"),       base);
    {
        CdoObject *m = cando_lib_meta_table(vm, "forms_flowlayout");
        meta_inherit(m, base);
        cando_lib_meta_define(vm, m, "setFlowDirection",  native_set_flow_direction);
        cando_lib_meta_define(vm, m, "getFlowDirection",  native_get_flow_direction);
        cando_lib_meta_define(vm, m, "setWrapContents",   native_set_wrap_contents);
        cando_lib_meta_define(vm, m, "getWrapContents",   native_get_wrap_contents);
        /* First real consumer of the layout vtable from Phase 1.3:
         * panel_wndproc / form_wndproc dispatch their WM_SIZE pass
         * through forms_layout_for(parent->kind) which returns this
         * arrange function for KIND_FLOWLAYOUT slots. */
        forms_layout_register(KIND_FLOWLAYOUT, flowlayout_arrange);
    }
    meta_inherit(cando_lib_meta_table(vm, "forms_tablelayout"),    base);

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
    libutil_set_method(vm, obj, "LinkLabel",     native_linklabel_create);
    libutil_set_method(vm, obj, "DateTimePicker", native_datetime_create);
    libutil_set_method(vm, obj, "MonthCalendar", native_calendar_create);
    libutil_set_method(vm, obj, "StatusBar",     native_statusbar_create);
    libutil_set_method(vm, obj, "Spinner",       native_spinner_create);
    /* Phase 2 containers. */
    libutil_set_method(vm, obj, "ScrollPanel",     native_scrollpanel_create);
    libutil_set_method(vm, obj, "TabControl",      native_tabcontrol_create);
    libutil_set_method(vm, obj, "FlowLayoutPanel", native_flowlayout_create);

    /* forms.Color -- a small palette of CSS-style named colours that
     * scripts can drop straight into setForeColor / setBackColor without
     * looking the hex value up.  The same names also work as strings:
     *
     *     b:setBackColor("cornflowerblue")
     *     b:setBackColor(forms.Color.cornflowerblue)
     *
     * are equivalent; the table form is a touch faster (no string lookup
     * per call) and gives editors a discoverable namespace. */
    {
        CandoValue cv = cando_bridge_new_object(vm);
        CdoObject *cobj = cando_bridge_resolve(vm, cv.as.handle);
        for (const NamedColor *p = g_named_colors; p->name; p++) {
            obj_set_number(cobj, p->name, (f64)p->rgb);
        }
        CdoString *kc = cdo_string_intern("Color", 5);
        cdo_object_rawset(obj, kc, cdo_object_value(
            cando_bridge_resolve(vm, cv.as.handle)), FIELD_NONE);
        cdo_string_release(kc);
    }

    /* forms.BorderStyle -- enum for setBorderStyle(). */
    {
        CandoValue bv = cando_bridge_new_object(vm);
        CdoObject *bobj = cando_bridge_resolve(vm, bv.as.handle);
        obj_set_number(bobj, "none",    1.0);
        obj_set_number(bobj, "single",  2.0);
        obj_set_number(bobj, "fixed3D", 3.0);
        CdoString *kb = cdo_string_intern("BorderStyle", 11);
        cdo_object_rawset(obj, kb, cdo_object_value(
            cando_bridge_resolve(vm, bv.as.handle)), FIELD_NONE);
        cdo_string_release(kb);
    }

    /* forms.Dock -- DockStyle constants for setDock(). */
    {
        CandoValue dv = cando_bridge_new_object(vm);
        CdoObject *d  = cando_bridge_resolve(vm, dv.as.handle);
        obj_set_number(d, "none",   (f64)FORMS_DOCK_NONE);
        obj_set_number(d, "top",    (f64)FORMS_DOCK_TOP);
        obj_set_number(d, "bottom", (f64)FORMS_DOCK_BOTTOM);
        obj_set_number(d, "left",   (f64)FORMS_DOCK_LEFT);
        obj_set_number(d, "right",  (f64)FORMS_DOCK_RIGHT);
        obj_set_number(d, "fill",   (f64)FORMS_DOCK_FILL);
        CdoString *kd = cdo_string_intern("Dock", 4);
        cdo_object_rawset(obj, kd, cdo_object_value(
            cando_bridge_resolve(vm, dv.as.handle)), FIELD_NONE);
        cdo_string_release(kd);
    }

    /* forms.Anchor -- bitmask constants for setAnchor(). */
    {
        CandoValue av = cando_bridge_new_object(vm);
        CdoObject *a  = cando_bridge_resolve(vm, av.as.handle);
        obj_set_number(a, "none",   (f64)FORMS_ANCHOR_NONE);
        obj_set_number(a, "left",   (f64)FORMS_ANCHOR_LEFT);
        obj_set_number(a, "top",    (f64)FORMS_ANCHOR_TOP);
        obj_set_number(a, "right",  (f64)FORMS_ANCHOR_RIGHT);
        obj_set_number(a, "bottom", (f64)FORMS_ANCHOR_BOTTOM);
        obj_set_number(a, "all",    (f64)FORMS_ANCHOR_ALL);
        CdoString *ka = cdo_string_intern("Anchor", 6);
        cdo_object_rawset(obj, ka, cdo_object_value(
            cando_bridge_resolve(vm, av.as.handle)), FIELD_NONE);
        cdo_string_release(ka);
    }

    /* forms.AutoSizeMode -- enum for setAutoSizeMode(). */
    {
        CandoValue mv = cando_bridge_new_object(vm);
        CdoObject *m  = cando_bridge_resolve(vm, mv.as.handle);
        obj_set_number(m, "grow",       (f64)FORMS_AUTOSIZE_GROW);
        obj_set_number(m, "growShrink", (f64)FORMS_AUTOSIZE_GROW_SHRINK);
        CdoString *km = cdo_string_intern("AutoSizeMode", 12);
        cdo_object_rawset(obj, km, cdo_object_value(
            cando_bridge_resolve(vm, mv.as.handle)), FIELD_NONE);
        cdo_string_release(km);
    }

    /* forms.Cursor -- friendly names for setCursor(). */
    {
        CandoValue cv = cando_bridge_new_object(vm);
        CdoObject *c  = cando_bridge_resolve(vm, cv.as.handle);
        obj_set_string(c, "default",     "default",     7);
        obj_set_string(c, "arrow",       "arrow",       5);
        obj_set_string(c, "hand",        "hand",        4);
        obj_set_string(c, "ibeam",       "ibeam",       5);
        obj_set_string(c, "wait",        "wait",        4);
        obj_set_string(c, "cross",       "cross",       5);
        obj_set_string(c, "sizeNS",      "size-ns",     7);
        obj_set_string(c, "sizeWE",      "size-we",     7);
        obj_set_string(c, "sizeNWSE",    "size-nwse",   9);
        obj_set_string(c, "sizeNESW",    "size-nesw",   9);
        obj_set_string(c, "sizeAll",     "size-all",    8);
        obj_set_string(c, "no",          "no",          2);
        obj_set_string(c, "help",        "help",        4);
        obj_set_string(c, "appStarting", "appstarting", 11);
        CdoString *kc = cdo_string_intern("Cursor", 6);
        cdo_object_rawset(obj, kc, cdo_object_value(
            cando_bridge_resolve(vm, cv.as.handle)), FIELD_NONE);
        cdo_string_release(kc);
    }

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
void forms_test_compute_dock_rect(int dock, int child_w, int child_h,
                                  int *left, int *top, int *right, int *bottom,
                                  int *out_x, int *out_y, int *out_w, int *out_h)
{
    DockRect r;
    compute_dock_rect(dock, child_w, child_h, left, top, right, bottom, &r);
    if (out_x) *out_x = r.x;
    if (out_y) *out_y = r.y;
    if (out_w) *out_w = r.w;
    if (out_h) *out_h = r.h;
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
