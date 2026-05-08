/*
 * src/controls/ctl_trackbar.h -- TrackBar native methods.
 *
 * setTickFrequency / setSmallStep / setLargeStep operate on
 * msctls_trackbar32 via TBM_* messages.  Phase 1.1 attached them only
 * to the forms_trackbar meta table.
 */

#ifndef CANDO_FORMS_CONTROLS_TRACKBAR_H
#define CANDO_FORMS_CONTROLS_TRACKBAR_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_set_tick_frequency(CandoVM *vm, int argc, CandoValue *args);
int native_set_small_step    (CandoVM *vm, int argc, CandoValue *args);
int native_set_large_step    (CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_TRACKBAR_H */
