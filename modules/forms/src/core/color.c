/*
 * src/core/color.c -- implementation of the pure colour helpers
 * declared in color.h.  See that header for the rationale.
 *
 * No Win32, no libcando, no global state other than the read-only
 * named-colour table.  Free-standing.
 */

#include "color.h"

#include <stddef.h>
#include <string.h>

/* Master named-colour table.  Mirrors the CSS basic + extended palette
 * with a few WinForms-friendly extras (cornflowerblue, steelblue, etc.)
 * and a handful of system-ish defaults.  Keys are all lowercase ASCII;
 * lookup folds the input. */
const NamedColor g_named_colors[] = {
    { "black",            0x000000 },
    { "white",            0xFFFFFF },
    { "red",              0xFF0000 },
    { "green",            0x008000 },   /* CSS-green, not lime */
    { "blue",             0x0000FF },
    { "yellow",           0xFFFF00 },
    { "cyan",             0x00FFFF },
    { "magenta",          0xFF00FF },
    { "orange",           0xFFA500 },
    { "purple",           0x800080 },
    { "gray",             0x808080 },
    { "grey",             0x808080 },   /* spelling alias */
    { "lightgray",        0xD3D3D3 },
    { "lightgrey",        0xD3D3D3 },
    { "darkgray",         0xA9A9A9 },
    { "darkgrey",         0xA9A9A9 },
    { "silver",           0xC0C0C0 },
    { "navy",             0x000080 },
    { "teal",             0x008080 },
    { "olive",            0x808000 },
    { "maroon",           0x800000 },
    { "lime",             0x00FF00 },
    { "aqua",             0x00FFFF },
    { "fuchsia",          0xFF00FF },
    { "pink",             0xFFC0CB },
    { "brown",            0xA52A2A },
    { "gold",             0xFFD700 },
    { "salmon",           0xFA8072 },
    { "coral",            0xFF7F50 },
    { "tomato",           0xFF6347 },
    { "indigo",           0x4B0082 },
    { "violet",           0xEE82EE },
    { "khaki",            0xF0E68C },
    { "beige",            0xF5F5DC },
    { "ivory",            0xFFFFF0 },
    { "snow",             0xFFFAFA },
    { "azure",            0xF0FFFF },
    { "mint",             0xF5FFFA },
    { "buttonface",       0xF0F0F0 },
    { "windowbg",         0xFFFFFF },
    { "controltext",      0x000000 },
    { "cornflowerblue",   0x6495ED },
    { "dodgerblue",       0x1E90FF },
    { "steelblue",        0x4682B4 },
    { "skyblue",          0x87CEEB },
    { "darkred",          0x8B0000 },
    { "darkgreen",        0x006400 },
    { "darkblue",         0x00008B },
    { "lightyellow",      0xFFFFE0 },
    { "lightblue",        0xADD8E6 },
    { "lightgreen",       0x90EE90 },
    { "transparent",      0x000000 },   /* synthetic; consumers gate via has_back */
    { NULL, 0 }
};

int ci_strneq(const char *a, const char *b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + 32);
        if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + 32);
        if (ac != bc) return 0;
    }
    return 1;
}

int lookup_named_color(const char *name, unsigned int n, unsigned int *rgb_out)
{
    if (!name || n == 0) return 0;
    for (const NamedColor *p = g_named_colors; p->name; p++) {
        unsigned int keylen = (unsigned int)strlen(p->name);
        if (keylen != n) continue;
        if (ci_strneq(name, p->name, n)) {
            if (rgb_out) *rgb_out = p->rgb;
            return 1;
        }
    }
    return 0;
}

int parse_hex_color(const char *s, unsigned int n, unsigned int *rgb_out)
{
    if (!s || n == 0) return 0;
    if (s[0] == '#') { s++; n--; }
    if (n != 3 && n != 6 && n != 8) return 0;

    unsigned int v = 0;
    for (unsigned int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned int d;
        if      (c >= '0' && c <= '9') d = (unsigned)c - '0';
        else if (c >= 'a' && c <= 'f') d = (unsigned)c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = (unsigned)c - 'A' + 10;
        else return 0;
        v = (v << 4) | d;
    }

    if (n == 3) {
        /* Expand "#abc" -> 0xAABBCC. */
        unsigned r = (v >> 8) & 0xF;
        unsigned g = (v >> 4) & 0xF;
        unsigned b = (v     ) & 0xF;
        v = ((r * 0x11) << 16) | ((g * 0x11) << 8) | (b * 0x11);
    } else if (n == 8) {
        v &= 0x00FFFFFFu;          /* drop alpha */
    }

    if (rgb_out) *rgb_out = v & 0xFFFFFFu;
    return 1;
}

unsigned int rgb_to_colorref(unsigned int rgb)
{
    unsigned char r = (rgb >> 16) & 0xFF;
    unsigned char g = (rgb >>  8) & 0xFF;
    unsigned char b = (rgb >>  0) & 0xFF;
    return (unsigned int)(((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r);
}

unsigned int colorref_to_rgb(unsigned int colorref)
{
    unsigned char b = (colorref >> 16) & 0xFF;
    unsigned char g = (colorref >>  8) & 0xFF;
    unsigned char r = (colorref >>  0) & 0xFF;
    return (unsigned int)(((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b);
}
