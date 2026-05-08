/*
 * src/core/textconv.h -- Win32 UTF-8 <-> wide-char conversion helpers.
 *
 * Win32 controls take wchar_t* throughout (UNICODE build); CanDo
 * scripts use UTF-8.  These helpers mediate.  Pulled out of
 * forms_module.c so per-control TUs can call them without dragging
 * the rest of the module in.
 *
 * The entire surface is Win32-only.  On non-Windows builds the header
 * exposes prototypes that forward to no-op stubs in textconv.c, so
 * call sites don't need to repeat the same `#if FORMS_HAVE_WIN32`
 * guard around every usage.  In practice no Linux call site invokes
 * them; the stubs exist purely so the .so build links.
 */

#ifndef CANDO_FORMS_CORE_TEXTCONV_H
#define CANDO_FORMS_CORE_TEXTCONV_H

#include "sync.h"   /* pulls <windows.h> on Windows builds */
#include <wchar.h>  /* wchar_t on non-Windows builds */

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a UTF-8 string of length `n` (or NUL-terminated when
 * n == -1) to a freshly-allocated wchar_t* string.  Caller frees with
 * free().  Returns NULL on alloc failure (or a freshly allocated
 * 1-element zero-string for s == NULL). */
wchar_t *utf8_to_wide(const char *s, int n);

/* Convert a NUL-terminated wide string to a freshly-allocated UTF-8
 * char* string.  Caller frees with free(). */
char *wide_to_utf8(const wchar_t *w);

/* Copy a UTF-8 source into a fixed-size wide buffer (truncates).
 * Always NUL-terminates dst when dstcap > 0. */
void utf8_into_wide_buf(const char *src, int srclen,
                        wchar_t *dst, int dstcap);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_TEXTCONV_H */
