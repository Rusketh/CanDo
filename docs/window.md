# Windows

The `window` binary module opens native OS windows from CanDo scripts
and runs LÖVE2D-style callbacks (`update`, `draw`, `keypressed`,
`mousepressed`, …) on each one.  Pair it with the [`draw`](draw.md)
module to render anything you like inside.

This page is a companion to [`modules/window/README.md`](../modules/window/README.md).

## Mental model

Three things to keep in mind:

1. **Windows are first-class user-writable objects.**  `w.title = "..."`,
   `w.update = FUNCTION(self, dt) { ... }`, `w.draw = FUNCTION(self) { ... }`
   are all just plain field assignments.  Methods (`w:close()`,
   `w:setSize(...)`, `w:getMouse()`) come from the `_meta.window`
   prototype that every instance chains to via `__index`.
2. **Open windows pin the process.**  Each `window.create(...)` acquires
   a VM lifeline; the script can return immediately and the process
   stays alive until the user closes the window (or the script calls
   `w:close()`, or anyone calls `app.quit()`).  No `await` required.
3. **All GL / GLFW work runs on a single manager thread.**  GLFW is
   not thread-safe; its window and event functions must run on one
   stable thread per process, so the module owns that thread.  User
   callbacks fire on the same thread, dispatched through a single
   child VM created via `cando_vm_init_child` -- so anything visible
   from your script's main flow (globals, includes, classes) is also
   visible from inside `w.draw`.

## Hello, world

```cando
VAR window = include("./modules/window/window.so");

VAR w = window.create("Hello", 400, 300);
w.draw = FUNCTION(self) {
    // (with the draw module loaded, you'd run draw.* here)
};
w.keypressed = FUNCTION(self, key) {
    IF key == window.keys.escape { self:close(); }
};
// script ends; the process keeps running until the user closes the
// window.
```

That's the whole user-facing story.  The renderer, event pump, and
process-lifetime gate are all hidden inside the module.

## Constructors and constants

```cando
VAR w = window.create("Title", 800, 600);          // positional form
VAR w = window.create({                            // options form
    title    = "Title",
    width    = 800,
    height   = 600,
    resizable = TRUE,    // honoured by the next chunk
    decorated = TRUE,
    samples  = 4,
});
```

Constants:

| Field                         | Description                          |
|-------------------------------|--------------------------------------|
| `window.VERSION`              | This module's version string.        |
| `window.glfwVersion`          | Linked GLFW build (e.g. `"3.4.0 X11 GLX ..."`). |
| `window.keys.<name>`          | `escape`, `a`..`z`, `_0`..`_9`, `f1`..`f12`, `space`, `return`, `tab`, arrows, modifiers (`lshift`, `lctrl`, …), punctuation. |
| `window.mouse.left/middle/right` | Mouse-button constants.           |

## Instance API

Every instance chains `__index` to `_meta.window`:

| Method                            | Description                                      |
|-----------------------------------|--------------------------------------------------|
| `w:close()`                       | Tear the window down (releases its lifeline).    |
| `w:isOpen()`                      | `TRUE` until torn down.                          |
| `w:setTitle(s)` / `w:getTitle()`  | Title string.                                    |
| `w:setSize(W, H)` / `w:getSize()` | Client-area size (multi-return).                 |
| `w:setPosition(x, y)` / `w:getPosition()` | Window position.                         |
| `w:setVisible(b)` / `w:isVisible()` | Show / hide.                                   |
| `w:focus()` / `w:hasFocus()`      | Request / query input focus.                     |
| `w:setVSync(b)`                   | Vsync (off by default).                          |
| `w:getMouse()`                    | Pointer position relative to client area (multi-return). |
| `w:getDPIScale()`                 | Hi-DPI scale factor (multi-return sx, sy).       |
| `w:getFramebufferSize()`          | Hi-DPI pixel framebuffer (multi-return).         |

Live property fields stamped on each instance (also user-writable):

| Field            | Behaviour                                                |
|------------------|----------------------------------------------------------|
| `w.title`        | Plain string field (kept in sync by `setTitle`).         |
| `w.width`, `w.height` | Updated by `setSize` and the resize event.          |
| `w.userdata`     | Reserved for the user; the module never reads it.        |

## Callbacks

Assign functions as plain fields.  Names and signatures match
`love.*` exactly so a LÖVE2D user can copy code over verbatim.

```cando
w.update       = FUNCTION(self, dt) { ... };
w.draw         = FUNCTION(self) { ... };
w.keypressed   = FUNCTION(self, key, isrepeat) { ... };
w.keyreleased  = FUNCTION(self, key) { ... };
w.textinput    = FUNCTION(self, text) { ... };       // utf-8 string
w.mousepressed  = FUNCTION(self, x, y, button) { ... };
w.mousereleased = FUNCTION(self, x, y, button) { ... };
w.mousemoved    = FUNCTION(self, x, y) { ... };
w.wheelmoved    = FUNCTION(self, dx, dy) { ... };
w.resize       = FUNCTION(self, width, height) { ... };
w.focus        = FUNCTION(self, focused) { ... };
```

Default handlers can be installed once on the prototype:

```cando
_meta.window.keypressed = FUNCTION(self, key) {
    IF key == window.keys.f10 { app.quit(); }
};
```

`update` is called every frame before `draw`; both fire on the
manager thread inside a child VM that shares the script's globals.

## Threading and process lifetime

The module owns one **GLFW manager thread**.  All `glfwInit`,
`glfwCreateWindow`, `glfwDestroyWindow`, `glfwPollEvents`, and GL
state-setup calls happen there.  Every accessor (`setSize`, `getMouse`,
…) on the script side posts a typed command to the manager and waits
for the result.

User callbacks (`w.update`, `w.draw`, key/mouse handlers) fire on the
manager thread too, dispatched through a single child VM.  This means
slow or buggy callback code blocks rendering for **all** windows in
the same process; if you need parallel windows you can run additional
`thread { ... }` blocks alongside.

Each open window holds a VM lifeline.  The standard
`cando_vm_wait_all_lifelines` call inside `cando_close` blocks process
teardown until the count drops to zero, which is why a script can
just call `window.create(...)` and return.  See
[metamethods.md](metamethods.md) for `_meta.*` and the broader
prototype-chain machinery.

## Working with `app`

```cando
// Close every window from anywhere -- including a request handler
// or a Ctrl+C signal handler -- and let the process exit cleanly.
app.quit();

// Diagnose "why won't my script exit?":
print("holds:", app.holds());

// Set the exit code that cando returns:
app.exitCode(1);
```

When `app.quit()` is called the window manager loop notices on its
next iteration, tears every alive window down (which releases their
lifelines), and the script's `cando_vm_wait_all_lifelines` returns.

## Caveats

* **No close button on xvfb (CI).**  GLFW under xvfb without a window
  manager reports `glfwWindowShouldClose` true at startup, so the
  module currently does not auto-tear-down on that flag.  Real
  desktops still see the close-button event arrive via the `quit`
  callback path; that wiring lands in a follow-up.
* **Single Render thread.**  All `w.draw` callbacks share one thread.
  Heavy CPU work in `draw` will starve other windows; offload via
  `thread { }` blocks.
* **Latin-1 only for text input** in the current draw module's font
  rendering -- the window module passes the full UTF-8 codepoint to
  `w.textinput`; only the draw module's atlas is limited.
