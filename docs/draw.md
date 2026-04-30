# Drawing

The `draw` binary module renders 2-D primitives, images, and text
into the GL context owned by the [`window`](window.md) module's
current frame.  The API is shaped after LÖVE2D's `love.graphics`:
stateful current colour and font, mode-string shapes, and short verb
names.

This page is a companion to [`modules/draw/README.md`](../modules/draw/README.md).

## Mental model

* **Inside a `w.draw` callback only.**  Every `draw.*` primitive
  operates on whichever GL context the window module's manager
  thread has made current for the active window.  Calling outside a
  `draw` (or `update`) callback is undefined -- there is simply no
  current GL context.
* **LÖVE-style coordinates.**  The window module sets up an
  orthographic projection in pixels with origin top-left and Y axis
  pointing down before each `w.draw` callback.  So
  `draw.rectangle("fill", 0, 0, 100, 100)` paints the top-left
  100 × 100 pixels regardless of window size.
* **State is global per process, not per window.**  `draw.setColor`,
  `draw.setFont`, and `draw.setLineWidth` set a single live value
  inherited by whichever window is currently rendering.  The
  transform stack is the GL modelview matrix, reset to identity
  before each callback.

## Hello, world

```cando
VAR window = include("./modules/window/window.so");
VAR draw   = include("./modules/draw/draw.so");

VAR w = window.create("Hello", 400, 300);
w.draw = FUNCTION(self) {
    draw.clear(0, 0, 0);
    draw.setColor(1, 1, 1);
    draw.rectangle("fill", 50, 50, 100, 100);
};
```

## State

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.clear(r, g, b[, a])`           | Clear the framebuffer.                  |
| `draw.setColor(r, g, b[, a])`        | Floats `0..1`, used by all subsequent draws. |
| `draw.getColor()`                    | Multi-return current colour.             |
| `draw.setLineWidth(px)` / `getLineWidth()` | Thickness for outline modes.       |
| `draw.setScissor(x, y, w, h)` / `setScissor()` | Set / disable clipping rect.   |
| `draw.setFont(font)`                 | Current font for `draw.print`.           |

## Transform stack

Mirrors `glPushMatrix` / `glPopMatrix` etc., named LÖVE-style:

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.push()` / `draw.pop()`         | Save / restore transform.                |
| `draw.translate(x, y)`               | Translate.                               |
| `draw.scale(sx[, sy])`               | Scale (uniform if `sy` omitted).         |
| `draw.rotate(rad)`                   | Rotate around origin (radians).          |
| `draw.origin()`                      | Reset to identity.                       |

The stack is *the GL modelview stack*, so it interacts with image
and text drawing automatically: `draw.translate / draw.draw(image)`
moves the image; `draw.translate / draw.print(...)` moves the text.

## Primitives

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.rectangle(mode, x, y, w, h)`   | `mode` is `"fill"` or `"line"`.          |
| `draw.circle(mode, cx, cy, r [, segments])` | Same modes.                       |
| `draw.line(x1, y1, x2, y2, ...)`     | Variadic polyline.                       |

## Images

```cando
VAR logo = draw.newImage("assets/logo.png");
print("size:", logo:getDimensions());

w.draw = FUNCTION(self) {
    draw.draw(logo, 100, 50);            // top-left at (100, 50)
    draw.draw(logo, 200, 50, 0, 2);      // 2x scale
    draw.draw(logo, 200, 50, 0.5);       // rotated 0.5 rad
};
```

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.newImage(path)`                | Load PNG / JPEG / BMP / TGA via stb_image. |
| `draw.draw(image, x, y[, r, sx, sy, ox, oy])` | LÖVE-exact signature.           |

`_meta.draw_image` provides:

| Method                               | Description                              |
|--------------------------------------|------------------------------------------|
| `img:getWidth()` / `img:getHeight()` / `img:getDimensions()` | Pixels.       |
| `img:setFilter("nearest" \| "linear")` | Texture min/mag filter.               |
| `img:release()`                      | Free CPU pixels + GL texture.            |

GL upload is deferred to the first `draw.draw(img, ...)` call so
`newImage` can run on any thread without a current context.

## Fonts

```cando
VAR font = draw.newFont("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 16);
draw.setFont(font);

w.draw = FUNCTION(self) {
    draw.setColor(1, 1, 1);
    draw.print("Hello, world", 20, 20);
};
```

| Field                                | Description                              |
|--------------------------------------|------------------------------------------|
| `draw.newFont(path, pixelHeight)`    | Load TTF; bake printable Latin-1 atlas. |
| `draw.setFont(font)`                 | Set the font used by `draw.print`.       |
| `draw.print(text, x, y[, r, sx, sy])` | Draw `text` at `(x, y)` in current colour. |

`_meta.draw_font` provides:

| Method                               | Description                              |
|--------------------------------------|------------------------------------------|
| `font:getWidth(text)`                | Pixel width at the font's size.          |
| `font:getHeight()`                   | Line height in pixels.                   |
| `font:release()`                     | Free atlas + GL texture.                 |

The atlas covers Latin-1 (codepoints 32..255) only.  Wider Unicode
coverage and printf-style wrapped text are out of scope for this
first cut; in the meantime you can compose multi-line layouts
manually with `font:getWidth` and `draw.translate`.

## A complete LÖVE-style example

```cando
VAR window = include("./modules/window/window.so");
VAR draw   = include("./modules/draw/draw.so");

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

## Caveats

* **Legacy GL.**  The module uses the GL 1.x fixed-function pipeline
  (`glBegin`, `glColor4f`, `glVertex2f`, the modelview / projection
  matrix stacks).  This is what GLFW gives you by default and what
  works in xvfb in CI.  A modern-GL backend can be a drop-in
  replacement later.
* **Single thread.**  Like the window module, every `draw.*` call
  runs on the manager thread.  Heavy work in `draw` blocks
  rendering for all open windows.
* **Latin-1 only** for the font atlas (see above).
