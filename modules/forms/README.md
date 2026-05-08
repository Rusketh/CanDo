# forms — native Windows GUI module for CanDo

`forms.dll` is a CanDo binary module that lets scripts build native
Windows GUIs.  The public surface is shaped after
`System.Windows.Forms`; every form and control is a script object
configured with chained `obj:setX(...)` setters, with event handlers
written as functions to named properties (`onClick`,
`onTextChanged`, …).

```cando
VAR forms = include("./forms.dll");

VAR f = forms.Form();
f:setText("Hello, CanDo");
f:setSize(400, 300);
f:center();

VAR b = forms.Button(f);
b:setText("Click me");
b:setLocation(20, 20);
b:setFont("Segoe UI", 12);
b:setBold(TRUE);
b:setForeColor("white");
b:setBackColor("cornflowerblue");
b.onClick = function(self) { print("clicked!"); };

f:show();
```

The script is **never blocked** by an open form.  Every form holds a
VM lifeline that keeps the process alive until it is destroyed
(either by `self:destroy()` from a callback, or by closing the last
form's window).  Callbacks run on a dedicated manager thread inside
a child VM that shares globals + handles with the script's VM.

## Table of contents

- [Platforms](#platforms)
- [Loading](#loading)
- [Construction conventions](#construction-conventions)
- [Common control methods](#common-control-methods)
- [Forms](#forms)
- [Inputs](#inputs)
- [Containers](#containers)
- [Lists / trees](#lists--trees)
- [Display controls](#display-controls)
- [Menus](#menus)
- [Dialogs](#dialogs)
- [Tray, timers, custom paint](#tray-timers-custom-paint)
- [Layout cookbook](#layout-cookbook)
- [Events catalogue](#events-catalogue)
- [Enums and namespaces](#enums-and-namespaces)
- [The `forms_util.cdo` wrapper](#the-forms_utilcdo-wrapper)
- [DPI and dark mode](#dpi-and-dark-mode)
- [Accessibility](#accessibility)
- [Worked example](#worked-example)
- [Building](#building)
- [Files](#files)

## Platforms

| Build               | Behaviour                                                |
| ------------------- | -------------------------------------------------------- |
| `forms.dll` (Win32) | full implementation against User32 / GDI / Comctl32 /     Comdlg32 / Shell32 / DwmApi / Ole32. |
| `forms.so` / `.dylib` | stub: loads cleanly so feature-detect works (`forms.supported == FALSE`), every constructor throws `"forms is only supported on Windows"`. |

DPI awareness is declared as **Per-Monitor V2** during module init
(falls back silently on Win8.1 and pre-1703 Win10).

## Loading

```cando
VAR forms = NULL;
TRY {
    forms = include("./forms.dll");
} CATCH (e) {
    forms = include("./forms.so");
}

IF forms.supported == FALSE {
    print(`forms not supported on this platform (${forms.platform})`);
    EXIT(0);
}
```

Module-level fields:

| Field              | Type    | Meaning                                  |
| ------------------ | ------- | ---------------------------------------- |
| `forms.VERSION`    | string  | semantic version of the binary            |
| `forms.platform`   | string  | `"windows"` or `"stub"`                  |
| `forms.supported`  | bool    | `TRUE` only on Windows                   |

Module-level helpers:

| Helper                              | Returns / does                          |
| ----------------------------------- | --------------------------------------- |
| `forms.dpiFor(control)`             | effective DPI of the control's monitor  |
| `forms.darkMode(b)`                 | toggle the immersive-dark-mode hint on every alive form |
| `forms.MessageBox(text, [title], [opts])` | see [Dialogs](#dialogs)            |
| `forms.OpenFileDialog([opts])` / `SaveFileDialog([opts])` / `FolderBrowserDialog([opts])` / `ColorDialog([initial])` / `FontDialog()` | see [Dialogs](#dialogs) |

## Construction conventions

Every constructor accepts one of three call shapes:

```cando
forms.Button(parent)                          // bare
forms.Button(parent, "Save")                  // text shorthand
forms.Button(parent, {                        // options table
    text:     "Save",
    location: [20, 20],
    size:     [120, 30],
    onClick:  function(self) { print("saved!"); }
})
```

Forms take the same shapes minus `parent`.  Options-table keys mirror
the `setX` methods (drop the `set`, lowercase the first letter):
`text`, `size`, `location`, `width`, `height`, `enabled`, `visible`,
`font`, `foreColor`, `backColor`, `padding`, `margin`, `dock`,
`anchor`, `autoSize`, `cursor`, `toolTip`, `tabIndex`, `tabStop`,
`borderStyle`, plus per-control extras and any `onX` event handler.

Identity is held internally; there are no public fields to corrupt.

## Common control methods

Every control's meta table chains `__index` to `_meta.forms_control_base`,
which carries the methods below.  Methods that don't apply to a
specific kind are not on its meta table — calling
`button:addItem("x")` is a hard error from the VM, not a silent no-op.

### Text

```
ctrl:setText(s)              ctrl:getText()
```

### Geometry

```
ctrl:setSize(w, h)           ctrl:getSize()              -- returns w, h
ctrl:setLocation(x, y)       ctrl:getLocation()          -- returns x, y
ctrl:setWidth(n)             ctrl:setHeight(n)
ctrl:getWidth()              ctrl:getHeight()
ctrl:sizeToContent()         ctrl:sizeToContentWidth()   ctrl:sizeToContentHeight()
ctrl:getPreferredSize()      -- returns w, h
```

### Visibility / state

```
ctrl:show()                  ctrl:hide()
ctrl:setVisible(b)           ctrl:getVisible()
ctrl:setEnabled(b)           ctrl:getEnabled()
ctrl:focus()                 ctrl:hasFocus()
ctrl:isOpen()
ctrl:destroy()
```

### Hierarchy

```
ctrl:setParent(other)        ctrl:getParent()
ctrl:getChildren()           ctrl:contains(other)
ctrl:bringToFront()          ctrl:sendToBack()
```

### Look

```
ctrl:setForeColor(...)       ctrl:getForeColor()        ctrl:clearForeColor()
ctrl:setBackColor(...)       ctrl:getBackColor()        ctrl:clearBackColor()
ctrl:setFont(...)            ctrl:getFont()             ctrl:clearFont()
ctrl:setFontSize(n)
ctrl:setBold(b)              ctrl:setItalic(b)
ctrl:setUnderline(b)         ctrl:setStrikeout(b)
ctrl:setBorderStyle(s)       -- "none" | "single" | "fixed3D"
ctrl:setCursor(name)         -- see forms.Cursor
ctrl:setToolTip(s)
```

`setForeColor` / `setBackColor` accept three numbers `(r, g, b)`, a
packed `0xRRGGBB` integer, a hex string `"#RGB"` / `"#RRGGBB"` /
`"#AARRGGBB"`, a CSS-style name (`"cornflowerblue"`), or any
`forms.Color.*` constant.

### Layout

```
ctrl:setPadding(...)         ctrl:getPadding()           -- 1, 2, or 4 numbers
ctrl:setMargin(...)          ctrl:getMargin()
ctrl:setAnchor(s)            ctrl:getAnchor()
   -- "left top right bottom", "all", "none", or a numeric mask
ctrl:setDock(side)           ctrl:getDock()
   -- "top" | "bottom" | "left" | "right" | "fill" | "none"
ctrl:setAutoSize(b)          ctrl:getAutoSize()
ctrl:setAutoSizeMode(s)      -- "grow" | "growShrink"
ctrl:relayout()              -- re-run parent's layout pass
ctrl:refresh()               -- force redraw
ctrl:invalidate([rect])      -- schedule WM_PAINT
```

### Tab order, accessibility

```
ctrl:setTabIndex(n)          ctrl:getTabIndex()
ctrl:setTabStop(b)
ctrl:setAccessibleName(s)
ctrl:setAccessibleDescription(s)
```

### Generic events

Set as instance properties:

```
ctrl.onClick           = function(self, button, x, y) { ... }
ctrl.onMouseDown       = function(self, button, x, y) { ... }
ctrl.onMouseUp         = function(self, button, x, y) { ... }
ctrl.onMouseMove       = function(self, x, y)         { ... }
ctrl.onKeyDown         = function(self, vk)           { ... }
ctrl.onKeyUp           = function(self, vk)           { ... }
ctrl.onFocus           = function(self)               { ... }
ctrl.onBlur            = function(self)               { ... }
ctrl.onResize          = function(self, w, h)         { ... }   -- forms / panels
ctrl.onPaint           = function(self)               { ... }   -- PaintSurface
```

Per-kind events are documented under each control's section and
indexed in [Events catalogue](#events-catalogue).

## Forms

```cando
VAR f = forms.Form({
    title:         "My App",
    size:          [800, 600],
    minSize:       [480, 320],
    icon:          "assets/app.ico",
    center:        TRUE
});
```

Form-only methods (no-op on child controls):

```
f:center()                   f:setIcon(path)
f:setOpacity(a)              f:getOpacity()      -- 0..1 float, or 0..255 int in
f:setTopMost(b)
f:setMinSize(w, h)           f:setMaxSize(w, h)
f:setResizable(b)            f:setMinimizeBox(b)   f:setMaximizeBox(b)
f:setShowInTaskbar(b)
f:setWindowState(s)          f:getWindowState()    -- "normal" | "maximized" | "minimized"
f:maximize()                 f:minimize()          f:restore()
f:flash([n])
f:setAcceptButton(btn)       f:setCancelButton(btn)
f:setMenu(menustrip)         -- attach a forms.MenuStrip
f:show()                     f:close([dialogResult])
```

Form-only events:

```
f.onShown        = function(self) { ... }
f.onClose        = function(self) { ... }
f.onResize       = function(self, w, h) { ... }
```

## Inputs

### Button

```cando
VAR b = forms.Button(parent, {
    text:     "Save",
    size:     [96, 28],
    onClick:  function(self) { ... }
});
```

### CheckBox / RadioButton

```cando
VAR c = forms.CheckBox(parent, { text: "Remember me" });
c:setChecked(b)              c:getChecked()
c.onClick = function(self) { print(self:getChecked()); };

VAR r1 = forms.RadioButton(group, { text: "Small" });
VAR r2 = forms.RadioButton(group, { text: "Medium" });
```

### TextBox

```cando
VAR t = forms.TextBox(parent, {
    placeholder: "your name",
    maxLength:   64
});

t:setMultiline(b)            t:setReadOnly(b)
t:setPlaceholder(s)          t:setMaxLength(n)
t:setPasswordChar(c)         t:setTextAlign("left" | "center" | "right")
t:selectAll()                t:appendText(s)             t:clearText()
t:setValue(s)                t:getValue()

t.onTextChanged = function(self) { print(self:getText()); };
```

### NumericUpDown / TrackBar / Spinner

```cando
VAR n = forms.NumericUpDown(parent, { value: 50 });
n:setValue(75)               n:getValue()
n:setRange(lo, hi)           n:setIncrement(n)
n.onValueChanged = function(self) { ... };

VAR t = forms.TrackBar(parent, { orientation: "horizontal" });
t:setValue(n)                t:getValue()
t:setRange(lo, hi)           t:setTickFrequency(n)
t:setSmallStep(n)            t:setLargeStep(n)
```

### ComboBox / ListBox

```cando
VAR c = forms.ComboBox(parent, { items: ["Apples", "Bananas"] });
c:addItem("Cherries")        c:removeItem(i)             c:clearItems()
c:getItem(i)                 c:getItems()                c:getItemCount()
c:getSelectedIndex()         c:setSelectedIndex(i)
c:clear()
c.onSelectionChanged = function(self) { ... };
```

`forms.ListBox` exposes the same item surface.

### DateTimePicker / MonthCalendar

```cando
VAR d = forms.DateTimePicker(parent);
d.onValueChanged = function(self) { ... };

VAR m = forms.MonthCalendar(parent);
m.onSelectionChanged = function(self) { ... };
```

## Containers

### Panel / GroupBox

```cando
VAR p = forms.Panel(form, { dock: "fill", borderStyle: "single" });
VAR g = forms.GroupBox(form, { text: "Options", size: [200, 120] });
```

### ScrollPanel

```cando
VAR sp = forms.ScrollPanel(form, { dock: "fill" });
sp:setAutoScroll(TRUE);
sp:setScrollSize(2000, 2000);
sp:scrollTo(x, y)            sp:getScrollPosition()    -- returns x, y
```

### TabControl

```cando
VAR tabs = forms.TabControl(form, { dock: "fill" });
tabs:addTab("General")       tabs:addTab("Advanced")
tabs:setSelectedIndex(0)     tabs:getSelectedIndex()   tabs:getTabCount()
tabs:removeTab(i)            tabs:clearTabs()
tabs:getTabText(i)           tabs:setTabText(i, title)
tabs.onTabChanged = function(self, idx) { ... };
```

To switch content panels per tab, manage child visibility in the
`onTabChanged` handler (or use `ui.Tabs` from
[`forms_util.cdo`](#the-forms_utilcdo-wrapper)).

### FlowLayoutPanel

```cando
VAR flow = forms.FlowLayoutPanel(form, {
    dock:          "top",
    flowDirection: "leftToRight",  -- "rightToLeft" | "topDown" | "bottomUp"
    wrapContents:  TRUE
});
FOR i IN 1 -> 10 {
    forms.Button(flow, { text: `B${i}`, size: [60, 24], margin: 2 });
}
```

The arrange callback is registered automatically; it honours each
child's margin and the container's padding.  Children with explicit
`dock` are skipped so dock + flow can compose.

### TableLayoutPanel

```cando
VAR t = forms.TableLayoutPanel(form, { dock: "fill" });
t:setColumns(2);
t:setRows(3);
t:setCellPadding(4);

t:add(forms.Label(t, "Name:"),  0, 0);
t:add(forms.TextBox(t),          1, 0);
t:add(forms.Label(t, "Email:"), 0, 1);
t:add(forms.TextBox(t),          1, 1);
t:add(forms.RichTextBox(t),     0, 2, 2, 1);   -- colSpan = 2, rowSpan = 1
```

Children with no explicit `cell` placement are auto-placed in
row-major order.  Auto-measured column widths and row heights take
the max preferred size of single-cell children; leftover space is
distributed evenly across empty axes.

### Splitter

```cando
VAR side = forms.Panel(f,    { dock: "left", size: [200, 0] });
VAR sp   = forms.Splitter(f, { dock: "left", size: [4, 0] });
VAR body = forms.Panel(f,    { dock: "fill" });
-- sp's default target is the previous alive sibling (`side`); drag
-- horizontally to resize.

sp:setOrientation("vertical")    -- or "horizontal"
sp:setTarget(other)              -- explicit drag target
```

Splitter uses Win32 mouse capture to track drags; the parent's
layout pass re-runs on every drag-move so docks and arrange
callbacks (Flow, Table) follow the new sibling size.

## Lists / trees

### TreeView

Uses opaque numeric handles for nodes (`HTREEITEM` cast through
`uintptr_t` to a double; the round-trip is exact for every Windows
pointer model).

```cando
VAR tv = forms.TreeView(form, { dock: "left", size: [220, 0] });

VAR root = tv:addNode(NULL, "Project");
VAR src  = tv:addNode(root, "src");
tv:addNode(src, "main.c");
tv:addNode(src, "util.c");

tv:expandNode(root)              tv:collapseNode(root)
tv:setSelectedNode(handle)       tv:getSelectedNode()      -- returns handle
tv:getNodeText(handle)           tv:setNodeText(handle, s)
tv:removeNode(handle)            tv:clearNodes()
tv:getNodeCount()

tv.onNodeSelected  = function(self, h) { print(self:getNodeText(h)); };
tv.onNodeExpanded  = function(self, h) { ... };
tv.onNodeCollapsed = function(self, h) { ... };
```

### ListView

Reports / list / icon / tile views.  Multi-column rows accept either
a single string (column 0 only) or an array of strings (one per
column; missing columns are blank).

```cando
VAR lv = forms.ListView(form, { dock: "fill" });
lv:addColumn("Name", 200);
lv:addColumn("Size", 80);
lv:addColumn("Modified", 140);
lv:setFullRowSelect(TRUE);
lv:setGridLines(TRUE);

lv:addItem(["main.c",  "12 kB", "2026-05-08"]);
lv:addItem(["util.c",  "3 kB",  "2026-05-08"]);

lv:setView("details" | "list" | "smallIcon" | "largeIcon" | "tile")
lv:setColumnWidth(col, w)        lv:getColumnCount()
lv:setSubItem(row, col, text)    lv:getItemText(row, col)
lv:removeItem(row)               lv:clearItems()
lv:getItemCount()
lv:getSelectedIndex()            lv:setSelectedIndex(i)
lv:getSelectedIndices()          -- array of int
lv:setMultiSelect(b)

lv.onItemActivated     = function(self, row) { ... };  -- NM_DBLCLK
lv.onSelectionChanged  = function(self, row) { ... };  -- LVN_ITEMCHANGED
```

## Display controls

### Label / LinkLabel

```cando
forms.Label(parent, { text: "Status: idle", autoSize: TRUE });
forms.LinkLabel(parent, { text: "<a>Visit cando.dev</a>" })
    .onClick = function(self) { ... };
```

### PictureBox

```cando
VAR pb = forms.PictureBox(parent, { size: [300, 200] });
pb:setText("...")                -- StatusBar / PictureBox accept text
```

### ProgressBar

```cando
VAR p = forms.ProgressBar(parent, { value: 25 });
p:setValue(n)                    p:getValue()
p:setRange(lo, hi)
p:setStep(n)                     p:stepIt()
p:setMarquee(active, [speed])
p:setState(s)                    -- "normal" | "warning" | "error" | "paused"
                                 -- aliases: "green", "yellow", "red"
```

### StatusBar

```cando
VAR sb = forms.StatusBar(form);
sb:setText("Ready.");
```

### PaintSurface

A panel-shaped HWND that fires `onPaint(self)` on every WM_PAINT.
Useful today for timer-driven animation (combine with `forms.Timer`
+ `ctrl:invalidate()`); a synchronous gfx bridge to `modules/draw`
is a future enhancement.

```cando
VAR canvas = forms.PaintSurface(form, { dock: "fill" });
canvas.onPaint = function(self) { print("paint!"); };
```

## Menus

```cando
VAR menu = forms.MenuStrip();

VAR file = menu:addItem("&File");
VAR open = file:addItem("&Open\tCtrl+O");
open.onClick = function(_) { ... };
file:addSeparator();
file:addItem("E&xit").onClick = function(_) { f:close(); };

VAR edit = menu:addItem("&Edit");
edit:addItem("&Undo").onClick = function(_) { ... };
edit:addItem("&Redo").onClick = function(_) { ... };

f:setMenu(menu);
```

Context menus are popups bound to a control or coordinate:

```cando
VAR ctx = forms.ContextMenu();
ctx:addItem("Cut").onClick   = function(_) { ... };
ctx:addItem("Copy").onClick  = function(_) { ... };
ctx:addItem("Paste").onClick = function(_) { ... };

panel.onContextMenu = function(self, x, y) { ctx:show(self, x, y); };
```

`MenuItem` methods:

```
item:setEnabled(b)
item:addItem(text)               item:addSeparator()
item.onClick = function(self) { ... }
```

## Dialogs

All dialog functions are module-level (no instance), block until the
user dismisses, and return a string / number / object / `NULL`.

```cando
VAR ans = forms.MessageBox("Save changes?", "Document", {
    buttons: "yesNoCancel",      -- "ok" | "okCancel" | "yesNo" |
                                 -- "yesNoCancel" | "abortRetryIgnore"
    icon:    "question"          -- "info" | "warning" | "error" |
                                 -- "question" | (none)
});
-- ans is one of: "ok" | "cancel" | "yes" | "no" | "abort" | "retry" | "ignore"

VAR path = forms.OpenFileDialog({
    title:      "Pick a file",
    filter:     "Text files (*.txt)|*.txt|All files (*.*)|*.*",
    initialDir: "C:\\Users"
});                              -- string, or NULL on cancel

VAR savePath = forms.SaveFileDialog({
    fileName: "document.txt",
    filter:   "Text|*.txt"
});

VAR dir   = forms.FolderBrowserDialog({ title: "Choose a folder" });
VAR rgb   = forms.ColorDialog("#3366ff");           -- 0xRRGGBB number, or NULL
VAR font  = forms.FontDialog();                     -- {face, size, bold, ...} or NULL
```

## Tray, timers, custom paint

### Timer

```cando
VAR t = forms.Timer();
t:setInterval(1000)              t:getInterval()
t:start()                        t:stop()        t:isRunning()
t.onTick = function(self) { ... };
```

Backed by `SetTimer` on the manager hidden window; `WM_TIMER` fires
`onTick` via the dispatcher.

### NotifyIcon (system tray)

```cando
VAR n = forms.NotifyIcon();
n:setIcon("assets/app.ico");
n:setText("My App is running");
n:show();
n:balloon("Update available", "info");

n.onClick = function(self, button) {
    -- button: 1 = left, 2 = right
};
```

### PaintSurface

See [Display controls](#display-controls).

## Layout cookbook

### Anchor (corner-tracking)

```cando
VAR ok = forms.Button(form, { text: "OK", anchor: "bottom right" });
ok:setLocation(form:getWidth() - 100, form:getHeight() - 40);
```

### Dock (sidebar + content)

```cando
forms.Panel(form, { dock: "left",  size: [160, 0], backColor: "lightgray" });
forms.StatusBar(form);                            -- implicit dock = bottom
forms.Panel(form, { dock: "fill" });
```

### Flow (wrap a row of buttons)

```cando
VAR bar = forms.FlowLayoutPanel(form, {
    dock:          "top",
    flowDirection: "leftToRight",
    wrapContents:  TRUE,
    padding:       4
});
FOR name IN ["New", "Open", "Save", "Close"] {
    forms.Button(bar, { text: name, margin: 2 });
}
```

### Table (form layout)

```cando
VAR g = forms.TableLayoutPanel(form, { dock: "fill", cellPadding: 4 });
g:setColumns(2);
g:setRows(3);
g:add(forms.Label(g, "Name:"),  0, 0);
g:add(forms.TextBox(g),          1, 0);
g:add(forms.Label(g, "Email:"), 0, 1);
g:add(forms.TextBox(g),          1, 1);
g:add(forms.Button(g, "Submit"), 0, 2, 2, 1);
```

### Split (drag-resize panes)

```cando
VAR side = forms.Panel(form,    { dock: "left", size: [220, 0] });
VAR sp   = forms.Splitter(form, { dock: "left", size: [4, 0] });
VAR body = forms.Panel(form,    { dock: "fill" });
```

## Events catalogue

| Event                 | Receivers                                | Signature                              |
| --------------------- | ---------------------------------------- | -------------------------------------- |
| `onClick`             | every clickable control                  | `(self, button, x, y)`                 |
| `onMouseDown` / `onMouseUp` | every control                      | `(self, button, x, y)`                 |
| `onMouseMove`         | every control                            | `(self, x, y)`                         |
| `onKeyDown` / `onKeyUp` | focusable controls                     | `(self, vk)`                           |
| `onFocus` / `onBlur`  | focusable controls                       | `(self)`                               |
| `onTextChanged`       | TextBox, ComboBox                        | `(self)`                               |
| `onValueChanged`      | NumericUpDown, TrackBar, ProgressBar, DateTimePicker, MonthCalendar | `(self)`     |
| `onSelectionChanged`  | ListBox, ComboBox, ListView, MonthCalendar, TreeView | `(self)` / `(self, row)`   |
| `onItemActivated`     | ListView                                 | `(self, row)`                          |
| `onTabChanged`        | TabControl                               | `(self, index)`                        |
| `onNodeSelected`      | TreeView                                 | `(self, handle)`                       |
| `onNodeExpanded` / `onNodeCollapsed` | TreeView                  | `(self, handle)`                       |
| `onTick`              | Timer                                    | `(self)`                               |
| `onPaint`             | PaintSurface                             | `(self)`                               |
| `onClose` / `onShown` / `onResize` | Form                        | `(self, ...)`                          |

NotifyIcon's `onClick(self, button)` receives `1` for left-click and
`2` for right-click.  MenuItem's `onClick(self)` is fired by both
menu-bar and context-menu activation.

## Enums and namespaces

```cando
forms.Dock          -- none / top / bottom / left / right / fill (numbers)
forms.Anchor        -- none / left / top / right / bottom / all (bitmask)
forms.AutoSizeMode  -- grow / growShrink (strings)
forms.BorderStyle   -- none / single / fixed3D
forms.Cursor        -- arrow / hand / ibeam / wait / cross /
                    -- sizeNS / sizeWE / sizeNWSE / sizeNESW /
                    -- sizeAll / no / help / appStarting / default
forms.Color         -- a CSS-style palette (red, cornflowerblue,
                    -- darkgreen, …) holding 0xRRGGBB ints.  The same
                    -- names also work as plain strings to setForeColor /
                    -- setBackColor.
```

## The `forms_util.cdo` wrapper

`script/forms_util.cdo` is a CanDo-side convenience wrapper: pure
script, no native dependency, optional.  It re-exports everything
`forms.*` exposes and adds declarative builders, an `id` registry,
state observables, dialog helpers, a menu builder, and theming.

```cando
VAR ui = include("./script/forms_util.cdo");

VAR f = ui.Window({
    title:  "Sign in",
    size:   [360, 220],
    center: TRUE,
    body: {
        kind: "Stack",
        direction: "topDown",
        padding:   12,
        children: [
            { kind: "Row", children: [
                { kind: "Label",   text: "Username:", width: 80 },
                { kind: "TextBox", id: "user" }
            ]},
            { kind: "Row", children: [
                { kind: "Label",   text: "Password:", width: 80 },
                { kind: "TextBox", id: "pass", passwordChar: "*" }
            ]},
            { kind: "Row", children: [
                { kind: "Button", text: "Cancel", role: "cancel" },
                { kind: "Button", text: "Sign in", role: "accept",
                  onClick: function(self) {
                      VAR form = self:getParent():getParent();
                      print("user:", form:field("user"):getText());
                      form:close("ok");
                  }}
            ]}
        ]
    }
});
VAR result = f:showModal();
```

Builders shipped:

| Builder            | Wraps                                            |
| ------------------ | ------------------------------------------------ |
| `ui.Window`        | `forms.Form` + body tree                         |
| `ui.Stack`         | `forms.FlowLayoutPanel`                          |
| `ui.Row` / `ui.Column` | shorthand stacks                             |
| `ui.Grid`          | `forms.TableLayoutPanel` + auto cell-add         |
| `ui.Tabs`          | `forms.TabControl` + per-tab content panel       |
| `ui.Split`         | `Panel + Splitter + Panel`                       |
| `ui.ScrollArea`    | `forms.ScrollPanel`                              |
| `ui.Panel` / `Group` / `GroupBox`                                       |
| `ui.Label / Button / TextBox / CheckBox / RadioButton / ComboBox / ListBox / TreeView / ListView / NumericUpDown / TrackBar / ProgressBar / DateTimePicker / MonthCalendar / StatusBar / Spinner / PaintSurface / LinkLabel / PictureBox` |
| `ui.Timer / NotifyIcon`                                                |

Extras:

```cando
ui.dialog.message(text, opts)                 -- forms.MessageBox
ui.dialog.confirm(text, opts)                 -- yes/no -> bool
ui.dialog.openFile / saveFile / openFolder    -- file system dialogs
ui.dialog.color([initial])                    -- ColorDialog
ui.dialog.font()                              -- FontDialog

ui.menu([{text, items, onClick, separator}, ...])     -- MenuStrip
ui.contextMenu(...)                                   -- ContextMenu

ui.observable(initial)                        -- get/set/snapshot/subscribe
ui.eventBus()                                 -- on/emit

ui.applyTheme(form, { fontFace, fontSize, foreColor, backColor })
ui.toast(form, text, { duration })            -- short-lived label
```

`id` registry: any descriptor with `id: "foo"` registers the
resulting control on the owning form's `__fields` table; access via
`form:field("foo")`.

`role` shorthand: `role: "accept"` / `role: "cancel"` wires
`form:setAcceptButton` / `setCancelButton` automatically.

## DPI and dark mode

Per-Monitor V2 DPI awareness is declared during module init.  Query
the effective DPI of any control's monitor with:

```cando
VAR dpi = forms.dpiFor(form);   -- e.g. 96, 120, 144, 192
```

Toggle the immersive-dark-mode hint on every alive form:

```cando
forms.darkMode(TRUE);
```

Best-effort: silently no-ops on Windows builds before 1809 where
`DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` isn't
available.

## Accessibility

Every control inherits MSAA from comctl32.  Add screen-reader-friendly
names and descriptions explicitly:

```cando
btn:setAccessibleName("Save document");
btn:setAccessibleDescription("Writes the current document to disk.");
```

Stored as window properties (`SetPropW`) so screen readers (NVDA /
Narrator) pick them up via the standard `IAccessible::get_accName` /
`get_accDescription` chain.

## Worked example

```cando
VAR forms = include("./forms.dll");
VAR ui    = include("./script/forms_util.cdo");

VAR state = ui.observable({ path: NULL, dirty: FALSE });

VAR titleFor = function() {
    VAR base = state:get("path") != NULL ? state:get("path") : "Untitled";
    return state:get("dirty") ? `* ${base}` : base;
};

VAR f = ui.Window({
    title:  titleFor(),
    size:   [800, 600],
    center: TRUE,
    body: {
        kind: "Stack",
        direction: "topDown",
        children: [
            { kind: "TextBox",  id: "editor", multiline: TRUE,
              dock: "fill" },
            { kind: "Label",    id: "status", text: "Ready.",
              dock: "bottom", padding: [4, 4] }
        ]
    }
});

VAR editor = f:field("editor");
VAR status = f:field("status");

editor.onTextChanged = function(_) {
    state:set("dirty", TRUE);
    f:setText(titleFor());
};

VAR doOpen = function() {
    VAR p = ui.dialog.openFile({ filter: "Text|*.txt|All|*.*" });
    IF p == NULL { return; }
    -- (load the file's text into the editor here)
    state:set("path", p);
    state:set("dirty", FALSE);
    f:setText(titleFor());
    status:setText(`Opened ${p}`);
};

f:setMenu(ui.menu([
    { text: "&File", items: [
        { text: "&Open\tCtrl+O", onClick: function(_) { doOpen(); } },
        { separator: TRUE },
        { text: "E&xit", onClick: function(_) { f:close(); } }
    ]}
]));

f.onClose = function(self) {
    IF state:get("dirty") {
        VAR ans = forms.MessageBox("Save before closing?", "Editor",
            { buttons: "yesNoCancel", icon: "question" });
        IF ans == "cancel" { return FALSE; }
        -- handle yes / no
    }
    return TRUE;
};

f:show();
forms.run();   -- optional explicit blocker; the lifeline keeps the
               -- process alive without it.
```

## Building

```bash
make -C modules/forms                        # forms.so + test_forms (Linux)
make -C modules/forms forms.dll \            # Windows DLL via MinGW
    MINGW_CC=x86_64-w64-mingw32-gcc
make -C modules/forms test                   # run C unit tests
```

`forms.dll` is fully self-contained:

| Dependency       | How it's satisfied                                |
| ---------------- | ------------------------------------------------- |
| libgcc           | statically linked (`-static-libgcc`)              |
| winpthread       | statically linked (`--whole-archive -lwinpthread`)|
| `user32.dll`, `gdi32.dll`, `comctl32.dll`, `comdlg32.dll`, `ole32.dll`, `uuid.dll`, `shell32.dll`, `dwmapi.dll` | shipped with every Windows install |
| `libcando.dll`   | ships next to `cando.exe`                         |

CI fails the build if any non-system MinGW runtime DLL ends up as an
import (no `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`,
`libstdc++-6.dll`, `libssl-3-x64.dll`, or `libcrypto-3-x64.dll`).

## Files

```
modules/forms/
├── forms_module.c            module entry point + Win32 backend
├── Makefile                  per-module build rules
├── README.md                 this file
├── cando.api.json            language-server type manifest
├── src/
│   ├── core/                 cross-control infrastructure
│   │   ├── color.{c,h}       named colour table + parsers
│   │   ├── geom.{c,h}        DockRect + compute_dock_rect
│   │   ├── events.{c,h}      EventKind + ring-buffer queue
│   │   ├── sync.h            mutex / condvar shim
│   │   ├── slots.{c,h}       ControlKind + FormsSlot + allocator
│   │   ├── dispatch.{c,h}    EventKind -> "onX" name map
│   │   ├── manager.h         ManagerState enum
│   │   ├── textconv.{c,h}    UTF-8 <-> wchar_t helpers
│   │   ├── layout.{c,h}      parsers + arrange-fn vtable
│   │   └── cando_compat.h    libcando includes / test stubs
│   └── controls/             per-kind native methods
│       ├── ctl_common.{c,h}  shared helpers (slot_from_inst, ...)
│       ├── ctl_checkbox / progress / trackbar / numeric / textbox /
│       │   listbox / form / scrollpanel / tabcontrol / flowlayout /
│       │   tablelayout / splitter / treeview / listview {.c,.h}
├── script/
│   └── forms_util.cdo        declarative wrapper layer
├── test_forms.c              C unit tests (event queue + slot + colour + dock)
├── test_forms.cdo            integration script (Windows-only)
└── test_forms_smoke.cdo      headless smoke check (any platform)
```
