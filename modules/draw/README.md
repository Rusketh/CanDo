# Draw Module

A binary extension that renders 2-D primitives, images, and text into
the GL context owned by the [`window`](../window/README.md) module's
current frame.  The API is shaped after LÖVE2D's `love.graphics`:
stateful current colour and font, mode-string shapes, and short verb
names.

The module **statically links the vendored stb_image / stb_truetype**
single-header libraries -- no extra runtime dependency.  It uses the
GL 1.x fixed-function pipeline that GLFW provides by default, so no
shader plumbing is required either.

## Building

```bash
make modules                                  # Linux / macOS host
make -C modules/draw                          # build only this module
make -C modules/draw test                     # run the C unit tests
./cando modules/draw/test_draw_smoke.cdo      # script-side load smoke
```

Linux runtime needs `libGL.so.1` (preinstalled on every mainstream
desktop distro).

Windows cross-compile from Linux:

```bash
make -C modules/draw draw.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

## Loading

```cando
VAR window = include("./window.so");
VAR draw   = include("./draw.so");

VAR w = window.create("Hello", 400, 300);
w.draw = FUNCTION(self) {
    draw.clear(0, 0, 0);
    draw.setColor(1, 1, 1);
    draw.rectangle("fill", 50, 50, 100, 100);
};
```

## When to call

Every `draw.*` primitive operates on the GL context that the window
module's manager thread has made current for the active window.  In
practice, that means **call `draw.*` only inside a window's `draw`
(or `update`) callback**.  Calling outside is undefined -- there is
no current GL context.

The window module sets up an LÖVE-style coordinate system before
each `w.draw` callback:

* origin at the **top-left** of the framebuffer
* units in **pixels**, Y axis pointing down
* alpha blending enabled (`glBlendFunc(GL_SRC_ALPHA,
  GL_ONE_MINUS_SRC_ALPHA)`)
* depth test off, framebuffer cleared to black

So `draw.rectangle("fill", 0, 0, 100, 100)` paints the top-left
100 × 100 pixels regardless of window size.

## Public API

### State

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.clear(r, g, b[, a])`           | Clear the framebuffer.                  |
| `draw.setColor(r, g, b[, a])`        | Set the colour for subsequent draws (floats `0..1`). |
| `draw.getColor()`                    | Multi-return current colour.             |
| `draw.setLineWidth(px)` / `getLineWidth()` | Line thickness.                    |
| `draw.setScissor(x, y, w, h)` / `setScissor()` | Set / disable scissor clipping. |
| `draw.setFont(font)`                 | Current font for `draw.print`.           |

### Transform stack

Mirrors GL's modelview stack, named LÖVE-style:

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.push()` / `draw.pop()`         | Save / restore transform.                |
| `draw.translate(x, y)`               | Translate.                               |
| `draw.scale(sx[, sy])`               | Scale (uniform if `sy` omitted).         |
| `draw.rotate(rad)`                   | Rotate around origin (radians).          |
| `draw.origin()`                      | Reset to identity.                       |

### Primitives

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.rectangle(mode, x, y, w, h)`   | `mode` is `"fill"` or `"line"`.          |
| `draw.circle(mode, cx, cy, r [, segments])` | Same modes.                       |
| `draw.line(x1, y1, x2, y2, ...)`     | Variadic polyline.                       |

### Images

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.newImage(path)`                | Load PNG/JPEG/BMP/TGA via stb_image.     |
| `draw.draw(image, x, y[, r, sx, sy, ox, oy])` | Blit (LOVE-exact signature).    |

`_meta.draw_image` provides per-instance methods:

| Method                               | Description                              |
|--------------------------------------|------------------------------------------|
| `img:getWidth()` / `img:getHeight()` / `img:getDimensions()` | Pixels.       |
| `img:setFilter("nearest" \| "linear")` | GL min/mag filter for the texture.     |
| `img:release()`                      | Free CPU pixels + GL texture.            |

### Fonts

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.newFont(path, pixelHeight)`    | Load TTF, bake printable Latin-1 atlas via stb_truetype. |
| `draw.print(text, x, y[, r, sx, sy])` | Render `text` at `(x, y)` in the current font + colour. |

`_meta.draw_font` provides:

| Method                               | Description                              |
|--------------------------------------|------------------------------------------|
| `font:getWidth(text)`                | Pixel width of `text` at the font's size. |
| `font:getHeight()`                   | Line height in pixels.                   |
| `font:release()`                     | Free atlas + GL texture.                 |

Wider Unicode coverage (CJK), printf-style wrapped text, and SDF /
distance-field rasterisation are out of scope for this first cut.

## Worked example: ball follows mouse

```cando
VAR window = include("./window.so");
VAR draw   = include("./draw.so");

VAR w = window.create("Follow", 600, 400);
w.x = 300; w.y = 200;

w.update = FUNCTION(self, dt) {
    VAR mx, my = self:getMouse();
    self.x = self.x + (mx - self.x) * 5 * dt;
    self.y = self.y + (my - self.y) * 5 * dt;
};

w.draw = FUNCTION(self) {
    draw.clear(0.1, 0.1, 0.15);
    draw.setColor(0.4, 0.8, 1);
    draw.circle("fill", self.x, self.y, 20);
};

w.keypressed = FUNCTION(self, key) {
    IF key == window.keys.escape { self:close(); }
};
```
