/*
 * src/controls/ctl_numeric.h -- NumericUpDown native methods.
 *
 * Today only setIncrement is kind-specific; setRange / setValue /
 * getValue come from the shared natives in forms_module.c (which the
 * forms_numeric meta table reuses).
 */

#ifndef CANDO_FORMS_CONTROLS_NUMERIC_H
#define CANDO_FORMS_CONTROLS_NUMERIC_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int native_set_increment(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_NUMERIC_H */
