# forms — Windows Forms binding

The `forms` module ships a binary extension (`forms.dll` on Windows,
`forms.so` stub on Linux/macOS) that lets CanDo scripts build native
GUIs.  The public surface is shaped after `System.Windows.Forms` and
the calling convention is styled after Garry's Mod's Derma — every
form / control is an object you configure with chained
`obj:setX(...)` setters and to which you attach event handlers by
writing functions onto named properties:

```cando
VAR forms = include("./forms.dll");

VAR f = forms.Form()
f:setText("Hello")
f:setSize(400, 300)

VAR b = forms.Button(f)
b:setText("Click me")
b:setLocation(20, 20)
b.onClick = function(self) { print("clicked"); }

f:show()
```

The script is **never blocked** by an open form.  Every form holds a
VM lifeline that keeps the process alive until the form is destroyed
(either by the user calling `:destroy()` from a callback or by closing
the last form's window).  Callbacks run on a dedicated manager thread
inside a child VM that shares globals + handles with the script's VM.

## Loading the module

```cando
VAR forms = NULL;
TRY {
    forms = include("./forms.dll");        // Windows
} CATCH (e) {
    forms = include("./forms.so");         // Linux / macOS stub
}

IF forms.supported == FALSE {
    print("forms not supported on this platform");
    EXIT(0);
}
```

Module-level fields:

| Field              | Type      | Meaning                                |
| ------------------ | --------- | -------------------------------------- |
| `forms.VERSION`    | string    | semantic-version string                |
| `forms.platform`   | string    | `"windows"` or `"stub"`                |
| `forms.supported`  | bool      | `TRUE` only on Windows                 |

## Constructors

All constructors return a fresh object stamped with the
`_meta.forms_control` meta table.  Forms have no parent; every other
control requires a parent passed as the first argument.  Each
constructor accepts one of three call shapes:

```cando
forms.Button(parent)                          // bare
forms.Button(parent, "label text")            // text shorthand
forms.Button(parent, {                        // options table
    text     = "Save",
    x        = 20, y = 20,
    width    = 120, height = 30,
    multiline = FALSE,                        // TextBox only
    password  = FALSE,                        // TextBox only
})
```

Constructors:

| Native                          | Underlying control      |
| ------------------------------- | ----------------------- |
| `forms.Form([opts])`            | top-level window        |
| `forms.Button(parent, ...)`     | `BUTTON`                |
| `forms.Label(parent, ...)`      | `STATIC SS_LEFT`        |
| `forms.TextBox(parent, ...)`    | `EDIT`                  |
| `forms.CheckBox(parent, ...)`   | `BUTTON BS_AUTOCHECKBOX` |
| `forms.RadioButton(parent, ...)`| `BUTTON BS_AUTORADIOBUTTON` |
| `forms.ComboBox(parent, ...)`   | `COMBOBOX`              |
| `forms.ListBox(parent, ...)`    | `LISTBOX`               |
| `forms.Panel(parent, ...)`      | static container        |
| `forms.GroupBox(parent, ...)`   | `BUTTON BS_GROUPBOX`    |
| `forms.ProgressBar(parent, ...)`| `msctls_progress32`     |
| `forms.TrackBar(parent, ...)`   | `msctls_trackbar32`     |
| `forms.NumericUpDown(parent, ...)` | numeric-only `EDIT`  |
| `forms.PictureBox(parent, ...)` | `STATIC SS_BITMAP`      |

## Common methods

Every instance carries the same method set via the
`_meta.forms_control` meta table.  All setters return `self` for
chaining.

```cando
btn:setText("Hi"):setSize(100, 30):setLocation(10, 10);
```

| Method                       | Returns       | Notes                          |
| ---------------------------- | ------------- | ------------------------------ |
| `setText(s)` / `setTitle(s)` | self          |                                |
| `getText()`                  | string        | reads the live HWND text       |
| `setSize(w, h)`              | self          |                                |
| `getSize()`                  | `w, h`        | two return values              |
| `setLocation(x, y)`          | self          | aliased `setPosition`          |
| `getLocation()`              | `x, y`        | two return values              |
| `show()` / `hide()`          | self          | shortcuts for `setVisible`     |
| `setVisible(b)`              | self          |                                |
| `setEnabled(b)`              | self          |                                |
| `focus()`                    | self          | grab keyboard focus            |
| `setParent(other)`           | self          | pass `NULL` to detach          |
| `destroy()`                  | bool          | tears down the HWND            |
| `isOpen()`                   | bool          | `TRUE` while the HWND lives    |

Plus, control-specific methods that no-op on the wrong kind:

| Method                  | Applies to                       |
| ----------------------- | -------------------------------- |
| `setChecked(b)` / `getChecked()` | CheckBox, RadioButton    |
| `addItem(text)`         | ComboBox, ListBox                |
| `clearItems()`          | ComboBox, ListBox                |
| `getSelectedIndex()` / `setSelectedIndex(i)` | ComboBox, ListBox |
| `setValue(n)` / `getValue()` | ProgressBar, TrackBar, NumericUpDown, TextBox |
| `setRange(lo, hi)`      | ProgressBar, TrackBar            |

## Events

You attach an event handler by **writing a function onto a named
property** of the instance.  No `:on(...)` registration call, no
disposable subscription:

```cando
btn.onClick = function(self, button, x, y) {
    print("clicked at", x, y);
};
```

Event property names and signatures:

| Property                | Signature                            | Fires for                        |
| ----------------------- | ------------------------------------ | -------------------------------- |
| `onClick`               | `(self, button, x, y)`               | Button, CheckBox/Radio (after toggle), Form |
| `onClose`               | `(self)`                             | Form (X button pressed)          |
| `onShown`               | `(self)`                             | Form (after first `show()`)      |
| `onResize`              | `(self, w, h)`                       | Form                             |
| `onTextChanged`         | `(self)`                             | TextBox, NumericUpDown           |
| `onValueChanged`        | `(self)`                             | CheckBox, Radio, ProgressBar, TrackBar |
| `onSelectionChanged`    | `(self)`                             | ComboBox, ListBox                |
| `onKeyDown` / `onKeyUp` | `(self, vk_code)`                    | Form                             |
| `onMouseDown` / `onMouseUp` | `(self, button, x, y)`           | Form                             |
| `onMouseMove`           | `(self, x, y)`                       | Form                             |
| `onFocus` / `onBlur`    | `(self)`                             | Form                             |

The default `onClose` for a form hides it (matching WinForms'
`FormClosing`).  Call `self:destroy()` from your handler if you want
the form actually torn down:

```cando
f.onClose = function(self) { self:destroy(); };
```

## Threading model

There is one dedicated **manager thread** that owns every Win32
HWND and runs the single shared message pump for the whole module.
This is the same model `Application.Run` uses internally for WinForms
and matches the existing `modules/window` GLFW manager.

The script thread (the VM running your `.cdo` code) and the manager
thread are different.  Cross-thread calls are handled for you:

| Operation                      | Crosses to manager via |
| ------------------------------ | ---------------------- |
| Constructors                   | synchronous `SendMessageW(WM_FORMS_CMD)` |
| `setText`, `setSize`, ...      | `SendMessageW` directly (Win32 marshals) |
| Property mirrors (getText, etc.)| `SendMessageW` directly                  |
| `destroy`                      | `SendMessageW(WM_FORMS_CMD)`             |
| Event dispatch                 | manager pulls events from a ring buffer and calls the child VM |

Callbacks run inside a **child VM** (`cando_vm_init_child`) that shares
globals, handles, strings, and lifelines with the root VM, so they can
read and mutate the same script-side state — but each callback is
invoked in isolation: errors in one call don't poison the next, and
the manager doesn't hold any locks while a callback runs.

## Lifecycle

1. The first call to any constructor lazy-starts the manager thread
   and (on first form creation) initialises the dispatch child VM.
2. The instance acquires a VM **lifeline** so the script can `return`
   without the process exiting.
3. The manager pumps Win32 messages, queues input events into a ring
   buffer, and drains the ring on a 16 ms timer (and after every
   message dispatch).
4. Callbacks fire on the manager thread inside the child VM.
5. `instance:destroy()` (or the user closing every form) releases the
   lifeline; once the count hits zero the script's VM exits.

## Errors

* All script-visible errors are throwables — wrap construction in
  `TRY { ... } CATCH (e) { ... }` if you need to recover.
* On non-Windows, every constructor throws
  `"forms is only supported on Windows (loaded module is the stub
  build)"`.  Feature-detect with `forms.supported` to skip the GUI
  path entirely.

## Runtime dependencies

`forms.dll` is **fully self-contained** apart from the Windows OS
itself.  Specifically:

| Dependency       | How it's satisfied                                |
| ---------------- | ------------------------------------------------- |
| libgcc           | statically linked (`-static-libgcc`)              |
| winpthread       | statically linked (`--whole-archive -lwinpthread`)|
| `user32.dll`, `gdi32.dll`, `comctl32.dll`, `comdlg32.dll`, `ole32.dll`, `uuid.dll` | shipped with every Windows install |
| `libcando.dll`   | ships next to `cando.exe` in the cando distro     |

No .NET runtime is required.  The "WinForms-shaped" API is implemented
directly on top of the Win32 common controls that WinForms itself
wraps internally — `BUTTON`, `EDIT`, `STATIC`, `LISTBOX`, `COMBOBOX`,
`msctls_progress32`, `msctls_trackbar32`.  This means the module:

- works on every Windows version supported by `cando.exe` (Vista+),
- has no MSVC redistributable requirement,
- has no OpenSSL or other third-party DLL requirement,
- has no managed runtime requirement.

The CI workflow's `Verify no MinGW runtime DLL deps` step explicitly
checks `forms.dll` against an allow-list and fails the build if any
MinGW or OpenSSL runtime DLLs sneak in as imports.

## Building

```bash
make -C modules/forms                                 # forms.so (host)
make -C modules/forms forms.dll \
    MINGW_CC=x86_64-w64-mingw32-gcc                   # cross to Windows
make -C modules/forms test                            # C unit tests
```

The artefact is dropped alongside the source (`modules/forms/forms.dll`
or `forms.so`) and is also packaged in the per-platform CI artefact
under `dist/modules/forms/`, so it ships next to `cando.exe` /
`libcando.dll`.

## Worked example

```cando
VAR forms = include("./forms.dll");

VAR f = forms.Form();
f:setText("Counter");
f:setSize(240, 160);

VAR label = forms.Label(f);
label:setText("0");
label:setLocation(20, 20);
label:setSize(200, 24);

VAR up = forms.Button(f);
up:setText("+");
up:setLocation(20, 60);
up:setSize(80, 30);

VAR down = forms.Button(f);
down:setText("-");
down:setLocation(120, 60);
down:setSize(80, 30);

VAR n = 0;
up.onClick   = function(self) { n = n + 1; label:setText(string.from(n)); };
down.onClick = function(self) { n = n - 1; label:setText(string.from(n)); };

f.onClose = function(self) { self:destroy(); };
f:show();
```
