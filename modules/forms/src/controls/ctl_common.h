/*
 * src/controls/ctl_common.h -- shared helpers used by every per-control
 * native TU.
 *
 * Phase 1.2a of the rewrite (REWRITE_PLAN.md): pulls the
 * libcando-bound identity / error / arg-parsing helpers out of
 * forms_module.c so future src/controls/ctl_<kind>.c files can call
 * them without the giant TU pulling them in.
 *
 * Everything in this header is libcando-bound, so the entire content
 * is gated on FORMS_MODULE_TEST_BUILD: the test harness builds the
 * pure-C core only and never touches these prototypes.
 */

#ifndef CANDO_FORMS_CONTROLS_COMMON_H
#define CANDO_FORMS_CONTROLS_COMMON_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"
#include "../core/slots.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a script-side instance value into its FormsSlot, or NULL on
 * a corrupted / forged / destroyed handle.  Cross-checks slot index,
 * generation, and recorded ControlKind (the latter stamped as
 * FIELD_STATIC by build_instance, see Phase 0.5). */
FormsSlot *slot_from_inst(CandoVM *vm, CandoValue v);

/* slot_teardown stays in forms_module.c for now -- it's Win32-bound
 * (HWND destroy, brush/font/icon cleanup, manager-thread command
 * round-trip) so it moves with the backend split in Phase 1+. */

/* printf-style throw -- mirrors modules/window's window_throw. */
void forms_throw(CandoVM *vm, const char *fmt, ...);

/* Errors with "forms is only supported on Windows" when invoked on
 * the stub build.  Returns 1 on Windows, 0 on the stub (caller should
 * `return -1` after this returns 0). */
int require_supported(CandoVM *vm, const char *who);

/* Pull the receiver (slot) from args[0] or throw.  Returns NULL on
 * failure (caller should `return -1`). */
FormsSlot *arg_self(CandoVM *vm, int argc, CandoValue *args,
                    const char *who);

/* Copy text from a CandoValue (string -> caller-owned char buf).
 * Truncates if longer than outcap-1.  Writes empty string for
 * non-string values. */
void parse_text_arg(CandoVM *vm, CandoValue v, char *out, int outcap);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_COMMON_H */
