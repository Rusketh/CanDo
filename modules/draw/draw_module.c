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

/* stb_image -- vendored, header-only.  We instantiate the impl here. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_HACKS
#include "stb_image.h"

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#include <GL/gl.h>

#define DRAW_MODULE_VERSION "0.2.0"

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
 * Image objects (`draw.newImage` / `_meta.draw_image`).
 *
 * Pixel data is decoded by stb_image and uploaded to a GL texture
 * lazily on the first draw call (must run on the thread that owns
 * the GL context, i.e. inside w.draw).  The CanDo-side instance
 * carries `__draw_image_slot` + `__draw_image_gen` so methods can
 * find their backing struct.
 * ===================================================================== */

#define DRAW_MAX_IMAGES 256

typedef struct ImageSlot {
    int           alive;
    int           generation;
    unsigned char *pixels;       /* RGBA8 from stb_image, owned        */
    int           width;
    int           height;
    GLuint        texture;       /* 0 until uploaded                   */
    int           filter_linear; /* 0 = nearest, 1 = linear           */
    int           uploaded;
} ImageSlot;

static ImageSlot g_images[DRAW_MAX_IMAGES];

static int img_alloc(void)
{
    for (int i = 0; i < DRAW_MAX_IMAGES; i++) {
        if (!g_images[i].alive) {
            g_images[i].alive       = 1;
            g_images[i].generation++;
            g_images[i].pixels      = NULL;
            g_images[i].texture     = 0;
            g_images[i].uploaded    = 0;
            g_images[i].filter_linear = 1;
            return i;
        }
    }
    return -1;
}

static void img_release(ImageSlot *s)
{
    if (!s || !s->alive) return;
    if (s->pixels)  { stbi_image_free(s->pixels); s->pixels = NULL; }
    if (s->texture) { glDeleteTextures(1, &s->texture); s->texture = 0; }
    s->uploaded = 0;
    s->alive    = 0;
}

#define IMAGE_SLOT_KEY "__draw_image_slot"
#define IMAGE_GEN_KEY  "__draw_image_gen"

static ImageSlot *resolve_image(CandoVM *vm, CandoValue v, const char *fn)
{
    if (!cando_is_object(v)) {
        cando_vm_error(vm, "%s: expected image instance", fn);
        return NULL;
    }
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    f64 fslot = -1.0, fgen = -1.0;
    CdoString *kslot = cdo_string_intern(IMAGE_SLOT_KEY,
                                         (u32)strlen(IMAGE_SLOT_KEY));
    CdoString *kgen  = cdo_string_intern(IMAGE_GEN_KEY,
                                         (u32)strlen(IMAGE_GEN_KEY));
    CdoValue  vv;
    if (cdo_object_rawget(obj, kslot, &vv) && vv.tag == CDO_NUMBER)
        fslot = vv.as.number;
    if (cdo_object_rawget(obj, kgen, &vv) && vv.tag == CDO_NUMBER)
        fgen = vv.as.number;
    cdo_string_release(kslot);
    cdo_string_release(kgen);
    int idx = (int)fslot;
    int gen = (int)fgen;
    if (idx < 0 || idx >= DRAW_MAX_IMAGES) {
        cando_vm_error(vm, "%s: not an image instance", fn);
        return NULL;
    }
    ImageSlot *s = &g_images[idx];
    if (!s->alive || s->generation != gen) {
        cando_vm_error(vm, "%s: image has been released", fn);
        return NULL;
    }
    return s;
}

static void obj_set_number_local(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

/* draw.newImage(path) -- decode via stb_image and return an image
 * instance.  Decoding happens on the calling thread.  The GL upload
 * is deferred to the first draw.draw() so we don't need a current
 * context here. */
static int draw_new_image(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || args[0].tag != CDO_STRING || !args[0].as.string) {
        cando_vm_error(vm, "draw.newImage: (path) required");
        return -1;
    }
    /* Copy path to a NUL-terminated buffer. */
    u32 plen = args[0].as.string->length;
    if (plen >= 1024) plen = 1023;
    char path[1024];
    memcpy(path, args[0].as.string->data, plen);
    path[plen] = '\0';

    int w = 0, h = 0, n = 0;
    unsigned char *pixels = stbi_load(path, &w, &h, &n, 4);
    if (!pixels) {
        cando_vm_error(vm, "draw.newImage: %s: %s", path, stbi_failure_reason());
        return -1;
    }

    int idx = img_alloc();
    if (idx < 0) {
        stbi_image_free(pixels);
        cando_vm_error(vm, "draw.newImage: too many images (max %d)",
                       DRAW_MAX_IMAGES);
        return -1;
    }
    g_images[idx].pixels = pixels;
    g_images[idx].width  = w;
    g_images[idx].height = h;

    CandoValue inst = cando_bridge_new_object(vm);
    CdoObject *o    = cando_bridge_resolve(vm, inst.as.handle);
    obj_set_number_local(o, IMAGE_SLOT_KEY, (f64)idx);
    obj_set_number_local(o, IMAGE_GEN_KEY,  (f64)g_images[idx].generation);
    obj_set_number_local(o, "width",  (f64)w);
    obj_set_number_local(o, "height", (f64)h);
    cando_lib_meta_attach(vm, o, "draw_image");

    cando_vm_push(vm, inst);
    return 1;
}

/* Lazily upload the slot's pixels to a GL texture.  Must be called
 * from the thread that owns the GL context (inside w.draw). */
static void img_ensure_uploaded(ImageSlot *s)
{
    if (s->uploaded) return;
    glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    GLint filt = s->filter_linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->width, s->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    s->uploaded = 1;
}

/* draw.draw(image, x, y[, r, sx, sy, ox, oy])
 * Mirrors love.graphics.draw exactly: rotate then scale around (ox, oy)
 * on the source image, then place at (x, y).  Defaults: r=0, sx=1, sy=sx,
 * ox=0, oy=0.  Color modulation uses the current draw color. */
static int draw_draw(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        cando_vm_error(vm, "draw.draw: (image, x, y) required");
        return -1;
    }
    ImageSlot *s = resolve_image(vm, args[0], "draw.draw");
    if (!s) return -1;

    float x  = (float)argd(args[1], 0.0);
    float y  = (float)argd(args[2], 0.0);
    float r  = (float)(argc >= 4 ? argd(args[3], 0.0) : 0.0);
    float sx = (float)(argc >= 5 ? argd(args[4], 1.0) : 1.0);
    float sy = (float)(argc >= 6 ? argd(args[5], (double)sx) : (double)sx);
    float ox = (float)(argc >= 7 ? argd(args[6], 0.0) : 0.0);
    float oy = (float)(argc >= 8 ? argd(args[7], 0.0) : 0.0);

    img_ensure_uploaded(s);

    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    if (r != 0.0f) glRotatef(r * 180.0f / (float)M_PI, 0.0f, 0.0f, 1.0f);
    if (sx != 1.0f || sy != 1.0f) glScalef(sx, sy, 1.0f);
    if (ox != 0.0f || oy != 0.0f) glTranslatef(-ox, -oy, 0.0f);

    float w = (float)s->width;
    float h = (float)s->height;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glColor4f(g_color_r, g_color_g, g_color_b, g_color_a);
    glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(w,    0.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, h);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(w,    h);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    glPopMatrix();

    cando_vm_push(vm, cando_null());
    return 1;
}

/* image:getWidth() / image:getHeight() / image:getDimensions() */

static int image_get_width(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_error(vm, "image.getWidth: (image) required"); return -1; }
    ImageSlot *s = resolve_image(vm, args[0], "image.getWidth");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->width));
    return 1;
}

static int image_get_height(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_error(vm, "image.getHeight: (image) required"); return -1; }
    ImageSlot *s = resolve_image(vm, args[0], "image.getHeight");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->height));
    return 1;
}

static int image_get_dimensions(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_error(vm, "image.getDimensions: (image) required"); return -1; }
    ImageSlot *s = resolve_image(vm, args[0], "image.getDimensions");
    if (!s) return -1;
    cando_vm_push(vm, cando_number((f64)s->width));
    cando_vm_push(vm, cando_number((f64)s->height));
    return 2;
}

/* image:setFilter("nearest" | "linear") */
static int image_set_filter(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        cando_vm_error(vm, "image.setFilter: (image, mode) required");
        return -1;
    }
    ImageSlot *s = resolve_image(vm, args[0], "image.setFilter");
    if (!s) return -1;
    if (arg_streq(args[1], "linear"))      s->filter_linear = 1;
    else if (arg_streq(args[1], "nearest")) s->filter_linear = 0;
    else {
        cando_vm_error(vm, "image.setFilter: mode must be \"nearest\" or \"linear\"");
        return -1;
    }
    /* If already uploaded, update GL state (deferred until next draw
     * if no current context). */
    if (s->uploaded && s->texture) {
        glBindTexture(GL_TEXTURE_2D, s->texture);
        GLint filt = s->filter_linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* image:release() */
static int image_release(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_error(vm, "image.release: (image) required"); return -1; }
    if (!cando_is_object(args[0])) { cando_vm_push(vm, cando_bool(false)); return 1; }
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    f64 fslot = -1.0;
    CdoString *kslot = cdo_string_intern(IMAGE_SLOT_KEY,
                                         (u32)strlen(IMAGE_SLOT_KEY));
    CdoValue v;
    if (cdo_object_rawget(obj, kslot, &v) && v.tag == CDO_NUMBER)
        fslot = v.as.number;
    cdo_string_release(kslot);
    int idx = (int)fslot;
    if (idx >= 0 && idx < DRAW_MAX_IMAGES) {
        img_release(&g_images[idx]);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Module entry point.
 * ===================================================================== */

CandoValue cando_module_init(CandoVM *vm)
{
    cando_lib_meta_register(vm);
    CdoObject *image_meta = cando_lib_meta_table(vm, "draw_image");
    (void)cando_lib_meta_table(vm, "draw_font");

    /* Image instance methods. */
    cando_lib_meta_define(vm, image_meta, "getWidth",      image_get_width);
    cando_lib_meta_define(vm, image_meta, "getHeight",     image_get_height);
    cando_lib_meta_define(vm, image_meta, "getDimensions", image_get_dimensions);
    cando_lib_meta_define(vm, image_meta, "setFilter",     image_set_filter);
    cando_lib_meta_define(vm, image_meta, "release",       image_release);

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

    /* Image */
    libutil_set_method(vm, obj, "newImage",      draw_new_image);
    libutil_set_method(vm, obj, "draw",          draw_draw);

    return tbl;
}
