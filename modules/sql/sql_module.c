/*
 * modules/sql/sql_module.c -- CanDo SQL binary module.
 *
 * Loaded into a script with:
 *
 *     VAR sql = include("./sql.so");          // Linux / macOS
 *     VAR sql = include("./sql.dll");          // Windows
 *
 * Exposes one combined surface that talks to either PostgreSQL or
 * MySQL/MariaDB.  Both drivers are implemented in pure C against the
 * native wire protocols -- there is NO runtime dependency on libpq,
 * libmysqlclient, or any other third-party library.  OpenSSL is used
 * via libcando, which already links it for the socket/http modules,
 * so a fresh CanDo install can connect to both engines without the
 * user installing anything.
 *
 * API mirrors the SQLite module's shape (open / exec / prepare / run /
 * get / all / iterate / transactions) so scripts that already use
 * `sqlite.so` stay portable.  Both call styles work on every method:
 *
 *     sql.exec(db, "...")                    // function-style
 *     db:exec("...")                          // method-style
 *
 * Threading: handles are guarded by an internal mutex around every
 * round-trip, so a single db handle can be shared across `thread {...}`
 * blocks.  For better concurrency open one handle per thread.  No
 * async API is provided -- scripts compose threads themselves.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/array.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"

#include "sql_driver.h"
#include "sql_pg.h"
#include "sql_mysql.h"
#include "sql_escape.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION sql_mutex_t;
#  define SQL_MUTEX_INIT(m)   InitializeCriticalSection(m)
#  define SQL_MUTEX_LOCK(m)   EnterCriticalSection(m)
#  define SQL_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#else
#  include <pthread.h>
   typedef pthread_mutex_t sql_mutex_t;
#  define SQL_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#  define SQL_MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define SQL_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#endif

#define SQL_MODULE_VERSION "0.1.0"

#define INTEGER_SAFE_MAX 9007199254740992.0  /* 2^53 */

/* =========================================================================
 * Connection slot pool
 *
 * Script-facing handles carry a single number field `__sql_db_slot`
 * that indexes into the static pool below.  A slot stores either a
 * PgConn or MyConn plus the driver tag so dispatch can pick the right
 * vtable.  Each slot owns a per-connection mutex so cross-thread use
 * is safe.
 * ===================================================================== */

#define SQL_MAX_DBS    256
#define SQL_DB_SLOT_KEY "__sql_db_slot"

typedef struct SqlSlot {
    bool          in_use;
    SqlDriverKind driver;
    PgConn        pg;
    MyConn        my;
    sql_mutex_t   lock;
    bool          bigint_string;
    int           tx_depth;       /* 0 = none; 1 = BEGIN; >1 = SAVEPOINT */
} SqlSlot;

#define SQL_MAX_STMTS    1024
#define SQL_STMT_SLOT_KEY "__sql_stmt_slot"
#define SQL_STMT_SQL_KEY  "sourceSQL"

typedef struct SqlStmtSlot {
    bool   in_use;
    int    db_slot;
    /* For Postgres, statement is referenced by server-side name. */
    char  *pg_name;
    PgPreparedDesc pg_desc;
    /* For MySQL, statement carries server stmt_id + cached column types. */
    MyStmt my_stmt;
    char  *sql;
} SqlStmtSlot;

static SqlSlot      g_db_pool[SQL_MAX_DBS];
static SqlStmtSlot  g_stmt_pool[SQL_MAX_STMTS];
static sql_mutex_t  g_pool_mutex;
static _Atomic(int) g_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_pool_inited, &expected, 1)) {
        SQL_MUTEX_INIT(&g_pool_mutex);
        for (int i = 0; i < SQL_MAX_DBS; i++) {
            memset(&g_db_pool[i], 0, sizeof(g_db_pool[i]));
            SQL_MUTEX_INIT(&g_db_pool[i].lock);
        }
        for (int i = 0; i < SQL_MAX_STMTS; i++) {
            memset(&g_stmt_pool[i], 0, sizeof(g_stmt_pool[i]));
        }
    }
}

static int db_pool_alloc(void)
{
    ensure_pool_inited();
    SQL_MUTEX_LOCK(&g_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SQL_MAX_DBS; i++) {
        if (!g_db_pool[i].in_use) {
            g_db_pool[i].in_use   = true;
            g_db_pool[i].tx_depth = 0;
            g_db_pool[i].bigint_string = false;
            idx = i;
            break;
        }
    }
    SQL_MUTEX_UNLOCK(&g_pool_mutex);
    return idx;
}

static void db_pool_release(int idx)
{
    if (idx < 0 || idx >= SQL_MAX_DBS) return;
    SQL_MUTEX_LOCK(&g_pool_mutex);
    g_db_pool[idx].in_use = false;
    g_db_pool[idx].tx_depth = 0;
    g_db_pool[idx].bigint_string = false;
    SQL_MUTEX_UNLOCK(&g_pool_mutex);
}

static int stmt_pool_alloc(int db_slot)
{
    ensure_pool_inited();
    SQL_MUTEX_LOCK(&g_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SQL_MAX_STMTS; i++) {
        if (!g_stmt_pool[i].in_use) {
            memset(&g_stmt_pool[i], 0, sizeof(g_stmt_pool[i]));
            g_stmt_pool[i].in_use  = true;
            g_stmt_pool[i].db_slot = db_slot;
            idx = i;
            break;
        }
    }
    SQL_MUTEX_UNLOCK(&g_pool_mutex);
    return idx;
}

static void stmt_pool_release(int idx)
{
    if (idx < 0 || idx >= SQL_MAX_STMTS) return;
    SQL_MUTEX_LOCK(&g_pool_mutex);
    SqlStmtSlot *s = &g_stmt_pool[idx];
    free(s->pg_name);
    free(s->sql);
    pg_prepared_desc_free(&s->pg_desc);
    my_stmt_free(&s->my_stmt);
    memset(s, 0, sizeof(*s));
    SQL_MUTEX_UNLOCK(&g_pool_mutex);
}

/* =========================================================================
 * Multi-value error throws
 *
 * `CATCH (msg, code, sqlstate)` -- error_vals[0]=msg, [1]=code, [2]=sqlstate.
 * ===================================================================== */

extern void cando_value_release(CandoValue v);

static void sql_attach_extra(CandoVM *vm, int code, const char *sqlstate)
{
    cando_value_release(vm->error_vals[1]);
    vm->error_vals[1] = cando_number((f64)code);

    cando_value_release(vm->error_vals[2]);
    const char *s = sqlstate ? sqlstate : "";
    CandoString *cs = cando_string_new(s, (u32)strlen(s));
    vm->error_vals[2] = cando_string_value(cs);

    if (vm->error_val_count < 3) vm->error_val_count = 3;
}

static void sql_throw(CandoVM *vm, int code, const char *sqlstate,
                      const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    cando_vm_error(vm, "%s", buf);
    sql_attach_extra(vm, code, sqlstate);
}

static void sql_throw_from_err(CandoVM *vm, const char *prefix,
                               const SqlError *e)
{
    cando_vm_error(vm, "%s: %s", prefix, e->message);
    sql_attach_extra(vm, e->code, e->sqlstate);
}

/* =========================================================================
 * Object helpers (mirrors the SQLite module pattern)
 * ===================================================================== */

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

__attribute__((unused))
static void obj_set_bool_field(CdoObject *obj, const char *key, bool value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(value), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Method registry (function-style + method-style call shapes)
 * ===================================================================== */

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
    f64 s = cando_is_number(sentinel) ? cando_as_number(sentinel) : 0.0;
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
 * DB / stmt handle wrappers
 * ===================================================================== */

static CandoValue make_db_handle(CandoVM *vm, int slot, SqlDriverKind drv)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    obj_set_number(obj, SQL_DB_SLOT_KEY, (f64)slot);
    obj_set_string(obj, "driver",
                   drv == SQL_DRIVER_POSTGRES ? "postgres" : "mysql",
                   drv == SQL_DRIVER_POSTGRES ? 8 : 5);
    attach_methods_for(obj, METHOD_ON_DB);
    return v;
}

static int db_handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    f64 idx = -1.0;
    if (!obj_get_number(obj, SQL_DB_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= SQL_MAX_DBS) return -1;
    if (!g_db_pool[i].in_use) return -1;
    return i;
}

static SqlSlot *db_handle_unwrap(CandoVM *vm, CandoValue v)
{
    int slot = db_handle_slot(vm, v);
    if (slot < 0) {
        sql_throw(vm, 0, "", "sql: expected database handle");
        return NULL;
    }
    return &g_db_pool[slot];
}

static CandoValue make_stmt_handle(CandoVM *vm, int slot,
                                   const char *sql, u32 sql_len)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    obj_set_number(obj, SQL_STMT_SLOT_KEY, (f64)slot);
    obj_set_string(obj, SQL_STMT_SQL_KEY, sql, sql_len);
    attach_methods_for(obj, METHOD_ON_STMT);
    return v;
}

static int stmt_handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    f64 idx = -1.0;
    if (!obj_get_number(obj, SQL_STMT_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= SQL_MAX_STMTS) return -1;
    if (!g_stmt_pool[i].in_use) return -1;
    return i;
}

/* =========================================================================
 * Driver value -> Cando value
 *
 * Drivers hand us SqlValue cells.  Most cells from PostgreSQL come back
 * as TEXT (text-format protocol), so the script layer is responsible
 * for parsing numeric / boolean text.  We use the column's type_oid
 * to pick the right reification.  MySQL binary-protocol values arrive
 * already typed (INTEGER / DOUBLE / TEXT / BLOB / NULL).
 * ===================================================================== */

/* PostgreSQL OIDs we treat as numeric / boolean.  See pg_type.h. */
#define PG_OID_BOOL        16
#define PG_OID_BYTEA       17
#define PG_OID_INT8        20
#define PG_OID_INT2        21
#define PG_OID_INT4        23
#define PG_OID_TEXT        25
#define PG_OID_OID         26
#define PG_OID_FLOAT4      700
#define PG_OID_FLOAT8      701
#define PG_OID_NUMERIC     1700
#define PG_OID_VARCHAR     1043
#define PG_OID_BPCHAR      1042

static bool oid_is_pg_integer(uint32_t oid)
{
    switch (oid) {
        case PG_OID_INT2: case PG_OID_INT4: case PG_OID_INT8:
        case PG_OID_OID:
            return true;
        default: return false;
    }
}
static bool oid_is_pg_float(uint32_t oid)
{
    return oid == PG_OID_FLOAT4 || oid == PG_OID_FLOAT8 || oid == PG_OID_NUMERIC;
}

static CandoValue cell_to_cando_pg(CandoVM *vm, const SqlValue *cell,
                                   uint32_t pg_oid, bool bigint_string)
{
    if (cell->kind == SQL_VAL_NULL) return cando_null();
    if (cell->kind != SQL_VAL_TEXT && cell->kind != SQL_VAL_BLOB)
        return cando_null();

    if (pg_oid == PG_OID_BOOL && cell->len >= 1) {
        return cando_bool(cell->data[0] == 't' || cell->data[0] == '1');
    }
    if (oid_is_pg_integer(pg_oid)) {
        char tmp[32];
        size_t n = cell->len < sizeof(tmp) - 1 ? cell->len : sizeof(tmp) - 1;
        memcpy(tmp, cell->data, n);
        tmp[n] = '\0';
        char *end = NULL;
        long long v = strtoll(tmp, &end, 10);
        if (end == tmp) {
            CandoString *s = cando_string_new(cell->data, (u32)cell->len);
            return cando_string_value(s);
        }
        if (bigint_string && (v >  (long long)INTEGER_SAFE_MAX
                           || v < -(long long)INTEGER_SAFE_MAX)) {
            CandoString *s = cando_string_new(cell->data, (u32)cell->len);
            return cando_string_value(s);
        }
        return cando_number((f64)v);
    }
    if (oid_is_pg_float(pg_oid)) {
        char tmp[64];
        size_t n = cell->len < sizeof(tmp) - 1 ? cell->len : sizeof(tmp) - 1;
        memcpy(tmp, cell->data, n);
        tmp[n] = '\0';
        return cando_number(strtod(tmp, NULL));
    }
    /* Default: pass through as a Cando string (UTF-8 / bytes). */
    (void)vm;
    CandoString *s = cando_string_new(cell->data, (u32)cell->len);
    return cando_string_value(s);
}

static CandoValue cell_to_cando_my(CandoVM *vm, const SqlValue *cell,
                                   bool bigint_string)
{
    switch (cell->kind) {
        case SQL_VAL_NULL: return cando_null();
        case SQL_VAL_BOOL: return cando_bool(cell->i64 ? true : false);
        case SQL_VAL_INTEGER: {
            int64_t v = cell->i64;
            if (bigint_string && (v >  (int64_t)INTEGER_SAFE_MAX
                               || v < -(int64_t)INTEGER_SAFE_MAX)) {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
                CandoString *s = cando_string_new(buf, (u32)n);
                return cando_string_value(s);
            }
            return cando_number((f64)v);
        }
        case SQL_VAL_DOUBLE: return cando_number(cell->f64);
        case SQL_VAL_TEXT:
        case SQL_VAL_BLOB: {
            (void)vm;
            CandoString *s = cando_string_new(cell->data, (u32)cell->len);
            return cando_string_value(s);
        }
    }
    return cando_null();
}

/* =========================================================================
 * Cando value -> SqlParam (binding)
 *
 * Same shape as the SQLite module:
 *   null      -> SQL_PARAM_NULL
 *   bool      -> SQL_PARAM_BOOL
 *   number    -> SQL_PARAM_INT64 if whole and within ±2^53, else DOUBLE
 *   string    -> SQL_PARAM_TEXT
 *   { blob: <string> } -> SQL_PARAM_BLOB
 * ===================================================================== */

static bool param_from_cando(CandoVM *vm, CandoValue v, SqlParam *out,
                             const char *what, int idx)
{
    if (cando_is_null(v)) { out->kind = SQL_PARAM_NULL; return true; }
    if (cando_is_bool(v)) {
        out->kind = SQL_PARAM_BOOL;
        out->i64  = cando_as_bool(v) ? 1 : 0;
        return true;
    }
    if (cando_is_number(v)) {
        f64 d = cando_as_number(v);
        if (isfinite(d) && fabs(d) <= INTEGER_SAFE_MAX && d == (f64)(int64_t)d) {
            out->kind = SQL_PARAM_INT64;
            out->i64  = (int64_t)d;
        } else {
            out->kind = SQL_PARAM_DOUBLE;
            out->f64  = d;
        }
        return true;
    }
    if (cando_is_string(v)) {
        out->kind = SQL_PARAM_TEXT;
        out->data = cando_as_string(v)->data;
        out->len  = cando_as_string(v)->length;
        return true;
    }
    if (cando_is_object(v)) {
        CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
        const char *bd = NULL; size_t bl = 0;
        if (obj_get_string(o, "blob", &bd, &bl)) {
            out->kind = SQL_PARAM_BLOB;
            out->data = bd;
            out->len  = bl;
            return true;
        }
    }
    sql_throw(vm, 0, "", "%s: parameter %d has unsupported type", what, idx + 1);
    return false;
}

/* Convert one or more args at args[start..argc-1] into a SqlParam[].
 * Supports the array shape: stmt:run([a, b, c]).  Returns -1 on error.
 * On success, returns count and writes the heap-allocated array into
 * *out_arr (caller frees). */
static int collect_params(CandoVM *vm, int argc, CandoValue *args, int start,
                          SqlParam **out_arr, const char *what)
{
    *out_arr = NULL;
    int n_provided = argc - start;
    /* Single-arg array shape. */
    if (n_provided == 1 && cando_is_object(args[start])) {
        CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(args[start]));
        if (o->kind == OBJ_ARRAY) {
            u32 n = cdo_array_len(o);
            SqlParam *arr = (SqlParam *)calloc(n > 0 ? n : 1, sizeof(SqlParam));
            if (!arr) {
                sql_throw(vm, 0, "", "%s: out of memory", what);
                return -1;
            }
            for (u32 i = 0; i < n; i++) {
                CdoValue ev;
                if (!cdo_array_rawget_idx(o, (u32)i, &ev)) continue;
                CandoValue cv = cando_bridge_to_cando(vm, ev);
                if (!param_from_cando(vm, cv, &arr[i], what, (int)i)) {
                    free(arr);
                    return -1;
                }
            }
            *out_arr = arr;
            return (int)n;
        }
    }
    if (n_provided <= 0) return 0;
    SqlParam *arr = (SqlParam *)calloc((size_t)n_provided, sizeof(SqlParam));
    if (!arr) {
        sql_throw(vm, 0, "", "%s: out of memory", what);
        return -1;
    }
    for (int i = 0; i < n_provided; i++) {
        if (!param_from_cando(vm, args[start + i], &arr[i], what, i)) {
            free(arr);
            return -1;
        }
    }
    *out_arr = arr;
    return n_provided;
}

/* =========================================================================
 * Build SqlConnectOpts from the script-side options object.
 * ===================================================================== */

static void fill_opts(CandoVM *vm, CandoValue v, SqlConnectOpts *out,
                      bool is_pg)
{
    memset(out, 0, sizeof(*out));
    out->charset = "utf8mb4";
    out->connect_timeout_ms = 10000;
    out->io_timeout_ms      = 0;        /* blocking */
    out->port               = is_pg ? 5432 : 3306;

    if (!cando_is_object(v)) return;
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    const char *s; size_t sl; bool b; f64 n;
    if (obj_get_string(o, "host",     &s, &sl)) out->host     = s;
    if (obj_get_string(o, "user",     &s, &sl)) out->user     = s;
    if (obj_get_string(o, "username", &s, &sl)) out->user     = s;
    if (obj_get_string(o, "password", &s, &sl)) out->password = s;
    if (obj_get_string(o, "database", &s, &sl)) out->database = s;
    if (obj_get_string(o, "applicationName", &s, &sl))
        out->application_name = s;
    if (obj_get_number(o, "port",     &n)) out->port = (int)n;
    if (obj_get_number(o, "connectTimeout", &n)) out->connect_timeout_ms = (int)n;
    if (obj_get_number(o, "ioTimeout",      &n)) out->io_timeout_ms      = (int)n;
    if (obj_get_bool  (o, "tls",            &b)) out->tls        = b;
    if (obj_get_bool  (o, "tlsVerify",      &b)) out->tls_verify = b;
    if (obj_get_string(o, "tlsCa",          &s, &sl)) out->tls_ca_pem = s;
    if (obj_get_string(o, "tlsClientCert",  &s, &sl)) out->tls_client_cert = s;
    if (obj_get_string(o, "tlsClientKey",   &s, &sl)) out->tls_client_key  = s;
    if (obj_get_string(o, "charset",        &s, &sl)) out->charset = s;
}

/* =========================================================================
 * Open / Close
 * ===================================================================== */

static int native_sql_open(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.open: (driver, options) required");
        return -1;
    }
    const char *drvname = libutil_arg_cstr_at(args, argc, 0);
    if (!drvname) {
        sql_throw(vm, 0, "",
            "sql.open: first argument must be the driver name "
            "(\"postgres\" or \"mysql\")");
        return -1;
    }
    SqlDriverKind drv;
    if (strcmp(drvname, "postgres") == 0 || strcmp(drvname, "pg") == 0
     || strcmp(drvname, "postgresql") == 0) drv = SQL_DRIVER_POSTGRES;
    else if (strcmp(drvname, "mysql") == 0 || strcmp(drvname, "mariadb") == 0)
        drv = SQL_DRIVER_MYSQL;
    else {
        sql_throw(vm, 0, "", "sql.open: unknown driver \"%s\"", drvname);
        return -1;
    }

    SqlConnectOpts opts;
    fill_opts(vm, argc >= 2 ? args[1] : cando_null(), &opts,
              drv == SQL_DRIVER_POSTGRES);

    int slot = db_pool_alloc();
    if (slot < 0) {
        sql_throw(vm, 0, "",
            "sql.open: connection pool exhausted (max %d)", SQL_MAX_DBS);
        return -1;
    }
    g_db_pool[slot].driver = drv;

    bool ok;
    if (drv == SQL_DRIVER_POSTGRES) {
        ok = pg_connect(&g_db_pool[slot].pg, &opts);
        if (!ok) {
            SqlError e = g_db_pool[slot].pg.last_err;
            db_pool_release(slot);
            sql_throw_from_err(vm, "sql.open", &e);
            return -1;
        }
    } else {
        ok = my_connect(&g_db_pool[slot].my, &opts);
        if (!ok) {
            SqlError e = g_db_pool[slot].my.last_err;
            db_pool_release(slot);
            sql_throw_from_err(vm, "sql.open", &e);
            return -1;
        }
    }

    cando_vm_push(vm, make_db_handle(vm, slot, drv));
    return 1;
}

/* Module-level convenience wrappers. */
static int native_sql_open_pg(CandoVM *vm, int argc, CandoValue *args)
{
    /* Insert "postgres" as first arg and dispatch. */
    CandoValue extra[1 + 16];
    int n = argc < 16 ? argc : 16;
    extra[0] = cando_string_value(cando_string_new("postgres", 8));
    for (int i = 0; i < n; i++) extra[i + 1] = args[i];
    return native_sql_open(vm, n + 1, extra);
}

static int native_sql_open_mysql(CandoVM *vm, int argc, CandoValue *args)
{
    CandoValue extra[1 + 16];
    int n = argc < 16 ? argc : 16;
    extra[0] = cando_string_value(cando_string_new("mysql", 5));
    for (int i = 0; i < n; i++) extra[i + 1] = args[i];
    return native_sql_open(vm, n + 1, extra);
}

static int native_sql_close(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.close: (db) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    SqlSlot *s = &g_db_pool[slot];
    SQL_MUTEX_LOCK(&s->lock);
    if (s->driver == SQL_DRIVER_POSTGRES) pg_close(&s->pg);
    else                                  my_close(&s->my);
    SQL_MUTEX_UNLOCK(&s->lock);

    db_pool_release(slot);
    if (cando_is_object(args[0])) {
        CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
        obj_set_number(obj, SQL_DB_SLOT_KEY, -1.0);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * exec(db, sql) -> { affected, insertId, tag }
 * ===================================================================== */

static int native_sql_exec(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sql_throw(vm, 0, "", "sql.exec: (db, sql) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    const char *sql = libutil_arg_cstr_at(args, argc, 1);
    if (!sql) {
        sql_throw(vm, 0, "", "sql.exec: sql must be a string");
        return -1;
    }

    int64_t affected = 0;
    int64_t insert_id = 0;
    const char *tag = "";
    bool ok;

    SQL_MUTEX_LOCK(&slot->lock);
    if (slot->driver == SQL_DRIVER_POSTGRES) {
        PgResult res;
        pg_result_init(&res);
        ok = pg_query(&slot->pg, sql, &res);
        if (ok) {
            affected = res.affected;
            tag      = res.cmd_tag;
        }
        pg_result_free(&res);
        if (!ok) {
            SqlError e = slot->pg.last_err;
            SQL_MUTEX_UNLOCK(&slot->lock);
            sql_throw_from_err(vm, "sql.exec", &e);
            return -1;
        }
    } else {
        MyResult res;
        my_result_init(&res);
        ok = my_query(&slot->my, sql, &res);
        if (ok) {
            affected   = (int64_t)res.affected;
            insert_id  = (int64_t)res.insert_id;
        }
        my_result_free(&res);
        if (!ok) {
            SqlError e = slot->my.last_err;
            SQL_MUTEX_UNLOCK(&slot->lock);
            sql_throw_from_err(vm, "sql.exec", &e);
            return -1;
        }
    }
    SQL_MUTEX_UNLOCK(&slot->lock);

    CandoValue rv  = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(rv));
    obj_set_number(obj, "affected", (f64)affected);
    obj_set_number(obj, "insertId", (f64)insert_id);
    if (tag && tag[0]) obj_set_string(obj, "tag", tag, (u32)strlen(tag));
    cando_vm_push(vm, rv);
    return 1;
}

/* =========================================================================
 * Build a single row-object from driver cells.
 * ===================================================================== */

static CandoValue build_row_pg(CandoVM *vm, PgResult *res, int row_idx,
                               bool bigint_string)
{
    PgRow *r = &res->rows[row_idx];
    CandoValue ov = cando_bridge_new_object(vm);
    CdoObject *o  = cando_bridge_resolve(vm, cando_as_handle(ov));
    for (int c = 0; c < r->ncols && c < res->ncols; c++) {
        const char *name = res->columns[c].name;
        CandoValue cv = cell_to_cando_pg(vm, &r->cells[c],
                                         res->columns[c].type_oid,
                                         bigint_string);
        CdoString *k = cdo_string_intern(name, (u32)strlen(name));
        cdo_object_rawset(o, k, cando_bridge_to_cdo(vm, cv), FIELD_NONE);
        cdo_string_release(k);
    }
    return ov;
}

static CandoValue build_row_my(CandoVM *vm, MyResult *res, int row_idx,
                               bool bigint_string)
{
    MyRow *r = &res->rows[row_idx];
    CandoValue ov = cando_bridge_new_object(vm);
    CdoObject *o  = cando_bridge_resolve(vm, cando_as_handle(ov));
    for (int c = 0; c < r->ncols && c < res->ncols; c++) {
        const char *name = res->columns[c].name;
        CandoValue cv = cell_to_cando_my(vm, &r->cells[c], bigint_string);
        CdoString *k = cdo_string_intern(name, (u32)strlen(name));
        cdo_object_rawset(o, k, cando_bridge_to_cdo(vm, cv), FIELD_NONE);
        cdo_string_release(k);
    }
    return ov;
}

/* =========================================================================
 * prepare(db, sql) -> stmt
 * ===================================================================== */

static int native_sql_prepare(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sql_throw(vm, 0, "", "sql.prepare: (db, sql) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    int db_slot = db_handle_slot(vm, args[0]);
    const char *sql = libutil_arg_cstr_at(args, argc, 1);
    if (!sql) {
        sql_throw(vm, 0, "", "sql.prepare: sql must be a string");
        return -1;
    }

    int idx = stmt_pool_alloc(db_slot);
    if (idx < 0) {
        sql_throw(vm, 0, "",
            "sql.prepare: statement pool exhausted (max %d)",
            SQL_MAX_STMTS);
        return -1;
    }
    SqlStmtSlot *st = &g_stmt_pool[idx];
    st->sql = strdup(sql);

    SQL_MUTEX_LOCK(&slot->lock);
    bool ok;
    if (slot->driver == SQL_DRIVER_POSTGRES) {
        ok = pg_prepare(&slot->pg, sql, &st->pg_name, &st->pg_desc);
        if (!ok) {
            SqlError e = slot->pg.last_err;
            SQL_MUTEX_UNLOCK(&slot->lock);
            stmt_pool_release(idx);
            sql_throw_from_err(vm, "sql.prepare", &e);
            return -1;
        }
    } else {
        ok = my_prepare(&slot->my, sql, &st->my_stmt);
        if (!ok) {
            SqlError e = slot->my.last_err;
            SQL_MUTEX_UNLOCK(&slot->lock);
            stmt_pool_release(idx);
            sql_throw_from_err(vm, "sql.prepare", &e);
            return -1;
        }
    }
    SQL_MUTEX_UNLOCK(&slot->lock);

    cando_vm_push(vm, make_stmt_handle(vm, idx, sql, (u32)strlen(sql)));
    return 1;
}

/* =========================================================================
 * Internal: execute a prepared stmt with given params, return result
 * via callback or pointer.  Returns true on success.  Caller frees
 * results with the appropriate free() helper.
 * ===================================================================== */

static bool exec_prepared_pg(CandoVM *vm, SqlSlot *slot, SqlStmtSlot *st,
                             const SqlParam *params, int nparams,
                             PgResult *out, const char *what)
{
    SQL_MUTEX_LOCK(&slot->lock);
    bool ok = pg_execute_prepared(&slot->pg, st->pg_name,
                                  params, nparams, out);
    if (!ok) {
        SqlError e = slot->pg.last_err;
        SQL_MUTEX_UNLOCK(&slot->lock);
        sql_throw_from_err(vm, what, &e);
        return false;
    }
    SQL_MUTEX_UNLOCK(&slot->lock);
    return true;
}

static bool exec_prepared_my(CandoVM *vm, SqlSlot *slot, SqlStmtSlot *st,
                             const SqlParam *params, int nparams,
                             MyResult *out, const char *what)
{
    SQL_MUTEX_LOCK(&slot->lock);
    bool ok = my_execute(&slot->my, &st->my_stmt, params, nparams, out);
    if (!ok) {
        SqlError e = slot->my.last_err;
        SQL_MUTEX_UNLOCK(&slot->lock);
        sql_throw_from_err(vm, what, &e);
        return false;
    }
    SQL_MUTEX_UNLOCK(&slot->lock);
    return true;
}

/* =========================================================================
 * stmt:run(...params) -> { affected, insertId }
 * ===================================================================== */

static int native_sql_run(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.run: (stmt, ...params) required");
        return -1;
    }
    int idx = stmt_handle_slot(vm, args[0]);
    if (idx < 0) {
        sql_throw(vm, 0, "", "sql.run: expected statement handle");
        return -1;
    }
    SqlStmtSlot *st = &g_stmt_pool[idx];
    SqlSlot *slot = &g_db_pool[st->db_slot];
    if (!slot->in_use) {
        sql_throw(vm, 0, "", "sql.run: parent database closed");
        return -1;
    }
    SqlParam *params = NULL;
    int nparams = collect_params(vm, argc, args, 1, &params, "sql.run");
    if (nparams < 0) return -1;

    int64_t affected = 0, insert_id = 0;
    if (slot->driver == SQL_DRIVER_POSTGRES) {
        PgResult res;
        pg_result_init(&res);
        bool ok = exec_prepared_pg(vm, slot, st, params, nparams, &res,
                                   "sql.run");
        if (ok) affected = res.affected;
        pg_result_free(&res);
        if (!ok) { free(params); return -1; }
    } else {
        MyResult res;
        my_result_init(&res);
        bool ok = exec_prepared_my(vm, slot, st, params, nparams, &res,
                                   "sql.run");
        if (ok) {
            affected  = (int64_t)res.affected;
            insert_id = (int64_t)res.insert_id;
        }
        my_result_free(&res);
        if (!ok) { free(params); return -1; }
    }
    free(params);

    CandoValue rv  = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(rv));
    obj_set_number(obj, "affected", (f64)affected);
    obj_set_number(obj, "insertId", (f64)insert_id);
    cando_vm_push(vm, rv);
    return 1;
}

/* =========================================================================
 * stmt:get(...params) -> row | null
 * stmt:all(...params) -> array of rows
 * ===================================================================== */

static int native_sql_get(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.get: (stmt, ...params) required");
        return -1;
    }
    int idx = stmt_handle_slot(vm, args[0]);
    if (idx < 0) {
        sql_throw(vm, 0, "", "sql.get: expected statement handle");
        return -1;
    }
    SqlStmtSlot *st = &g_stmt_pool[idx];
    SqlSlot *slot = &g_db_pool[st->db_slot];
    if (!slot->in_use) {
        sql_throw(vm, 0, "", "sql.get: parent database closed");
        return -1;
    }
    SqlParam *params = NULL;
    int nparams = collect_params(vm, argc, args, 1, &params, "sql.get");
    if (nparams < 0) return -1;

    if (slot->driver == SQL_DRIVER_POSTGRES) {
        PgResult res;
        pg_result_init(&res);
        bool ok = exec_prepared_pg(vm, slot, st, params, nparams, &res,
                                   "sql.get");
        free(params);
        if (!ok) { pg_result_free(&res); return -1; }
        if (res.nrows == 0) {
            pg_result_free(&res);
            cando_vm_push(vm, cando_null());
            return 1;
        }
        CandoValue row = build_row_pg(vm, &res, 0, slot->bigint_string);
        pg_result_free(&res);
        cando_vm_push(vm, row);
        return 1;
    } else {
        MyResult res;
        my_result_init(&res);
        bool ok = exec_prepared_my(vm, slot, st, params, nparams, &res,
                                   "sql.get");
        free(params);
        if (!ok) { my_result_free(&res); return -1; }
        if (res.nrows == 0) {
            my_result_free(&res);
            cando_vm_push(vm, cando_null());
            return 1;
        }
        CandoValue row = build_row_my(vm, &res, 0, slot->bigint_string);
        my_result_free(&res);
        cando_vm_push(vm, row);
        return 1;
    }
}

static int native_sql_all(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.all: (stmt, ...params) required");
        return -1;
    }
    int idx = stmt_handle_slot(vm, args[0]);
    if (idx < 0) {
        sql_throw(vm, 0, "", "sql.all: expected statement handle");
        return -1;
    }
    SqlStmtSlot *st = &g_stmt_pool[idx];
    SqlSlot *slot = &g_db_pool[st->db_slot];
    if (!slot->in_use) {
        sql_throw(vm, 0, "", "sql.all: parent database closed");
        return -1;
    }
    SqlParam *params = NULL;
    int nparams = collect_params(vm, argc, args, 1, &params, "sql.all");
    if (nparams < 0) return -1;

    CandoValue av = cando_bridge_new_array(vm);
    CdoObject *a  = cando_bridge_resolve(vm, cando_as_handle(av));

    if (slot->driver == SQL_DRIVER_POSTGRES) {
        PgResult res;
        pg_result_init(&res);
        bool ok = exec_prepared_pg(vm, slot, st, params, nparams, &res,
                                   "sql.all");
        free(params);
        if (!ok) { pg_result_free(&res); return -1; }
        for (int i = 0; i < res.nrows; i++) {
            CandoValue row = build_row_pg(vm, &res, i, slot->bigint_string);
            cdo_array_push(a, cando_bridge_to_cdo(vm, row));
        }
        pg_result_free(&res);
    } else {
        MyResult res;
        my_result_init(&res);
        bool ok = exec_prepared_my(vm, slot, st, params, nparams, &res,
                                   "sql.all");
        free(params);
        if (!ok) { my_result_free(&res); return -1; }
        for (int i = 0; i < res.nrows; i++) {
            CandoValue row = build_row_my(vm, &res, i, slot->bigint_string);
            cdo_array_push(a, cando_bridge_to_cdo(vm, row));
        }
        my_result_free(&res);
    }
    cando_vm_push(vm, av);
    return 1;
}

/* =========================================================================
 * stmt:finalize() -- close the prepared statement on the server.
 * Idempotent.
 * ===================================================================== */

static int native_sql_finalize(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.finalize: (stmt) required");
        return -1;
    }
    int idx = stmt_handle_slot(vm, args[0]);
    if (idx < 0) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    SqlStmtSlot *st = &g_stmt_pool[idx];
    SqlSlot *slot = &g_db_pool[st->db_slot];
    if (slot->in_use) {
        SQL_MUTEX_LOCK(&slot->lock);
        if (slot->driver == SQL_DRIVER_POSTGRES) {
            if (st->pg_name) pg_close_prepared(&slot->pg, st->pg_name);
        } else {
            my_close_stmt(&slot->my, &st->my_stmt);
        }
        SQL_MUTEX_UNLOCK(&slot->lock);
    }
    stmt_pool_release(idx);
    if (cando_is_object(args[0])) {
        CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
        obj_set_number(obj, SQL_STMT_SLOT_KEY, -1.0);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Transactions: begin / commit / rollback / inTransaction / transaction
 *
 * Both engines support standard BEGIN / COMMIT / ROLLBACK and named
 * SAVEPOINT for nesting.  Use the same depth-counter trick as the
 * SQLite module so nested transactions degrade to savepoints.
 * ===================================================================== */

static bool exec_simple(SqlSlot *slot, const char *sql, SqlError *err_out)
{
    bool ok;
    SQL_MUTEX_LOCK(&slot->lock);
    if (slot->driver == SQL_DRIVER_POSTGRES) {
        ok = pg_simple_exec(&slot->pg, sql);
        if (!ok && err_out) *err_out = slot->pg.last_err;
    } else {
        ok = my_simple_exec(&slot->my, sql);
        if (!ok && err_out) *err_out = slot->my.last_err;
    }
    SQL_MUTEX_UNLOCK(&slot->lock);
    return ok;
}

static int native_sql_begin(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.begin: (db) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    int depth = slot->tx_depth;
    char buf[64];
    if (depth == 0) snprintf(buf, sizeof(buf), "BEGIN");
    else            snprintf(buf, sizeof(buf), "SAVEPOINT cdo_sp_%d", depth);

    SqlError e = {0};
    if (!exec_simple(slot, buf, &e)) {
        sql_throw_from_err(vm, "sql.begin", &e);
        return -1;
    }
    slot->tx_depth = depth + 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sql_commit(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.commit: (db) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    int depth = slot->tx_depth;
    if (depth <= 0) {
        sql_throw(vm, 0, "", "sql.commit: no active transaction");
        return -1;
    }
    char buf[64];
    if (depth == 1) snprintf(buf, sizeof(buf), "COMMIT");
    else            snprintf(buf, sizeof(buf), "RELEASE SAVEPOINT cdo_sp_%d",
                             depth - 1);
    SqlError e = {0};
    if (!exec_simple(slot, buf, &e)) {
        sql_throw_from_err(vm, "sql.commit", &e);
        return -1;
    }
    slot->tx_depth = depth - 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sql_rollback(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.rollback: (db) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    int depth = slot->tx_depth;
    if (depth <= 0) {
        sql_throw(vm, 0, "", "sql.rollback: no active transaction");
        return -1;
    }
    char buf[128];
    if (depth == 1) {
        snprintf(buf, sizeof(buf), "ROLLBACK");
    } else {
        snprintf(buf, sizeof(buf),
                 "ROLLBACK TO SAVEPOINT cdo_sp_%d; RELEASE SAVEPOINT cdo_sp_%d",
                 depth - 1, depth - 1);
    }
    SqlError e = {0};
    if (!exec_simple(slot, buf, &e)) {
        sql_throw_from_err(vm, "sql.rollback", &e);
        return -1;
    }
    slot->tx_depth = depth - 1;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_sql_in_transaction(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0) { cando_vm_push(vm, cando_bool(false)); return 1; }
    cando_vm_push(vm, cando_bool(g_db_pool[slot].tx_depth > 0));
    return 1;
}

static int native_sql_transaction(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sql_throw(vm, 0, "", "sql.transaction: (db, fn[, args...]) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    int depth = slot->tx_depth;
    char buf[64];
    if (depth == 0) snprintf(buf, sizeof(buf), "BEGIN");
    else            snprintf(buf, sizeof(buf), "SAVEPOINT cdo_sp_%d", depth);

    SqlError e = {0};
    if (!exec_simple(slot, buf, &e)) {
        sql_throw_from_err(vm, "sql.transaction", &e);
        return -1;
    }
    slot->tx_depth = depth + 1;

    u32 saved_try_depth = vm->try_depth;
    vm->try_depth = 0;
    int ret_count = cando_vm_call_value(vm, args[1],
                                         argc > 2 ? &args[2] : NULL,
                                         (u32)(argc > 2 ? argc - 2 : 0));
    vm->try_depth = saved_try_depth;
    (void)ret_count;

    if (vm->has_error) {
        char rb[128];
        if (depth == 0) {
            snprintf(rb, sizeof(rb), "ROLLBACK");
        } else {
            snprintf(rb, sizeof(rb),
                "ROLLBACK TO SAVEPOINT cdo_sp_%d; RELEASE SAVEPOINT cdo_sp_%d",
                depth, depth);
        }
        SqlError ee = {0};
        exec_simple(slot, rb, &ee);
        slot->tx_depth = depth;
        return -1;
    }

    char close[64];
    if (depth == 0) snprintf(close, sizeof(close), "COMMIT");
    else            snprintf(close, sizeof(close),
                             "RELEASE SAVEPOINT cdo_sp_%d", depth);
    if (!exec_simple(slot, close, &e)) {
        slot->tx_depth = depth;
        sql_throw_from_err(vm, "sql.transaction", &e);
        return -1;
    }
    slot->tx_depth = depth;
    return ret_count;
}

/* =========================================================================
 * bigintMode + ping
 * ===================================================================== */

static int native_sql_bigint_mode(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.bigintMode: (db[, mode]) required");
        return -1;
    }
    int slot = db_handle_slot(vm, args[0]);
    if (slot < 0) {
        sql_throw(vm, 0, "", "sql.bigintMode: invalid handle");
        return -1;
    }
    if (argc >= 2) {
        const char *m = libutil_arg_cstr_at(args, argc, 1);
        if (!m) {
            sql_throw(vm, 0, "",
                "sql.bigintMode: mode must be \"number\" or \"string\"");
            return -1;
        }
        if      (strcmp(m, "number") == 0) g_db_pool[slot].bigint_string = false;
        else if (strcmp(m, "string") == 0) g_db_pool[slot].bigint_string = true;
        else {
            sql_throw(vm, 0, "",
                "sql.bigintMode: unknown mode \"%s\"", m);
            return -1;
        }
    }
    libutil_push_cstr(vm, g_db_pool[slot].bigint_string ? "string" : "number");
    return 1;
}

static int native_sql_ping(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        sql_throw(vm, 0, "", "sql.ping: (db) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    SqlError e = {0};
    bool ok = exec_simple(slot, "SELECT 1", &e);
    if (!ok) {
        sql_throw_from_err(vm, "sql.ping", &e);
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * escape(value) / escapeIdentifier(name)
 *
 * For safe construction of SQL strings when prepared-statement binding
 * isn't an option (dynamic identifiers, IN-list builders, dialect-specific
 * syntax that doesn't accept placeholders).  Both functions are
 * driver-aware: the PostgreSQL driver uses E'...' escape strings and
 * "..." identifiers; MySQL uses '...' with backslash escapes and
 * `...` identifiers.
 *
 * Accepted value kinds and their result:
 *     null     -> "NULL"
 *     bool     -> "TRUE" / "FALSE" (PG) or "1" / "0" (MySQL)
 *     number   -> stringified (no quotes, no locale)
 *     string   -> quoted literal in the engine's syntax
 *     anything else -> error
 * ===================================================================== */

/* Helper: emit a number as a safe SQL literal.  Uses %.17g for floats
 * (round-trip safe) and %lld for whole values within the safe range. */
static char *escape_number(double d)
{
    char buf[48];
    int n;
    if (isfinite(d) && d == (double)(int64_t)d
        && d >= -INTEGER_SAFE_MAX && d <= INTEGER_SAFE_MAX) {
        n = snprintf(buf, sizeof(buf), "%lld", (long long)d);
    } else if (!isfinite(d)) {
        /* NaN / inf -- not valid SQL.  Stringify and let the server
         * reject the query rather than silently succeed. */
        n = snprintf(buf, sizeof(buf), "'NaN'::float");
        if (d > 0)            n = snprintf(buf, sizeof(buf), "'Infinity'::float");
        else if (d < 0)       n = snprintf(buf, sizeof(buf), "'-Infinity'::float");
    } else {
        n = snprintf(buf, sizeof(buf), "%.17g", d);
    }
    if (n < 0) return NULL;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, buf, (size_t)n + 1);
    return s;
}

static int native_sql_escape(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sql_throw(vm, 0, "", "sql.escape: (db, value) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    CandoValue v = args[1];
    char *out = NULL;

    if (cando_is_null(v)) {
        out = strdup("NULL");
    } else if (cando_is_bool(v)) {
        if (slot->driver == SQL_DRIVER_POSTGRES) {
            out = strdup(cando_as_bool(v) ? "TRUE" : "FALSE");
        } else {
            out = strdup(cando_as_bool(v) ? "1" : "0");
        }
    } else if (cando_is_number(v)) {
        out = escape_number(cando_as_number(v));
    } else if (cando_is_string(v)) {
        const char *s = cando_as_string(v)->data;
        size_t       n = cando_as_string(v)->length;
        if (slot->driver == SQL_DRIVER_POSTGRES)
            out = sql_escape_pg_literal(s, n);
        else
            out = sql_escape_my_literal(s, n);
        if (!out) {
            sql_throw(vm, 0, "22021",
                "sql.escape: input contains a NUL byte (PostgreSQL "
                "text literals cannot carry one -- bind as bytea instead)");
            return -1;
        }
    } else {
        sql_throw(vm, 0, "",
            "sql.escape: unsupported value type "
            "(expected null, bool, number, or string)");
        return -1;
    }

    if (!out) {
        sql_throw(vm, 0, "HY001", "sql.escape: out of memory");
        return -1;
    }
    libutil_push_cstr(vm, out);
    free(out);
    return 1;
}

static int native_sql_escape_identifier(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        sql_throw(vm, 0, "", "sql.escapeIdentifier: (db, name) required");
        return -1;
    }
    SqlSlot *slot = db_handle_unwrap(vm, args[0]);
    if (!slot) return -1;
    const char *s = libutil_arg_cstr_at(args, argc, 1);
    if (!s) {
        sql_throw(vm, 0, "",
            "sql.escapeIdentifier: name must be a string");
        return -1;
    }
    size_t n = cando_as_string(args[1])->length;
    char *out = (slot->driver == SQL_DRIVER_POSTGRES)
              ? sql_escape_pg_identifier(s, n)
              : sql_escape_my_identifier(s, n);
    if (!out) {
        sql_throw(vm, 0, "22021",
            "sql.escapeIdentifier: name contains a NUL byte");
        return -1;
    }
    libutil_push_cstr(vm, out);
    free(out);
    return 1;
}

/* =========================================================================
 * Module init
 * ===================================================================== */

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
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(tbl));

    /* Module-only entry points. */
    register_method(vm, obj, "open",          native_sql_open,
                    METHOD_MODULE_ONLY);
    register_method(vm, obj, "openPostgres",  native_sql_open_pg,
                    METHOD_MODULE_ONLY);
    register_method(vm, obj, "openMySQL",     native_sql_open_mysql,
                    METHOD_MODULE_ONLY);

    /* Database-handle methods (callable as sql.exec(db, ...) AND db:exec(...)). */
    register_method(vm, obj, "close",         native_sql_close,         METHOD_ON_DB);
    register_method(vm, obj, "exec",          native_sql_exec,          METHOD_ON_DB);
    register_method(vm, obj, "prepare",       native_sql_prepare,       METHOD_ON_DB);
    register_method(vm, obj, "begin",         native_sql_begin,         METHOD_ON_DB);
    register_method(vm, obj, "commit",        native_sql_commit,        METHOD_ON_DB);
    register_method(vm, obj, "rollback",      native_sql_rollback,      METHOD_ON_DB);
    register_method(vm, obj, "inTransaction", native_sql_in_transaction, METHOD_ON_DB);
    register_method(vm, obj, "transaction",   native_sql_transaction,   METHOD_ON_DB);
    register_method(vm, obj, "bigintMode",    native_sql_bigint_mode,   METHOD_ON_DB);
    register_method(vm, obj, "ping",          native_sql_ping,          METHOD_ON_DB);
    register_method(vm, obj, "escape",         native_sql_escape,         METHOD_ON_DB);
    register_method(vm, obj, "escapeIdentifier", native_sql_escape_identifier, METHOD_ON_DB);

    /* Statement-handle methods. */
    register_method(vm, obj, "run",      native_sql_run,      METHOD_ON_STMT);
    register_method(vm, obj, "get",      native_sql_get,      METHOD_ON_STMT);
    register_method(vm, obj, "all",      native_sql_all,      METHOD_ON_STMT);
    register_method(vm, obj, "finalize", native_sql_finalize, METHOD_ON_STMT);

    obj_set_string(obj, "VERSION", SQL_MODULE_VERSION,
                   (u32)(sizeof(SQL_MODULE_VERSION) - 1));

    /* Driver-name constants. */
    obj_set_string(obj, "DRIVER_POSTGRES", "postgres", 8);
    obj_set_string(obj, "DRIVER_MYSQL",    "mysql",    5);

    return tbl;
}
