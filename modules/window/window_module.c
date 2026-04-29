/*
 * modules/window/window_module.c -- CanDo window module entry point.
 *
 * Loaded into a script with:
 *
 *     VAR window = include("./window.so");      // Linux / macOS
 *     VAR window = include("./window.dll");     // Windows
 *
 * This chunk wires the vendored GLFW into the .so and exposes its
 * runtime version string at `window.glfwVersion`.  The `_meta.window`
 * prototype is reserved (empty for now); window creation, manager
 * thread, render thread, and events arrive in the next chunks.
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

#include <GLFW/glfw3.h>

#define WINDOW_MODULE_VERSION "0.0.2"

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

    /* Expose the linked GLFW version so scripts can sanity-check the
     * binary they loaded.  Calling glfwGetVersionString does not require
     * glfwInit; it is documented as safe to call before / without init. */
    const char *gv = glfwGetVersionString();
    if (gv) obj_set_string(obj, "glfwVersion", gv, (u32)strlen(gv));

    return tbl;
}
