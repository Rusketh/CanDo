# forms — CanDo Windows Forms binding

A CanDo binary module for building native Windows GUIs from script.  The
public surface mirrors `System.Windows.Forms` and the calling convention
is styled after Garry's Mod's Derma — every form / control is an object,
properties are set with `:setX(...)` methods, and event handlers are
written as functions to a named property on the instance (`.onClick`,
`.onTextChanged`, …).

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
b:setBackColor("cornflowerblue");
b:setForeColor("white");
b.onClick = function(self) {
    print("clicked!");
};

f:show();           // non-blocking: the script keeps running.
```

## Backend

* **Windows** — real Win32 backend.  All UI work runs on a single
  dedicated manager thread that owns the message pump for every form.
  Property setters cross threads via `SendMessageW`, which Win32
  marshals to the owning thread automatically; window creation and
  destruction post a synchronous command to the manager.
* **Linux / macOS** — a stub backend that loads cleanly so feature
  detection works (`forms.supported == FALSE`) but throws
  `"forms is only supported on Windows"` from every constructor.

## Quick reference

### Constructors (on the module table)

| Constructor                | Wraps                          |
| -------------------------- | ------------------------------ |
| `forms.Form()`             | top-level form                 |
| `forms.Button(parent)`     | `BUTTON`                       |
| `forms.Label(parent)`      | `STATIC`                       |
| `forms.LinkLabel(parent)`  | `SysLink` (clickable hyperlink)|
| `forms.TextBox(parent)`    | `EDIT`                         |
| `forms.CheckBox(parent)`   | `BUTTON BS_AUTOCHECKBOX`       |
| `forms.RadioButton(parent)`| `BUTTON BS_AUTORADIOBUTTON`    |
| `forms.ComboBox(parent)`   | `COMBOBOX`                     |
| `forms.ListBox(parent)`    | `LISTBOX`                      |
| `forms.Panel(parent)`      | static-class container         |
| `forms.GroupBox(parent)`   | `BUTTON BS_GROUPBOX`           |
| `forms.ProgressBar(parent)`| `msctls_progress32`            |
| `forms.TrackBar(parent)`   | `msctls_trackbar32`            |
| `forms.NumericUpDown(parent)` | numeric-only `EDIT`         |
| `forms.PictureBox(parent)` | `STATIC SS_BITMAP`             |
| `forms.DateTimePicker(parent)` | `SysDateTimePick32`        |
| `forms.MonthCalendar(parent)`  | `SysMonthCal32`            |
| `forms.StatusBar(parent)`  | `msctls_statusbar32`           |
| `forms.Spinner(parent)`    | `msctls_updown32`              |

Every constructor accepts the same option shapes:

```cando
forms.Button(parent)                          // bare
forms.Button(parent, "label text")            // text shorthand
forms.Button(parent, {                        // options table
    text     = "Save",
    x        = 20, y = 20,
    width    = 120, height = 30,
})
```

### Methods (on every instance)

| Method              | Notes                                                   |
| ------------------- | ------------------------------------------------------- |
| `setText(s)`        | aliased as `setTitle` for forms                         |
| `getText()`         | returns the current Win32 caption / value               |
| `setLocation(x, y)` | aliased as `setPosition`                                |
| `getLocation()`     | aliased as `getPosition` -- returns `x, y`              |
| `setSize(w, h)`     |                                                         |
| `getSize()`         | returns `w, h`                                          |
| `setWidth(w)` / `setHeight(h)` | set one axis without touching the other      |
| `getWidth()` / `getHeight()` | each returns a single number                    |
| `sizeToContent()`   | resize to the control's preferred (natural) size        |
| `sizeToContentWidth()` / `sizeToContentHeight()` | one-axis variants          |
| `getPreferredSize()` | returns `w, h` without applying it to the control      |
| `setAutoSize(b)` / `getAutoSize()` | continuously fit content (re-runs after `setText` / `setFont` / `setPadding` / `addItem`) |
| `setAutoSizeMode(m)` | `"grow"` (only enlarge) or `"growShrink"` (default)   |
| `setPadding(...)` / `getPadding()` | inner padding -- 1 / 2 / 4 numbers (`all`, `h v`, or `l t r b`).  Consumed by `sizeToContent` and the auto-fit pass. |
| `setMargin(...)` / `getMargin()`   | outer margin -- same shapes; reserved for layout managers. |
| `setAnchor(...)`     | edge tracking on parent resize -- `"left"`, `"top"`, `"right"`, `"bottom"`, `"all"`, `"none"`, or a space-separated list (`"top right"`); also accepts a numeric mask. |
| `getAnchor()`        | returns the numeric mask                                |
| `show()` / `hide()` | aliases for `setVisible(true/false)`                    |
| `setVisible(b)`     |                                                         |
| `setEnabled(b)`     |                                                         |
| `getEnabled()` / `isEnabled()` | runtime state, asks Win32 directly           |
| `getVisible()` / `isVisible()` | runtime state, asks Win32 directly           |
| `focus()`           | give keyboard focus                                     |
| `setParent(other)`  | reparent (pass `NULL` to detach)                        |
| `bringToFront()`    | raise above sibling controls                            |
| `sendToBack()`      | lower below siblings                                    |
| `refresh()`         | force a synchronous redraw (alias `invalidate`)         |
| `destroy()`         | tear the HWND down + release the VM lifeline           |
| `isOpen()`          | `TRUE` while the HWND still exists                      |

### Form-only methods

These are no-ops (or apply to whatever the underlying Win32 control
permits) when called on a child control, so script code can stay loose.

| Method                  | Notes                                          |
| ----------------------- | ---------------------------------------------- |
| `center()` / `centre()` | move the form to the centre of its monitor's work area |
| `setOpacity(a)`         | `0..1` (float) or `0..255` (int).  Enables WS_EX_LAYERED. |
| `getOpacity()`          | returns the current alpha as `0..1`            |
| `setTopMost(b)`         | keep the form above all other windows          |
| `setMinSize(w, h)`      | clamp `WM_GETMINMAXINFO` so user-resize can't shrink past this |
| `setMaxSize(w, h)`      | same, upper bound                              |
| `setBorderStyle(s)`     | `"none"`, `"single"`, or `"3d"` -- works on any control |

### Control-specific methods

| Method                  | Targets                                  |
| ----------------------- | ---------------------------------------- |
| `setChecked(b)` / `getChecked()` | CheckBox, RadioButton           |
| `addItem(text)`         | ComboBox, ListBox                        |
| `removeItem(index)`     | ComboBox, ListBox                        |
| `clearItems()`          | ComboBox, ListBox                        |
| `getItem(index)`        | ComboBox, ListBox -- text at index       |
| `getItems()`            | ComboBox, ListBox -- array of all items  |
| `getItemCount()`        | ComboBox, ListBox                        |
| `getSelectedIndex()` / `setSelectedIndex(i)` | ComboBox, ListBox   |
| `setValue(n)` / `getValue()` | ProgressBar, TrackBar, NumericUpDown, TextBox |
| `setRange(lo, hi)`      | ProgressBar, TrackBar                    |
| `setMarquee(active, [speed])`  | ProgressBar (needs `marquee=true` at construction) |
| `setState("normal"\|"warning"\|"error"\|"paused")` | ProgressBar |

Methods that don't apply to a given control are silent no-ops, so you
can call `setChecked` on anything that has a check without writing a
type guard.

### Auto-sizing, padding, margin, anchor

`sizeToContent()` measures the control's natural size (caption text +
per-kind padding for buttons / labels / check boxes / text boxes; the
widest item plus a vertical stack for `ListBox`; the dropdown for
`ComboBox`; `MCM_GETMINREQRECT` for `MonthCalendar`) and resizes the
control to fit.  For `Form`, `Panel`, and `GroupBox` it instead takes
the bounding box of every direct child plus a small margin (and the
non-client frame, for forms).

```cando
b:setText("Save changes");
b:sizeToContent();      // button now exactly fits its caption
panel:sizeToContent();  // panel shrinks/grows to wrap its children
form:sizeToContent();   // form auto-fits the layout it contains
```

`sizeToContentWidth()` / `sizeToContentHeight()` change a single axis;
`getPreferredSize()` returns the computed `w, h` pair without applying
it.

`setAutoSize(true)` makes the fit continuous: every `setText`,
`setFont`, `setPadding`, or `addItem` call re-runs the calculation.
`setAutoSizeMode("grow")` allows the control to enlarge only -- never
shrink below its current size; the default `"growShrink"` lets it
contract too.  Combine with `setMinSize(...)` to clamp the auto-fit's
lower bound on forms.

`setPadding(l, t, r, b)` is the gap inside the control between its
border and its contents -- it adds to the auto-fit calculation.
`setMargin(l, t, r, b)` is the outer gap reserved around the control
for layout managers.  Both accept 1 / 2 / 4 numeric arguments
(all-sides, horizontal+vertical, LTRB).

`setAnchor("top right")` fixes a child's distance to its parent's top
and right edges; resizing the parent moves the child to keep those
gaps constant.  Anchoring opposite edges stretches the child:

```cando
btn:setAnchor("bottom right")     // floats with the form's BR corner
panel:setAnchor("left top right") // stretches horizontally on resize
panel:setAnchor("all")            // == "left top right bottom"
```

### Fonts

```cando
btn:setFont("Segoe UI", 12)                    // face + size
btn:setFont("Segoe UI", 12, "bold italic")     // + style trail
btn:setFont({face = "Consolas", size = 14,     // options table
             bold = TRUE, italic = TRUE,
             underline = FALSE,
             strikeout = FALSE})
btn:setFontSize(16)                            // size only
btn:setBold(TRUE)
btn:setItalic(TRUE)
btn:setUnderline(TRUE)
btn:setStrikeout(TRUE)
btn:clearFont()                                // revert to parent font
```

| Method                | Notes                                              |
| --------------------- | -------------------------------------------------- |
| `setFont(...)`        | accepts (face), (face, size), (face, size, style) or `{face=, size=, bold=, italic=, underline=, strikeout=}` |
| `setFontSize(n)`      | point size, keeps current face / style              |
| `setBold(b)` / `setItalic(b)` / `setUnderline(b)` / `setStrikeout(b)` | toggle individual style flags |
| `clearFont()`         | drop the override; control re-inherits parent font  |
| `getFont()`           | returns `{face, size, bold, italic, underline, strikeout}` or `NULL` |

Setters always create a fresh `HFONT` from the slot's accumulated state
and apply it via `WM_SETFONT`, so calling `setBold(TRUE)` on a button
that has never been styled before still works (it picks up the system
default GUI face/size first).

### Colours

`setForeColor` / `setBackColor` accept any of:

* three numbers `(r, g, b)` -- `0..255` each
* one packed integer `0xRRGGBB`
* a hex string `"#RRGGBB"`, `"#RGB"`, or `"#AARRGGBB"` (alpha is dropped)
* a CSS-style named colour: `"red"`, `"cornflowerblue"`, `"darkgreen"`, ...

```cando
b:setForeColor(255, 0, 0)
b:setForeColor(0xFF0000)
b:setForeColor("#ff0000")
b:setForeColor("#f00")
b:setForeColor("red")
b:setBackColor(forms.Color.cornflowerblue)
```

`forms.Color` is a discoverable namespace populated with the same names
the string parser recognises -- handy for editor autocomplete.

| Method                              | Notes                                |
| ----------------------------------- | ------------------------------------ |
| `setForeColor(...)` / `setColor(...)` | text colour                        |
| `setBackColor(...)` / `setBackground(...)` | background                    |
| `getForeColor()` / `getBackColor()` | returns `0xRRGGBB` or `NULL` if no override |
| `clearForeColor()` / `clearBackColor()` | revert to system default         |

### Docking

| Method                              | Notes                          |
| ----------------------------------- | ------------------------------ |
| `setDock(side)`                     | `"top"`, `"bottom"`, `"left"`, `"right"`, `"fill"`, `"none"` -- or use `forms.Dock.*` constants |
| `getDock()`                         | numeric Dock value             |
| `relayout()`                        | force the parent to re-run docking now |

Docked children are laid out automatically when the parent form
resizes.  Children peel rects off the parent's client area in
allocation order (`TOP`, `BOTTOM`, `LEFT`, `RIGHT` -- using each
child's stored size for the docked dimension).  A single `FILL` child
takes whatever's left.

```cando
VAR top    = forms.Panel(f);     top:setSize(0, 40):setBackColor(forms.Color.steelblue):setDock("top")
VAR side   = forms.Panel(f);     side:setSize(120, 0):setDock("left")
VAR status = forms.Label(f);     status:setSize(0, 20):setDock("bottom")
VAR body   = forms.Panel(f);     body:setDock("fill")
```

### Events (set as instance properties)

| Property              | Signature                                |
| --------------------- | ---------------------------------------- |
| `onClick`             | `function(self, button, x, y)`           |
| `onClose`             | `function(self)` (forms only)            |
| `onShown`             | `function(self)` (forms only)            |
| `onResize`            | `function(self, w, h)` (forms only)      |
| `onTextChanged`       | `function(self)`                         |
| `onValueChanged`      | `function(self)`                         |
| `onSelectionChanged`  | `function(self)`                         |
| `onKeyDown` / `onKeyUp` | `function(self, vk)`                   |
| `onMouseDown` / `onMouseUp` | `function(self, button, x, y)`     |
| `onMouseMove`         | `function(self, x, y)`                   |
| `onFocus` / `onBlur`  | `function(self)`                         |

`LinkLabel` activations (mouse click and Enter on focus) fire
`onClick`.  `DateTimePicker` raises `onValueChanged` whenever the
selected date changes; `MonthCalendar` raises `onSelectionChanged`.

The script is **not blocked** while forms are open.  Each form holds a
VM lifeline so the script can `return` from `main` without tearing the
form down; callbacks fire from the manager thread inside a child VM
that shares globals + handles with the script's VM.  When every form
has been destroyed (or closed via the X button — the default `onClose`
hides; call `self:destroy()` to actually free it) the lifelines drop
and the process exits.

### TextBox extras

| Method                       | Notes                                          |
| ---------------------------- | ---------------------------------------------- |
| `setMultiline(b)`            | toggle multi-line edit + word-wrap             |
| `setReadOnly(b)`             | EM_SETREADONLY                                 |
| `setPlaceholder(s)` / `setHint(s)` | EM_SETCUEBANNER                          |
| `setPasswordChar(c)`         | character (or a single-char string) used for masking |
| `setMaxLength(n)`            | EM_SETLIMITTEXT                                |
| `setTextAlign("left"/"center"/"right")` | also works on `Label`               |
| `selectAll()`                | EM_SETSEL 0..-1                                |
| `appendText(s)`              | append to the end without disturbing selection |
| `clear()`                    | empty the contents (works on `ComboBox` and `ListBox` too) |

### Tooltip + cursor

| Method            | Notes                                                |
| ----------------- | ---------------------------------------------------- |
| `setToolTip(s)`   | shows `s` in a Win32 tooltip when the user hovers.  Pass `""` to remove. |
| `setCursor(name)` | `"arrow"`, `"hand"`, `"ibeam"`, `"wait"`, `"cross"`, `"size-ns"`, `"size-we"`, `"size-nwse"`, `"size-nesw"`, `"size-all"`, `"no"`, `"help"`, `"appstarting"`, or `"default"`. |

### Form-only state

| Method                       | Notes                                       |
| ---------------------------- | ------------------------------------------- |
| `setIcon(path)`              | load an `.ico` and apply via `WM_SETICON`   |
| `flash([n])`                 | `FlashWindowEx` for `n` (default 3) cycles  |
| `maximize()` / `minimize()` / `restore()` | one-shot window-state helpers  |
| `setWindowState("normal" / "maximized" / "minimized")` | set + read |
| `getWindowState()`           | returns `"normal"`, `"maximized"`, or `"minimized"` |
| `setResizable(b)`            | toggles `WS_THICKFRAME` + `WS_MAXIMIZEBOX`  |
| `setMinimizeBox(b)` / `setMaximizeBox(b)` | individual title-bar buttons   |
| `setShowInTaskbar(b)`        | `WS_EX_APPWINDOW` vs `WS_EX_TOOLWINDOW`     |
| `setAcceptButton(btn)`       | mark `btn` as `BS_DEFPUSHBUTTON`            |
| `setCancelButton(btn)`       | stored on the form for later use            |

### Numeric / Progress / TrackBar extras

| Method                       | Notes                                       |
| ---------------------------- | ------------------------------------------- |
| `setStep(n)` / `stepIt()`    | `ProgressBar` -- `PBM_SETSTEP` / `PBM_STEPIT` |
| `setTickFrequency(n)`        | `TrackBar` -- `TBM_SETTICFREQ`              |
| `setSmallStep(n)` / `setLargeStep(n)` | `TrackBar` -- arrow-key + page-key step |
| `setIncrement(n)`            | `NumericUpDown` accelerator -- `UDM_SETACCEL` |

### Tree, focus, tab order

| Method                       | Notes                                       |
| ---------------------------- | ------------------------------------------- |
| `getParent()` / `getChildren()` | walk the form tree (returns instances or array) |
| `contains(other)`            | `true` if `other` is a descendant           |
| `hasFocus()`                 | `GetFocus() == hwnd`                        |
| `setTabIndex(n)` / `getTabIndex()` | tab-order index                       |
| `setTabStop(b)`              | toggle `WS_TABSTOP`                         |
| `remove()`                   | alias for `destroy()` (Derma-flavoured)     |

### Derma-style PascalCase aliases

Scripts coming from gmod can use familiar names instead -- every entry
below is a pure alias.

| Derma name        | Equivalent                       |
| ----------------- | -------------------------------- |
| `:SetText`, `:GetText`, `:SetSize`, `:SetPos`, `:GetPos`, `:SetVisible`, `:SetEnabled` | the obvious counterparts |
| `:MoveToFront` / `:MoveToBack` | `bringToFront` / `sendToBack` |
| `:Remove`         | `destroy`                        |
| `:Center`         | `center`                         |
| `:Dock(side)`     | `setDock(side)`                  |
| `:DockPadding(...)` / `:DockMargin(...)` | `setPadding` / `setMargin` |
| `:InvalidateLayout` | `relayout`                     |
| `:SizeToContents` | `sizeToContent`                  |

### Constants

The module table exports a handful of enum-like objects so editor
autocomplete can surface valid arguments:

* `forms.Dock` -- `none`, `top`, `bottom`, `left`, `right`, `fill`
* `forms.Anchor` -- `none`, `left`, `top`, `right`, `bottom`, `all`
  (bitmask values; combine with `+` or pass the string form to `setAnchor`)
* `forms.AutoSizeMode` -- `grow`, `growShrink`
* `forms.BorderStyle` -- `none`, `single`, `fixed3D`
* `forms.Cursor` -- string aliases for `setCursor` (`arrow`, `hand`,
  `ibeam`, `wait`, `cross`, `sizeNS`, `sizeWE`, `sizeNWSE`, `sizeNESW`,
  `sizeAll`, `no`, `help`, `appStarting`)
* `forms.Color` -- a CSS-style palette (`red`, `cornflowerblue`,
  `darkgreen`, `steelblue`, …) holding `0xRRGGBB` packed integers.
  Also reachable as plain strings -- `setBackColor("cornflowerblue")`
  is equivalent to `setBackColor(forms.Color.cornflowerblue)`.

## Runtime dependencies

`forms.dll` is **fully self-contained** apart from the Windows operating
system itself.  Specifically:

| Dependency       | How it's satisfied                                |
| ---------------- | ------------------------------------------------- |
| libgcc           | statically linked (`-static-libgcc`)              |
| winpthread       | statically linked (`--whole-archive -lwinpthread`)|
| `user32.dll`, `gdi32.dll`, `comctl32.dll`, `comdlg32.dll`, `ole32.dll`, `uuid.dll` | shipped with every Windows install |
| `libcando.dll`   | ships next to `cando.exe` in the cando distro     |

There is **no .NET runtime requirement, no MSVC redistributable, no
OpenSSL**.  The "WinForms-shaped" API is implemented directly on top of
the Win32 controls (`BUTTON`, `EDIT`, `STATIC`, common controls, ...)
that WinForms wraps internally — same surface, none of the managed
runtime baggage.

The CI workflow's `Verify no MinGW runtime DLL deps` step explicitly
checks `forms.dll` and fails the build if it accidentally picks up
`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
`libssl-3-x64.dll`, or `libcrypto-3-x64.dll` as imports.

## Building

```bash
make -C modules/forms                   # forms.so + test_forms (Linux/macOS)
make -C modules/forms forms.dll \       # Windows DLL via MinGW
    MINGW_CC=x86_64-w64-mingw32-gcc
make -C modules/forms test              # run C unit tests
```

The full integration test (`test_forms.cdo`) is Windows-only; CI runs
the headless smoke check (`test_forms_smoke.cdo`) on every push.

## Files

| File                       | Purpose                                  |
| -------------------------- | ---------------------------------------- |
| `forms_module.c`           | single C source: Win32 backend + stub    |
| `Makefile`                 | per-module build rules                   |
| `test_forms.c`             | C unit tests (event queue + slot table)  |
| `test_forms.cdo`           | full integration script (Windows-only)   |
| `test_forms_smoke.cdo`     | headless smoke check (any platform)      |
