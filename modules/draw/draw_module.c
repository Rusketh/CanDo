/*
 * modules/draw/draw_module.c -- CanDo draw module entry point.
 *
 * Loaded into a script with:
 *
 *     VAR draw = include("./draw.so");          // Linux / macOS
 *     VAR draw = include("./draw.dll");         // Windows
 *
 * Operates on the GL context that the active window's render loop
 * has made current.  Call any draw.* primitive inside a window's
 * `draw` (or `update`) callback.  Calling outside that scope is
 * undefined -- there's simply no current GL context.
 *
 * This chunk wires up the primitives:
 *   draw.clear(r, g, b[, a])
 *   draw.setColor(r, g, b[, a]) / draw.getColor()
 *   draw.setLineWidth(px) / draw.getLineWidth()
 *   draw.setScissor(x, y, w, h) / draw.setScissor()
 *   draw.rectangle(mode, x, y, w, h)
 *   draw.circle(mode, cx, cy, r [, segments])
 *   draw.line(x1, y1, x2, y2, ...)
 *   draw.points(points)
 *   draw.polygon(mode, points)
 *   draw.push() / draw.pop()
 *   draw.translate(x, y) / draw.scale(sx[, sy]) / draw.rotate(rad) / draw.origin()
 *
 * Image / font primitives (newImage, draw.draw, print, ...) follow.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"
#include "lib/meta.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#include <GL/gl.h>

#define DRAW_MODULE_VERSION "0.1.0"

/* =========================================================================
 * Module state -- current colour / line width.  Shared by all windows
 * (LOVE-style: there's no per-window draw state, just the live GL
 * context).  The transform stack is GL's own modelview matrix stack;
 * scissor uses GL's scissor enable + glScissor.
 * ===================================================================== */

static float g_color_r = 1.0f;
static float g_color_g = 1.0f;
static float g_color_b = 1.0f;
static float g_color_a = 1.0f;
static float g_line_w  = 1.0f;

/* =========================================================================
 * Helpers
 * ===================================================================== */

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static double argd(CandoValue v, double dflt)
{
    return (v.tag == CDO_NUMBER) ? v.as.number : dflt;
}

/* Match a string CandoValue against a literal -- helper for
 * mode-string discrimination ("fill" vs "line" on rectangle()). */
static bool arg_streq(CandoValue v, const char *s)
{
    if (v.tag != CDO_STRING || !v.as.string) return false;
    u32 n = (u32)strlen(s);
    if (v.as.string->length != n) return false;
    return memcmp(v.as.string->data, s, n) == 0;
}

/* =========================================================================
 * draw.clear(r, g, b[, a])
 * ===================================================================== */

static int draw_clear(CandoVM *vm, int argc, CandoValue *args)
{
    (void)vm;
    float r = (float)(argc >= 1 ? argd(args[0], 0.0) : 0.0);
    float g = (float)(argc >= 2 ? argd(args[1], 0.0) : 0.0);
    float b = (float)(argc >= 3 ? argd(args[2], 0.0) : 0.0);
    float a = (float)(argc >= 4 ? argd(args[3], 1.0) : 1.0);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * draw.setColor(r, g, b[, a]) / draw.getColor()
 * ===================================================================== */

static int draw_set_color(CandoVM *vm, int argc, CandoValue *args)
{
    g_color_r = (float)(argc >= 1 ? argd(args[0], 1.0) : 1.0);
    g_color_g = (float)(argc >= 2 ? argd(args[1], 1.0) : 1.0);
    g_color_b = (float)(argc >= 3 ? argd(args[2], 1.0) : 1.0);
    g_color_a = (float)(argc >= 4 ? argd(args[3], 1.0) : 1.0);
    glColor4f(g_color_r, g_color_g, g_color_b, g_color_a);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_get_color(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)g_color_r));
    cando_vm_push(vm, cando_number((f64)g_color_g));
    cando_vm_push(vm, cando_number((f64)g_color_b));
    cando_vm_push(vm, cando_number((f64)g_color_a));
    return 4;
}

/* =========================================================================
 * draw.setLineWidth(px) / draw.getLineWidth()
 * ===================================================================== */

static int draw_set_line_width(CandoVM *vm, int argc, CandoValue *args)
{
    g_line_w = (float)(argc >= 1 ? argd(args[0], 1.0) : 1.0);
    if (g_line_w < 0.5f) g_line_w = 0.5f;
    glLineWidth(g_line_w);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_get_line_width(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)g_line_w));
    return 1;
}

/* =========================================================================
 * draw.setScissor(x, y, w, h) / draw.setScissor()
 *
 * GL's glScissor uses bottom-left coordinates, but our coordinate
 * system is top-left.  Convert by querying the current viewport
 * height and flipping y.
 * ===================================================================== */

static int draw_set_scissor(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc == 0) {
        glDisable(GL_SCISSOR_TEST);
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (argc < 4) {
        cando_vm_error(vm, "draw.setScissor: (x, y, w, h) required (or no args to disable)");
        return -1;
    }
    int x = (int)argd(args[0], 0.0);
    int y = (int)argd(args[1], 0.0);
    int w = (int)argd(args[2], 0.0);
    int h = (int)argd(args[3], 0.0);

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    int vp_h = viewport[3];
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, vp_h - (y + h), w, h);

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * draw.rectangle(mode, x, y, w, h)
 * ===================================================================== */

static int draw_rectangle(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 5) {
        cando_vm_error(vm, "draw.rectangle: (mode, x, y, w, h) required");
        return -1;
    }
    bool fill = arg_streq(args[0], "fill");
    bool line = arg_streq(args[0], "line");
    if (!fill && !line) {
        cando_vm_error(vm, "draw.rectangle: mode must be \"fill\" or \"line\"");
        return -1;
    }
    float x = (float)argd(args[1], 0.0);
    float y = (float)argd(args[2], 0.0);
    float w = (float)argd(args[3], 0.0);
    float h = (float)argd(args[4], 0.0);

    if (fill) {
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x,     y + h);
        glVertex2f(x + w, y + h);
        glEnd();
    } else {
        glBegin(GL_LINE_LOOP);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
        glEnd();
    }
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * draw.circle(mode, cx, cy, r [, segments])
 * ===================================================================== */

static int draw_circle(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4) {
        cando_vm_error(vm, "draw.circle: (mode, cx, cy, r) required");
        return -1;
    }
    bool fill = arg_streq(args[0], "fill");
    bool line = arg_streq(args[0], "line");
    if (!fill && !line) {
        cando_vm_error(vm, "draw.circle: mode must be \"fill\" or \"line\"");
        return -1;
    }
    float cx = (float)argd(args[1], 0.0);
    float cy = (float)argd(args[2], 0.0);
    float  r = (float)argd(args[3], 0.0);
    int seg  = (argc >= 5) ? (int)argd(args[4], 0.0) : 0;
    if (seg <= 0) {
        /* LOVE's heuristic: more segments for larger circles. */
        seg = (int)(10.0f + r * 0.5f);
        if (seg < 8)   seg = 8;
        if (seg > 256) seg = 256;
    }

    glBegin(fill ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
    if (fill) glVertex2f(cx, cy);
    for (int i = 0; i <= (fill ? seg : seg); i++) {
        float t = (float)i / (float)seg * (float)(2.0 * M_PI);
        glVertex2f(cx + cosf(t) * r, cy + sinf(t) * r);
    }
    glEnd();
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * draw.line(x1, y1, x2, y2, ...)
 * ===================================================================== */

static int draw_line(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4 || (argc & 1) != 0) {
        cando_vm_error(vm, "draw.line: needs at least 4 numeric args, in (x, y) pairs");
        return -1;
    }
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < argc; i += 2) {
        glVertex2f((float)argd(args[i], 0.0),
                   (float)argd(args[i + 1], 0.0));
    }
    glEnd();
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * draw.push() / draw.pop()
 * draw.translate(x, y) / draw.scale(sx[, sy]) / draw.rotate(rad) / draw.origin()
 * ===================================================================== */

static int draw_push(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    glPushMatrix();
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_pop(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    glPopMatrix();
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_translate(CandoVM *vm, int argc, CandoValue *args)
{
    float x = (float)(argc >= 1 ? argd(args[0], 0.0) : 0.0);
    float y = (float)(argc >= 2 ? argd(args[1], 0.0) : 0.0);
    glTranslatef(x, y, 0.0f);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_scale(CandoVM *vm, int argc, CandoValue *args)
{
    float sx = (float)(argc >= 1 ? argd(args[0], 1.0) : 1.0);
    float sy = (float)(argc >= 2 ? argd(args[1], (double)sx) : (double)sx);
    glScalef(sx, sy, 1.0f);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_rotate(CandoVM *vm, int argc, CandoValue *args)
{
    float rad = (float)(argc >= 1 ? argd(args[0], 0.0) : 0.0);
    glRotatef(rad * 180.0f / (float)M_PI, 0.0f, 0.0f, 1.0f);
    cando_vm_push(vm, cando_null());
    return 1;
}

static int draw_origin(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    glLoadIdentity();
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Module entry point.
 * ===================================================================== */

CandoValue cando_module_init(CandoVM *vm)
{
    cando_lib_meta_register(vm);
    (void)cando_lib_meta_table(vm, "draw_image");
    (void)cando_lib_meta_table(vm, "draw_font");

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   DRAW_MODULE_VERSION,
                   (u32)sizeof(DRAW_MODULE_VERSION) - 1);

    /* State */
    libutil_set_method(vm, obj, "clear",         draw_clear);
    libutil_set_method(vm, obj, "setColor",      draw_set_color);
    libutil_set_method(vm, obj, "getColor",      draw_get_color);
    libutil_set_method(vm, obj, "setLineWidth",  draw_set_line_width);
    libutil_set_method(vm, obj, "getLineWidth",  draw_get_line_width);
    libutil_set_method(vm, obj, "setScissor",    draw_set_scissor);

    /* Primitives */
    libutil_set_method(vm, obj, "rectangle",     draw_rectangle);
    libutil_set_method(vm, obj, "circle",        draw_circle);
    libutil_set_method(vm, obj, "line",          draw_line);

    /* Transform stack */
    libutil_set_method(vm, obj, "push",          draw_push);
    libutil_set_method(vm, obj, "pop",           draw_pop);
    libutil_set_method(vm, obj, "translate",     draw_translate);
    libutil_set_method(vm, obj, "scale",         draw_scale);
    libutil_set_method(vm, obj, "rotate",        draw_rotate);
    libutil_set_method(vm, obj, "origin",        draw_origin);

    return tbl;
}
