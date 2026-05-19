/*
 * lib/console.h -- Console / terminal standard library for CanDo.
 *
 * Registers a global `console` object with primitives for:
 *
 *   - Output:     write / print / moveCursor / colours / clear / scroll
 *   - Mode:       rawMode / echo / cursorVisible / alternateScreen /
 *                 enableMouse / size / isatty / title
 *   - Input:      readKey / readKeyTimeout / pollKey / readMouse /
 *                 readLine / flushInput
 *   - Async:      start / stop / wait / running + onKey / onMouse /
 *                 onResize / onLine / onError handlers
 *   - Lifecycle:  enable / disable / enabled / exists /
 *                 attach / detach / hide / show
 *
 * Cross-platform: termios + ANSI escapes on POSIX, Win32 console API on
 * Windows.  Both ends translate to the same key / mouse event shapes.
 *
 * Embedder control: every console.* native respects vm->console_enabled
 * (see cando.h / vm.h: cando_console_set_enabled / is_enabled /
 * cando_console_detach).  Hosts that don't want script code touching
 * their stdio can flip the flag at open time.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_CONSOLE_H
#define CANDO_LIB_CONSOLE_H

#include "../vm/vm.h"

CANDO_API void cando_lib_console_register(CandoVM *vm);

#endif /* CANDO_LIB_CONSOLE_H */
