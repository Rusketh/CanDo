# forms — CanDo Windows Forms binding

A CanDo binary module for building native Windows GUIs from script.  The
public surface mirrors `System.Windows.Forms` (Form / Button / Label /
TextBox / CheckBox / RadioButton / ComboBox / ListBox / Panel / GroupBox
/ ProgressBar / TrackBar / NumericUpDown / PictureBox), and the calling
convention is styled after Garry's Mod's Derma:

```cando
VAR forms = include("./forms.dll");

VAR f = forms.Form();
f:setText("Hello, CanDo");
f:setSize(400, 300);

VAR b = forms.Button(f);
b:setText("Click me");
b:setLocation(20, 20);
b.onClick = function(self) {
    print("clicked!");
};

f:show();           // non-blocking: the script keeps running.
```

Every form / control is an object.  Properties are set with `:setX(...)`
methods; event handlers are assigned by writing a function to a named
property on the instance (`.onClick`, `.onTextChanged`, ...).

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

| Constructor                | Wraps             |
| -------------------------- | ----------------- |
| `forms.Form()`             | top-level form    |
| `forms.Button(parent)`     | `BUTTON`          |
| `forms.Label(parent)`      | `STATIC`          |
| `forms.TextBox(parent)`    | `EDIT`            |
| `forms.CheckBox(parent)`   | `BUTTON BS_AUTOCHECKBOX` |
| `forms.RadioButton(parent)`| `BUTTON BS_AUTORADIOBUTTON` |
| `forms.ComboBox(parent)`   | `COMBOBOX`        |
| `forms.ListBox(parent)`    | `LISTBOX`         |
| `forms.Panel(parent)`      | static-class container |
| `forms.GroupBox(parent)`   | `BUTTON BS_GROUPBOX` |
| `forms.ProgressBar(parent)`| `msctls_progress32` |
| `forms.TrackBar(parent)`   | `msctls_trackbar32` |
| `forms.NumericUpDown(parent)` | numeric-only `EDIT` |
| `forms.PictureBox(parent)` | `STATIC SS_BITMAP` |

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

| Method              | Notes                                          |
| ------------------- | ---------------------------------------------- |
| `setText(s)`        | aliased as `setTitle` for forms                |
| `getText()`         | returns the current Win32 caption / value      |
| `setLocation(x, y)` | aliased as `setPosition`                       |
| `getLocation()`     | aliased as `getPosition` -- returns `x, y`     |
| `setSize(w, h)`     |                                                |
| `getSize()`         | returns `w, h`                                 |
| `show()` / `hide()` | aliases for `setVisible(true/false)`           |
| `setVisible(b)`     |                                                |
| `setEnabled(b)`     |                                                |
| `focus()`           | give keyboard focus                            |
| `setParent(other)`  | reparent (pass `NULL` to detach)               |
| `destroy()`         | tear the HWND down + release the VM lifeline   |
| `isOpen()`          | `TRUE` while the HWND still exists             |

### Control-specific methods

| Method                  | Targets                          |
| ----------------------- | -------------------------------- |
| `setChecked(b)` / `getChecked()` | CheckBox, RadioButton    |
| `addItem(text)`         | ComboBox, ListBox                |
| `clearItems()`          | ComboBox, ListBox                |
| `getSelectedIndex()` / `setSelectedIndex(i)` | ComboBox, ListBox |
| `setValue(n)` / `getValue()` | ProgressBar, TrackBar, NumericUpDown, TextBox |
| `setRange(lo, hi)`      | ProgressBar, TrackBar            |

Methods that don't apply to a given control are silent no-ops, so you
can call `setChecked` on anything that has a check without writing a
type guard.

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

The script is **not blocked** while forms are open.  Each form holds a
VM lifeline so the script can `return` from `main` without tearing the
form down; callbacks fire from the manager thread inside a child VM
that shares globals + handles with the script's VM.  When every form
has been destroyed (or closed via the X button — the default `onClose`
hides; call `self:destroy()` to actually free it) the lifelines drop
and the process exits.

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
