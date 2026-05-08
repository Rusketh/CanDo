# Forms 2.0 — API specification & usage examples

> Companion to `REWRITE_PLAN.md`. The plan describes *how* the rewrite
> is built; this document describes *what* the surface looks like to a
> CanDo script and shows real code for every control.
>
> All snippets assume:
>
> ```cando
> VAR forms = include("./forms.dll");      // Windows
> VAR ui    = include("./forms_util.cdo"); // utility wrapper (optional)
> ```

---

## Table of contents

1. [Module surface](#1-module-surface)
2. [Construction conventions](#2-construction-conventions)
3. [Common control API (every control)](#3-common-control-api-every-control)
4. [Forms](#4-forms)
5. [Inputs — buttons, text, picks](#5-inputs)
6. [Containers — Panel, ScrollPanel, GroupBox, Tabs, Split, Flow, Table](#6-containers)
7. [Lists, trees, and grids](#7-lists-trees-and-grids)
8. [Display controls — Label, LinkLabel, PictureBox, ProgressBar, StatusBar, ToolStrip](#8-display-controls)
9. [Menus](#9-menus)
10. [Dialogs](#10-dialogs)
11. [Tray icon, timers, image lists, tooltips](#11-tray-timers-imagelists-tooltips)
12. [Drag-and-drop](#12-drag-and-drop)
13. [Custom drawing — PaintSurface](#13-custom-drawing)
14. [Layout cookbook](#14-layout-cookbook)
15. [Events catalogue](#15-events-catalogue)
16. [Enums and constants](#16-enums-and-constants)
17. [Utility wrapper layer (`forms_util.cdo`)](#17-utility-wrapper-layer)
18. [Worked example — full text editor](#18-worked-example)
19. [Migration from forms 1.x](#19-migration-from-forms-1x)

---

## 1. Module surface

```cando
forms.VERSION           // "2.0.0"
forms.platform          // "windows" or "stub"
forms.supported         // TRUE on Windows, FALSE elsewhere

// Lifecycle
forms.run([form])       // optional: block until every form is closed.
forms.exit()            // break out of run() / unblock showModal.
forms.idle()            // pump pending events without blocking
                        //   (useful inside scripted main loops).

// Environment
forms.dpiFor(control)   // effective DPI for a control's monitor
forms.scale             // "logical" (default) | "device"
forms.darkMode(b)       // toggle dark-mode hint (Win10 1809+)

// Constructors — see §4–§13
forms.Form, forms.Button, forms.Label, forms.LinkLabel, forms.TextBox,
forms.RichTextBox, forms.MaskedTextBox, forms.CheckBox, forms.RadioButton,
forms.ToggleButton, forms.ComboBox, forms.ListBox, forms.CheckedListBox,
forms.NumericUpDown, forms.TrackBar, forms.ProgressBar, forms.PictureBox,
forms.PaintSurface, forms.DateTimePicker, forms.MonthCalendar,
forms.StatusBar, forms.ToolStrip, forms.Spinner, forms.Panel,
forms.ScrollPanel, forms.GroupBox, forms.TabControl, forms.TabPage,
forms.SplitContainer, forms.Splitter, forms.FlowLayoutPanel,
forms.TableLayoutPanel, forms.TreeView, forms.ListView, forms.DataGrid,
forms.MenuStrip, forms.ContextMenu, forms.MenuItem, forms.MessageBox,
forms.OpenFileDialog, forms.SaveFileDialog, forms.FolderBrowserDialog,
forms.ColorDialog, forms.FontDialog, forms.NotifyIcon, forms.Timer,
forms.ImageList, forms.ToolTip

// Namespaces
forms.Color, forms.Cursor, forms.Dock, forms.Anchor, forms.AutoSizeMode,
forms.BorderStyle, forms.MessageBoxButtons, forms.MessageBoxIcon,
forms.DialogResult, forms.Clipboard, forms.Screen, forms.Keys
```

### Loading the module

```cando
VAR forms = NULL;
TRY {
    forms = include("./forms.dll");        // Windows
} CATCH (e) {
    forms = include("./forms.so");         // stub everywhere else
}

IF forms.supported == FALSE {
    print("forms 2.0 needs Windows.");
    EXIT(0);
}

print(`forms ${forms.VERSION} on ${forms.platform}`);
```

---

## 2. Construction conventions

Every constructor takes one of three shapes:

```cando
// 1) bare
VAR b = forms.Button(parent);

// 2) text shorthand (only for controls where text is the dominant property)
VAR b = forms.Button(parent, "Save");

// 3) options table (canonical, supports every property)
VAR b = forms.Button(parent, {
    text     = "Save",
    location = { 20, 20 },
    size     = { 120, 30 },
    enabled  = TRUE,
    onClick  = function(self) { print("saved!"); }
});
```

`Form` takes the same shapes minus `parent`. Options-table keys mirror
the `setX` methods (drop the `set`, lower-case the first letter):
`text`, `size`, `location`, `width`, `height`, `enabled`, `visible`,
`font`, `foreColor`, `backColor`, `padding`, `margin`, `dock`,
`anchor`, `autoSize`, `cursor`, `toolTip`, `tabIndex`, `tabStop`,
`borderStyle`, plus per-control extras and any `onX` event handler.

The constructor returns the live native instance. The instance is
**only** addressable through methods; there are no public fields.
Identity is held internally so users cannot accidentally orphan it.

---

## 3. Common control API (every control)

Every control inherits this table. Methods that don't apply to a
specific control are **not present** on its meta table — calling
`button:addItem("x")` is a hard error from the VM.

### 3.1 Text

```cando
ctrl:setText("hello")
ctrl:getText()                    // "hello"
ctrl:appendText(" world")         // text-edit controls only
ctrl:clearText()
```

### 3.2 Geometry

```cando
ctrl:setSize(w, h)                ctrl:getSize()        // returns w, h
ctrl:setLocation(x, y)            ctrl:getLocation()    // returns x, y
ctrl:setWidth(n)                  ctrl:getWidth()
ctrl:setHeight(n)                 ctrl:getHeight()
ctrl:setMinSize(w, h)             ctrl:setMaxSize(w, h)
ctrl:setPreferredSize(w, h)       ctrl:getPreferredSize()
ctrl:sizeToContent()              ctrl:sizeToContentWidth()
                                  ctrl:sizeToContentHeight()
ctrl:getBounds()                  // returns x, y, w, h
ctrl:getClientSize()              // returns w, h (inside borders)
```

### 3.3 Visibility / state

```cando
ctrl:show()                       ctrl:hide()
ctrl:setVisible(b)                ctrl:getVisible()
ctrl:setEnabled(b)                ctrl:getEnabled()
ctrl:setFocus()                   ctrl:hasFocus()
ctrl:isAlive()                    ctrl:destroy()
```

### 3.4 Hierarchy

```cando
ctrl:setParent(other)             ctrl:getParent()
ctrl:getChildren()                // array of native instances
ctrl:contains(other)              // TRUE if descendant
ctrl:bringToFront()               ctrl:sendToBack()
```

### 3.5 Look

```cando
ctrl:setForeColor(...)            ctrl:clearForeColor()    ctrl:getForeColor()
ctrl:setBackColor(...)            ctrl:clearBackColor()    ctrl:getBackColor()
ctrl:setFont(...)                 ctrl:clearFont()         ctrl:getFont()
ctrl:setFontSize(n)
ctrl:setBold(b) / ctrl:setItalic(b) / ctrl:setUnderline(b) / ctrl:setStrikeout(b)
ctrl:setBorderStyle("none" | "single" | "fixed3D")
ctrl:setCursor(name)
ctrl:setToolTip("text")
```

`setForeColor` / `setBackColor` accept three numbers, a packed
`0xRRGGBB` integer, a hex string `"#RGB"` / `"#RRGGBB"` /
`"#AARRGGBB"`, a CSS-style name `"cornflowerblue"`, or a
`forms.Color.*` constant.

### 3.6 Layout

```cando
ctrl:setPadding(...)              ctrl:getPadding()        // 1/2/4 numbers
ctrl:setMargin(...)               ctrl:getMargin()
ctrl:setAnchor("top right")       ctrl:getAnchor()
ctrl:setDock("top" | "bottom" | "left" | "right" | "fill" | "none")
                                  ctrl:getDock()
ctrl:setAutoSize(b)               ctrl:getAutoSize()
ctrl:setAutoSizeMode("grow" | "growShrink")
ctrl:relayout()                   // re-run parent's layout now
ctrl:invalidate([{x, y, w, h}])   // schedule repaint (optional rect)
ctrl:refresh()                    // synchronous redraw
```

### 3.7 Tab order, accessibility

```cando
ctrl:setTabIndex(n)               ctrl:getTabIndex()
ctrl:setTabStop(b)
ctrl:setAccessibleName("Save")
ctrl:setAccessibleDescription("Saves the document to disk.")
```

### 3.8 Drag-and-drop, context menu

```cando
ctrl:setContextMenu(forms.ContextMenu({...}))
ctrl:dragEnable(["files", "text"])  // accept incoming drops; see §12
```

### 3.9 Generic events

Every control supports these (assigned to instance properties):

```cando
ctrl.onClick         = function(self, button, x, y) {}
ctrl.onMouseDown     = function(self, button, x, y) {}
ctrl.onMouseUp       = function(self, button, x, y) {}
ctrl.onMouseMove     = function(self, x, y) {}
ctrl.onMouseEnter    = function(self) {}
ctrl.onMouseLeave    = function(self) {}
ctrl.onMouseWheel    = function(self, delta, x, y) {}
ctrl.onKeyDown       = function(self, vk, modifiers) {}
ctrl.onKeyUp         = function(self, vk, modifiers) {}
ctrl.onKeyPress      = function(self, char) {}
ctrl.onFocus         = function(self) {}
ctrl.onBlur          = function(self) {}
ctrl.onResize        = function(self, w, h) {}
ctrl.onPaint         = function(self, gfx) {}     // owner-draw, see §13
ctrl.onContextMenu   = function(self, x, y) {}
ctrl.onDragEnter     = function(self, data) {}    // see §12
ctrl.onDragOver      = function(self, data, x, y) {}
ctrl.onDrop          = function(self, data, x, y) {}
```

`modifiers` is a small object: `{ ctrl=TRUE, shift=FALSE, alt=FALSE,
win=FALSE }`. `vk` is a `forms.Keys.*` value.

---

## 4. Forms

### 4.1 Construct

```cando
VAR f = forms.Form({
    title           = "My App",
    size            = { 800, 600 },
    minSize         = { 480, 320 },
    startPosition   = "centerScreen",  // "manual" | "centerScreen" | "centerParent"
    icon            = "assets/app.ico",
    resizable       = TRUE,
    minimizeBox     = TRUE,
    maximizeBox     = TRUE,
    showInTaskbar   = TRUE,
    topMost         = FALSE
});
```

### 4.2 Form-only methods

```cando
f:center()
f:setIcon("path/to.ico")
f:setOpacity(0.85)               // 0..1 float, or 0..255 int
f:setTopMost(b)
f:setResizable(b)
f:setMinimizeBox(b)              f:setMaximizeBox(b)
f:setShowInTaskbar(b)
f:setStartPosition("centerScreen")

f:setWindowState("normal" | "maximized" | "minimized")
f:getWindowState()
f:maximize()    f:minimize()    f:restore()
f:flash([3])

f:setMenu(menuStrip)             // see §9
f:setStatusBar(statusBar)
f:setAcceptButton(btn)           // pressed when user hits Enter
f:setCancelButton(btn)           // pressed when user hits Esc

f:show()                         // non-blocking
f:close([dialogResult])
f:showModal([owner]) -> string   // blocks the calling VM until close
```

### 4.3 Form-only events

```cando
f.onShown        = function(self) {}
f.onClose        = function(self) {}        // return FALSE to cancel
f.onActivated    = function(self) {}
f.onDeactivated  = function(self) {}
f.onResize       = function(self, w, h) {}
f.onMove         = function(self, x, y) {}
f.onDpiChanged   = function(self, dpi) {}
```

### 4.4 Example

```cando
VAR f = forms.Form({ title = "Hello", size = { 400, 300 }, startPosition = "centerScreen" });

VAR label = forms.Label(f, { text = "Hello, world!", location = { 20, 20 }, autoSize = TRUE });
VAR btn   = forms.Button(f, {
    text     = "Close",
    location = { 20, 60 },
    size     = { 100, 30 },
    onClick  = function(self) { self:getParent():close(); }
});

f.onClose = function(self) { print("bye"); };
f:show();
forms.run();        // optional: blocks until f is closed
```

### 4.5 Modal dialogs

```cando
VAR dlg = forms.Form({ title = "Pick a colour", size = { 300, 200 }, resizable = FALSE });

VAR ok = forms.Button(dlg, { text = "OK", location = { 100, 130 }, size = { 80, 28 } });
ok.onClick = function(self) { self:getParent():close("ok"); };

VAR cancel = forms.Button(dlg, { text = "Cancel", location = { 200, 130 }, size = { 80, 28 } });
cancel.onClick = function(self) { self:getParent():close("cancel"); };

dlg:setAcceptButton(ok);
dlg:setCancelButton(cancel);

VAR result = dlg:showModal(parentForm);   // "ok" or "cancel"
print(`dialog returned: ${result}`);
```

---

## 5. Inputs

### 5.1 Button

```cando
VAR b = forms.Button(parent, {
    text     = "Save",
    location = { 16, 16 },
    size     = { 96, 28 },
    onClick  = function(self) { print("clicked"); }
});

b:performClick();                   // programmatic activation
b:setDialogResult("ok");            // sets parent form result on click
```

### 5.2 CheckBox / RadioButton / ToggleButton

```cando
VAR c = forms.CheckBox(parent, { text = "Remember me", checked = TRUE });
c:setChecked(b)            c:getChecked()
c:setThreeState(TRUE)      c:setCheckState("indeterminate")
c.onCheckedChanged = function(self) { print(self:getChecked()); };

VAR r1 = forms.RadioButton(group, { text = "Small",  group = "size" });
VAR r2 = forms.RadioButton(group, { text = "Medium", group = "size", checked = TRUE });
VAR r3 = forms.RadioButton(group, { text = "Large",  group = "size" });

VAR t = forms.ToggleButton(parent, { text = "On/Off" });
t.onCheckedChanged = function(self) { ... };
```

### 5.3 TextBox

```cando
VAR t = forms.TextBox(parent, {
    location    = { 16, 16 },
    size        = { 240, 24 },
    placeholder = "your name",
    maxLength   = 64
});

t:setMultiline(b)          t:setReadOnly(b)         t:setPasswordChar("*")
t:setPlaceholder(s)        t:setMaxLength(n)
t:setTextAlign("left" | "center" | "right")
t:selectAll()              t:setSelection(start, length)
t:getSelection()                                   // returns start, length
t:appendText(s)            t:clearText()

t.onTextChanged = function(self) { print(self:getText()); };
t.onEnter       = function(self) {}    // pressed Enter on a single-line box
```

### 5.4 RichTextBox

```cando
VAR rt = forms.RichTextBox(parent, { size = { 480, 320 } });

rt:loadFile("readme.rtf")              rt:saveFile("out.rtf")
rt:setText(s)                          rt:appendText(s)
rt:setSelection(start, length)
rt:setSelectionFont({ face = "Consolas", size = 11, bold = TRUE })
rt:setSelectionColor("red")
rt:setSelectionBackColor("yellow")
rt:setSelectionAlignment("left" | "center" | "right" | "justify")
rt:setReadOnly(b)
rt:undo() / rt:redo() / rt:canUndo() / rt:canRedo()

rt.onLinkClicked = function(self, href) { print(`click ${href}`); };
```

### 5.5 MaskedTextBox

```cando
VAR phone = forms.MaskedTextBox(parent, { mask = "(###) ###-####" });
phone:getMaskCompleted()                 // TRUE when fully filled
phone:getUnmaskedText()                  // strips fixed chars
```

### 5.6 NumericUpDown / TrackBar / Spinner

```cando
VAR n = forms.NumericUpDown(parent, {
    minimum = 0, maximum = 100, value = 50, increment = 1, decimalPlaces = 0
});
n:setValue(75)                  n:getValue()
n:setRange(lo, hi)              n:setIncrement(n)
n.onValueChanged = function(self) { ... };

VAR t = forms.TrackBar(parent, {
    minimum = 0, maximum = 100, value = 30, tickFrequency = 10,
    orientation = "horizontal"     // or "vertical"
});
t:setSmallStep(n)               t:setLargeStep(n)
t.onValueChanged = function(self) { ... };
```

### 5.7 ComboBox

```cando
VAR c = forms.ComboBox(parent, {
    items = ["Apples", "Bananas", "Cherries"],
    style = "dropDown"          // "simple" | "dropDown" | "dropDownList"
});

c:addItem("Dates")              c:insertItem(0, "Acai")
c:removeItem(2)                 c:clearItems()
c:getItem(i)                    c:getItems()
c:getItemCount()
c:setSelectedIndex(i)           c:getSelectedIndex()
c:getSelectedItem()             // string at current index, or NULL
c:setText(s)                    c:getText()
c.onSelectionChanged = function(self) { print(self:getSelectedItem()); };
```

### 5.8 DateTimePicker / MonthCalendar

```cando
VAR d = forms.DateTimePicker(parent, {
    format = "long" | "short" | "time" | "custom",
    customFormat = "yyyy-MM-dd"
});
d:setValue("2026-05-08")        // string, number (epoch), or { y, m, d }
d:getValue()                    // returns "yyyy-MM-dd HH:MM:SS"
d:getDate()                     // returns y, m, d
d.onValueChanged = function(self) { print(self:getValue()); };

VAR m = forms.MonthCalendar(parent, { selectionMode = "single" | "range" });
m:setSelection(start, [end])
m:getSelection()                // returns startISO, endISO
m.onSelectionChanged = function(self) { ... };
```

---

## 6. Containers

### 6.1 Panel / GroupBox

```cando
VAR p = forms.Panel(form, {
    location = { 0, 0 }, size = { 200, 200 },
    backColor = "white",
    borderStyle = "single"
});

VAR g = forms.GroupBox(form, {
    text = "Options", location = { 16, 16 }, size = { 200, 120 }
});
forms.CheckBox(g, { text = "Enable foo", location = { 12, 24 } });
forms.CheckBox(g, { text = "Enable bar", location = { 12, 48 } });
```

### 6.2 ScrollPanel

```cando
VAR sp = forms.ScrollPanel(form, {
    location = { 0, 0 }, size = { 320, 240 },
    autoScroll = TRUE,
    scrollSize = { 1024, 1024 }    // virtual content area
});

// Children are positioned in the virtual coordinate system.
forms.PictureBox(sp, { image = "big.png", location = { 0, 0 }, size = { 1024, 1024 } });

sp:setAutoScroll(b)
sp:setScrollSize(w, h)
sp:scrollTo(x, y)
sp:getScrollPosition()       // returns x, y

sp.onScroll = function(self, x, y) { ... };
```

### 6.3 TabControl + TabPage

```cando
VAR tabs = forms.TabControl(form, {
    location = { 0, 0 }, size = { 480, 320 }, dock = "fill"
});

VAR general = tabs:addTab("General");
forms.CheckBox(general, { text = "Show toolbar", location = { 12, 12 } });

VAR advanced = tabs:addTab("Advanced");
forms.Label(advanced, { text = "Hold on, advanced.", location = { 12, 12 } });

tabs:setSelectedIndex(0)
tabs:getSelectedIndex()
tabs:getTabCount()
tabs:getTab(i)              // returns the TabPage
tabs:removeTab(i)

tabs.onTabChanging = function(self, fromIndex, toIndex) {
    // return FALSE to cancel
};
tabs.onTabChanged  = function(self, index) { print(`now on tab ${index}`); };
```

### 6.4 SplitContainer

```cando
VAR split = forms.SplitContainer(form, {
    dock           = "fill",
    orientation    = "vertical",      // splits left | right
    splitterDistance = 200,
    fixedPanel     = "panel1"         // "panel1" | "panel2" | "none"
});

VAR left  = split:getPanel1();        // a Panel-shaped child
VAR right = split:getPanel2();

forms.TreeView(left,  { dock = "fill" });
forms.RichTextBox(right, { dock = "fill" });

split:setOrientation("horizontal")    // top | bottom
split:setSplitterDistance(n)
split:setFixedPanel("panel1" | "panel2" | "none")
split.onSplitterMoved = function(self, distance) { ... };
```

### 6.5 FlowLayoutPanel

```cando
VAR flow = forms.FlowLayoutPanel(form, {
    dock          = "top",
    flowDirection = "leftToRight",    // "leftToRight" | "rightToLeft" |
                                      // "topDown" | "bottomUp"
    wrapContents  = TRUE,
    autoSize      = TRUE
});

FOR i OF 1 -> 10 {
    forms.Button(flow, { text = `B${i}`, size = { 60, 24 }, margin = 4 });
}
```

### 6.6 TableLayoutPanel

```cando
VAR table = forms.TableLayoutPanel(form, {
    dock    = "fill",
    columns = 2,
    rows    = 3,
    columnStyles = ["auto", "percent,100"],
    rowStyles    = ["auto", "auto", "percent,100"]
});

table:add(forms.Label(table, "Name:"),  0, 0);
table:add(forms.TextBox(table),         1, 0);

table:add(forms.Label(table, "Email:"), 0, 1);
table:add(forms.TextBox(table),         1, 1);

table:add(forms.RichTextBox(table),     0, 2, 2, 1);   // colSpan=2, rowSpan=1
```

### 6.7 Splitter

```cando
VAR left  = forms.Panel(form, { dock = "left", size = { 160, 0 } });
VAR sp    = forms.Splitter(form, { dock = "left", size = { 4, 0 } });
VAR right = forms.Panel(form, { dock = "fill" });
sp:setMinExtra(80);     // never let `right` shrink below this
sp:setMinSize(60);      // never let `left`  shrink below this
```

---

## 7. Lists, trees, and grids

### 7.1 ListBox

```cando
VAR lb = forms.ListBox(form, {
    items = ["Alpha", "Beta", "Gamma"],
    selectionMode = "single",        // "none" | "single" | "multi" | "extended"
    sorted = FALSE
});

lb:addItem("Delta")       lb:addItems(["Eta", "Zeta"])
lb:insertItem(0, "Zero")  lb:removeItem(i)        lb:clearItems()
lb:getItemCount()         lb:getItem(i)            lb:getItems()
lb:getSelectedIndex()     lb:setSelectedIndex(i)
lb:getSelectedIndices()   lb:setSelectedIndices([0, 2])
lb:getSelectedItem()      lb:getSelectedItems()
lb:scrollIntoView(i)

lb.onSelectionChanged = function(self) { ... };
lb.onItemActivated    = function(self, i) { ... };       // double-click / Enter
```

### 7.2 CheckedListBox

Same as `ListBox` plus:

```cando
clb:setChecked(i, TRUE)   clb:getChecked(i)
clb:getCheckedIndices()   clb:getCheckedItems()
clb.onItemChecked = function(self, i, checked) { ... };
```

### 7.3 TreeView

Nodes are first-class objects.

```cando
VAR tv = forms.TreeView(form, { dock = "left", size = { 220, 0 } });

VAR root = tv:addNode(NULL, "Project");
VAR src  = root:addChild("src");
VAR docs = root:addChild("docs");
src:addChild("main.c");
src:addChild("util.c");
docs:addChild("README.md");

root:expand()
tv:setSelectedNode(src)
tv:getSelectedNode()                // returns Node or NULL
tv:setImageList(imageList)          // see §11.3

// Node methods
node:setText(s)              node:getText()
node:setData(any)            node:getData()
node:setImageIndex(i)        node:setSelectedImageIndex(i)
node:addChild(text, [opts])  node:remove()           node:clearChildren()
node:expand()                node:collapse()         node:isExpanded()
node:getChildren()           node:getParent()        node:getRoot()
node:setChecked(b)           node:getChecked()
node:beginEdit()             node:endEdit([newText])

tv.onNodeSelected      = function(self, node) { ... };
tv.onNodeExpanded      = function(self, node) { ... };
tv.onNodeCollapsed     = function(self, node) { ... };
tv.onNodeChecked       = function(self, node, checked) { ... };
tv.onNodeDoubleClicked = function(self, node) { ... };
tv.onLabelEdit         = function(self, node, newText) { /* return FALSE to cancel */ };
```

### 7.4 ListView

Configurable view: details (report), list, large icons, small icons,
tile.

```cando
VAR lv = forms.ListView(form, {
    dock          = "fill",
    view          = "details",
    fullRowSelect = TRUE,
    gridLines     = TRUE,
    multiSelect   = TRUE,
    checkBoxes    = FALSE
});

lv:addColumn("Name", 200)
lv:addColumn("Size", 80)
lv:addColumn("Modified", 140)

VAR row = lv:addItem({ "main.c", "12 kB", "2026-05-08" });
row:setIcon(imageList, 0)

// Row API
row:setSubItem(i, text)
row:getSubItem(i)
row:setData(any)             row:getData()
row:setChecked(b)            row:getChecked()
row:setSelected(b)           row:isSelected()
row:remove()                 row:ensureVisible()

// Container API
lv:setView("details" | "list" | "largeIcon" | "smallIcon" | "tile")
lv:getItem(i)                lv:getItems()
lv:clearItems()
lv:getSelectedItems()        lv:getSelectedIndices()
lv:setColumnWidth(i, w)      lv:setSortIndicator(col, "asc" | "desc" | "none")

lv.onItemActivated         = function(self, item) { ... };
lv.onItemSelectionChanged  = function(self) { ... };
lv.onItemChecked           = function(self, item, checked) { ... };
lv.onColumnClicked         = function(self, columnIndex) { ... };
```

### 7.5 DataGrid (lightweight)

Built on ListView (report-mode) + cell editors. Phase-3b feature; the
shape mirrors the simpler subset of `DataGridView`.

```cando
VAR g = forms.DataGrid(form, {
    dock = "fill",
    columns = [
        { name = "name",  header = "Name",  width = 160, type = "text" },
        { name = "qty",   header = "Qty",   width = 60,  type = "number", min = 0 },
        { name = "stock", header = "In stock", width = 80, type = "checkbox" }
    ]
});

g:setRows([
    { name = "Apples",  qty = 12, stock = TRUE  },
    { name = "Bananas", qty = 5,  stock = FALSE }
]);

g:addRow({ name = "Cherries", qty = 0, stock = FALSE });
g:setCell(rowIndex, "qty", 7);
g:getCell(rowIndex, "qty");
g:getRow(rowIndex);                      // returns the object

g.onCellChanged = function(self, row, col, oldValue, newValue) { ... };
g.onRowSelected = function(self, row) { ... };
```

---

## 8. Display controls

### 8.1 Label

```cando
VAR l = forms.Label(parent, {
    text = "Status: idle",
    autoSize = TRUE,
    textAlign = "middleCenter"      // tl, tc, tr, ml, mc, mr, bl, bc, br
});

l:setTextAlign("middleCenter")
l:setUseMnemonic(TRUE)              // & in text becomes Alt-shortcut
```

### 8.2 LinkLabel

```cando
VAR ll = forms.LinkLabel(parent, {
    text = "See <a>the docs</a> or <a id='wiki'>wiki</a>."
});
ll.onLinkClicked = function(self, id) {
    print(`clicked link id = ${id}`);
};
```

### 8.3 PictureBox

```cando
VAR pb = forms.PictureBox(parent, {
    location = { 0, 0 }, size = { 300, 200 },
    image    = "logo.png",          // path; or pass an Image object
    sizeMode = "zoom"                // "normal" | "stretch" | "center" |
                                     // "zoom" | "autoSize"
});

pb:setImage("other.png" | imageObj | NULL)
pb:setSizeMode("zoom")
pb:setBorderStyle("single")

pb.onPaint = function(self, gfx) {   // optional overlay
    gfx:setPen("red", 2);
    gfx:rect(10, 10, 50, 50);
};
```

### 8.4 ProgressBar

```cando
VAR p = forms.ProgressBar(parent, {
    minimum = 0, maximum = 100, value = 25,
    style   = "blocks"               // "blocks" | "marquee"
});

p:setRange(lo, hi)        p:setValue(n)        p:getValue()
p:setStep(n)              p:stepIt()
p:setMarquee(active, [speed])
p:setState("normal" | "warning" | "error" | "paused")
```

### 8.5 StatusBar

```cando
VAR sb = forms.StatusBar(form);
sb:setText("Ready.")
sb:setPanels([
    { text = "Ready", width = "auto" },
    { text = "Ln 1, Col 1", width = 120 },
    { text = "UTF-8", width = 80 }
]);
sb:setPanelText(0, "Saving…")
```

### 8.6 ToolStrip

```cando
VAR ts = forms.ToolStrip(form, { dock = "top" });
ts:addButton({ text = "New",  icon = "icons/new.png",  onClick = ... })
ts:addButton({ text = "Open", icon = "icons/open.png", onClick = ... })
ts:addSeparator()
ts:addCheck({ text = "Bold", checked = FALSE, onClick = ... })
ts:addCombo({ name = "size", items = ["8", "10", "12"], onChange = ... })
```

---

## 9. Menus

```cando
VAR menu = forms.MenuStrip(form, [
    { text = "&File", items = [
        { text = "&New",     shortcut = "Ctrl+N", onClick = doNew  },
        { text = "&Open…",   shortcut = "Ctrl+O", onClick = doOpen },
        { text = "&Save",    shortcut = "Ctrl+S", onClick = doSave, id = "fileSave" },
        { separator = TRUE },
        { text = "E&xit",    shortcut = "Alt+F4", onClick = function(_) { f:close(); } }
    ]},
    { text = "&Edit", items = [
        { text = "&Undo",    shortcut = "Ctrl+Z", onClick = doUndo },
        { text = "&Redo",    shortcut = "Ctrl+Y", onClick = doRedo, enabled = FALSE }
    ]},
    { text = "&View", items = [
        { text = "&Toolbar", checked = TRUE, onClick = toggleToolbar },
        { text = "&Status bar", checked = TRUE, onClick = toggleStatus },
        { text = "&Theme", items = [
            { text = "&Light", group = "theme", radio = TRUE, checked = TRUE, onClick = ... },
            { text = "&Dark",  group = "theme", radio = TRUE, onClick = ... }
        ]}
    ]}
]);

f:setMenu(menu);

// Look up an item later by id
menu:getItem("fileSave"):setEnabled(FALSE);

// Context menu (popup) — same descriptor shape
VAR ctx = forms.ContextMenu([
    { text = "Cut",   shortcut = "Ctrl+X", onClick = doCut  },
    { text = "Copy",  shortcut = "Ctrl+C", onClick = doCopy },
    { text = "Paste", shortcut = "Ctrl+V", onClick = doPaste }
]);
textbox:setContextMenu(ctx);

// Programmatically pop up:
ctx:show(control, x, y);

// MenuItem methods
item:setText(s)         item:setEnabled(b)      item:setChecked(b)
item:setShortcut("Ctrl+Shift+S")
item:setIcon(imageList, index)
item:addSubItem({ ... })
item:remove()
```

---

## 10. Dialogs

### 10.1 MessageBox

```cando
VAR result = forms.MessageBox("Save changes?", "Document", {
    buttons = "yesNoCancel",      // "ok" | "okCancel" | "yesNo" |
                                  // "yesNoCancel" | "abortRetryIgnore"
    icon    = "question"          // "none" | "info" | "warning" |
                                  // "error" | "question"
});
// result is one of: "ok" "cancel" "yes" "no" "abort" "retry" "ignore"

IF result == "yes" { saveDocument(); }
```

### 10.2 OpenFileDialog / SaveFileDialog

```cando
VAR open = forms.OpenFileDialog({
    title       = "Pick a file",
    initialDir  = "C:\\Users",
    filter      = "Text files (*.txt)|*.txt|All files (*.*)|*.*",
    filterIndex = 1,
    multiSelect = FALSE
});
VAR path = open:show(parentForm);    // string, or NULL on cancel

VAR save = forms.SaveFileDialog({
    title    = "Save as",
    fileName = "document.txt",
    filter   = "Text files (*.txt)|*.txt",
    overwritePrompt = TRUE
});
VAR savePath = save:show(parentForm);
```

### 10.3 FolderBrowserDialog

```cando
VAR fb = forms.FolderBrowserDialog({ title = "Choose a folder" });
VAR dir = fb:show(parentForm);
```

### 10.4 ColorDialog / FontDialog

```cando
VAR cd = forms.ColorDialog({ initialColor = "#3366ff" });
VAR color = cd:show(parentForm);     // 0xRRGGBB number, or NULL

VAR fd = forms.FontDialog({ initialFont = { face = "Segoe UI", size = 11 } });
VAR f = fd:show(parentForm);          // {face, size, bold, italic, underline, strikeout} or NULL
```

---

## 11. Tray, timers, image lists, tooltips

### 11.1 NotifyIcon

```cando
VAR tray = forms.NotifyIcon({
    icon    = "assets/app.ico",
    text    = "My App is running",
    visible = TRUE
});

tray:setIcon(path)         tray:setText(s)         tray:show()    tray:hide()
tray:balloon("Update available", "Click to install", "info")
tray:setContextMenu(forms.ContextMenu([
    { text = "Show",  onClick = function(_) { f:show(); f:bringToFront(); } },
    { text = "Quit",  onClick = function(_) { tray:hide(); forms.exit(); } }
]));

tray.onClick       = function(self) { ... };
tray.onDoubleClick = function(self) { ... };
tray.onBalloonClicked = function(self) { ... };
```

### 11.2 Timer

```cando
VAR ticker = forms.Timer({ interval = 1000, autoStart = TRUE });
ticker.onTick = function(self) { sb:setText(`time: ${os.time()}`); };

ticker:setInterval(500)
ticker:start()           ticker:stop()           ticker:isRunning()
```

### 11.3 ImageList

```cando
VAR imgs = forms.ImageList({
    size = { 16, 16 },
    images = [
        { path = "icons/file.png" },
        { path = "icons/folder.png" },
        { path = "icons/folder-open.png" }
    ]
});

imgs:add({ path = "icons/extra.png" })
imgs:remove(i)
imgs:count()
imgs:getSize()              // returns w, h

treeView:setImageList(imgs);
node:setImageIndex(1);
```

### 11.4 ToolTip object

```cando
VAR tip = forms.ToolTip({ delayInitial = 500, delayShow = 5000, delayReshow = 100 });
tip:setText(button, "Saves the document.");
tip:setText(label,  "Total bytes processed.");
tip:remove(button);
```

(`ctrl:setToolTip("…")` on any control internally forwards to a
default `ToolTip` instance; the explicit object lets you tune
timings.)

---

## 12. Drag-and-drop

```cando
// As a drop target
panel:dragEnable(["files", "text"]);
panel.onDragEnter = function(self, data) {
    // data = { types = ["files"], files = ["C:/a.txt", ...], text = NULL }
    return TRUE;     // accept; return FALSE to reject
};
panel.onDragOver  = function(self, data, x, y) {
    self:setBackColor("lightyellow");
    return "copy";    // "copy" | "move" | "link" | "none"
};
panel.onDrop      = function(self, data, x, y) {
    self:setBackColor("white");
    FOR f OF data.files { print(`dropped ${f}`); }
};
panel.onDragLeave = function(self) { self:setBackColor("white"); };

// As a drop source (anything draggable)
listView:dragSource({
    onBegin = function(self, item) {
        return { types = ["text"], text = item:getText() };
    }
});
```

---

## 13. Custom drawing

`PaintSurface` is the dedicated owner-draw control. `Panel`,
`PictureBox`, and `Form` also accept `onPaint` for owner-draw, but
`PaintSurface` is recommended for animations.

```cando
VAR draw = include("./draw.dll");           // existing draw module

VAR canvas = forms.PaintSurface(form, {
    dock = "fill",
    doubleBuffered = TRUE,
    backColor      = "white"
});

VAR x = 0;
canvas.onPaint = function(self, gfx) {
    // gfx is a draw-module surface scoped to this control's HDC
    gfx:clear("white");
    gfx:setPen("cornflowerblue", 3);
    gfx:line(0, 0, x, 200);
    gfx:setBrush("steelblue");
    gfx:circle(x, 100, 20);
    gfx:drawText("hello", 20, 20, { face = "Segoe UI", size = 14 });
};

VAR tick = forms.Timer({ interval = 16, autoStart = TRUE });
tick.onTick = function(_) {
    x = (x + 2) % 400;
    canvas:invalidate();      // schedules WM_PAINT, fires onPaint
};
```

`gfx` exposes the same primitive surface as `modules/draw`. The
control owns the HDC; you don't free it.

---

## 14. Layout cookbook

### 14.1 Anchor — keep a button glued to the bottom-right

```cando
VAR ok = forms.Button(form, { text = "OK", anchor = "bottom right" });
ok:setLocation(form:getWidth() - 100, form:getHeight() - 40);
```

### 14.2 Dock — sidebar + content

```cando
VAR side    = forms.Panel(form, { dock = "left",   size = { 160, 0 }, backColor = "lightgray" });
VAR status  = forms.StatusBar(form);                          // implicitly docks bottom
VAR content = forms.Panel(form, { dock = "fill" });
```

### 14.3 Flow — wrap a row of buttons

```cando
VAR bar = forms.FlowLayoutPanel(form, {
    dock = "top", autoSize = TRUE,
    flowDirection = "leftToRight", wrapContents = TRUE,
    padding = 4
});
FOR name OF ["New", "Open", "Save", "Save as", "Close"] {
    forms.Button(bar, { text = name, margin = 2 });
}
```

### 14.4 Table — labelled form

```cando
VAR t = forms.TableLayoutPanel(form, {
    dock = "fill",
    columns = 2,
    columnStyles = ["auto", "percent,100"],
    padding = 8, cellPadding = 4
});

VAR addRow = function(label, control) {
    VAR row = t:addRow("auto");
    t:add(forms.Label(t, label), 0, row);
    t:add(control,                1, row);
};

addRow("Name:",     forms.TextBox(t));
addRow("Email:",    forms.TextBox(t));
addRow("Country:",  forms.ComboBox(t, { items = ["UK", "US", "DE"] }));
```

### 14.5 Split — file tree | editor | properties

```cando
VAR outer = forms.SplitContainer(form, { dock = "fill", orientation = "vertical", splitterDistance = 220 });
VAR inner = forms.SplitContainer(outer:getPanel2(), {
    dock = "fill", orientation = "vertical", splitterDistance = 600
});

forms.TreeView(outer:getPanel1(),     { dock = "fill" });
forms.RichTextBox(inner:getPanel1(),  { dock = "fill" });
forms.PropertyGrid(inner:getPanel2(), { dock = "fill" });   // future
```

---

## 15. Events catalogue

| Event                  | Receivers                         | Signature                                         |
| ---------------------- | --------------------------------- | ------------------------------------------------- |
| onClick                | every clickable control           | `(self, button, x, y)`                            |
| onDoubleClick          | every clickable control           | `(self, button, x, y)`                            |
| onMouseDown / onMouseUp| every control                     | `(self, button, x, y)`                            |
| onMouseMove            | every control                     | `(self, x, y)`                                    |
| onMouseEnter / onMouseLeave | every control                | `(self)`                                          |
| onMouseWheel           | every control                     | `(self, delta, x, y)`                             |
| onKeyDown / onKeyUp    | focusable controls                | `(self, vk, modifiers)`                           |
| onKeyPress             | focusable controls                | `(self, char)`                                    |
| onFocus / onBlur       | focusable controls                | `(self)`                                          |
| onTextChanged          | TextBox, RichTextBox, ComboBox    | `(self)`                                          |
| onValueChanged         | NumericUpDown, TrackBar, ProgressBar, DateTimePicker, MonthCalendar | `(self)` |
| onCheckedChanged       | CheckBox, RadioButton, ToggleButton, ToolStrip checkable | `(self)`                  |
| onSelectionChanged     | ListBox, ComboBox, ListView, TreeView, MonthCalendar | `(self)`                       |
| onItemActivated        | ListBox, ListView                 | `(self, item)`                                    |
| onItemChecked          | CheckedListBox, ListView, TreeView| `(self, item, checked)`                           |
| onColumnClicked        | ListView                          | `(self, columnIndex)`                             |
| onLinkClicked          | LinkLabel, RichTextBox            | `(self, id_or_href)`                              |
| onTabChanging / onTabChanged | TabControl                  | `(self, fromIndex, toIndex)` / `(self, index)`    |
| onSplitterMoved        | SplitContainer, Splitter          | `(self, distance)`                                |
| onScroll               | ScrollPanel                       | `(self, x, y)`                                    |
| onNodeSelected / onNodeExpanded / onNodeCollapsed / onNodeChecked / onNodeDoubleClicked / onLabelEdit | TreeView | `(self, node, ...)`                          |
| onCellChanged / onRowSelected | DataGrid                   | `(self, row, col, oldVal, newVal)` / `(self, row)`|
| onPaint                | PaintSurface, Panel, PictureBox, Form | `(self, gfx)`                                 |
| onResize               | Form, Panel, ScrollPanel          | `(self, w, h)`                                    |
| onShown / onActivated / onDeactivated / onClose / onMove / onDpiChanged | Form | `(self, …)`                            |
| onContextMenu          | every control                     | `(self, x, y)`                                    |
| onDragEnter / onDragOver / onDragLeave / onDrop | drop targets | `(self, data, [x, y])`                  |
| onTick                 | Timer                             | `(self)`                                          |
| onClick / onDoubleClick / onBalloonClicked | NotifyIcon          | `(self)`                                          |
| onMenuItemClicked      | MenuStrip / ContextMenu (also fires per item) | `(self, item)`                        |

---

## 16. Enums and constants

### 16.1 Dock / Anchor

```cando
forms.Dock.none = 0    forms.Dock.top = 1    forms.Dock.bottom = 2
forms.Dock.left = 3    forms.Dock.right = 4  forms.Dock.fill = 5

// Anchor flags (bitmask)
forms.Anchor.none = 0
forms.Anchor.left = 1   forms.Anchor.top = 2
forms.Anchor.right = 4  forms.Anchor.bottom = 8
forms.Anchor.all  = 15

ctrl:setAnchor("top right")           // string form
ctrl:setAnchor(forms.Anchor.top + forms.Anchor.right)
```

### 16.2 BorderStyle / AutoSizeMode

```cando
forms.BorderStyle.none    = "none"
forms.BorderStyle.single  = "single"
forms.BorderStyle.fixed3D = "fixed3D"

forms.AutoSizeMode.grow       = "grow"
forms.AutoSizeMode.growShrink = "growShrink"
```

### 16.3 Cursor / Color

```cando
forms.Cursor.arrow / hand / ibeam / wait / cross / sizeNS / sizeWE /
sizeNWSE / sizeNESW / sizeAll / no / help / appStarting / default

forms.Color.red, forms.Color.cornflowerblue, forms.Color.dodgerblue, ...
//   each is a 0xRRGGBB integer; equivalent to passing the same name
//   as a string to setBackColor / setForeColor.
```

### 16.4 Dialog enums

```cando
forms.MessageBoxButtons.ok / okCancel / yesNo / yesNoCancel / abortRetryIgnore
forms.MessageBoxIcon.none / info / warning / error / question
forms.DialogResult.ok / cancel / yes / no / abort / retry / ignore / none
```

### 16.5 Keys

```cando
forms.Keys.escape, forms.Keys.enter, forms.Keys.tab, forms.Keys.f1 ... forms.Keys.f24,
forms.Keys.a ... forms.Keys.z, forms.Keys.num0 ... forms.Keys.num9, forms.Keys.left,
forms.Keys.right, forms.Keys.up, forms.Keys.down, forms.Keys.home, forms.Keys.end,
forms.Keys.pageUp, forms.Keys.pageDown, forms.Keys.insert, forms.Keys.delete,
forms.Keys.space, forms.Keys.backspace, ...
```

### 16.6 Screen / Clipboard / Cursor utilities

```cando
forms.Screen.primary()                  // { x, y, w, h, workArea, dpi, primary = TRUE }
forms.Screen.all()                      // array of screen objects
forms.Screen.fromControl(ctrl)
forms.Screen.fromPoint(x, y)

forms.Clipboard.getText()               forms.Clipboard.setText(s)
forms.Clipboard.getImage()              forms.Clipboard.setImage(img)
forms.Clipboard.hasText()               forms.Clipboard.hasImage()
forms.Clipboard.clear()

forms.Cursor.position()                 // returns x, y on screen
forms.Cursor.setPosition(x, y)
forms.Cursor.show() / forms.Cursor.hide()
```

---

## 17. Utility wrapper layer

`forms_util.cdo` is plain CanDo. It re-exports the native module
verbatim and adds declarative builders, an `id` registry,
state-binding, and dialog helpers.

```cando
VAR ui = include("./forms_util.cdo");

// `ui` includes everything `forms` does, so you only need one import.
print(ui.VERSION);                  // forwarded
print(ui.supported);
```

### 17.1 Declarative builders

Every container builder returns a live form/control with two extras:
`form:field("id")` for lookups, and `form:state()` for the bound state
object.

```cando
VAR f = ui.Window({
    title = "Sign in",
    size  = { 360, 220 },
    center = TRUE,
    body = ui.Stack({
        direction = "vertical",
        padding   = 16,
        gap       = 8,
        children  = [
            ui.Row({ gap = 8, children = [
                ui.Label({ text = "Username:", width = 80 }),
                ui.TextBox({ id = "user", grow = TRUE })
            ]}),
            ui.Row({ gap = 8, children = [
                ui.Label({ text = "Password:", width = 80 }),
                ui.TextBox({ id = "pass", grow = TRUE, password = TRUE })
            ]}),
            ui.CheckBox({ id = "remember", text = "Remember me" }),
            ui.Spacer({ grow = TRUE }),
            ui.Row({
                align = "end", gap = 8,
                children = [
                    ui.Button({ text = "Cancel", role = "cancel" }),
                    ui.Button({ text = "Sign in", role = "accept",
                        onClick = function(self) {
                            VAR form = self:form();
                            print("user:",     form:field("user"):getText());
                            print("remember:", form:field("remember"):getChecked());
                            form:close("ok");
                        }})
                ]
            })
        ]
    })
});

VAR result = f:showModal();         // "ok" or "cancel"
```

#### Builders shipped

| Builder              | Wraps                        | Notes                            |
| -------------------- | ---------------------------- | -------------------------------- |
| `ui.Window`          | `forms.Form`                 | `body` is a single child layout. |
| `ui.Panel`           | `forms.Panel`                |                                  |
| `ui.Group`           | `forms.GroupBox`             |                                  |
| `ui.Stack`           | `forms.FlowLayoutPanel`      | `direction = "vertical"|"horizontal"`. |
| `ui.Row`             | shorthand for horizontal Stack |                                |
| `ui.Column`          | shorthand for vertical Stack |                                  |
| `ui.Grid`            | `forms.TableLayoutPanel`     | `columns`, `rowGap`, `columnGap`, child `cell = { col, row, colSpan, rowSpan }`. |
| `ui.Tabs`            | `forms.TabControl`           | `pages = [ { title, body }, ... ]`. |
| `ui.Split`           | `forms.SplitContainer`       | `panel1`, `panel2`, `orientation`, `distance`. |
| `ui.ScrollArea`      | `forms.ScrollPanel`          | `body` plus `scrollSize`.        |
| `ui.Spacer`          | invisible Panel              | `grow = TRUE` consumes leftover. |
| `ui.Label / TextBox / Button / CheckBox / RadioButton / ComboBox / ListBox / TreeView / ListView / NumericUpDown / TrackBar / ProgressBar / DateTimePicker / RichTextBox / PictureBox / PaintSurface` | matching `forms.*` constructor | accept all native options + `id`, `bind`, `role`, `grow`, `align`. |

`role`:
- `"accept"` — wires the control as the form's accept button.
- `"cancel"` — wires as cancel.
- `"primary"` / `"secondary"` — purely decorative theming hint.

`grow` (on `Stack` children):
- `TRUE` — child takes all remaining space along the stack axis.
- a number — sets a flex weight.

`align` (on `Stack`):
- `"start"` (default), `"center"`, `"end"`, `"stretch"`.

### 17.2 `id` registry

Any descriptor key with `id = "name"` registers the resulting native
instance on the owning form.

```cando
VAR f = ui.Window({ ..., body = ui.Stack({ children = [
    ui.TextBox({ id = "name" }),
    ui.Button({ text = "OK", onClick = function(self) {
        print(self:form():field("name"):getText());
    } })
]})});

f:field("name"):setText("Alice");   // also externally accessible
```

`form:fields()` returns the `{name = control, ...}` map.

### 17.3 State binding

Build an observable state object once; bind controls to it. Changes
propagate both ways.

```cando
VAR state = ui.observable({
    name = "",
    age  = 0,
    role = "user"
});

VAR f = ui.Window({ title = "Edit user", size = { 360, 200 }, body = ui.Grid({
    columns = 2, columnGap = 8, rowGap = 4, padding = 12,
    children = [
        ui.Label({ text = "Name:" }),  ui.TextBox({ bind = state.bind("name") }),
        ui.Label({ text = "Age:"  }),  ui.NumericUpDown({ bind = state.bind("age"), minimum = 0, maximum = 120 }),
        ui.Label({ text = "Role:" }),  ui.ComboBox({
            bind = state.bind("role"),
            items = ["user", "admin", "guest"]
        })
    ]
})});

state.subscribe(function(key, oldVal, newVal) {
    print(`state.${key}: ${oldVal} -> ${newVal}`);
});

f:showModal();
print("final state:", state.snapshot());
```

`bind` automatically picks the right event (`onTextChanged`,
`onValueChanged`, `onCheckedChanged`, `onSelectionChanged`) for the
control kind.

### 17.4 Dialog helpers

```cando
ui.dialog.message("Saved.", { title = "Note", icon = "info" });
VAR ok = ui.dialog.confirm("Delete this file?", { title = "Confirm", default = "no" });

VAR name = ui.dialog.prompt("Your name?", { default = "Anon" });
VAR n    = ui.dialog.promptNumber("How many?", { min = 1, max = 100, default = 5 });

VAR file = ui.dialog.openFile({ filter = "Text|*.txt|All|*.*" });
VAR dst  = ui.dialog.saveFile({ fileName = "out.txt" });
VAR dir  = ui.dialog.openFolder();
VAR col  = ui.dialog.color({ initial = "#3366ff" });
VAR fnt  = ui.dialog.font();

// All return NULL on cancel.
```

### 17.5 Menu sugar

`ui.menu` accepts the same descriptor as `forms.MenuStrip` but resolves
shortcut strings, generates ids automatically when missing, and wires a
single root `onCommand` handler if you provide one.

```cando
VAR menu = ui.menu([
    { text = "&File", items = [
        { id = "new",  text = "&New",  shortcut = "Ctrl+N" },
        { id = "open", text = "&Open", shortcut = "Ctrl+O" },
        { id = "save", text = "&Save", shortcut = "Ctrl+S" }
    ]}
], {
    onCommand = function(id) {
        IF id == "new"  { newDoc();  }
        IF id == "open" { openDoc(); }
        IF id == "save" { saveDoc(); }
    }
});
form:setMenu(menu);
```

### 17.6 Theme

```cando
ui.applyTheme(form, {
    fontFace = "Segoe UI",
    fontSize = 11,
    foreColor = "controltext",
    backColor = "buttonface",
    accent    = "cornflowerblue"
});
```

Walks the form's tree and applies the palette; safe to call multiple
times (e.g. on `darkMode` toggle).

### 17.7 Toasts and event bus

```cando
ui.toast(form, "Saved.", { duration = 2.0, position = "topRight" });

VAR bus = ui.eventBus();
bus.on("status", function(text) { sb:setText(text); });

// Anywhere
bus.emit("status", "Loading…");
```

---

## 18. Worked example — full text editor

```cando
VAR forms = include("./forms.dll");
VAR ui    = include("./forms_util.cdo");

VAR state = ui.observable({ path = NULL, dirty = FALSE });

VAR titleFor = function() {
    VAR base = state.snapshot().path != NULL ? state.snapshot().path : "Untitled";
    return state.snapshot().dirty ? `* ${base}` : base;
};

VAR f = ui.Window({
    title = titleFor(),
    size  = { 800, 600 },
    center = TRUE,
    body  = ui.Stack({
        direction = "vertical",
        children  = [
            ui.RichTextBox({ id = "editor", grow = TRUE }),
            ui.Label({ id = "status", text = "Ready.", padding = { 4, 4 } })
        ]
    })
});

VAR editor = f:field("editor");
VAR status = f:field("status");

editor.onTextChanged = function(_) {
    state.set("dirty", TRUE);
    f:setText(titleFor());
};

VAR doNew = function() {
    IF state.snapshot().dirty {
        VAR r = ui.dialog.confirm("Discard unsaved changes?");
        IF r == FALSE { return; }
    }
    editor:setText("");
    state.set("path", NULL);
    state.set("dirty", FALSE);
    f:setText(titleFor());
};

VAR doOpen = function() {
    VAR path = ui.dialog.openFile({ filter = "Text|*.txt|All|*.*" });
    IF path == NULL { return; }
    editor:loadFile(path);
    state.set("path", path);
    state.set("dirty", FALSE);
    f:setText(titleFor());
    status:setText(`Opened ${path}`);
};

VAR doSave = function() {
    VAR path = state.snapshot().path;
    IF path == NULL {
        path = ui.dialog.saveFile({ filter = "Text|*.txt" });
        IF path == NULL { return; }
        state.set("path", path);
    }
    editor:saveFile(path);
    state.set("dirty", FALSE);
    f:setText(titleFor());
    status:setText(`Saved ${path}`);
};

VAR menu = ui.menu([
    { text = "&File", items = [
        { id = "new",  text = "&New",  shortcut = "Ctrl+N" },
        { id = "open", text = "&Open", shortcut = "Ctrl+O" },
        { id = "save", text = "&Save", shortcut = "Ctrl+S" },
        { separator = TRUE },
        { id = "exit", text = "E&xit" }
    ]},
    { text = "&Edit", items = [
        { id = "undo", text = "&Undo", shortcut = "Ctrl+Z" },
        { id = "redo", text = "&Redo", shortcut = "Ctrl+Y" }
    ]}
], {
    onCommand = function(id) {
        IF id == "new"  { doNew();  }
        IF id == "open" { doOpen(); }
        IF id == "save" { doSave(); }
        IF id == "exit" { f:close(); }
        IF id == "undo" { editor:undo(); }
        IF id == "redo" { editor:redo(); }
    }
});
f:setMenu(menu);

f.onClose = function(self) {
    IF state.snapshot().dirty {
        VAR r = forms.MessageBox("Save before closing?", "Editor",
            { buttons = "yesNoCancel", icon = "question" });
        IF r == "yes"    { doSave(); }
        IF r == "cancel" { return FALSE; }   // cancel close
    }
    return TRUE;
};

f:show();
forms.run();
```

Roughly 80 lines — vs. the same script written against the raw native
API alone, which would be ~250 lines of `setText`/`setSize`/`setLocation`
plumbing.

---

## 19. Migration from forms 1.x

Given the user has chosen a clean break, the table below is a porting
crib sheet rather than a compatibility promise.

| forms 1.x                                   | forms 2.0                                 |
| ------------------------------------------- | ----------------------------------------- |
| Single shared meta table                    | Per-control meta tables; type errors are now hard errors |
| `__forms_slot`, `__forms_gen` script fields | Internal handle; not visible              |
| `setTitle` (form-only alias for `setText`)  | `forms.Form({ title = ... })` or `setText` |
| Derma `SetText`, `SetPos`, `Dock`, `Remove` | Removed. Use camelCase only.              |
| `setOpacity(0.5)` on a child silent no-op   | Method does not exist on child meta table; calling it throws |
| `addItem` on Button silently ignored        | Hard error                                |
| `setBorderStyle("3d")`                      | `setBorderStyle("fixed3D")`               |
| Single `forms.Color` palette                | Same; expanded                            |
| Stub backend                                | Same                                       |
| Implicit lifeline keeps process alive       | Same; plus optional `forms.run()` blocker |
| No tabs / scroll panel / tree / list view / menus / dialogs / tray / timer / paint surface / drag-drop / dpi handling / dark mode | All shipped |

A standalone `MIGRATING.md` will accompany the implementation.

---

*End of API specification.*
