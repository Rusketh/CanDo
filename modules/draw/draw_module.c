/*
 * modules/draw/draw_module.c -- CanDo draw module entry point.
 *
 * Loaded into a script with:
 *
 *     VAR draw = include("./draw.so");          // Linux / macOS
 *     VAR draw = include("./draw.dll");         // Windows
 *
 * The draw module exposes a flat, LOVE-shaped API (`setColor`,
 * `rectangle("fill", ...)`, `print`, `newImage`, `newFont`, ...)
 * operating on the GL context that the active window's render thread
 * has made current.  This is the skeleton chunk: only
 * `cando_module_init` and the empty namespace + meta tables are
 * wired up.  Primitives, transforms, images, and fonts land in later
 * chunks.
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
#include <string.h>

#define DRAW_MODULE_VERSION "0.0.1"

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

CandoValue cando_module_init(CandoVM *vm)
{
    /* Reserve the prototypes so user code can attach methods now and
     * have them visible once the native primitives land in later
     * chunks. */
    cando_lib_meta_register(vm);
    (void)cando_lib_meta_table(vm, "draw_image");
    (void)cando_lib_meta_table(vm, "draw_font");

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   DRAW_MODULE_VERSION,
                   (u32)sizeof(DRAW_MODULE_VERSION) - 1);

    return tbl;
}
