/*
 * src/core/dispatch.c -- pure dispatcher helpers.  See dispatch.h for
 * the contract.
 */

#include "dispatch.h"

#include <stddef.h>

const char *event_callback_name(EventKind k)
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
    case EV_TAB_CHANGED:        return "onTabChanged";
    case EV_NODE_SELECTED:      return "onNodeSelected";
    case EV_NODE_EXPANDED:      return "onNodeExpanded";
    case EV_NODE_COLLAPSED:     return "onNodeCollapsed";
    case EV_ITEM_ACTIVATED:          return "onItemActivated";
    case EV_LIST_SELECTION_CHANGED:  return "onSelectionChanged";
    case EV_NONE:               return NULL;
    }
    return NULL;
}
