# `draw` module

2D drawing primitives layered on the [`window`](../window/README.md)
module.  Provides shapes, transforms, sprites/images, fonts, and text
rendering against the current OpenGL context.

## Loading

```cdo
VAR draw   = include("./modules/draw/draw");
VAR window = include("./modules/window/window");
```

The `draw` module assumes there's a current GL context — `window.create`
makes one for you.

## State

Drawing state is global within the module: setting the colour, line
width, or scissor rect affects every subsequent draw call until you
change it again.

| Function                   | Description |
|----------------------------|-------------|
| `draw.clear(r, g, b, a*)`  | Clear the framebuffer to the given colour. |
| `draw.setColor(r, g, b, a*)` | Set the current draw colour (components in `[0,1]`). |
| `draw.getColor() → array`  | Current `[r, g, b, a]`. |
| `draw.setLineWidth(px)`    | Stroke width for line/outline operations. |
| `draw.getLineWidth() → number` | |
| `draw.setScissor(x, y, w, h*)` | Restrict drawing to a rect.  Pass no arguments to disable. |

## Primitives

| Function                            | Description |
|-------------------------------------|-------------|
| `draw.rectangle(x, y, w, h, mode*)` | `mode` is `"fill"` (default) or `"line"`. |
| `draw.circle(x, y, r, mode*)`       | Filled or outlined circle. |
| `draw.line(x1, y1, x2, y2)`         | Stroke a single line segment. |

## Transform stack

Transforms compose; `push` saves the current transform, `pop` restores
it.

| Function                | Description |
|-------------------------|-------------|
| `draw.push()`           | Save current transform. |
| `draw.pop()`            | Restore. |
| `draw.translate(x, y)`  | Move origin. |
| `draw.scale(sx, sy*)`   | Scale (uniform if `sy` omitted). |
| `draw.rotate(rad)`      | Rotate around the current origin. |
| `draw.origin()`         | Reset to the identity transform. |

```cdo
draw.push();
draw.translate(100, 100);
draw.rotate(math.pi / 4);
draw.rectangle(-25, -25, 50, 50);     // rotated square at (100,100)
draw.pop();
```

## Images and sprites

| Function                              | Description |
|---------------------------------------|-------------|
| `draw.newImage(path) → image`         | Load a PNG / JPEG into a GPU texture. |
| `draw.draw(image, x, y, opts*)`       | Blit the image at `(x, y)`.  `opts = { rotation, scaleX, scaleY }`. |

`image:width()` / `image:height()` give the source dimensions.

## Fonts and text

| Function                                | Description |
|-----------------------------------------|-------------|
| `draw.newFont(path, size) → font`       | Load a TTF / OTF at a pixel size. |
| `draw.setFont(font)`                    | Set the current font for `draw.print`. |
| `draw.print(text, x, y)`                | Render at the current colour. |

## Examples

### Bouncing box

```cdo
VAR window = include("./modules/window/window");
VAR draw   = include("./modules/draw/draw");

VAR win = window.create({ title: "bounce", width: 640, height: 480 });
VAR x = 0; VAR vx = 200;

VAR last = os.clock();
WHILE win:isOpen() {
    VAR now = os.clock();
    VAR dt  = now - last;
    last = now;

    win:pollEvents();
    x = x + vx * dt;
    IF x < 0 OR x > 600 { vx = -vx; }

    draw.clear(0.1, 0.1, 0.1, 1);
    draw.setColor(1, 0.5, 0, 1);
    draw.rectangle(x, 200, 40, 40);
    win:swap();
}
```

### Composite transform

```cdo
draw.push();
draw.translate(centerX, centerY);
FOR i IN 0 -> 11 {
    draw.push();
    draw.rotate((i / 12) * math.tau);
    draw.translate(0, 80);
    draw.circle(0, 0, 5);
    draw.pop();
}
draw.pop();
```

## Notes

- All coordinates are in window-points by default (the same unit
  `window:size()` returns).  For pixel-perfect HiDPI work, multiply by
  `window:contentScale()`.
- `draw.print` is suitable for HUDs and labels.  For complex text
  layout, render with the [`forms`](../forms/README.md) module's
  built-in widgets where possible.
