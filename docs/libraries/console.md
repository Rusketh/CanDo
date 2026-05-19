# `console`

Terminal control primitives — cursor positioning, colours, raw-mode
keyboard and mouse input, line input with editing, async event
dispatcher, plus lifecycle (hide / show / attach / detach) so scripts
running as services or GUIs can cleanly relinquish the console.

POSIX backend: termios + ANSI escape sequences.  Windows backend:
Console API (`SetConsoleMode`, `ReadConsoleInput`) + `ENABLE_VIRTUAL_
TERMINAL_PROCESSING` so the same ANSI escapes work on modern Windows
Terminal / Windows 10+.

## Quick example

```cdo
console.clear();
console.moveCursor(10, 5);
console.write("Hello", { fg: "red", bold: TRUE });
console.print(", world!", { fg: "cyan" });

VAR size = console.size();
print(`terminal is ${size.cols} x ${size.rows}`);
```

## Output

| Function | Description |
|---|---|
| `console.write(text, opts?)` | Emit `text` at the cursor.  `opts` may contain `fg`, `bg`, `bold`, `dim`, `italic`, `underline`, `blink`, `inverse`. |
| `console.print(text, opts?)` | Like `write` plus a trailing newline. |
| `console.moveCursor(col, row)` | 1-based absolute position. |
| `console.cursorUp(n?)` / `cursorDown(n?)` / `cursorLeft(n?)` / `cursorRight(n?)` | Relative motion. |
| `console.saveCursor()` / `restoreCursor()` | Single-deep cursor stack. |
| `console.clear()` | Clear screen + cursor → 1,1. |
| `console.clearLine()` | Clear current row. |
| `console.clearToEnd()` / `clearToStart()` | Clear from cursor. |
| `console.scrollUp(n?)` / `scrollDown(n?)` | Scroll the viewport. |
| `console.setScrollRegion(top, bottom)` / `resetScrollRegion()` | DECSTBM scroll region. |

## Colour spec

The `fg` / `bg` fields accept:

| Type | Example | Meaning |
|---|---|---|
| Named | `"red"`, `"bright-blue"`, `"default"` | Standard 16-colour ANSI palette. |
| Number | `42` | 256-colour palette index (0–255). |
| Array  | `[180, 90, 220]` | 24-bit truecolor `[r, g, b]`. |

```cdo
console.write("ok\n", { fg: "green", bold: TRUE });
console.write("bytes\n", { fg: [180, 90, 220], bg: 233 });
```

## Mode control

| Function | Description |
|---|---|
| `console.rawMode(bool)`        | Enter / leave raw mode.  Required before `readKey`. |
| `console.echo(bool)`           | Toggle echo of typed characters. |
| `console.cursorVisible(bool)`  | Hide / show the cursor. |
| `console.alternateScreen(bool)`| Switch in / out of the alt screen buffer. |
| `console.enableMouse(bool)`    | Enable SGR mouse tracking (DEC 1006). |
| `console.isatty()`             | TRUE if stdout is a terminal. |
| `console.size()`               | `{ cols, rows }` of the current window. |
| `console.title(s)`             | Set the window / tab title. |

## Input

```cdo
console.rawMode(TRUE);
VAR k = console.readKey();
console.rawMode(FALSE);
print(k.key);    // e.g. "Enter", "ArrowUp", "a"
```

| Function | Description |
|---|---|
| `console.readKey()`           | Block until a key event. |
| `console.readKeyTimeout(ms)`  | Block up to `ms`; returns `NULL` on timeout. |
| `console.pollKey()`           | Non-blocking; returns `NULL` if nothing buffered. |
| `console.readLine(prompt?)`   | Cooked-mode line read with Backspace / arrows / Home / End. |
| `console.flushInput()`        | Discard any pending input. |

Key event shape:

```
{ key: "a" | "Enter" | "ArrowUp" | "F5" | ...,
  raw: "\x1b[A",
  ctrl: bool, alt: bool, shift: bool, meta: bool }
```

Symbolic names: `Enter`, `Escape`, `Tab`, `Backspace`, `Delete`,
`Insert`, `Home`, `End`, `PageUp`, `PageDown`, `ArrowUp`, `ArrowDown`,
`ArrowLeft`, `ArrowRight`, `F1`…`F12`, `Space`.

## Mouse

```cdo
console.rawMode(TRUE);
console.enableMouse(TRUE);
console.onMouse = FUNCTION(m) {
    console.moveCursor(m.x, m.y);
    IF m.action == "press" { console.write("X"); }
};
console.start();
console.wait();
```

Mouse event shape:

```
{ x: 12, y: 3,
  button: "left" | "middle" | "right" | "wheel-up" | "wheel-down",
  action: "press" | "release" | "move" | "drag",
  ctrl: bool, alt: bool, shift: bool }
```

## Async dispatcher

Set one or more handlers, then call `start()`.  A worker thread reads
input and dispatches events on a child VM, so the main thread can
sit in `console.wait()` until `console.stop()` is called.

| Function | Description |
|---|---|
| `console.start()`    | Spawn the dispatcher thread. |
| `console.stop()`     | Signal it to exit.  Idempotent. |
| `console.wait()`     | Join the dispatcher.  Blocks. |
| `console.running()`  | TRUE while the dispatcher is alive. |

Handler fields (assigned to like regular properties):

| Field         | Signature             | Fires |
|---|---|---|
| `console.onKey`    | `FUNCTION(key)`    | Per key event. |
| `console.onMouse`  | `FUNCTION(event)`  | Per mouse event when mouse mode is on. |
| `console.onResize` | `FUNCTION(size)`   | Terminal resize. |
| `console.onError`  | `FUNCTION(err)`    | Anything thrown inside a callback. |

Scripts that prefer threads can ignore the dispatcher entirely and
just block in their own worker:

```cdo
thread {
    WHILE TRUE {
        VAR k = console.readKey();
        IF k.key == "q" { BREAK; }
        print(k.key);
    }
};
```

## Lifecycle (hide / show / attach / detach)

Useful for windowed apps (Forms / `window` module) and Windows
services that should drop the console at startup or runtime:

| Function | Description |
|---|---|
| `console.enable()` / `console.disable()` | Flip the script-level enabled flag.  Disabled state makes all `console.*` calls throw `"console is disabled"`. |
| `console.enabled()`           | Current enabled state. |
| `console.exists()`            | TRUE if a real console is attached.  Windows: `GetConsoleWindow() != NULL`.  POSIX: `isatty(stdout)`. |
| `console.attach()`            | Windows: `AllocConsole` + reopen stdio.  POSIX: open `/dev/tty` and dup over fd 0/1/2.  Returns TRUE on success. |
| `console.detach()`            | Windows: `FreeConsole`.  POSIX: redirect stdio onto `/dev/null`.  Also calls `disable()` on the calling VM. |
| `console.hide()` / `console.show()` | Windows-only: `ShowWindow(GetConsoleWindow(), SW_HIDE / SW_SHOW)`.  No-op on POSIX. |

## CLI flag — `--no-console`

```bash
cando --no-console my-service.cdo
```

Equivalent to calling `console.detach()` at startup.  On Windows it
calls `FreeConsole()` so a CanDo script launched from a GUI shortcut
won't flash a console window; on POSIX it redirects stdio to
`/dev/null`.  After this, every `console.*` call inside the script
throws `"console is disabled"`.

## Embedder API

Host applications linking `libcando` can flip the enabled flag from C
so embedded scripts don't touch the host's stdio:

```c
#include <cando.h>

CandoVM *vm = cando_open();
cando_openlibs(vm);
cando_console_set_enabled(vm, false);
cando_dofile(vm, "user-script.cdo");
cando_close(vm);
```

Public symbols:

- `void cando_console_set_enabled(CandoVM *, bool)`
- `bool cando_console_is_enabled(const CandoVM *)`
- `void cando_console_detach(void)` — process-wide.

## Errors

Every method throws when the library is disabled (via embedder API,
`--no-console`, or script-level `console.disable()`).  Wrap in
`TRY` / `CATCH` to handle gracefully:

```cdo
TRY {
    console.write("status");
} CATCH (e) {
    log.append("status update skipped: " + e);
}
```
