/*
 * src/controls/ctl_progress.h -- ProgressBar native methods.
 *
 * setStep / stepIt / setMarquee / setState all operate on
 * msctls_progress32 via PBM_* messages.  Phase 1.1 attached them only
 * to the forms_progress meta table, so calling them on anything else
 * is a hard VM error before the native is even reached.
 */

#ifndef CANDO_FORMS_CONTROLS_PROGRESS_H
#define CANDO_FORMS_CONTROLS_PROGRESS_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_set_step    (CandoVM *vm, int argc, CandoValue *args);
int native_step_it     (CandoVM *vm, int argc, CandoValue *args);
int native_set_marquee (CandoVM *vm, int argc, CandoValue *args);
int native_set_state   (CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_PROGRESS_H */
