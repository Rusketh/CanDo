# Forms module — full rewrite plan

> **Decisions locked in for this plan**
> - **Compatibility:** clean break. The new module replaces the existing
>   `modules/forms` wholesale. No legacy aliases. Old scripts must be
>   ported.
> - **Utility wrapper:** lives as a CanDo script module (`.cdo`) shipped
>   alongside the native `forms.dll`. The wrapper consumes the raw
>   native API; no extra C build target.
> - **Platforms:** Windows is the only real backend. Non-Windows builds
>   load a stub whose constructors throw immediately. The C internals
>   are organised so a future backend (GTK/Cocoa) could be added, but
>   that work is **not** in scope here.

---

## 1. Why a rewrite

The current module gets the "hello world" case right but breaks down
fast as soon as scripts grow:

| Symptom in today's module                                        | Root cause                                                              |
| ---------------------------------------------------------------- | ----------------------------------------------------------------------- |
| 5237-line single C TU; everything tangled together               | No separation between backend, slot/event core, and per-control logic.  |
| `setChecked` on a `Button` is a silent no-op                     | All controls share **one** meta table; methods do runtime kind checks.  |
| Hard cap of 256 controls per process                              | Static `g_slots[FORMS_MAX_SLOTS]` array.                                |
| Identity stamped as `__forms_slot` on the script instance        | Script can clobber the field; slot lifetime tied to a mutable property. |
| Missing modern WinForms surface (Tab/Split/Tree/ListView/Menus…) | Each control kind required hand-wiring across slot enum + WndProc + meta table; growth was discouraged. |
| No DPI awareness, no dark-mode hint, no modality, no drag/drop   | Backend was scoped at "draw HWND, route a few notifications" only.      |
| Scripts repeat huge amounts of boilerplate                        | No layered convenience API.                                             |

The clean-break rewrite fixes the architecture, expands the control
palette to mirror `System.Windows.Forms`, and adds a CanDo-side
utility layer.

---

## 2. Target architecture

### 2.1 Native side: split the one file into a directory

```
modules/forms/
├── Makefile
├── README.md
├── REWRITE_PLAN.md            ← this file
├── cando.api.json             ← regenerated; per-control type entries
├── forms_module.c             ← entry point only: cando_module_init, exports table
├── src/
│   ├── core/
│   │   ├── slots.{c,h}        ← dynamic slot table (vector, generation IDs, NIL = invalid)
│   │   ├── events.{c,h}       ← MPSC ring or growable queue, EventKind enum, push/pop
│   │   ├── manager.{c,h}      ← UI thread bootstrap, message-only HWND, command queue
│   │   ├── dispatch.{c,h}     ← drain queue, child VM, callback names, error trap
│   │   ├── identity.{c,h}     ← weak handle <-> CdoObject mapping (replaces __forms_slot)
│   │   ├── color.{c,h}        ← 0xRRGGBB <-> COLORREF, named-color table, parser
│   │   ├── font.{c,h}         ← HFONT cache, options-table parser, default GUI metrics
│   │   ├── geom.{c,h}         ← rect/size/point helpers, DPI scaling, padding/margin parsing
│   │   └── layout.{c,h}       ← dock, anchor, flow, table, splitter; pluggable layout vtable
│   ├── backend/
│   │   ├── backend.h          ← vtable: create/destroy/setX/getX/measure/...
│   │   ├── win32_common.{c,h} ← HWND helpers, subclass plumbing, WM_NOTIFY routing
│   │   ├── win32_form.c       ← top-level window class, accept/cancel, modality
│   │   ├── win32_panel.c      ← scrollable panel, group box, splitter, custom-draw
│   │   ├── win32_buttons.c    ← Button/CheckBox/RadioButton/ToggleButton
│   │   ├── win32_text.c       ← TextBox / RichTextBox / Label / LinkLabel
│   │   ├── win32_lists.c      ← ListBox / ComboBox / CheckedListBox
│   │   ├── win32_treeview.c
│   │   ├── win32_listview.c
│   │   ├── win32_tabs.c       ← TabControl, TabPage
│   │   ├── win32_progress.c   ← ProgressBar, TrackBar, Spinner, NumericUpDown
│   │   ├── win32_pickers.c    ← DateTimePicker, MonthCalendar
│   │   ├── win32_picture.c    ← PictureBox + draw-module bridge
│   │   ├── win32_status.c     ← StatusBar, ToolStrip
│   │   ├── win32_menu.c       ← MenuStrip, ContextMenu, MenuItem
│   │   ├── win32_dialogs.c    ← MessageBox, OpenFileDialog, SaveFileDialog, ColorDialog, FontDialog, FolderBrowserDialog
│   │   ├── win32_notify.c     ← NotifyIcon (tray)
│   │   ├── win32_timer.c      ← Timer (SetTimer + WM_TIMER)
│   │   ├── win32_dnd.c        ← drag/drop; OLE registration
│   │   └── stub.c             ← non-Windows fallback (every native throws)
│   ├── controls/
│   │   ├── ctl_form.{c,h}     ← native_* methods + meta table for Form
│   │   ├── ctl_button.{c,h}
│   │   ├── ctl_label.{c,h}
│   │   ├── ...                ← one .c per control kind
│   │   └── ctl_common.{c,h}   ← shared method implementations (setText, setSize, setEnabled, fonts, colors, padding/margin, anchor, dock, tooltip, cursor, focus, tab order, parent/children, destroy, getX/getY/getW/getH, refresh, hit test, bringToFront/sendToBack, ...)
│   └── api/
│       ├── meta_register.{c,h}← assemble per-control meta tables from common + per-kind
│       ├── enums.{c,h}        ← Dock/Anchor/AutoSizeMode/BorderStyle/Cursor/Color/MessageBoxButtons/...
│       └── exports.{c,h}      ← module table assembly (constructors + namespaces + enums)
├── script/
│   ├── forms.cdo              ← thin re-export of the .dll, version guard, helpers
│   ├── forms_util.cdo         ← UTILITY WRAPPER (see §6)
│   ├── forms_layout.cdo       ← optional: declarative layout helpers
│   └── forms_dialogs.cdo      ← optional: ergonomic message/file dialog wrappers
└── tests/
    ├── unit/                  ← C unit tests, headless
    │   ├── test_slots.c
    │   ├── test_events.c
    │   ├── test_color.c
    │   ├── test_geom.c
    │   ├── test_layout.c
    │   └── test_identity.c
    ├── smoke/                 ← script smoke checks, run on every platform
    │   └── smoke.cdo
    └── integration/           ← Windows-only, driven by user / manual CI
        ├── controls_gallery.cdo
        ├── tabs_and_split.cdo
        ├── menus_and_dialogs.cdo
        ├── treeview_listview.cdo
        └── custom_draw.cdo
```

Rule of thumb: each per-control source file owns its WndProc subclass,
its set of `setX/getX` natives, its create-options parser, and its
preferred-size measurement. Shared concerns (colour, font, layout)
sit in `core/`. The only file that #includes `<commctrl.h>` heavily
is `backend/`; the `controls/` layer talks to the backend through a
small vtable so the test harness can mock it.

### 2.2 Per-control meta tables

Replace the single `_meta.forms_control` with one meta table **per
control kind**, plus a shared base populated by `ctl_common`.

```
_meta.forms_control_base    ← setText, setSize, setEnabled, fonts, colors, …
_meta.forms_form            ← extends base + form-only methods
_meta.forms_button          ← extends base + setDialogResult, performClick
_meta.forms_textbox         ← extends base + setMultiline, setReadOnly, ...
_meta.forms_listbox         ← extends base + addItem, removeItem, getSelectedItems, ...
_meta.forms_tabcontrol      ← extends base + addTab, removeTab, getSelectedTab, ...
_meta.forms_treeview        ← extends base + addNode, expand/collapse, getSelectedNode
…
```

`meta_register.c` builds them at module init by:
1. Calling `ctl_common_register(base)`.
2. Cloning `base` keys into each kind's table (`meta_inherit`).
3. Layering kind-specific methods on top.

Net effect: methods that don't apply to a control aren't on its meta
table at all. `button:addItem("x")` is now a hard error from the VM,
not a silent no-op.

### 2.3 Identity model

Drop `__forms_slot` as a script-visible field. Replace with:

- A C-side **handle table** keyed by an opaque `u32 handle` (slot
  index packed with a generation counter — the high 12 bits).
- The handle is stored on the script-side instance via the existing
  `cdo_object_set_native_handle()` mechanism (used by `http`, `socket`
  modules). Scripts can't see or rewrite it.
- Helpers: `forms_handle_resolve(vm, value) -> FormsSlot* or NULL`,
  `forms_handle_check(vm, value, kind) -> FormsSlot*` (throws on
  mismatch — used by per-kind native methods).

Slot table is a growable vector (start at 64, double when full). The
generation counter still defends against stale handles after a slot is
freed and recycled.

### 2.4 Event / dispatch loop

Keep the "single dedicated UI thread + child VM dispatcher" model — it
already works. Improvements:

- Replace the fixed-size ring buffer with a growable queue (small
  initial capacity, double on overflow, never drop events).
- Add `EventKind` entries for the new controls: `EV_TAB_CHANGED`,
  `EV_TREE_NODE_EXPANDED`, `EV_TREE_NODE_COLLAPSED`,
  `EV_LIST_ITEM_ACTIVATED`, `EV_LIST_COLUMN_CLICKED`, `EV_DRAG_*`,
  `EV_PAINT`, `EV_TIMER_TICK`, `EV_MENU_ITEM_CLICKED`,
  `EV_NOTIFY_ICON_CLICKED`, `EV_DROP_FILES`, …
- Each event carries `(slot, generation, kind, payload union)`. The
  payload union grows: rects, point, indices, key+modifiers, an
  optional `wchar_t *` text fragment owned by the queue.
- Dispatcher uses a **named-callback registry** populated at meta-table
  build time so kind-specific handlers (`onTabChanged`,
  `onNodeExpanded`) are dispatched without hard-coded switches.
- Errors thrown from a callback get caught, logged via the `log`
  module if available, and don't kill the UI thread.

### 2.5 DPI, theming, accessibility

- Manifest the DLL as **Per-Monitor V2** DPI-aware via a linker
  resource. Document that the host (`cando.exe`) must also be PMv2 (it
  already ships a manifest; double-check during the rewrite).
- Add `forms.dpiFor(control) -> number` and have layout / measurement
  use it. Geometry setters interpret values in **logical pixels** by
  default; an opt-in `forms.scale = "device"` mode bypasses scaling.
- Enable visual styles via `InitCommonControlsEx` + the existing
  comctl32 v6 manifest. Add a `forms.darkMode` toggle that calls the
  undocumented but stable-since-1809 dark-mode APIs (`SetWindowTheme`
  + `WM_THEMECHANGED` propagation).
- Accessibility: every native control inherits MSAA from comctl32;
  expose `setAccessibleName` / `setAccessibleDescription` which call
  `SetWindowText` on the control's accessible-name property via
  `IAccessible`.

---

## 3. Control catalogue (target surface)

Greatly expanded vs. today. Each entry is a separate constructor on
the module table and a separate meta table.

### 3.1 Containers

| Constructor                           | Backed by                    | Notes                                          |
| ------------------------------------- | ---------------------------- | ---------------------------------------------- |
| `forms.Form([opts])`                  | top-level window             | accept/cancel button, modality, dialog result. |
| `forms.Panel(parent, [opts])`         | static class                 | optional border, custom paint.                 |
| `forms.ScrollPanel(parent, [opts])`   | panel + WS_VSCROLL/HSCROLL   | virtual viewport, `:scrollTo(x, y)`, anchored children stretch in viewport space. |
| `forms.GroupBox(parent, [opts])`      | BUTTON BS_GROUPBOX           | header text.                                   |
| `forms.TabControl(parent, [opts])`    | SysTabControl32              | child = TabPage; addTab/removeTab/onTabChanged.|
| `forms.TabPage(tabControl, [opts])`   | child panel attached to tab  | acts like a Panel inside the tab.              |
| `forms.SplitContainer(parent, [opts])`| custom (two panels + splitter)| `:setOrientation("horizontal" \| "vertical")`, `:setSplitterDistance(n)`, `:setFixedPanel("panel1" \| "panel2" \| "none")`. |
| `forms.FlowLayoutPanel(parent, [opts])`| panel + flow layout         | `:setFlowDirection("leftToRight" \| ...)`, `:setWrapContents(b)`. |
| `forms.TableLayoutPanel(parent, [opts])`| panel + grid layout       | `:setColumns(n)`, `:setRows(n)`, `:setColumnStyle(i, "auto" \| "absolute,n" \| "percent,n")`, `:add(child, col, row, [colSpan], [rowSpan])`. |
| `forms.Splitter(parent, [opts])`      | custom                       | drag-resize sibling.                           |

### 3.2 Inputs

| Constructor                                | Backed by                    |
| ------------------------------------------ | ---------------------------- |
| `forms.Button(parent, [opts])`             | BUTTON                       |
| `forms.CheckBox(parent, [opts])`           | BUTTON BS_AUTOCHECKBOX       |
| `forms.RadioButton(parent, [opts])`        | BUTTON BS_AUTORADIOBUTTON    |
| `forms.ToggleButton(parent, [opts])`       | BUTTON BS_PUSHLIKE           |
| `forms.TextBox(parent, [opts])`            | EDIT                         |
| `forms.RichTextBox(parent, [opts])`        | RichEdit50W                  |
| `forms.MaskedTextBox(parent, [opts])`      | EDIT + script-side mask validator |
| `forms.NumericUpDown(parent, [opts])`      | EDIT + UpDown                |
| `forms.ComboBox(parent, [opts])`           | COMBOBOX                     |
| `forms.ListBox(parent, [opts])`            | LISTBOX                      |
| `forms.CheckedListBox(parent, [opts])`     | LISTBOX LBS_OWNERDRAW...     |
| `forms.TrackBar(parent, [opts])`           | msctls_trackbar32            |
| `forms.DateTimePicker(parent, [opts])`     | SysDateTimePick32            |
| `forms.MonthCalendar(parent, [opts])`      | SysMonthCal32                |

### 3.3 Display

| Constructor                                | Backed by                    |
| ------------------------------------------ | ---------------------------- |
| `forms.Label(parent, [opts])`              | STATIC                       |
| `forms.LinkLabel(parent, [opts])`          | SysLink                      |
| `forms.PictureBox(parent, [opts])`         | STATIC SS_BITMAP / owner-draw|
| `forms.ProgressBar(parent, [opts])`        | msctls_progress32            |
| `forms.StatusBar(parent, [opts])`          | msctls_statusbar32           |
| `forms.ToolStrip(parent, [opts])`          | RebarWindow32 / custom       |

### 3.4 Trees, lists, grids

| Constructor                                | Backed by                                |
| ------------------------------------------ | ---------------------------------------- |
| `forms.TreeView(parent, [opts])`           | SysTreeView32                            |
| `forms.ListView(parent, [opts])`           | SysListView32 (Report/List/Icon/Tile)    |
| `forms.DataGrid(parent, [opts])`           | custom (built on ListView LVS_REPORT + cell editors). Phase 2; full DataGridView is out of scope. |

### 3.5 Menus / dialogs / shell

| Constructor                                | Backed by / Notes                                           |
| ------------------------------------------ | ----------------------------------------------------------- |
| `forms.MenuStrip(parent, [opts])`          | HMENU on a form's main menu bar.                            |
| `forms.ContextMenu([opts])`                | popup HMENU; attach via `:setContextMenu(menu)` on a control.|
| `forms.MenuItem({...})`                    | HMENU entry; supports nested items, separators, accelerators, checked state, icons. |
| `forms.MessageBox(text, [title], [opts])`  | MessageBoxW; returns the user's button choice as a string.   |
| `forms.OpenFileDialog([opts])`             | IFileOpenDialog (preferred over GetOpenFileName).            |
| `forms.SaveFileDialog([opts])`             | IFileSaveDialog                                              |
| `forms.FolderBrowserDialog([opts])`        | IFileOpenDialog with FOS_PICKFOLDERS                         |
| `forms.ColorDialog([opts])`                | ChooseColorW                                                 |
| `forms.FontDialog([opts])`                 | ChooseFontW                                                  |
| `forms.NotifyIcon([opts])`                 | Shell_NotifyIconW (tray icon, balloon).                      |

### 3.6 Non-visual helpers

| Constructor                                | Notes                                                       |
| ------------------------------------------ | ----------------------------------------------------------- |
| `forms.Timer([opts])`                      | SetTimer / WM_TIMER on the manager thread; `onTick`.         |
| `forms.ImageList([opts])`                  | comctl32 image list; consumed by ListView/TreeView/MenuItem. |
| `forms.ToolTip()`                          | shared TOOLTIPS_CLASS instance; preferred over per-control `setToolTip` (kept as a thin wrapper that forwards to a default ToolTip). |
| `forms.Clipboard`                          | namespace: `getText/setText`, `getImage/setImage`, `clear`.  |
| `forms.Screen`                             | namespace: `primary()`, `all()`, `fromControl(c)` returning `{x,y,w,h, workArea, dpi}`. |
| `forms.Cursor`                             | namespace: `position() -> x,y`, `setPosition(x,y)`, named cursors. |

### 3.7 Custom drawing

A new `forms.PaintSurface(parent, [opts])` control exposes:
- `onPaint = function(self, gfx) ... end` — `gfx` is a thin wrapper
  around a GDI HDC. We bridge to the existing `modules/draw` module so
  scripts use the same drawing primitives (`gfx:line(...)`,
  `gfx:fillRect(...)`, `gfx:drawText(...)`, image blits via PictureBox
  bitmaps).
- Double-buffered by default; `:setDoubleBuffered(false)` to opt out.
- `Panel`, `PictureBox`, `Form` also gain `onPaint` (owner-draw), but
  the dedicated `PaintSurface` is recommended for animations / games.

---

## 4. Layout system

Today's module has dock + anchor only. The rewrite adds:

- **Flow layout** (`FlowLayoutPanel`): children stack along an axis,
  wrapping when they hit the edge.
- **Table layout** (`TableLayoutPanel`): rows + columns with auto /
  absolute / percent sizing per axis.
- **Splitter** (`SplitContainer`): two-pane resizer.
- A **layout vtable** in `core/layout.h` so each container type
  supplies its own `arrange(parent, children, rect)`. Custom container
  controls implemented in script-land can register a CanDo callback.
- `setMinSize` / `setMaxSize` / `setPreferredSize` on every control,
  consumed uniformly by every layout.
- DPI-aware: padding/margin/min-size are in logical pixels.

---

## 5. Native API (script-visible) — the canonical surface

Brief style notes:
- camelCase only. No PascalCase aliases. No `Derma`-flavoured names.
- Methods that don't apply to a control are **not** present on its
  meta table (no silent no-ops).
- Geometry getters that return two numbers continue to use multi-value
  return (`local w, h = c:getSize()`), matching CanDo idiom.
- Constructors accept `(parent)`, `(parent, "text")`, or
  `(parent, { … options table … })`. Form is the same minus parent.

### 5.1 Common control methods (every meta table inherits)

```
:setText(s)                       :getText()
:setSize(w, h)                    :getSize()
:setLocation(x, y)                :getLocation()
:setWidth(n) / :setHeight(n)      :getWidth() / :getHeight()
:setMinSize(w, h)                 :setMaxSize(w, h)
:setPreferredSize(w, h)           :getPreferredSize()
:sizeToContent()                  :sizeToContentWidth() / :sizeToContentHeight()
:setVisible(b) / :show() / :hide():getVisible()
:setEnabled(b)                    :getEnabled()
:setFocus()                       :hasFocus()
:setParent(other)                 :getParent()
:getChildren()                    :contains(other)
:bringToFront()                   :sendToBack()
:setForeColor(...) / :clearForeColor()
:setBackColor(...) / :clearBackColor()
:setFont(...)                     :clearFont() / :getFont()
:setFontSize(n) / :setBold(b) / :setItalic(b) / :setUnderline(b) / :setStrikeout(b)
:setPadding(...)                  :getPadding()
:setMargin(...)                   :getMargin()
:setAnchor(...)                   :getAnchor()
:setDock(side)                    :getDock()
:setAutoSize(b) / :setAutoSizeMode("grow"|"growShrink") / :getAutoSize()
:setBorderStyle("none"|"single"|"fixed3D")
:setCursor(name)                  :setToolTip(s)
:setAccessibleName(s)             :setAccessibleDescription(s)
:setTabIndex(n) / :setTabStop(b) / :getTabIndex()
:refresh() / :invalidate([rect])
:relayout()
:destroy()                        :isAlive()
:contextMenu(menu)                ← attach a ContextMenu
:dragEnable(types)                ← types: array of "files" | "text" | "image"
:on<Event> = function(self, …) end (assign to instance property)
```

Events on the base: `onClick`, `onMouseDown`, `onMouseUp`,
`onMouseMove`, `onMouseEnter`, `onMouseLeave`, `onMouseWheel`,
`onKeyDown`, `onKeyUp`, `onKeyPress`, `onFocus`, `onBlur`, `onResize`,
`onPaint`, `onContextMenu`, `onDragEnter`, `onDragOver`, `onDrop`.

### 5.2 Form-specific

```
:center()                         :setIcon(path)
:setOpacity(a) / :getOpacity()    :setTopMost(b)
:setResizable(b)                  :setMinimizeBox(b) / :setMaximizeBox(b)
:setShowInTaskbar(b)              :setStartPosition("manual"|"centerScreen"|"centerParent")
:setWindowState("normal"|"maximized"|"minimized")  :getWindowState()
:maximize() / :minimize() / :restore()
:flash([n])
:setMenu(menuStrip)               ← attach a MenuStrip
:setStatusBar(statusBar)
:setAcceptButton(btn)             :setCancelButton(btn)
:showModal([owner]) -> dialogResult ← blocks the calling VM until close
:close([dialogResult])
Events: onClose, onShown, onActivated, onDeactivated, onResize, onMove,
        onDpiChanged
```

### 5.3 Per-control highlights (not exhaustive)

- **TabControl:** `:addTab(title) -> TabPage`, `:removeTab(index|page)`,
  `:getTabCount()`, `:getSelectedIndex()`, `:setSelectedIndex(i)`,
  `onTabChanging`, `onTabChanged`.
- **ScrollPanel:** `:setAutoScroll(b)`, `:setScrollSize(w, h)`,
  `:scrollTo(x, y)`, `:getScrollPosition()`, `onScroll`.
- **TreeView:** `:addNode(parentNode|nil, text, [opts]) -> Node`,
  `Node:addChild(...)`, `Node:remove()`, `Node:expand()`,
  `Node:collapse()`, `:getSelectedNode()`, `:setImageList(imgList)`,
  `onNodeSelected`, `onNodeExpanded`, `onNodeCollapsed`,
  `onNodeChecked`, `onNodeDoubleClicked`, `onLabelEdit`.
- **ListView:** `:setView("details"|"list"|"largeIcon"|"smallIcon"|"tile")`,
  `:addColumn(title, [width])`, `:addItem({...row}) -> Item`,
  `:setImageList(small, large)`, `:setCheckBoxes(b)`,
  `:getSelectedItems()`, `:setMultiSelect(b)`, `onItemActivated`,
  `onItemChecked`, `onColumnClicked`, `onItemSelectionChanged`.
- **MenuStrip / ContextMenu / MenuItem:** declarative tree:
  ```
  forms.MenuStrip(form, {
      { text = "&File", items = {
          { text = "&Open…", shortcut = "Ctrl+O", onClick = ... },
          { separator = true },
          { text = "E&xit", onClick = ... }
      }},
      { text = "&Help", items = { { text = "&About", onClick = ... } } }
  })
  ```
- **Dialogs:** `forms.MessageBox(text, title, { buttons = "yesNo",
  icon = "warning" }) -> "yes"|"no"|"cancel"|...`. File dialogs return
  the chosen path(s) or `NULL` on cancel.
- **NotifyIcon:** `:setIcon(path)`, `:setText(s)`, `:show()`,
  `:hide()`, `:balloon(title, body, [icon])`, `:setContextMenu(menu)`,
  `onClick`, `onDoubleClick`.
- **Timer:** `:setInterval(ms)`, `:start()`, `:stop()`, `onTick`.

### 5.4 Module-level namespaces

```
forms.VERSION         "2.0.0"
forms.platform        "windows" | "stub"
forms.supported       boolean
forms.run([form])     run the message pump until every form is closed
                      (script can also rely on the implicit lifeline as today)
forms.exit()          break out of run() / unblock showModal()
forms.dpiFor(control) effective DPI
forms.scale           "logical" (default) or "device"
forms.darkMode(b)     toggle dark-mode hint
forms.Color           (table of named 0xRRGGBB colours)
forms.Cursor          (table of cursor name strings)
forms.Dock            (top/bottom/left/right/fill/none integers)
forms.Anchor          (left/top/right/bottom bitmask)
forms.AutoSizeMode    (grow / growShrink)
forms.BorderStyle     (none / single / fixed3D)
forms.MessageBoxButtons   (ok / okCancel / yesNo / yesNoCancel / abortRetryIgnore)
forms.MessageBoxIcon      (none / info / warning / error / question)
forms.DialogResult        (ok / cancel / yes / no / abort / retry / ignore / none)
forms.Clipboard / forms.Screen  (namespaces)
```

---

## 6. Utility wrapper layer (`script/forms_util.cdo`)

Goal: cut the boilerplate in half for common UI patterns. The wrapper
**only** uses the public native API; nothing in `forms.dll` depends on
it. Scripts can opt out by ignoring the file.

### 6.1 Style-of-API examples

```cando
VAR forms = include("./forms.dll");
VAR ui    = include("./forms_util.cdo");   // re-exports forms.* + helpers

// Declarative form construction
VAR f = ui.Window({
    title = "Hello",
    size  = { 480, 360 },
    center = TRUE,
    body = ui.Stack({
        direction = "vertical",
        padding   = 12,
        gap       = 8,
        children  = [
            ui.Label("Name:"),
            ui.TextBox({ id = "name", placeholder = "your name" }),
            ui.CheckBox({ id = "agree", text = "I agree" }),
            ui.Row({
                gap = 8,
                children = [
                    ui.Button({ text = "Cancel", role = "cancel" }),
                    ui.Button({ text = "OK",     role = "accept",
                                onClick = function(self) {
                                    print("name =", self:form():field("name"):getText());
                                    self:form():close("ok");
                                }
                            })
                ]
            })
        ]
    })
});

VAR result = f:showModal();   // "ok" or "cancel"
```

### 6.2 Feature list

- **Declarative trees:** `ui.Window`, `ui.Panel`, `ui.Stack` (vertical
  / horizontal flow), `ui.Row`, `ui.Column`, `ui.Grid`, `ui.Tabs`,
  `ui.Split`, `ui.ScrollArea`, `ui.Group`. Each accepts a `children`
  array of nested descriptors.
- **`id` registry:** any descriptor with `id = "name"` is registered
  on the form; `form:field("name")` returns the live native instance.
- **`role` shorthand:** `role = "accept" | "cancel"` wires the button
  to `setAcceptButton` / `setCancelButton` and a `dialogResult`.
- **`bind` system:** `ui.TextBox({ bind = state.name })` wires
  `onTextChanged` to a small observable in the wrapper, plus
  `:set(name, "value")` to write back. `state` is a CanDo object built
  by `ui.observable({ name = "", agree = FALSE })`.
- **`ui.dialog.message(text, [opts])`,
  `ui.dialog.confirm(text, [opts])`,
  `ui.dialog.prompt(text, [opts])`,
  `ui.dialog.openFile([opts])`,
  `ui.dialog.saveFile([opts])`,
  `ui.dialog.color([initial])`,
  `ui.dialog.font([initial])`** — high-level wrappers around the
  native dialog constructors.
- **`ui.toast(form, text, [opts])`** — non-modal short-lived label.
- **`ui.layout.dock`, `ui.layout.flow`, `ui.layout.grid`** — programmatic
  builders that call `setDock` / set up `FlowLayoutPanel` /
  `TableLayoutPanel` from a small description.
- **`ui.menu({...tree})`** — converts a CanDo tree directly to
  `MenuStrip` / `ContextMenu`.
- **`ui.applyTheme(form, theme)`** — walks the tree applying a font
  family, base size, palette to every control.
- **`ui.iconLoader(path)`** / `ui.imageList({...})` — convenience over
  `forms.ImageList`.
- **`ui.eventBus()`** — small pub/sub for cross-control coordination
  (e.g. status-bar updates from any handler).

The wrapper file is plain CanDo; the only "magic" is that it
auto-promotes plain table descriptors into native control instances at
construction time. Zero new C code, fully iterable in script land.

---

## 7. Migration notes (clean break)

Because we drop the existing API, the rewrite is essentially a new
version. Plan:

1. New module is shipped at version `2.0.0`.
2. Old (`1.x`) module's `forms.dll` is **deleted** from the tree on
   the rewrite branch — no aliasing, no compatibility shims.
3. `docs/forms.md` is rewritten from scratch.
4. The integration script (`test_forms.cdo`) is deleted; the new
   `tests/integration/controls_gallery.cdo` becomes the canonical
   sample.
5. Any references in other modules / docs are audited and updated.
6. Add a one-page `MIGRATING.md` that diffs old → new for every
   removed / renamed call so people porting scripts have a checklist.

---

## 8. Phased delivery

Splitting the rewrite into mergeable phases lets the branch stay
reviewable. Each phase ends with green C unit tests + a working smoke
check.

### Phase 0 — scaffolding (no behaviour change yet)
- Create `src/`, `tests/`, `script/` directories.
- Move colour, font, layout, slot, event-queue helpers into separate
  TUs. Existing tests still pass against the moved code.
- Introduce the per-control-meta-table builder but populate it from a
  single shared list (preserves today's behaviour).
- Switch to a growable slot table and the handle-based identity model.

### Phase 1 — control catalogue base
- Per-control C files for the existing 19 controls.
- Per-control meta tables (no-op methods removed).
- Anchor / dock / autosize / padding / margin re-implemented through
  the new `core/layout.h` vtable.
- Phase 1 ships a clean, smaller, type-checked module that's already
  better than today's even before new controls land.

### Phase 2 — new containers + layouts
- `Panel` regains scroll variant: `ScrollPanel`.
- `TabControl` + `TabPage`.
- `SplitContainer`, `Splitter`.
- `FlowLayoutPanel`, `TableLayoutPanel`.
- `setMinSize` / `setMaxSize` / `setPreferredSize` on every control.

### Phase 3 — trees / lists / grid
- `TreeView`, `ListView`, `CheckedListBox`.
- `ImageList`.
- DataGrid (lightweight, ListView-LVS_REPORT–based) — phase 3b /
  optional.

### Phase 4 — menus, dialogs, tray
- `MenuStrip`, `ContextMenu`, `MenuItem`.
- `MessageBox`, `OpenFileDialog`, `SaveFileDialog`,
  `FolderBrowserDialog`, `ColorDialog`, `FontDialog`.
- `NotifyIcon`.
- `Timer`, `ToolTip` (object).

### Phase 5 — drawing + drag/drop
- `PaintSurface` + bridge to `modules/draw`.
- Owner-draw `onPaint` for `Panel` / `PictureBox` / `Form`.
- Drag-and-drop (files, text, image).

### Phase 6 — utility wrapper (`script/forms_util.cdo`)
- Declarative builders.
- `id` registry, `role`, `bind`.
- `ui.dialog.*`, `ui.menu`, `ui.layout.*`, `ui.applyTheme`.

### Phase 7 — DPI, dark mode, accessibility, polish
- PMv2 manifest, logical-pixel scaling.
- `forms.darkMode(b)`.
- `setAccessibleName` / `setAccessibleDescription`.
- `forms.run` / `forms.exit` semantics, `showModal` finalised.

Phases 0–1 are non-negotiable; everything else can ship as it lands.

---

## 9. Testing strategy

- **C unit tests** cover the pure pieces: slot vector grow/recycle,
  generation guard, event queue grow/wrap, colour parsing (hex, named,
  3-arg / packed-int variants), DPI scaling math, layout (dock,
  anchor, flow, table) — none of these need an HWND. CI continues to
  run them on Linux + macOS.
- **Smoke script** (`tests/smoke/smoke.cdo`) loads the module, asserts
  the namespaces and constants exist, and on non-Windows asserts every
  constructor throws. Fast enough for every PR.
- **Integration scripts** (`tests/integration/*.cdo`) are Windows-only
  and human-driven. Each sample is a small, self-contained demo of one
  feature area. Add a `--auto-close N` arg so we can run them in a
  Windows VM if/when CI gets one.
- **Regression catalogue:** keep small `.cdo` files reproducing every
  bug we fix, executed by the smoke runner where possible.

---

## 10. Risks + open questions

| Risk                                          | Mitigation                                                       |
| --------------------------------------------- | ---------------------------------------------------------------- |
| Modal dialog (`showModal`) blocking the calling VM thread while events still need to dispatch in the manager thread. | The manager thread already owns the message pump; `showModal` blocks the script VM via a condvar, while the UI thread keeps pumping. Existing `Command` round-trip pattern proves this is feasible. |
| Drag-and-drop requires OLE init.              | Initialise OLE on the manager thread; uninit on shutdown.        |
| RichEdit class needs `Riched20.dll` / `Msftedit.dll` loaded by name. | Lazy-load on first `RichTextBox` construction; clear error message if it fails. |
| TableLayoutPanel measurement is genuinely fiddly. | Borrow the WinForms algorithm (well-documented) and pin it in `core/layout.c` with unit tests. |
| Dark-mode APIs are technically undocumented.  | Wrap behind `forms.darkMode(b)`; on failure (older Windows) downgrade silently. |
| Scope creep — the catalogue is large.         | Strict phase gates. Phases 0–1 deliver value alone; any phase past 4 is bonus.|

Open questions to confirm during implementation:
- Should `forms.run()` be required to block, or do we keep today's
  implicit "lifeline keeps the process alive" model? Recommend: keep
  the lifeline, add `forms.run()` as an optional explicit blocker for
  scripts that want a single line.
- Should the wrapper layer be auto-loaded from `forms.dll`'s init, or
  remain a separate `include("./forms_util.cdo")`? Recommend the
  latter — keeps the .dll free of script dependencies and lets people
  swap in their own wrapper.

---

## 11. Acceptance criteria (rewrite is "done")

- `forms.dll` builds with no MinGW runtime DLL deps (current CI rule
  preserved).
- All Phase 0–1 C unit tests pass on Linux/macOS/Windows.
- Phase 0–4 integration scripts run cleanly on a Windows host.
- The control gallery script demonstrates every constructor in §3 and
  every base method in §5.1.
- `MIGRATING.md` documents every breaking change.
- `docs/forms.md` is rewritten end-to-end, no stale references.
- The wrapper sample in §6.1 is in `tests/integration/` and runs.

---

*End of plan.*
