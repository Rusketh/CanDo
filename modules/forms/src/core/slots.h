/*
 * src/core/slots.h -- forms-module slot table.
 *
 * Every form and every control occupies one slot.  Lifecycle: a
 * generation counter advances on every (re)allocation so a script
 * holding an old slot index whose entry has been recycled is rejected
 * on resolve.
 *
 * The struct definition is exposed in this header (rather than
 * forward-declared) because forms_module.c reads/writes its fields
 * directly from many call sites.  The follow-up phase (0.5) replaces
 * the script-visible __forms_slot field with an opaque handle + lookup
 * table, at which point most of those direct accesses can move behind
 * helpers and the struct can become opaque.
 *
 * Win32-typed fields (HWND / HBRUSH / HFONT / HICON / WNDPROC) are
 * gated behind FORMS_HAVE_WIN32 so the test build (which strips
 * windows.h) still compiles cleanly.
 */

#ifndef CANDO_FORMS_CORE_SLOTS_H
#define CANDO_FORMS_CORE_SLOTS_H

#include "cando_compat.h"   /* CandoValue (real or test stub) */
#include "sync.h"           /* fm_mutex_t + Win32 types       */
#include "geom.h"           /* FORMS_DOCK_NONE                */

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  define FORMS_HAVE_WIN32 1
#else
#  define FORMS_HAVE_WIN32 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Control taxonomy.  Stable enum values: the C unit tests pin
 * specific positions and a lot of WndProc routing keys off these.
 * Append new kinds at the end. */
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
    KIND_LINKLABEL,
    KIND_DATETIMEPICKER,
    KIND_MONTHCALENDAR,
    KIND_STATUSBAR,
    KIND_SPINNER,
    KIND_KIND_COUNT
} ControlKind;

/* Practical cap on simultaneously-live forms + controls in a process.
 *
 * Phase 0.6 raises this from the original 256 to a value that's no
 * longer a real-world ceiling for any UI a CanDo script is likely to
 * build (a thousand controls is already far more than any reasonable
 * form tree).  The slot array is still statically allocated -- a
 * truly dynamic vector requires either chunked storage with a
 * per-access indirection or realloc that invalidates cached
 * FormsSlot* pointers, both of which are bigger surgical changes
 * than fit in Phase 0's "scaffolding without behaviour change"
 * mandate.  The dynamic vector lands in Phase 1 alongside the
 * backend split (REWRITE_PLAN.md). */
#define FORMS_MAX_SLOTS 4096

#define FORMS_SLOT_KEY  "__forms_slot"
#define FORMS_GEN_KEY   "__forms_gen"
#define FORMS_KIND_KEY  "__forms_kind"

/* Anchor flags.  Combine via bitwise-or; defaults to LEFT|TOP. */
#define FORMS_ANCHOR_NONE   0
#define FORMS_ANCHOR_LEFT   (1 << 0)
#define FORMS_ANCHOR_TOP    (1 << 1)
#define FORMS_ANCHOR_RIGHT  (1 << 2)
#define FORMS_ANCHOR_BOTTOM (1 << 3)
#define FORMS_ANCHOR_ALL    (FORMS_ANCHOR_LEFT | FORMS_ANCHOR_TOP | \
                             FORMS_ANCHOR_RIGHT | FORMS_ANCHOR_BOTTOM)
#define FORMS_ANCHOR_DEFAULT (FORMS_ANCHOR_LEFT | FORMS_ANCHOR_TOP)

/* Auto-size mode -- WinForms-flavoured: 1 = grow only, 2 = grow+shrink. */
#define FORMS_AUTOSIZE_GROW         1
#define FORMS_AUTOSIZE_GROW_SHRINK  2

/* Cursor identifiers exposed to script via setCursor("..."). */
#define FORMS_CURSOR_DEFAULT   0
#define FORMS_CURSOR_ARROW     1
#define FORMS_CURSOR_HAND      2
#define FORMS_CURSOR_IBEAM     3
#define FORMS_CURSOR_WAIT      4
#define FORMS_CURSOR_CROSS     5
#define FORMS_CURSOR_SIZE_NS   6
#define FORMS_CURSOR_SIZE_WE   7
#define FORMS_CURSOR_SIZE_NWSE 8
#define FORMS_CURSOR_SIZE_NESW 9
#define FORMS_CURSOR_SIZE_ALL  10
#define FORMS_CURSOR_NO        11
#define FORMS_CURSOR_HELP      12
#define FORMS_CURSOR_APPSTART  13

typedef struct FormsSlot {
    int          alive;
    int          generation;
    ControlKind  kind;
    int          parent_slot;       /* -1 for top-level forms */
#if FORMS_HAVE_WIN32
    HWND         hwnd;
    WNDPROC      orig_proc;         /* for subclassed standard controls */
#endif
    int          x, y, w, h;
    int          visible;
    int          enabled;
    int          has_fore;
    int          has_back;
    unsigned int fore_color;        /* 0x00BBGGRR (Win32 COLORREF order) */
    unsigned int back_color;
#if FORMS_HAVE_WIN32
    HBRUSH       back_brush;
#endif
    int          dock;              /* one of FORMS_DOCK_* */
    int          has_font;
    char         font_face[64];
    int          font_size;
    int          font_bold;
    int          font_italic;
    int          font_underline;
    int          font_strikeout;
#if FORMS_HAVE_WIN32
    HFONT        hfont;
#endif
    int          border_style_set;
    int          border_style;
    int          has_opacity;
    int          opacity;           /* 0..255 */
    int          topmost;
    int          has_min_size, min_w, min_h;
    int          has_max_size, max_w, max_h;
    int          autosize;
    int          autosize_mode;
    int          pad_l, pad_t, pad_r, pad_b;
    int          margin_l, margin_t, margin_r, margin_b;
    int          anchor;
    int          anchor_l, anchor_t, anchor_r, anchor_b;
    int          anchor_w, anchor_h; /* parent client size at capture */
    int          tab_index;
    int          tab_stop;
    int          cursor_kind;
    char        *tooltip;
    int          accept_btn_slot;
    int          cancel_btn_slot;
#if FORMS_HAVE_WIN32
    HICON        hicon_small;
    HICON        hicon_big;
    HWND         tooltip_hwnd;
#endif
    /* Retained handle to the script-side instance so callbacks survive
     * the script returning. */
    CandoValue   inst_val;
    int          inst_val_held;
    int          has_lifeline;
} FormsSlot;

/* Storage + lock are file-scope in slots.c; exposed via extern so
 * forms_module.c (and future controls TUs) can keep poking fields
 * directly until phase 0.5 introduces handle-based access. */
extern FormsSlot g_slots[FORMS_MAX_SLOTS];
extern fm_mutex_t g_slot_mutex;

/* Find an unused slot, mark it alive, advance its generation, write
 * the kind / parent / default field values, and return its index.
 * Returns -1 on full table.  Caller must hold g_slot_mutex (the
 * _locked variant) or use slot_alloc which takes the lock for you. */
int slot_alloc_locked(ControlKind kind, int parent_slot);
int slot_alloc(ControlKind kind, int parent_slot);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_SLOTS_H */
