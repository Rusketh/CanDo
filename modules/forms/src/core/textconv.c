/*
 * src/core/textconv.c -- UTF-8 <-> wide-char conversion.  See
 * textconv.h for the contract.
 *
 * Win32 implementation uses MultiByteToWideChar / WideCharToMultiByte
 * with CP_UTF8.  Non-Windows builds get no-op stubs so any code path
 * that happens to call them on Linux/macOS still links cleanly.
 */

#include "textconv.h"

#include <stdlib.h>
#include <string.h>

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)

wchar_t *utf8_to_wide(const char *s, int n)
{
    if (!s) {
        wchar_t *w = (wchar_t *)calloc(1, sizeof(wchar_t));
        return w;
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t *out = (wchar_t *)calloc((size_t)wn + 1, sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, n, out, wn);
    out[wn] = 0;
    return out;
}

char *wide_to_utf8(const wchar_t *w)
{
    if (!w) {
        char *o = (char *)calloc(1, 1);
        return o;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *out = (char *)calloc((size_t)n + 1, 1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    return out;
}

void utf8_into_wide_buf(const char *src, int srclen,
                        wchar_t *dst, int dstcap)
{
    if (dstcap <= 0) return;
    if (!src || srclen == 0) { dst[0] = 0; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, src, srclen, dst, dstcap - 1);
    if (n < 0) n = 0;
    dst[n] = 0;
}

#else  /* !Windows -- no-op stubs so the .so build still links. */

wchar_t *utf8_to_wide(const char *s, int n) { (void)s; (void)n; return NULL; }
char    *wide_to_utf8(const wchar_t *w)     { (void)w; return NULL; }
void     utf8_into_wide_buf(const char *src, int srclen,
                            wchar_t *dst, int dstcap)
{
    (void)src; (void)srclen; (void)dst; (void)dstcap;
}

#endif
