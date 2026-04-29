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
 * Both calling styles are supported on every handle method:
 *
 *     sql.exec(db, "...")        // function-style, like modules/ldap
 *     db:exec("...")             // method-style, like node:sqlite
 *
 * Internally the natives always read args[0] as the handle, so both
 * call shapes route through the same native function.
 *
 * Chunk 2 -- open / close / exec + slot pool + multi-value error
 * reporter.  Prepared statements arrive in chunk 3.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>

/* libcando does not export its cando_os_mutex_* helpers (they lack
 * CANDO_API), so a binary module can't link them in on Windows where
 * MinGW switches to "explicit exports only" mode the moment any
 * __declspec(dllexport) appears in the DLL.  Roll our own tiny
 * CRITICAL_SECTION / pthread_mutex_t wrapper instead -- same trick
 * the LDAP module uses. */
#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION sqlite_mutex_t;
#  define SQLITE_MUTEX_INIT(m)   InitializeCriticalSection(m)
#  define SQLITE_MUTEX_LOCK(m)   EnterCriticalSection(m)
#  define SQLITE_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#else
#  include <pthread.h>
   typedef pthread_mutex_t sqlite_mutex_t;
#  define SQLITE_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#  define SQLITE_MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define SQLITE_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#endif

#include "vendor/sqlite3.h"
#include "sqlite_helpers.h"

#define SQLITE_MODULE_VERSION "0.2.0"

/* =========================================================================
 * Connection pool
 *
 * Script-facing connection objects carry a single number field
 * `__sqlite_db_slot` that is an index into the static pool below.  This
 * avoids the f64 split-pointer trick, gives us strict handle validation
 * (a slot marked unused returns a clean error rather than UB), and
 * follows the pattern already used by modules/ldap/ldap_module.c.
 * ======================================================================= */

#define SQLITE_MAX_DBS    256
#define SQLITE_DB_SLOT_KEY "__sqlite_db_slot"

typedef struct SqliteSlot {
    sqlite3 *db;
    bool     in_use;
} SqliteSlot;

static SqliteSlot   g_db_pool[SQLITE_MAX_DBS];
static sqlite_mutex_t g_db_pool_mutex;
static _Atomic(int)   g_db_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_db_pool_inited, &expected, 1)) {
        SQLITE_MUTEX_INIT(&g_db_pool_mutex);
        for (int i = 0; i < SQLITE_MAX_DBS; i++) {
            g_db_pool[i].db     = NULL;
            g_db_pool[i].in_use = false;
        }
    }
}

static int db_pool_alloc(sqlite3 *db)
{
    ensure_pool_inited();
    SQLITE_MUTEX_LOCK(&g_db_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SQLITE_MAX_DBS; i++) {
        if (!g_db_pool[i].in_use) {
            g_db_pool[i].db     = db;
            g_db_pool[i].in_use = true;
            idx = i;
            break;
        }
    }
    SQLITE_MUTEX_UNLOCK(&g_db_pool_mutex);
    return idx;
}

static sqlite3 *db_pool_get(int idx)
{
    if (idx < 0 || idx >= SQLITE_MAX_DBS) return NULL;
    if (!g_db_pool[idx].in_use)           return NULL;
    return g_db_pool[idx].db;
}

static void db_pool_release(int idx)
{
    if (idx < 0 || idx >= SQLITE_MAX_DBS) return;
    SQLITE_MUTEX_LOCK(&g_db_pool_mutex);
    g_db_pool[idx].db     = NULL;
    g_db_pool[idx].in_use = false;
    SQLITE_MUTEX_UNLOCK(&g_db_pool_mutex);
}

/* =========================================================================
 * Multi-value error throws
 *
 * Scripts catch SQLite errors with `CATCH (msg, code, sqlstate)`:
 *   - msg      = formatted error message (string)
 *   - code     = numeric extended SQLite result code (0 for module-side errors)
 *   - sqlstate = symbolic name (e.g. "SQLITE_BUSY"), or "" if not from SQLite
 *
 * cando_vm_error sets error_vals[0]; we extend slots [1] and [2] in
 * place, identical to the LDAP module's ldap_attach_extra.
 * ======================================================================= */

extern void cando_value_release(CandoValue v);

static void sqlite_attach_extra(CandoVM *vm, int code, const char *sqlstate)
{
    cando_value_release(vm->error_vals[1]);
    vm->error_vals[1] = cando_number((f64)code);

    cando_value_release(vm->error_vals[2]);
    const char *s = sqlstate ? sqlstate : "";
    CandoString *cs = cando_string_new(s, (u32)strlen(s));
    vm->error_vals[2] = cando_string_value(cs);

    if (vm->error_val_count < 3) vm->error_val_count = 3;
}

/* sqlite_throw -- fire a multi-value throw from native code.  Pass the
 * sqlite3* (or NULL) and the result code; the human-readable message is
 * built from the printf-style format below.  After this call, return -1. */
static void sqlite_throw(CandoVM *vm, sqlite3 *db, int code,
                         const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    cando_vm_error(vm, "%s", buf);

    /* If we have a live db handle and the caller didn't supply a code,
     * pull the extended code out of SQLite directly. */
    int ecode = code;
    if (ecode == 0 && db) ecode = sqlite3_extended_errcode(db);

    const char *sqlstate = (ecode == 0) ? "" : sqlite_errcode_name(ecode);
    sqlite_attach_extra(vm, ecode, sqlstate);
}

/* =========================================================================
 * Helpers -- object property access (mirrors modules/ldap)
 * ======================================================================= */

/* Used by chunk 3+ for reading prepared-statement source SQL etc. */
__attribute__((unused))
static bool obj_get_string(CdoObject *obj, const char *key,
                           const char **out, size_t *out_len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_STRING) return false;
    *out = v.as.string->data;
    if (out_len) *out_len = v.as.string->length;
    return true;
}

static bool obj_get_number(CdoObject *obj, const char *key, f64 *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = v.as.number;
    return true;
}

static bool obj_get_bool(CdoObject *obj, const char *key, bool *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_BOOL) return false;
    *out = v.as.boolean;
    return true;
}

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static void obj_set_number(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Method sentinel registry
 *
 * libutil_set_method registers a native function with the VM and stores
 * its sentinel (a number) under a name on an object.  We capture each
 * sentinel in a static slot below so that when a fresh db handle is
 * created we can re-attach the same sentinel to it -- making both
 * `sql.exec(db, ...)` and `db:exec(...)` resolve to the same native.
 * ======================================================================= */

typedef struct MethodEntry {
    const char *name;
    f64         sentinel;
    bool        on_db;       /* attach to db handles */
} MethodEntry;

#define MAX_METHOD_ENTRIES 32
static MethodEntry g_methods[MAX_METHOD_ENTRIES];
static int         g_method_count = 0;

static void register_method(CandoVM *vm, CdoObject *mod_obj,
                            const char *name, CandoNativeFn fn,
                            bool on_db)
{
    CandoValue sentinel = cando_vm_add_native(vm, fn);
    /* libutil_set_method asserts on full table; we trust the module
     * author to keep the table small enough.  Mirror its behaviour. */
    f64 s = cando_is_number(sentinel) ? sentinel.as.number : 0.0;
    obj_set_number(mod_obj, name, s);

    if (g_method_count < MAX_METHOD_ENTRIES) {
        g_methods[g_method_count].name     = name;
        g_methods[g_method_count].sentinel = s;
        g_methods[g_method_count].on_db    = on_db;
        g_method_count++;
    }
}

static void attach_db_methods(CdoObject *handle)
{
    for (int i = 0; i < g_method_count; i++) {
        if (g_methods[i].on_db) {
            obj_set_number(handle, g_methods[i].name, g_methods[i].sentinel);
        }
    }
}

/* =========================================================================
 * DB handle: pool slot index boxed inside an object
 * ======================================================================= */

static CandoValue make_db_handle(CandoVM *vm, int slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(obj, SQLITE_DB_SLOT_KEY, (f64)slot);
    attach_db_methods(obj);
    return v;
}

static int db_handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    f64 idx = -1.0;
    if (!obj_get_number(obj, SQLITE_DB_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= SQLITE_MAX_DBS) return -1;
    return i;
}

/* Resolve to a live sqlite3*.  Throws and returns NULL if the value
 * isn't a database handle or the slot has been released. */
static sqlite3 *db_handle_unwrap(CandoVM *vm, CandoValue v)
{
    int slot = db_handle_slot(vm, v);
    if (slot < 0) {
        sqlite_throw(vm, NULL, 0, "sqlite: expected database handle");
        return NULL;
    }
    sqlite3 *db = db_pool_get(slot);
    if (!db) {
        sqlite_throw(vm, NULL, 0, "sqlite: database has been closed");
        return NULL;
    }
    return db;
}

static void db_handle_mark_closed(CandoVM *vm, CandoValue v)
{
    int slot = db_handle_slot(vm, v);
    if (slot >= 0) db_pool_release(slot);
    if (cando_is_object(v)) {
        CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
        obj_set_number(obj, SQLITE_DB_SLOT_KEY, -1.0);
    }
}

/* =========================================================================
 * native_sqlite_open(path[, options]) -> db
 *
 * Options object:
 *   readonly             bool    open RO instead of RW+CREATE          (default FALSE)
 *   create               bool    add SQLITE_OPEN_CREATE                 (default TRUE)
 *   uri                  bool    interpret path as a URI                (default FALSE)
 *   timeout              number  busy-timeout in ms                     (default 5000)
 *   enableForeignKeys    bool    PRAGMA foreign_keys = ON               (default TRUE)
 *   enableLoadExtension  bool    allow db:loadExtension(...)            (default FALSE)
 * ======================================================================= */

static int native_sqlite_open(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    if (!path) {
        sqlite_throw(vm, NULL, 0, "sqlite.open: path must be a string");
        return -1;
    }

    bool   opt_readonly        = false;
    bool   opt_create          = true;
    bool   opt_uri             = false;
    f64    opt_timeout_ms      = 5000.0;
    bool   opt_foreign_keys    = true;
    bool   opt_enable_load_ext = false;

    if (argc >= 2 && cando_is_object(args[1])) {
        CdoObject *o = cando_bridge_resolve(vm, args[1].as.handle);
        bool b; f64 n;
        if (obj_get_bool   (o, "readonly",            &b)) opt_readonly       = b;
        if (obj_get_bool   (o, "create",              &b)) opt_create         = b;
        if (obj_get_bool   (o, "uri",                 &b)) opt_uri            = b;
        if (obj_get_number (o, "timeout",             &n)) opt_timeout_ms     = n;
        if (obj_get_bool   (o, "enableForeignKeys",   &b)) opt_foreign_keys   = b;
        if (obj_get_bool   (o, "enableLoadExtension", &b)) opt_enable_load_ext = b;
    }

    int flags = opt_readonly ? SQLITE_OPEN_READONLY
                             : SQLITE_OPEN_READWRITE;
    if (!opt_readonly && opt_create) flags |= SQLITE_OPEN_CREATE;
    if (opt_uri)                     flags |= SQLITE_OPEN_URI;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        char tmp[256];
        const char *msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        snprintf(tmp, sizeof(tmp), "%s", msg ? msg : "open failed");
        if (db) sqlite3_close(db);
        sqlite_throw(vm, NULL, rc, "sqlite.open: %s", tmp);
        return -1;
    }

    /* Busy timeout (the OS / WAL contention safeguard). */
    if (opt_timeout_ms > 0) {
        sqlite3_busy_timeout(db, (int)opt_timeout_ms);
    }

    /* Foreign keys default ON.  PRAGMA reapplies cleanly even when the
     * compile-time default already matches. */
    sqlite3_exec(db,
                 opt_foreign_keys ? "PRAGMA foreign_keys = ON;"
                                  : "PRAGMA foreign_keys = OFF;",
                 NULL, NULL, NULL);

    if (opt_enable_load_ext) {
        sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL);
    }

    int slot = db_pool_alloc(db);
    if (slot < 0) {
        sqlite3_close(db);
        sqlite_throw(vm, NULL, 0,
            "sqlite.open: connection pool exhausted (max %d)",
            SQLITE_MAX_DBS);
        return -1;
    }

    cando_vm_push(vm, make_db_handle(vm, slot));
    return 1;
}

/* =========================================================================
 * native_sqlite_close(db) -> true
 *
 * Idempotent: calling close on an already-closed handle is a no-op,
 * matching node:sqlite which is lenient about double-close.
 * ======================================================================= */

static int native_sqlite_close(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.close: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    sqlite3 *db = db_pool_get(slot);
    if (db) {
        /* sqlite3_close_v2 tolerates open statements and zombies the
         * connection until they finalise (relevant once chunk 3 lands
         * prepared statements). */
        sqlite3_close_v2(db);
    }
    db_handle_mark_closed(vm, args[0]);

    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_sqlite_exec(db, sql) -> true
 *
 * Runs one or more semicolon-separated statements with no result rows.
 * Throws on the first error, matching node:sqlite's DatabaseSync.exec.
 * ======================================================================= */

static int native_sqlite_exec(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sqlite_throw(vm, NULL, 0, "sqlite.exec: (db, sql) required");
        return -1;
    }
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;

    const char *sql = libutil_arg_cstr_at(args, argc, 1);
    if (!sql) {
        sqlite_throw(vm, db, 0, "sqlite.exec: sql must be a string");
        return -1;
    }

    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", errmsg ? errmsg : sqlite3_errstr(rc));
        if (errmsg) sqlite3_free(errmsg);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.exec: %s", tmp);
        return -1;
    }
    if (errmsg) sqlite3_free(errmsg);

    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Module init
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
CandoValue cando_module_init(CandoVM *vm)
{
    ensure_pool_inited();
    g_method_count = 0;

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    /* `open` lives only on the module -- there's no handle to attach
     * it to, since the caller is creating one. */
    register_method(vm, obj, "open",  native_sqlite_open,  /*on_db=*/false);

    /* db handle methods: callable as both `sql.close(db)` and `db:close()`. */
    register_method(vm, obj, "close", native_sqlite_close, /*on_db=*/true);
    register_method(vm, obj, "exec",  native_sqlite_exec,  /*on_db=*/true);

    /* Module-version string. */
    obj_set_string(obj, "VERSION",
                   SQLITE_MODULE_VERSION,
                   (u32)sizeof(SQLITE_MODULE_VERSION) - 1);

    /* SQLITE_VERSION -- the vendored SQLite library version. */
    const char *slv = sqlite3_libversion();
    obj_set_string(obj, "SQLITE_VERSION", slv, (u32)strlen(slv));

    /* Open-flag constants -- matches the SQLITE_OPEN_* names so scripts
     * doing flag arithmetic stay portable to other SQLite bindings. */
    obj_set_number(obj, "OPEN_READONLY",  (f64)SQLITE_OPEN_READONLY);
    obj_set_number(obj, "OPEN_READWRITE", (f64)SQLITE_OPEN_READWRITE);
    obj_set_number(obj, "OPEN_CREATE",    (f64)SQLITE_OPEN_CREATE);
    obj_set_number(obj, "OPEN_URI",       (f64)SQLITE_OPEN_URI);
    obj_set_number(obj, "OPEN_MEMORY",    (f64)SQLITE_OPEN_MEMORY);

    return tbl;
}
