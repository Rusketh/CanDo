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
#include "object/array.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"

#include <math.h>

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

#define SQLITE_MODULE_VERSION "0.4.0"

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
    bool     bigint_string;  /* true => return INTEGERs > 2^53 as decimal strings */
    int      tx_depth;       /* 0 = none; 1 = BEGIN; >1 = SAVEPOINT cdo_sp_N */
} SqliteSlot;

#define SQLITE_MAX_STMTS    1024
#define SQLITE_STMT_SLOT_KEY "__sqlite_stmt_slot"
#define SQLITE_STMT_SQL_KEY  "sourceSQL"

typedef struct SqliteStmtSlot {
    sqlite3_stmt *st;
    int           db_slot;     /* parent db slot index */
    bool          in_use;
} SqliteStmtSlot;

static SqliteSlot       g_db_pool[SQLITE_MAX_DBS];
static SqliteStmtSlot   g_stmt_pool[SQLITE_MAX_STMTS];
static sqlite_mutex_t   g_db_pool_mutex;
static sqlite_mutex_t   g_stmt_pool_mutex;
static _Atomic(int)     g_db_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_db_pool_inited, &expected, 1)) {
        SQLITE_MUTEX_INIT(&g_db_pool_mutex);
        SQLITE_MUTEX_INIT(&g_stmt_pool_mutex);
        for (int i = 0; i < SQLITE_MAX_DBS; i++) {
            g_db_pool[i].db            = NULL;
            g_db_pool[i].in_use        = false;
            g_db_pool[i].bigint_string = false;
            g_db_pool[i].tx_depth      = 0;
        }
        for (int i = 0; i < SQLITE_MAX_STMTS; i++) {
            g_stmt_pool[i].st      = NULL;
            g_stmt_pool[i].db_slot = -1;
            g_stmt_pool[i].in_use  = false;
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
    g_db_pool[idx].db            = NULL;
    g_db_pool[idx].in_use        = false;
    g_db_pool[idx].bigint_string = false;
    g_db_pool[idx].tx_depth      = 0;
    SQLITE_MUTEX_UNLOCK(&g_db_pool_mutex);
}

/* ---- Statement pool ----------------------------------------------------- */

static int stmt_pool_alloc(sqlite3_stmt *st, int db_slot)
{
    ensure_pool_inited();
    SQLITE_MUTEX_LOCK(&g_stmt_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SQLITE_MAX_STMTS; i++) {
        if (!g_stmt_pool[i].in_use) {
            g_stmt_pool[i].st      = st;
            g_stmt_pool[i].db_slot = db_slot;
            g_stmt_pool[i].in_use  = true;
            idx = i;
            break;
        }
    }
    SQLITE_MUTEX_UNLOCK(&g_stmt_pool_mutex);
    return idx;
}

static SqliteStmtSlot *stmt_pool_get(int idx)
{
    if (idx < 0 || idx >= SQLITE_MAX_STMTS) return NULL;
    if (!g_stmt_pool[idx].in_use)           return NULL;
    return &g_stmt_pool[idx];
}

static void stmt_pool_release(int idx)
{
    if (idx < 0 || idx >= SQLITE_MAX_STMTS) return;
    SQLITE_MUTEX_LOCK(&g_stmt_pool_mutex);
    g_stmt_pool[idx].st      = NULL;
    g_stmt_pool[idx].db_slot = -1;
    g_stmt_pool[idx].in_use  = false;
    SQLITE_MUTEX_UNLOCK(&g_stmt_pool_mutex);
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

static void obj_set_bool_field(CdoObject *obj, const char *key, bool value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(value), FIELD_NONE);
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

typedef enum {
    METHOD_MODULE_ONLY = 0,
    METHOD_ON_DB       = 1,
    METHOD_ON_STMT     = 2,
    METHOD_ON_ITER     = 3,
} MethodTarget;

typedef struct MethodEntry {
    const char  *name;
    f64          sentinel;
    MethodTarget target;
} MethodEntry;

#define MAX_METHOD_ENTRIES 48
static MethodEntry g_methods[MAX_METHOD_ENTRIES];
static int         g_method_count = 0;

static void register_method(CandoVM *vm, CdoObject *mod_obj,
                            const char *name, CandoNativeFn fn,
                            MethodTarget target)
{
    CandoValue sentinel = cando_vm_add_native(vm, fn);
    /* libutil_set_method asserts on full table; mirror that behaviour
     * by trusting the module author to keep the table small enough. */
    f64 s = cando_is_number(sentinel) ? sentinel.as.number : 0.0;
    obj_set_number(mod_obj, name, s);

    if (g_method_count < MAX_METHOD_ENTRIES) {
        g_methods[g_method_count].name     = name;
        g_methods[g_method_count].sentinel = s;
        g_methods[g_method_count].target   = target;
        g_method_count++;
    }
}

static void attach_methods_for(CdoObject *handle, MethodTarget target)
{
    for (int i = 0; i < g_method_count; i++) {
        if (g_methods[i].target == target) {
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
    attach_methods_for(obj, METHOD_ON_DB);
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
 * Stmt handle: pool slot index boxed inside an object
 * ======================================================================= */

static CandoValue make_stmt_handle(CandoVM *vm, int slot, const char *sql, u32 sql_len)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(obj, SQLITE_STMT_SLOT_KEY, (f64)slot);
    obj_set_string(obj, SQLITE_STMT_SQL_KEY,  sql, sql_len);
    attach_methods_for(obj, METHOD_ON_STMT);
    return v;
}

static int stmt_handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    f64 idx = -1.0;
    if (!obj_get_number(obj, SQLITE_STMT_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= SQLITE_MAX_STMTS) return -1;
    return i;
}

/* Resolve to a (live stmt slot, parent db) pair.  Throws and returns
 * NULL if either has been finalised / closed. */
static SqliteStmtSlot *stmt_handle_unwrap(CandoVM *vm, CandoValue v,
                                          sqlite3 **out_db)
{
    int slot = stmt_handle_slot(vm, v);
    if (slot < 0) {
        sqlite_throw(vm, NULL, 0, "sqlite: expected statement handle");
        return NULL;
    }
    SqliteStmtSlot *s = stmt_pool_get(slot);
    if (!s || !s->st) {
        sqlite_throw(vm, NULL, 0, "sqlite: statement has been finalised");
        return NULL;
    }
    sqlite3 *db = db_pool_get(s->db_slot);
    if (!db) {
        sqlite_throw(vm, NULL, 0, "sqlite: parent database has been closed");
        return NULL;
    }
    if (out_db) *out_db = db;
    return s;
}

static void stmt_handle_mark_finalised(CandoVM *vm, CandoValue v)
{
    int slot = stmt_handle_slot(vm, v);
    if (slot >= 0) stmt_pool_release(slot);
    if (cando_is_object(v)) {
        CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
        obj_set_number(obj, SQLITE_STMT_SLOT_KEY, -1.0);
    }
}

/* =========================================================================
 * Parameter binding
 *
 * Three call shapes for run / get / all / bind:
 *   stmt:run()                       -- no parameters
 *   stmt:run(p0, p1, ...)             -- positional
 *   stmt:run([p0, p1, ...])           -- positional via array
 *   stmt:run({ name1: v1, name2: v2 })-- named (:name / @name / $name)
 *
 * Per element value mapping:
 *   null                  -> sqlite3_bind_null
 *   bool                  -> sqlite3_bind_int(0|1)
 *   number (whole, |x|<=2^53) -> sqlite3_bind_int64
 *   number (fractional / overflow) -> sqlite3_bind_double
 *   string                -> sqlite3_bind_text   (SQLITE_TRANSIENT)
 *   { blob: <string> }    -> sqlite3_bind_blob   (SQLITE_TRANSIENT)
 *   anything else         -> error
 *
 * Returns true on success.  On error, throws (the caller returns -1).
 * ======================================================================= */

#define INTEGER_SAFE_MAX 9007199254740992.0  /* 2^53 */

static bool bind_one_value(CandoVM *vm, sqlite3_stmt *st, int idx,
                           CandoValue v, const char *what)
{
    if (cando_is_null(v)) {
        sqlite3_bind_null(st, idx);
        return true;
    }
    if (cando_is_bool(v)) {
        sqlite3_bind_int(st, idx, v.as.boolean ? 1 : 0);
        return true;
    }
    if (cando_is_number(v)) {
        f64 d = v.as.number;
        if (isfinite(d) && fabs(d) <= INTEGER_SAFE_MAX && d == (f64)(int64_t)d) {
            sqlite3_bind_int64(st, idx, (sqlite3_int64)d);
        } else {
            sqlite3_bind_double(st, idx, d);
        }
        return true;
    }
    if (cando_is_string(v)) {
        const char *s = v.as.string->data;
        u32         n = v.as.string->length;
        sqlite3_bind_text(st, idx, s, (int)n, SQLITE_TRANSIENT);
        return true;
    }
    if (cando_is_object(v)) {
        CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
        /* { blob: <string> } -> bind as BLOB */
        CdoString *kblob = cdo_string_intern("blob", 4);
        CdoValue   bv;
        bool has = cdo_object_rawget(o, kblob, &bv);
        cdo_string_release(kblob);
        if (has && bv.tag == CDO_STRING) {
            const char *s = bv.as.string->data;
            u32         n = bv.as.string->length;
            sqlite3_bind_blob(st, idx, s, (int)n, SQLITE_TRANSIENT);
            return true;
        }
        sqlite_throw(vm, NULL, 0,
            "%s: parameter %d is an object without a 'blob' string field",
            what, idx);
        return false;
    }
    sqlite_throw(vm, NULL, 0,
        "%s: parameter %d has unsupported type", what, idx);
    return false;
}

/* Binds parameters from args[start..argc-1] onto st.  Returns true on
 * success, false on error (with throw already fired). */
static bool bind_params(CandoVM *vm, sqlite3_stmt *st,
                        int argc, CandoValue *args, int start,
                        const char *what)
{
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);

    int n_provided = argc - start;

    /* Single-arg array -> positional from array elements. */
    if (n_provided == 1 && cando_is_object(args[start])) {
        CdoObject *o = cando_bridge_resolve(vm, args[start].as.handle);
        if (o->kind == OBJ_ARRAY) {
            u32 n = cdo_array_len(o);
            for (u32 i = 0; i < n; i++) {
                CdoValue ev;
                if (!cdo_array_rawget_idx(o, i, &ev)) continue;
                CandoValue cv = cando_bridge_to_cando(vm, ev);
                if (!bind_one_value(vm, st, (int)(i + 1), cv, what)) return false;
            }
            return true;
        }
        /* Single-arg plain object -> named-bind by SQL parameter name. */
        if (o->kind == OBJ_OBJECT) {
            int total = sqlite3_bind_parameter_count(st);
            for (int i = 1; i <= total; i++) {
                const char *pname = sqlite3_bind_parameter_name(st, i);
                if (!pname) {
                    sqlite_throw(vm, NULL, 0,
                        "%s: positional ? parameter %d cannot be bound from an object",
                        what, i);
                    return false;
                }
                /* Skip the leading sigil: ':', '@', or '$'. */
                const char *key = pname + 1;
                CdoString *k = cdo_string_intern(key, (u32)strlen(key));
                CdoValue   ev;
                bool has = cdo_object_rawget(o, k, &ev);
                cdo_string_release(k);
                if (!has) {
                    /* Missing keys bind to NULL, mirroring node:sqlite's
                     * setAllowUnknownNamedParameters default-off behaviour
                     * but lenient on the missing side. */
                    sqlite3_bind_null(st, i);
                    continue;
                }
                CandoValue cv = cando_bridge_to_cando(vm, ev);
                if (!bind_one_value(vm, st, i, cv, what)) return false;
            }
            return true;
        }
    }

    /* Otherwise treat each remaining arg as a positional parameter. */
    for (int i = 0; i < n_provided; i++) {
        if (!bind_one_value(vm, st, i + 1, args[start + i], what)) return false;
    }
    return true;
}

/* =========================================================================
 * Column reading -- builds a row object keyed by column name.
 * ======================================================================= */

static CandoValue read_column(CandoVM *vm, sqlite3_stmt *st, int col,
                              bool bigint_string)
{
    int t = sqlite3_column_type(st, col);
    switch (t) {
        case SQLITE_NULL:
            return cando_null();
        case SQLITE_INTEGER: {
            sqlite3_int64 v = sqlite3_column_int64(st, col);
            f64 d = (f64)v;
            if (bigint_string && (v >  (sqlite3_int64)INTEGER_SAFE_MAX ||
                                  v < -(sqlite3_int64)INTEGER_SAFE_MAX)) {
                char buf[24];
                int  n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
                CandoString *s = cando_string_new(buf, (u32)n);
                return cando_string_value(s);
            }
            return cando_number(d);
        }
        case SQLITE_FLOAT:
            return cando_number(sqlite3_column_double(st, col));
        case SQLITE_TEXT: {
            const unsigned char *s = sqlite3_column_text(st, col);
            int n = sqlite3_column_bytes(st, col);
            CandoString *cs = cando_string_new((const char *)s, (u32)n);
            return cando_string_value(cs);
        }
        case SQLITE_BLOB: {
            /* Cando strings are byte-safe, so blobs round-trip cleanly
             * as opaque strings.  Use { blob: ... } to bind back. */
            const void *p = sqlite3_column_blob(st, col);
            int n = sqlite3_column_bytes(st, col);
            CandoString *cs = cando_string_new((const char *)p, (u32)n);
            return cando_string_value(cs);
        }
        default:
            (void)vm;
            return cando_null();
    }
}

static CandoValue build_row(CandoVM *vm, sqlite3_stmt *st, bool bigint_string)
{
    int ncols = sqlite3_column_count(st);
    CandoValue rowv = cando_bridge_new_object(vm);
    CdoObject *row  = cando_bridge_resolve(vm, rowv.as.handle);
    for (int c = 0; c < ncols; c++) {
        const char *name = sqlite3_column_name(st, c);
        if (!name) name = "?";
        CandoValue cv = read_column(vm, st, c, bigint_string);
        CdoString *k  = cdo_string_intern(name, (u32)strlen(name));
        cdo_object_rawset(row, k, cando_bridge_to_cdo(vm, cv), FIELD_NONE);
        cdo_string_release(k);
    }
    return rowv;
}

/* =========================================================================
 * native_sqlite_prepare(db, sql) -> stmt
 * ======================================================================= */

static int native_sqlite_prepare(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sqlite_throw(vm, NULL, 0, "sqlite.prepare: (db, sql) required");
        return -1;
    }
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;

    int db_slot = db_handle_slot(vm, args[0]);

    const char *sql = libutil_arg_cstr_at(args, argc, 1);
    if (!sql) {
        sqlite_throw(vm, db, 0, "sqlite.prepare: sql must be a string");
        return -1;
    }
    u32 sql_len = (u32)strlen(sql);

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, (int)sql_len + 1, &st, NULL);
    if (rc != SQLITE_OK || !st) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.prepare: %s", tmp);
        if (st) sqlite3_finalize(st);
        return -1;
    }

    int slot = stmt_pool_alloc(st, db_slot);
    if (slot < 0) {
        sqlite3_finalize(st);
        sqlite_throw(vm, NULL, 0,
            "sqlite.prepare: statement pool exhausted (max %d)",
            SQLITE_MAX_STMTS);
        return -1;
    }

    cando_vm_push(vm, make_stmt_handle(vm, slot, sql, sql_len));
    return 1;
}

/* =========================================================================
 * native_sqlite_finalize(stmt) -> true
 * Idempotent.
 * ======================================================================= */

static int native_sqlite_finalize(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.finalize: (stmt) required");
        return -1;
    }
    int slot = stmt_handle_slot(vm, args[0]);
    if (slot >= 0) {
        SqliteStmtSlot *s = stmt_pool_get(slot);
        if (s && s->st) {
            sqlite3_finalize(s->st);
            s->st = NULL;
        }
        stmt_handle_mark_finalised(vm, args[0]);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_sqlite_reset(stmt) -> true
 * Clears bindings and rewinds the cursor.
 * ======================================================================= */

static int native_sqlite_reset(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.reset: (stmt) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;
    sqlite3_reset(s->st);
    sqlite3_clear_bindings(s->st);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_sqlite_bind(stmt, params) -> true
 * Binds without stepping; useful for iterate() / re-use across rows.
 * ======================================================================= */

static int native_sqlite_bind(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.bind: (stmt, params...) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;
    if (!bind_params(vm, s->st, argc, args, 1, "sqlite.bind")) return -1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_sqlite_run(stmt, params...) -> { lastInsertRowid, changes }
 *
 * Steps the statement once; expects no row results (a single SQLITE_DONE).
 * Returns the rowid + changes object that node:sqlite scripts expect.
 * ======================================================================= */

static int native_sqlite_run(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.run: (stmt, params...) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;

    if (!bind_params(vm, s->st, argc, args, 1, "sqlite.run")) return -1;

    int rc = sqlite3_step(s->st);
    /* SQLITE_ROW from `run` is fine: scripts who used the wrong helper
     * still get a clean lastInsertRowid/changes back. */
    while (rc == SQLITE_ROW) rc = sqlite3_step(s->st);
    if (rc != SQLITE_DONE) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
        sqlite3_reset(s->st);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.run: %s", tmp);
        return -1;
    }

    sqlite3_int64 rowid   = sqlite3_last_insert_rowid(db);
    int            changes = sqlite3_changes(db);

    sqlite3_reset(s->st);

    /* Build { lastInsertRowid, changes } */
    CandoValue rv  = cando_bridge_new_object(vm);
    CdoObject *o   = cando_bridge_resolve(vm, rv.as.handle);
    obj_set_number(o, "lastInsertRowid", (f64)rowid);
    obj_set_number(o, "changes",         (f64)changes);

    cando_vm_push(vm, rv);
    return 1;
}

/* =========================================================================
 * native_sqlite_get(stmt, params...) -> row | null
 * ======================================================================= */

static int native_sqlite_get(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.get: (stmt, params...) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;

    if (!bind_params(vm, s->st, argc, args, 1, "sqlite.get")) return -1;

    int rc = sqlite3_step(s->st);
    if (rc == SQLITE_DONE) {
        sqlite3_reset(s->st);
        cando_vm_push(vm, cando_null());
        return 1;
    }
    if (rc != SQLITE_ROW) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
        sqlite3_reset(s->st);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.get: %s", tmp);
        return -1;
    }

    bool bigint_string = g_db_pool[s->db_slot].bigint_string;
    CandoValue row = build_row(vm, s->st, bigint_string);

    sqlite3_reset(s->st);
    cando_vm_push(vm, row);
    return 1;
}

/* =========================================================================
 * native_sqlite_all(stmt, params...) -> [row, ...]
 * ======================================================================= */

static int native_sqlite_all(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.all: (stmt, params...) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;

    if (!bind_params(vm, s->st, argc, args, 1, "sqlite.all")) return -1;

    bool bigint_string = g_db_pool[s->db_slot].bigint_string;

    CandoValue av = cando_bridge_new_array(vm);
    CdoObject *a  = cando_bridge_resolve(vm, av.as.handle);

    int rc;
    while ((rc = sqlite3_step(s->st)) == SQLITE_ROW) {
        CandoValue row = build_row(vm, s->st, bigint_string);
        cdo_array_push(a, cando_bridge_to_cdo(vm, row));
    }
    if (rc != SQLITE_DONE) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
        sqlite3_reset(s->st);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.all: %s", tmp);
        return -1;
    }

    sqlite3_reset(s->st);
    cando_vm_push(vm, av);
    return 1;
}

/* =========================================================================
 * native_sqlite_bigint_mode(db, mode) -> mode
 *
 * mode is "number" (default; INTEGERs > 2^53 become lossy doubles) or
 * "string" (overflow-safe; INTEGERs over 2^53 are returned as decimal
 * strings).  Returns the now-active mode.
 * ======================================================================= */

static int native_sqlite_bigint_mode(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.bigintMode: (db[, mode]) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0 || !db_pool_get(slot)) {
        sqlite_throw(vm, NULL, 0, "sqlite.bigintMode: invalid database handle");
        return -1;
    }
    if (argc >= 2) {
        const char *m = libutil_arg_cstr_at(args, argc, 1);
        if (!m) {
            sqlite_throw(vm, NULL, 0,
                "sqlite.bigintMode: mode must be \"number\" or \"string\"");
            return -1;
        }
        if      (strcmp(m, "number") == 0) g_db_pool[slot].bigint_string = false;
        else if (strcmp(m, "string") == 0) g_db_pool[slot].bigint_string = true;
        else {
            sqlite_throw(vm, NULL, 0,
                "sqlite.bigintMode: unknown mode \"%s\"", m);
            return -1;
        }
    }
    const char *out = g_db_pool[slot].bigint_string ? "string" : "number";
    libutil_push_cstr(vm, out);
    return 1;
}

/* node:sqlite-compat alias: db:setReadBigInts(true|false) */
static int native_sqlite_set_read_bigints(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sqlite_throw(vm, NULL, 0,
            "sqlite.setReadBigInts: (db, bool) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0 || !db_pool_get(slot)) {
        sqlite_throw(vm, NULL, 0,
            "sqlite.setReadBigInts: invalid database handle");
        return -1;
    }
    bool on = cando_is_bool(args[1]) && args[1].as.boolean;
    g_db_pool[slot].bigint_string = on;
    cando_vm_push(vm, cando_bool(true));
    return 1;
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
 * native_sqlite_expanded_sql(stmt) -> string
 *
 * Returns the prepared SQL with currently-bound parameters substituted
 * (sqlite3_expanded_sql).  Useful for logging.
 * ======================================================================= */

static int native_sqlite_expanded_sql(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.expandedSQL: (stmt) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;
    char *out = sqlite3_expanded_sql(s->st);
    if (!out) {
        sqlite_throw(vm, NULL, 0,
            "sqlite.expandedSQL: out of memory or unbound parameter");
        return -1;
    }
    libutil_push_cstr(vm, out);
    sqlite3_free(out);
    return 1;
}

/* =========================================================================
 * native_sqlite_columns(stmt) -> [ { name, type, table, column, database }, ... ]
 *
 * Each element describes the schema-side origin of a result column when
 * available (requires SQLITE_ENABLE_COLUMN_METADATA, which our build
 * enables).  Computed columns and aggregates have null table / column /
 * database fields.
 * ======================================================================= */

static int native_sqlite_columns(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.columns: (stmt) required");
        return -1;
    }
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;

    int n = sqlite3_column_count(s->st);
    CandoValue av = cando_bridge_new_array(vm);
    CdoObject *a  = cando_bridge_resolve(vm, av.as.handle);

    for (int i = 0; i < n; i++) {
        CandoValue ov = cando_bridge_new_object(vm);
        CdoObject *o  = cando_bridge_resolve(vm, ov.as.handle);

        const char *name = sqlite3_column_name(s->st, i);
        if (name) obj_set_string(o, "name", name, (u32)strlen(name));

        const char *decl = sqlite3_column_decltype(s->st, i);
        if (decl) obj_set_string(o, "type", decl, (u32)strlen(decl));

        const char *tbl  = sqlite3_column_table_name(s->st, i);
        if (tbl)  obj_set_string(o, "table",   tbl,  (u32)strlen(tbl));

        const char *orig = sqlite3_column_origin_name(s->st, i);
        if (orig) obj_set_string(o, "column",  orig, (u32)strlen(orig));

        const char *dbn  = sqlite3_column_database_name(s->st, i);
        if (dbn)  obj_set_string(o, "database", dbn, (u32)strlen(dbn));

        cdo_array_push(a, cando_bridge_to_cdo(vm, ov));
    }
    cando_vm_push(vm, av);
    return 1;
}

/* =========================================================================
 * Iterator handle (used by stmt:iterate)
 *
 * The iterator borrows the parent stmt slot.  Each :next() call steps
 * the cursor and returns a row, or NULL when the cursor is exhausted.
 * The iterator does not finalise the stmt; the caller still owns it.
 * ======================================================================= */

#define SQLITE_ITER_STMT_KEY "__sqlite_iter_stmt_slot"
/* Public-facing iterator field: set to TRUE once the cursor is
 * exhausted (or an error has been thrown).  Inspect with `iter.done`. */
#define SQLITE_ITER_DONE_KEY "done"

static CandoValue make_iterator(CandoVM *vm, int stmt_slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(obj, SQLITE_ITER_STMT_KEY, (f64)stmt_slot);
    obj_set_bool_field(obj, SQLITE_ITER_DONE_KEY, false);
    attach_methods_for(obj, METHOD_ON_ITER);
    return v;
}

static int native_sqlite_iterate(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.iterate: (stmt, params...) required");
        return -1;
    }
    int stmt_slot = stmt_handle_slot(vm, args[0]);
    sqlite3 *db = NULL;
    SqliteStmtSlot *s = stmt_handle_unwrap(vm, args[0], &db);
    if (!s) return -1;
    if (!bind_params(vm, s->st, argc, args, 1, "sqlite.iterate")) return -1;
    cando_vm_push(vm, make_iterator(vm, stmt_slot));
    return 1;
}

static int native_sqlite_iter_next(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        sqlite_throw(vm, NULL, 0, "sqlite.next: (iterator) required");
        return -1;
    }
    CdoObject *iobj = cando_bridge_resolve(vm, args[0].as.handle);
    f64 fslot = -1.0;
    if (!obj_get_number(iobj, SQLITE_ITER_STMT_KEY, &fslot)) {
        sqlite_throw(vm, NULL, 0, "sqlite.next: not an iterator handle");
        return -1;
    }
    int slot = (int)fslot;
    SqliteStmtSlot *s = stmt_pool_get(slot);
    if (!s || !s->st) {
        sqlite_throw(vm, NULL, 0, "sqlite.next: parent statement is gone");
        return -1;
    }
    sqlite3 *db = db_pool_get(s->db_slot);
    if (!db) {
        sqlite_throw(vm, NULL, 0, "sqlite.next: parent database is closed");
        return -1;
    }

    bool done = false;
    obj_get_bool(iobj, SQLITE_ITER_DONE_KEY, &done);
    if (done) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    int rc = sqlite3_step(s->st);
    if (rc == SQLITE_ROW) {
        bool bs = g_db_pool[s->db_slot].bigint_string;
        cando_vm_push(vm, build_row(vm, s->st, bs));
        return 1;
    }
    if (rc == SQLITE_DONE) {
        sqlite3_reset(s->st);
        obj_set_bool_field(iobj, SQLITE_ITER_DONE_KEY, true);
        cando_vm_push(vm, cando_null());
        return 1;
    }

    char tmp[480];
    snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
    sqlite3_reset(s->st);
    obj_set_bool_field(iobj, SQLITE_ITER_DONE_KEY, true);
    sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                 "sqlite.next: %s", tmp);
    return -1;
}

/* =========================================================================
 * Transactions: begin / commit / rollback / inTransaction / transaction
 *
 * begin / commit / rollback nest via SAVEPOINT.  The slot's tx_depth
 * tracks the nesting level so the caller can ROLLBACK the outermost
 * transaction or just the inner SAVEPOINT.  This mirrors better-sqlite3
 * semantics; node:sqlite's DatabaseSync exposes the same lower-level
 * primitives indirectly.
 * ======================================================================= */

static int native_sqlite_begin(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.begin: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;
    int depth = g_db_pool[slot].tx_depth;
    char buf[64];
    if (depth == 0) {
        snprintf(buf, sizeof(buf), "BEGIN");
    } else {
        snprintf(buf, sizeof(buf), "SAVEPOINT cdo_sp_%d", depth);
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, buf, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", err ? err : sqlite3_errstr(rc));
        if (err) sqlite3_free(err);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.begin: %s", tmp);
        return -1;
    }
    if (err) sqlite3_free(err);
    g_db_pool[slot].tx_depth = depth + 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sqlite_commit(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.commit: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;
    int depth = g_db_pool[slot].tx_depth;
    if (depth <= 0) {
        sqlite_throw(vm, NULL, 0, "sqlite.commit: no active transaction");
        return -1;
    }
    char buf[64];
    if (depth == 1) {
        snprintf(buf, sizeof(buf), "COMMIT");
    } else {
        snprintf(buf, sizeof(buf), "RELEASE cdo_sp_%d", depth - 1);
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, buf, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", err ? err : sqlite3_errstr(rc));
        if (err) sqlite3_free(err);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.commit: %s", tmp);
        return -1;
    }
    if (err) sqlite3_free(err);
    g_db_pool[slot].tx_depth = depth - 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sqlite_rollback(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.rollback: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;
    int depth = g_db_pool[slot].tx_depth;
    if (depth <= 0) {
        sqlite_throw(vm, NULL, 0, "sqlite.rollback: no active transaction");
        return -1;
    }
    char buf[96];
    if (depth == 1) {
        snprintf(buf, sizeof(buf), "ROLLBACK");
    } else {
        /* Roll the savepoint back AND release it -- otherwise the next
         * BEGIN/SAVEPOINT call collides with the still-open one and
         * the depth counter drifts.  This matches the standard
         * "ROLLBACK TO sp; RELEASE sp" idiom. */
        snprintf(buf, sizeof(buf),
                 "ROLLBACK TO cdo_sp_%d; RELEASE cdo_sp_%d",
                 depth - 1, depth - 1);
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, buf, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", err ? err : sqlite3_errstr(rc));
        if (err) sqlite3_free(err);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.rollback: %s", tmp);
        return -1;
    }
    if (err) sqlite3_free(err);
    g_db_pool[slot].tx_depth = depth - 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sqlite_in_transaction(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sqlite_throw(vm, NULL, 0, "sqlite.inTransaction: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0 || !db_pool_get(slot)) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    cando_vm_push(vm, cando_bool(g_db_pool[slot].tx_depth > 0));
    return 1;
}

/* db:transaction(fn[, ...args]) -> whatever fn returned
 *
 * Runs fn(...args) inside BEGIN ... COMMIT.  If fn throws, the
 * transaction is rolled back and the same throw is re-raised.  Nested
 * calls use SAVEPOINT.  Differs from node:sqlite's curried form
 * (`db.transaction(fn)` returns a function); the curried form was
 * dropped to keep the binding free of script-side closures.  Wrap if
 * you want the curried sugar:
 *     FUNCTION wrap(fn) { return FUNCTION (...a) { db:transaction(fn, ...a); }; }
 */
static int native_sqlite_transaction(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sqlite_throw(vm, NULL, 0,
            "sqlite.transaction: (db, fn[, args...]) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;

    int depth = g_db_pool[slot].tx_depth;
    char open_sql[64];
    if (depth == 0) {
        snprintf(open_sql, sizeof(open_sql), "BEGIN");
    } else {
        snprintf(open_sql, sizeof(open_sql), "SAVEPOINT cdo_sp_%d", depth);
    }
    int rc = sqlite3_exec(db, open_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.transaction: %s", sqlite3_errmsg(db));
        return -1;
    }
    g_db_pool[slot].tx_depth = depth + 1;

    /* Invoke the user's function with args[2..argc-1].  Hide the
     * outer try frames so a throw inside fn cannot be caught above us
     * before our rollback runs -- the throw must reach OUR frame so
     * we can roll back and re-raise.  Restored before we return. */
    u32 saved_try_depth = vm->try_depth;
    vm->try_depth = 0;
    int  ret_count = cando_vm_call_value(vm, args[1],
                                          argc > 2 ? &args[2] : NULL,
                                          (u32)(argc > 2 ? argc - 2 : 0));
    vm->try_depth = saved_try_depth;

    /* If the call propagated an error, roll back and bubble. */
    if (vm->has_error) {
        char rb_sql[96];
        if (depth == 0) {
            snprintf(rb_sql, sizeof(rb_sql), "ROLLBACK");
        } else {
            snprintf(rb_sql, sizeof(rb_sql),
                     "ROLLBACK TO cdo_sp_%d; RELEASE cdo_sp_%d",
                     depth, depth);
        }
        sqlite3_exec(db, rb_sql, NULL, NULL, NULL);
        g_db_pool[slot].tx_depth = depth;
        return -1;       /* propagate the existing error */
    }

    char close_sql[64];
    if (depth == 0) {
        snprintf(close_sql, sizeof(close_sql), "COMMIT");
    } else {
        snprintf(close_sql, sizeof(close_sql), "RELEASE cdo_sp_%d", depth);
    }
    rc = sqlite3_exec(db, close_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.transaction: commit failed: %s",
                     sqlite3_errmsg(db));
        g_db_pool[slot].tx_depth = depth;
        return -1;
    }
    g_db_pool[slot].tx_depth = depth;

    /* Forward the user fn's return values: ret_count are already on
     * the stack thanks to cando_vm_call_value. */
    return ret_count;
}

/* =========================================================================
 * native_sqlite_pragma(db, name[, value]) -> [row, ...]
 *
 * Always returns an array of row objects -- scripts that want the
 * scalar form do `db:pragma("journal_mode")[0].journal_mode`.  Setter
 * form `db:pragma("foo", val)` accepts numbers, booleans, and a
 * restricted subset of strings (alphanumeric / underscore / hyphen)
 * so the value can be safely interpolated into the PRAGMA statement.
 * ======================================================================= */

static bool pragma_value_safe(const char *s, size_t n)
{
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

static int native_sqlite_pragma(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sqlite_throw(vm, NULL, 0, "sqlite.pragma: (db, name[, value]) required");
        return -1;
    }
    sqlite3 *db = db_handle_unwrap(vm, args[0]);
    if (!db) return -1;

    const char *name = libutil_arg_cstr_at(args, argc, 1);
    if (!name || !pragma_value_safe(name, strlen(name))) {
        sqlite_throw(vm, NULL, 0,
            "sqlite.pragma: name must be alphanumeric / underscore");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    bool bigint_string = (slot >= 0) ? g_db_pool[slot].bigint_string : false;

    char sql[256];
    if (argc >= 3) {
        /* Setter: PRAGMA <name> = <value> */
        char value_buf[64];
        if (cando_is_bool(args[2])) {
            snprintf(value_buf, sizeof(value_buf),
                     args[2].as.boolean ? "ON" : "OFF");
        } else if (cando_is_number(args[2])) {
            f64 d = args[2].as.number;
            if (d == (f64)(int64_t)d) {
                snprintf(value_buf, sizeof(value_buf), "%lld",
                         (long long)d);
            } else {
                snprintf(value_buf, sizeof(value_buf), "%g", d);
            }
        } else if (cando_is_string(args[2])) {
            const char *vs = args[2].as.string->data;
            size_t      vn = args[2].as.string->length;
            if (!pragma_value_safe(vs, vn) || vn >= sizeof(value_buf)) {
                sqlite_throw(vm, NULL, 0,
                    "sqlite.pragma: value must be alphanumeric / underscore");
                return -1;
            }
            memcpy(value_buf, vs, vn);
            value_buf[vn] = '\0';
        } else {
            sqlite_throw(vm, NULL, 0,
                "sqlite.pragma: value must be number, bool, or simple string");
            return -1;
        }
        snprintf(sql, sizeof(sql), "PRAGMA %s = %s", name, value_buf);
    } else {
        snprintf(sql, sizeof(sql), "PRAGMA %s", name);
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK || !st) {
        if (st) sqlite3_finalize(st);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.pragma: %s", sqlite3_errmsg(db));
        return -1;
    }

    CandoValue av = cando_bridge_new_array(vm);
    CdoObject *a  = cando_bridge_resolve(vm, av.as.handle);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cdo_array_push(a, cando_bridge_to_cdo(vm,
            build_row(vm, st, bigint_string)));
    }
    if (rc != SQLITE_DONE) {
        char tmp[480];
        snprintf(tmp, sizeof(tmp), "%s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        sqlite_throw(vm, db, sqlite3_extended_errcode(db),
                     "sqlite.pragma: %s", tmp);
        return -1;
    }
    sqlite3_finalize(st);
    cando_vm_push(vm, av);
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
    register_method(vm, obj, "open",          native_sqlite_open,
                    METHOD_MODULE_ONLY);

    /* db handle methods: callable as both `sql.close(db)` and `db:close()`. */
    register_method(vm, obj, "close",         native_sqlite_close,
                    METHOD_ON_DB);
    register_method(vm, obj, "exec",          native_sqlite_exec,
                    METHOD_ON_DB);
    register_method(vm, obj, "prepare",       native_sqlite_prepare,
                    METHOD_ON_DB);
    register_method(vm, obj, "bigintMode",    native_sqlite_bigint_mode,
                    METHOD_ON_DB);
    register_method(vm, obj, "setReadBigInts", native_sqlite_set_read_bigints,
                    METHOD_ON_DB);

    /* stmt handle methods: callable as `sql.run(stmt, ...)` or `stmt:run(...)`. */
    register_method(vm, obj, "run",         native_sqlite_run,          METHOD_ON_STMT);
    register_method(vm, obj, "get",         native_sqlite_get,          METHOD_ON_STMT);
    register_method(vm, obj, "all",         native_sqlite_all,          METHOD_ON_STMT);
    register_method(vm, obj, "bind",        native_sqlite_bind,         METHOD_ON_STMT);
    register_method(vm, obj, "reset",       native_sqlite_reset,        METHOD_ON_STMT);
    register_method(vm, obj, "finalize",    native_sqlite_finalize,     METHOD_ON_STMT);
    register_method(vm, obj, "expandedSQL", native_sqlite_expanded_sql, METHOD_ON_STMT);
    register_method(vm, obj, "columns",     native_sqlite_columns,      METHOD_ON_STMT);
    register_method(vm, obj, "iterate",     native_sqlite_iterate,      METHOD_ON_STMT);

    /* Iterator handle method.  Only `next` is attached so iter:next()
     * works; the iterator's `done` field is a plain boolean property. */
    register_method(vm, obj, "next",        native_sqlite_iter_next,    METHOD_ON_ITER);

    /* Transaction primitives + pragma helper -- on db handles. */
    register_method(vm, obj, "begin",         native_sqlite_begin,         METHOD_ON_DB);
    register_method(vm, obj, "commit",        native_sqlite_commit,        METHOD_ON_DB);
    register_method(vm, obj, "rollback",      native_sqlite_rollback,      METHOD_ON_DB);
    register_method(vm, obj, "inTransaction", native_sqlite_in_transaction, METHOD_ON_DB);
    register_method(vm, obj, "transaction",   native_sqlite_transaction,   METHOD_ON_DB);
    register_method(vm, obj, "pragma",        native_sqlite_pragma,        METHOD_ON_DB);

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
