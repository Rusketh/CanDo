/*
 * modules/window/window_module.c -- CanDo window module entry point.
 *
 * Loaded into a script with:
 *
 *     VAR window = include("./window.so");      // Linux / macOS
 *     VAR window = include("./window.dll");     // Windows
 *
 * This is the skeleton chunk: cando_module_init builds the public
 * `window` namespace (VERSION + placeholder fields) and registers an
 * empty `_meta.window` prototype using the cando_lib_meta_* helpers.
 * Window creation, the GLFW manager thread, the per-window render
 * thread, and event dispatch arrive in later chunks.
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

#define WINDOW_MODULE_VERSION "0.0.1"

/* =========================================================================
 * Tiny obj_set_* helpers (same idiom as modules/sqlite/sqlite_module.c).
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

/* =========================================================================
 * Module entry point.
 * ===================================================================== */

CandoValue cando_module_init(CandoVM *vm)
{
    /* Ensure `_meta` exists, then create / fetch `_meta.window`.  Method
     * registration on the prototype lands in the next chunk -- for now
     * the table just exists so user scripts can attach handlers
     * directly: `_meta.window.draw = FUNCTION(self) { ... }`. */
    cando_lib_meta_register(vm);
    (void)cando_lib_meta_table(vm, "window");

    /* Build the namespace object that `include` returns. */
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    obj_set_string(obj, "VERSION",
                   WINDOW_MODULE_VERSION,
                   (u32)sizeof(WINDOW_MODULE_VERSION) - 1);

    return tbl;
}
