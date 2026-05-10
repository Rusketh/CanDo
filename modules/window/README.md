# `window` module

A cross-platform OS window with an OpenGL context.  Built on GLFW; runs
on Linux, macOS, and Windows.

The `window` module gives you a window and an event loop.  For drawing
inside the window, see [`draw/`](../draw/README.md).

## Loading

```cdo
VAR window = include("./modules/window/window");
```

## Creating a window

### `window.create(opts*) → win`

`opts`:

| Field        | Type   | Default       | Description |
|--------------|--------|---------------|-------------|
| `title`      | string | `"CanDo"`     | Window title. |
| `width`      | number | `800`         | Initial width in points. |
| `height`     | number | `600`         | Initial height. |
| `resizable`  | bool   | `TRUE`        | |
| `vsync`      | bool   | `TRUE`        | Vertical sync. |
| `glVersion`  | string | `"3.3"`       | Requested OpenGL core profile. |

```cdo
VAR win = window.create({ title: "Demo", width: 1024, height: 768 });
```

The window is shown immediately.  The current GL context is made
active; a draw library can take over from here.

## Window methods

| Method                              | Description |
|-------------------------------------|-------------|
| `win:close()`                       | Close the window. |
| `win:isOpen() → bool`               | `FALSE` after the OS has closed it. |
| `win:swap()`                        | Swap front and back buffers (call once per frame). |
| `win:pollEvents()`                  | Pump OS events.  Sets `win:keyDown(k)`, etc. |
| `win:waitEvents(timeoutMs*)`        | Block until an event arrives or the timeout expires. |
| `win:size() → number, number`       | Width, height in points. |
| `win:framebufferSize() → number, number` | Width, height in pixels (for HiDPI). |
| `win:contentScale() → number, number` | DPI scale factors. |
| `win:setSize(w, h)`                 | Resize. |
| `win:setTitle(title)`               | Update the title. |
| `win:keyDown(key) → bool`           | Is `key` currently held? |
| `win:mouseButton(b) → bool`         | Is mouse button `b` currently held? |
| `win:mouse() → number, number`      | Current mouse position. |
| `win:onKey(fn)`                     | Register `fn(key, action)` for key events. |
| `win:onResize(fn)`                  | Register `fn(width, height)`. |
| `win:onClose(fn)`                   | Register `fn()` when the user requests close. |

## Event loop

```cdo
VAR window = include("./modules/window/window");

VAR win = window.create({ title: "demo" });

WHILE win:isOpen() {
    win:pollEvents();
    /* ... draw ... */
    win:swap();
}
```

For an event-driven loop with no polling overhead:

```cdo
WHILE win:isOpen() {
    win:waitEvents();
    redraw_dirty_regions();
    win:swap();
}
```

## Examples

### Minimal interactive window

```cdo
VAR window = include("./modules/window/window");

VAR win = window.create({ title: "hello", width: 640, height: 480 });

win:onKey(FUNCTION(key, action) {
    IF key == "escape" AND action == "press" { win:close(); }
});

WHILE win:isOpen() {
    win:pollEvents();
    win:swap();
}
```

### Combined with `draw`

```cdo
VAR window = include("./modules/window/window");
VAR draw   = include("./modules/draw/draw");

VAR win = window.create({ title: "boxes" });

WHILE win:isOpen() {
    win:pollEvents();
    draw.clear(0.1, 0.1, 0.1, 1);
    draw.setColor(1, 0, 0, 1);
    draw.rectangle(50, 50, 200, 100);
    win:swap();
}
```

## Notes

- The module integrates with the runtime's `app.holds()` counter so
  embedders can detect when the script is "still doing something" vs
  waiting on a closed event loop.
- `window.create()` may only be called from the main thread on macOS
  due to AppKit constraints.  Spawn worker threads for everything that
  isn't event handling.
- The OpenGL context must be **current** to draw.  By default the
  context becomes current on `create()` and stays current.
