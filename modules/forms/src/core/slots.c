/*
 * src/core/slots.c -- slot-table storage + allocator.  See slots.h
 * for the contract.
 */

#include "slots.h"

#include <stddef.h>

FormsSlot  g_slots[FORMS_MAX_SLOTS];
fm_mutex_t g_slot_mutex;

int slot_alloc_locked(ControlKind kind, int parent_slot)
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
            s->has_fore = 0;
            s->has_back = 0;
            s->fore_color = 0;
            s->back_color = 0;
            s->dock = FORMS_DOCK_NONE;
            s->has_font = 0;
            s->font_face[0] = 0;
            s->font_size = 0;
            s->font_bold = 0;
            s->font_italic = 0;
            s->font_underline = 0;
            s->font_strikeout = 0;
            s->border_style_set = 0;
            s->border_style = 0;
            s->has_opacity = 0;
            s->opacity = 255;
            s->topmost = 0;
            s->has_min_size = 0;
            s->has_max_size = 0;
            s->min_w = s->min_h = 0;
            s->max_w = s->max_h = 0;
            s->autosize      = 0;
            s->autosize_mode = FORMS_AUTOSIZE_GROW_SHRINK;
            s->pad_l = s->pad_t = s->pad_r = s->pad_b = 0;
            s->margin_l = s->margin_t = s->margin_r = s->margin_b = 0;
            s->anchor   = FORMS_ANCHOR_DEFAULT;
            s->anchor_l = s->anchor_t = s->anchor_r = s->anchor_b = 0;
            s->anchor_w = s->anchor_h = 0;
            s->tab_index = -1;
            s->tab_stop  = 1;
            s->cursor_kind = FORMS_CURSOR_DEFAULT;
            s->tooltip = NULL;
            s->accept_btn_slot = -1;
            s->cancel_btn_slot = -1;
            s->auto_scroll = 0;
            s->scroll_w = s->scroll_h = 0;
            s->scroll_x = s->scroll_y = 0;
#if FORMS_HAVE_WIN32
            s->hwnd        = NULL;
            s->orig_proc   = NULL;
            s->back_brush  = NULL;
            s->hfont       = NULL;
            s->hicon_small = NULL;
            s->hicon_big   = NULL;
            s->tooltip_hwnd = NULL;
#endif
            return i;
        }
    }
    return -1;
}

int slot_alloc(ControlKind kind, int parent_slot)
{
    FM_MUTEX_LOCK(&g_slot_mutex);
    int slot = slot_alloc_locked(kind, parent_slot);
    FM_MUTEX_UNLOCK(&g_slot_mutex);
    return slot;
}
