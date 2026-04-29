/*
 * lib/app.h -- Application lifecycle global for Cando.
 *
 * Registers a global `app` object with these methods:
 *
 *   app.quit([code])      → null   request graceful shutdown
 *   app.exit([code])      → never  hard _exit() after running quit hooks
 *   app.isQuitting()      → bool   has app.quit() been called?
 *   app.holds()           → number current lifeline count
 *   app.exitCode([code])  → number get / set the process exit code
 *
 * The plan also calls for app.holdKinds() and app.onQuit(fn); both
 * require richer per-lifeline metadata (kind labels + per-acquire
 * quit hooks) that the v1 lifeline counter does not yet carry.
 * They land alongside the lifeline-registry rename in a follow-up.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_APP_H
#define CANDO_LIB_APP_H

#include "../vm/vm.h"

CANDO_API void cando_lib_app_register(CandoVM *vm);

#endif /* CANDO_LIB_APP_H */
