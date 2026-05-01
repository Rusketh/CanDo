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

### Constants

The module table exports three small enum-like objects so editor
autocomplete can surface valid arguments:

* `forms.Dock` -- `none`, `top`, `bottom`, `left`, `right`, `fill`
* `forms.BorderStyle` -- `none`, `single`, `fixed3D`
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
