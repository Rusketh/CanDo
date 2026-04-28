/*
 * modules/sqlite/sqlite_module.c -- CanDo SQLite binary module.
 *
 * Loaded into a script with:
 *
 *     VAR sql = include("./sqlite.so");        // Linux / macOS
 *     VAR sql = include("./sqlite.dll");       // Windows
 *
 * The module exposes an API on par with Node.js's `node:sqlite`
 * (DatabaseSync + StatementSync) on top of a vendored SQLite
 * amalgamation -- no system libsqlite is required at runtime.
 *
 * SQLite is built in serialized mode (SQLITE_THREADSAFE=1), so handles
 * are safe to use concurrently from `thread { ... }` blocks.  No
 * separate async API is provided; scripts compose threads themselves.
 *
 * This translation unit is chunk 1 -- the scaffold.  It only exposes
 * VERSION and SQLITE_VERSION; the database / statement surface is
 * implemented in subsequent chunks.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"

#include "vendor/sqlite3.h"

#define SQLITE_MODULE_VERSION "0.1.0"

CandoValue cando_module_init(CandoVM *vm)
{
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    /* VERSION -- module version, bumped per release. */
    CdoString *kver = cdo_string_intern("VERSION", 7);
    CdoString *vver = cdo_string_intern(SQLITE_MODULE_VERSION,
                                        (u32)sizeof(SQLITE_MODULE_VERSION) - 1);
    cdo_object_rawset(obj, kver, cdo_string_value(vver), FIELD_NONE);
    cdo_string_release(kver);
    cdo_string_release(vver);

    /* SQLITE_VERSION -- the vendored SQLite library version. */
    const char *slv = sqlite3_libversion();
    u32         sln = (u32)strlen(slv);
    CdoString *kslv = cdo_string_intern("SQLITE_VERSION", 14);
    CdoString *vslv = cdo_string_intern(slv, sln);
    cdo_object_rawset(obj, kslv, cdo_string_value(vslv), FIELD_NONE);
    cdo_string_release(kslv);
    cdo_string_release(vslv);

    return tbl;
}
