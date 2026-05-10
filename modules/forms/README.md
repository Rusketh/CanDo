# `forms` module

Native widget tree — a CanDo binding to native OS controls in a
declarative tree shaped like .NET WinForms.  Provides Form, Button,
TextBox, ListBox, TreeView, TabControl, MenuStrip, and dozens of other
controls plus the standard set of OS dialogs (Open / Save / Folder /
Color / Font / MessageBox).

## Platforms

- **Windows** — full implementation using Win32 / Common Controls
  6.0.  Supports per-monitor DPI, dark mode, themed visual styles.
- **Linux / macOS** — the module loads as a **stub** so feature
  detection (`include` returning a non-`NULL` value) and call-sites
  compile.  Constructor calls throw `"forms not supported on this
  platform"`.

The stub design lets cross-platform scripts use `forms` for their
Windows-specific UI code path while still loading on POSIX during
development and testing.

## Loading

```cdo
VAR forms = include("./modules/forms/forms");
```

## Quick example

```cdo
VAR forms = include("./modules/forms/forms");

VAR f = forms.Form({ title: "Demo", width: 400, height: 300 });

VAR b = forms.Button({ text: "Click me", x: 150, y: 130, width: 100, height: 30 });
b.onClick = FUNCTION() {
    forms.MessageBox.show("Hello!");
};
f:add(b);

f:show();
forms.run();      // blocks until all windows close
```

## Controls

The module exports constructors for every control kind.  All
constructors take a single options object and return a control handle.

### Containers

`Form`, `Panel`, `GroupBox`, `TabControl`, `FlowLayoutPanel`,
`TableLayoutPanel`, `ScrollPanel`, `Splitter`.

### Inputs

`Button`, `TextBox`, `Label`, `LinkLabel`, `CheckBox`, `RadioButton`,
`NumericUpDown`, `TrackBar`, `Spinner`, `DateTimePicker`,
`MonthCalendar`, `ComboBox`.

### Lists and trees

`ListBox`, `ListView`, `TreeView`.

### Display

`PictureBox`, `ProgressBar`, `StatusBar`, `PaintSurface`.

### Menus and notifications

`MenuStrip`, `ContextMenu`, `NotifyIcon`.

### Timers

`Timer` — non-UI callback fired on the UI thread.

## Common methods

Every control supports:

| Method                              | Description |
|-------------------------------------|-------------|
| `c:show()` / `c:hide()`             | Visibility. |
| `c:enable()` / `c:disable()`        | Interactive state. |
| `c:focus()`                         | Move keyboard focus. |
| `c:setSize(w, h)`                   | Resize. |
| `c:setPosition(x, y)`               | Reposition. |
| `c:close()`                         | For top-level forms. |
| `parent:add(child)`                 | Attach to a container. |
| `c.onXxx = fn`                      | Attach an event handler. |

## Events

Event handlers are assigned as ordinary fields on the control:

```cdo
button.onClick = FUNCTION() { … };
form.onClose   = FUNCTION() { … };
text.onChange  = FUNCTION(value) { … };
list.onSelect  = FUNCTION(index, item) { … };
```

The full event surface depends on the control; see the per-control
sections below for the events each one fires.

## Dialogs

| Dialog                            | Returns |
|-----------------------------------|---------|
| `forms.OpenFileDialog.show(opts*)` | Selected path, or `NULL` on cancel. |
| `forms.SaveFileDialog.show(opts*)` | Selected path, or `NULL`. |
| `forms.FolderBrowserDialog.show(opts*)` | Selected directory, or `NULL`. |
| `forms.ColorDialog.show(initial*)` | Picked colour, or `NULL`. |
| `forms.FontDialog.show(initial*)`  | Picked font descriptor, or `NULL`. |
| `forms.MessageBox.show(text, title*, buttons*) → string` | One of `"ok"`, `"cancel"`, `"yes"`, `"no"`, `"retry"`, `"abort"`, `"ignore"`. |

## Utilities

| Function               | Description |
|------------------------|-------------|
| `forms.run()`          | Enter the UI message loop.  Returns when all forms have closed. |
| `forms.exit()`         | Request the loop to exit. |
| `forms.darkMode(bool)` | Toggle dark mode for new windows (Windows 10+). |
| `forms.dpiFor(form) → number` | DPI for a given window. |

## Layout patterns

The two layout panels — `FlowLayoutPanel` and `TableLayoutPanel` —
arrange children automatically; everything else uses absolute
positioning.

```cdo
VAR row = forms.FlowLayoutPanel({ direction: "horizontal", spacing: 8 });
row:add(forms.Label({ text: "Name:" }));
row:add(forms.TextBox({ width: 200 }));
row:add(forms.Button({ text: "OK" }));

f:add(row);
```

## Examples

### Login dialog

```cdo
VAR forms = include("./modules/forms/forms");

VAR f = forms.Form({ title: "Login", width: 320, height: 200 });

VAR user = forms.TextBox({ x: 110, y: 30, width: 180, placeholder: "username" });
VAR pw   = forms.TextBox({ x: 110, y: 70, width: 180, password: TRUE });
VAR ok   = forms.Button({ text: "OK", x: 110, y: 110, width: 80 });
VAR cx   = forms.Button({ text: "Cancel", x: 200, y: 110, width: 80 });

f:add(forms.Label({ text: "User:", x: 30, y: 32 }));
f:add(forms.Label({ text: "Pass:", x: 30, y: 72 }));
f:add(user); f:add(pw); f:add(ok); f:add(cx);

ok.onClick = FUNCTION() {
    IF authenticate(user.text, pw.text) {
        f:close();
    } ELSE {
        forms.MessageBox.show("Invalid credentials");
    }
};
cx.onClick = FUNCTION() { f:close(); };

f:show();
forms.run();
```

### Background work with a progress bar

```cdo
VAR f = forms.Form({ title: "Working", width: 400, height: 80 });
VAR p = forms.ProgressBar({ x: 10, y: 20, width: 380, height: 30, max: 100 });
f:add(p);
f:show();

thread {
    FOR i IN 0 -> 100 {
        thread.sleep(50);
        p:setValue(i);
    }
    f:close();
};

forms.run();
```

## Errors

On non-Windows platforms, every constructor throws.  Wrap module
initialization in `TRY` / `CATCH`, or feature-detect with
`forms.darkMode != NULL` etc., to enable cross-platform fall-backs.
