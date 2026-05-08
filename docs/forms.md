# forms — native Windows GUI module

The `forms` module ships a binary extension (`forms.dll` on Windows,
`forms.so` stub on Linux/macOS) that lets CanDo scripts build native
GUIs.  The public surface is shaped after `System.Windows.Forms`:
every form / control is a script object configured with chained
`obj:setX(...)` setters and event handlers attached as functions to
named properties.

```cando
VAR forms = include("./forms.dll");

VAR f = forms.Form({ title: "Hello", size: [400, 300] });

VAR b = forms.Button(f, {
    text:     "Click me",
    location: [20, 20],
    size:     [100, 30],
    onClick:  function(self) { print("clicked"); }
});

f:show();
```

The script is **never blocked** by an open form.  Every form holds a
VM lifeline that keeps the process alive until it is destroyed (via
`self:destroy()` from a callback or by closing the last form's
window).  Callbacks run on a dedicated manager thread inside a child
VM that shares globals + handles with the script's VM.

For the full reference — every constructor, method, event,
`forms_util.cdo` builder, dialog helper, layout cookbook, and
worked end-to-end example — see [`modules/forms/README.md`][readme].

[readme]: ../modules/forms/README.md

## At a glance

### Loading

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

### Module-level fields

| Field             | Type    | Meaning                              |
| ----------------- | ------- | ------------------------------------ |
| `forms.VERSION`   | string  | semantic version                     |
| `forms.platform`  | string  | `"windows"` or `"stub"`              |
| `forms.supported` | bool    | `TRUE` only on Windows               |

### Controls

| Category    | Constructors                                                |
| ----------- | ----------------------------------------------------------- |
| Form        | `Form`                                                      |
| Inputs      | `Button` `CheckBox` `RadioButton` `TextBox` `ComboBox` `ListBox` `NumericUpDown` `TrackBar` `DateTimePicker` `MonthCalendar` `Spinner` `LinkLabel` |
| Display     | `Label` `PictureBox` `ProgressBar` `StatusBar` `PaintSurface` |
| Containers  | `Panel` `GroupBox` `ScrollPanel` `TabControl` `FlowLayoutPanel` `TableLayoutPanel` `Splitter` |
| Trees / lists | `TreeView` `ListView`                                     |
| Menus       | `MenuStrip` `ContextMenu` (items via `menu:addItem(text)`)  |
| Non-visual  | `Timer` `NotifyIcon`                                        |

### Dialogs

```cando
forms.MessageBox(text, [title], [opts])  -> "ok" | "cancel" | "yes" | …
forms.OpenFileDialog([opts])             -> path string or NULL
forms.SaveFileDialog([opts])             -> path string or NULL
forms.FolderBrowserDialog([opts])        -> path string or NULL
forms.ColorDialog([initial])             -> 0xRRGGBB number or NULL
forms.FontDialog()                       -> {face, size, bold, ...} or NULL
```

### Module-level helpers

```cando
forms.dpiFor(control)      -- effective DPI of the control's monitor
forms.darkMode(b)          -- toggle immersive dark mode on every form
```

### Layout

Every control supports `setDock("top" | "bottom" | "left" | "right" |
"fill" | "none")` and `setAnchor("top right" | "all" | …)`.  In
addition:

- `forms.FlowLayoutPanel` arranges children in flow order with
  optional wrap.
- `forms.TableLayoutPanel` arranges children in an auto-sized
  grid; `:add(child, col, row, [colSpan, rowSpan])`.
- `forms.Splitter` is a thin draggable bar that resizes its previous
  alive sibling (or an explicit `:setTarget(other)`).

### Events

Set as instance properties: `onClick`, `onMouseDown` / `Up` /
`Move`, `onKeyDown` / `Up`, `onFocus` / `Blur`, `onTextChanged`,
`onValueChanged`, `onSelectionChanged`, `onItemActivated`,
`onTabChanged`, `onNodeSelected` / `Expanded` / `Collapsed`,
`onTick`, `onPaint`, `onClose` / `Shown` / `Resize`.  See the
README's [Events catalogue][events] for the per-control signatures.

[events]: ../modules/forms/README.md#events-catalogue

### `forms_util.cdo`

A pure-CanDo wrapper at `modules/forms/script/forms_util.cdo`
provides a declarative tree API on top of the raw native surface:

```cando
VAR ui = include("./script/forms_util.cdo");

VAR f = ui.Window({
    title:  "Settings",
    size:   [400, 280],
    body: {
        kind: "Tabs",
        pages: [
            { title: "General", body: {
                kind: "Grid", columns: 2, cellPadding: 4,
                children: [
                    { kind: "Label",   text: "Theme:" },
                    { kind: "ComboBox", id: "theme", items: ["Light", "Dark"] }
                ]
            }},
            { title: "Advanced", body: { kind: "Label", text: "(coming soon)" } }
        ]
    }
});
f:show();
```

`ui.dialog.message / confirm / openFile / saveFile / openFolder /
color / font` wrap the native dialog functions; `ui.menu` /
`ui.contextMenu` build menu trees from declarative tables;
`ui.observable` / `ui.eventBus` provide light state-management
helpers.

## Building

```bash
make -C modules/forms                        # forms.so + test_forms (Linux)
make -C modules/forms forms.dll \            # Windows DLL via MinGW
    MINGW_CC=x86_64-w64-mingw32-gcc
make -C modules/forms test                   # run C unit tests
```

`forms.dll` is fully self-contained against the User32 / GDI /
Comctl32 / Comdlg32 / Shell32 / DwmApi / Ole32 stack that ships with
every Windows install.  No .NET runtime, no MSVC redistributable.

## Files

| File                                    | Purpose                          |
| --------------------------------------- | -------------------------------- |
| `modules/forms/forms_module.c`          | module entry + Win32 backend     |
| `modules/forms/src/core/`               | shared infrastructure (slots, events, layout, …) |
| `modules/forms/src/controls/`           | per-kind native method TUs       |
| `modules/forms/script/forms_util.cdo`   | declarative wrapper              |
| `modules/forms/test_forms.c`            | C unit tests                     |
| `modules/forms/test_forms.cdo`          | Windows-only integration script  |
| `modules/forms/test_forms_smoke.cdo`    | headless smoke check             |
