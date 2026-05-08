/*
 * src/core/color.h -- colour parsing + format conversion for the forms
 * module.
 *
 * All helpers here are pure: no Win32, no libcando, no global state
 * other than the read-only `g_named_colors[]` table.  They round-trip
 * between three representations:
 *
 *   - 0xRRGGBB         -- the form scripts see; what setForeColor /
 *                         setBackColor accept and getForeColor /
 *                         getBackColor return.
 *   - 0x00BBGGRR       -- Win32 COLORREF byte order.  Used internally
 *                         when talking to GDI.
 *   - "#rgb" / "#rrggbb" / "#aarrggbb" / "name"
 *                      -- the string forms accepted by setForeColor.
 *
 * The forms module's main TU includes this header to get the
 * prototypes; the C unit tests include the .c indirectly via
 * forms_module.c (test build) or link against color.o (release build).
 */

#ifndef CANDO_FORMS_CORE_COLOR_H
#define CANDO_FORMS_CORE_COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* CSS-style named-colour entry.  The table is keyed by lowercase name;
 * lookup folds the input to lowercase before comparing. */
typedef struct NamedColor {
    const char  *name;
    unsigned int rgb;       /* 0xRRGGBB                                */
} NamedColor;

/* The full colour table.  NULL-terminated (the last entry has
 * .name == NULL).  Iterators walk until they hit it. */
extern const NamedColor g_named_colors[];

/* Case-insensitive byte-range comparison.  ASCII fold only -- the
 * table keys are guaranteed lowercase ASCII so this is sufficient. */
int ci_strneq(const char *a, const char *b, unsigned int n);

/* Look up `name[0..n)` in the named-colour table.  On hit, writes the
 * 0xRRGGBB value to *rgb_out and returns 1.  Returns 0 on miss. */
int lookup_named_color(const char *name, unsigned int n,
                       unsigned int *rgb_out);

/* Parse "#RGB" / "#RRGGBB" / "#AARRGGBB" (and the same forms without
 * the leading '#').  Alpha is dropped for 8-digit input.  Returns 1
 * on success and writes the canonical 0xRRGGBB to *rgb_out, 0 on
 * malformed input. */
int parse_hex_color(const char *s, unsigned int n,
                    unsigned int *rgb_out);

/* 0xRRGGBB -> 0x00BBGGRR (Win32 COLORREF). */
unsigned int rgb_to_colorref(unsigned int rgb);

/* 0x00BBGGRR -> 0xRRGGBB. */
unsigned int colorref_to_rgb(unsigned int colorref);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_COLOR_H */
