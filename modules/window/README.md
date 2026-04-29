# Window Module

A binary extension that opens OS windows from Cando scripts and runs
LÖVE2D-style callbacks (`update`, `draw`, `keypressed`, `mousepressed`,
…) on each open window.  Pair it with the [`draw`](../draw/README.md)
module to render anything you like inside.

The module **statically links the vendored GLFW 3.4** -- no runtime
dependency on `libglfw` is needed.  Linux end-user systems still need
the standard X11 + OpenGL libraries (`libX11.so.6`, `libGL.so.1`,
`libxkbcommon.so.0`) preinstalled on every mainstream desktop distro.
On Windows nothing extra is required.

It ships as `window.so` / `window.dll` in the same CI artefact bundle
as `cando` itself.

## Building

```bash
make modules                                    # Linux / macOS host
make -C modules/window                          # build only this module
make -C modules/window test                     # run the C unit tests
./cando modules/window/test_window_smoke.cdo   # script-side load smoke
```

Linux build dependencies (provided on every CI runner; install on a
fresh dev box with `apt`):

```bash
sudo apt-get install -y \
  libx11-dev libxcursor-dev libxrandr-dev libxinerama-dev libxi-dev \
  libgl1-mesa-dev libxkbcommon-dev libxext-dev
```

Windows cross-compile from Linux:

```bash
make -C modules/window window.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

## Loading

```cando
VAR window = include("./window.so");           # or "./window.dll" on Windows
print(window.glfwVersion);                     # e.g. "3.4.0 X11 GLX ..."
```

## Hello, world

The script does **not** need to call `await` or run an event loop.
Each open window holds a *lifeline* on the VM, so the process stays
alive until the user closes the window (or the script calls
`w:close()` explicitly, or anyone calls `app.quit()`).

```cando
VAR window = include("./window.so");

VAR w = window.create("Hello", 400, 300);
w.draw = FUNCTION(self) {
    # When the draw module is also loaded, this is where you put
    # `draw.clear(...)`, `draw.rectangle(...)`, etc.
};
w.keypressed = FUNCTION(self, key) {
    IF key == window.keys.escape { self:close(); }
};
# script ends here -- the window keeps running until closed.
```

## Public API

### `window` namespace

| Field                             | Description                                      |
|-----------------------------------|--------------------------------------------------|
| `window.create(title, w, h)`      | Open a window; positional args optional.         |
| `window.create({title=, width=, height=})` | Same, options-table form.               |
| `window.VERSION`                  | Module version string.                           |
| `window.glfwVersion`              | Linked GLFW build string.                        |
| `window.keys.<name>`              | Key constants: `escape`, `a`..`z`, `_0`..`_9`, `f1`..`f12`, `space`, `return`, `tab`, arrows, modifiers, punctuation. |
| `window.mouse.left/middle/right`  | Mouse button constants.                          |

### `_meta.window` instance methods

Every window instance chains `__index` to `_meta.window`, so all of
these are available as colon methods:

| Method                            | Description                                      |
|-----------------------------------|--------------------------------------------------|
| `w:close()`                       | Tear the window down.                            |
| `w:isOpen()`                      | `TRUE` until the slot has been recycled.         |
| `w:setTitle(str)` / `w:getTitle()` | Title.                                          |
| `w:setSize(width, height)` / `w:getSize()` | Client size; `getSize` multi-returns.   |
| `w:setPosition(x, y)` / `w:getPosition()` | Position.                                |
| `w:setVisible(bool)` / `w:isVisible()` | Show / hide.                                |
| `w:focus()` / `w:hasFocus()`      | Request / query input focus.                     |
| `w:setVSync(bool)`                | Enable / disable vsync (off by default).         |
| `w:getMouse()`                    | Mouse position relative to client area (multi-return). |
| `w:getDPIScale()`                 | Hi-DPI scale factor (multi-return sx, sy).       |
| `w:getFramebufferSize()`          | DPI-aware pixel framebuffer size (multi-return). |

### LÖVE-style callbacks

Assign as plain fields on each window instance.  Names and signatures
match `love.*` exactly:

```cando
w.update       = FUNCTION(self, dt) { ... };           // every frame
w.draw         = FUNCTION(self) { ... };               // every frame
w.keypressed   = FUNCTION(self, key, isrepeat) { ... };
w.keyreleased  = FUNCTION(self, key) { ... };
w.textinput    = FUNCTION(self, text) { ... };         // UTF-8 string
w.mousepressed  = FUNCTION(self, x, y, button) { ... };
w.mousereleased = FUNCTION(self, x, y, button) { ... };
w.mousemoved    = FUNCTION(self, x, y) { ... };
w.wheelmoved    = FUNCTION(self, dx, dy) { ... };
w.resize       = FUNCTION(self, width, height) { ... };
w.focus        = FUNCTION(self, focused) { ... };
```

Default handlers may be installed once on the prototype:

```cando
_meta.window.keypressed = FUNCTION(self, key) {
    IF key == window.keys.f10 { app.quit(); }
};
```

## Threading model

The module owns one **GLFW manager thread** (because GLFW window /
event functions must run on a single, stable thread on Linux + Windows)
that handles `glfwInit`, `glfwCreateWindow`, `glfwDestroyWindow`, and
`glfwPollEvents`.  All `_meta.window` accessors post commands to this
thread and wait for the result.

User callbacks (`w.update`, `w.draw`, `w.keypressed`, ...) fire on the
same manager thread, dispatched through a single child VM created via
`cando_vm_init_child` -- so anything visible from the script's main
flow (globals, includes, classes) is also visible from inside a
callback.

## Process lifetime

Each open window holds a VM lifeline (see
[`include/cando.h`](../../include/cando.h) `cando_vm_lifeline_*`).  The
existing `cando_vm_wait_all_lifelines` call inside `cando_close` blocks
process teardown until the count drops to zero, so a script that opens
a window can simply return -- the process keeps running until the user
closes the window or calls `app.quit()`.
