/*
 * modules/sql/sql_driver.h -- Driver interface used by the script-facing
 * surface to talk to either PostgreSQL or MySQL/MariaDB.
 *
 * A SqlConn is the protocol-side state for one open database connection
 * (the TCP/TLS socket, parameters, server-side prepared statement
 * registry, last error, etc.).  Each driver provides one of the
 * SQL_DRIVER_* vtables defined below; the script-facing module looks
 * up which driver a handle owns and dispatches through the vtable.
 *
 * Errors are surfaced through SqlError -- a (sqlstate, code, message)
 * triple that the module-level shim feeds into Cando's three-value
 * `CATCH (msg, code, sqlstate)` mechanism.  Drivers populate the
 * SqlError, return `false`, and the caller throws.
 *
 * Result rows are emitted as an SqlRow value array in column order,
 * paired with column metadata (SqlColumn).  The script layer turns
 * that into an object keyed by column name.
 */

#ifndef CANDO_SQL_DRIVER_H
#define CANDO_SQL_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Forward decls -- avoid pulling in cando.h here so the unit tests can
 * include this header without a libcando link dependency. */
struct SqlConn;
struct SqlStmt;

/* =========================================================================
 * Error contract
 * ===================================================================== */

typedef struct SqlError {
    int   code;             /* driver-specific numeric code (PG: SQLSTATE
                             * ASCII packed; MySQL: errno).  0 == module-side. */
    char  sqlstate[8];      /* "08006", "23505", or empty string */
    char  message[480];
} SqlError;

static inline void sql_error_clear(SqlError *e)
{
    e->code = 0;
    e->sqlstate[0] = '\0';
    e->message[0]  = '\0';
}

/* =========================================================================
 * Connection options
 *
 * The script-facing module lifts these from the options object passed
 * to sql.openPostgres / sql.openMySQL.  Drivers interpret them.
 * ===================================================================== */

typedef struct SqlConnectOpts {
    const char *host;
    int         port;
    const char *user;
    const char *password;
    const char *database;
    const char *application_name;   /* PG only; may be NULL */

    /* TLS */
    bool        tls;                /* request TLS */
    bool        tls_verify;         /* verify peer cert */
    const char *tls_ca_pem;         /* optional CA bundle */
    const char *tls_client_cert;
    const char *tls_client_key;

    int         connect_timeout_ms; /* 0 = blocking */
    int         io_timeout_ms;      /* per-recv/send timeout */

    /* MySQL-specific */
    const char *charset;            /* default: "utf8mb4" */
} SqlConnectOpts;

/* =========================================================================
 * Result column / value -- driver-agnostic representation
 *
 * Values are always materialised as bytes; the script layer decides how
 * to reify them (number, string, blob).  type_oid is the driver's
 * native type code so the script layer can map it (for PG: pg_type
 * OID; for MySQL: enum_field_types).
 * ===================================================================== */

typedef enum SqlValueKind {
    SQL_VAL_NULL    = 0,
    SQL_VAL_INTEGER = 1,    /* i64-fits-in-double or decimal string */
    SQL_VAL_DOUBLE  = 2,
    SQL_VAL_TEXT    = 3,
    SQL_VAL_BLOB    = 4,
    SQL_VAL_BOOL    = 5,
} SqlValueKind;

typedef struct SqlValue {
    SqlValueKind kind;
    /* INTEGER: value or NULL=heap text decimal (when overflow > 2^53);
     * DOUBLE:  value;
     * TEXT/BLOB: data + len point into the row arena (stable until row free);
     * BOOL: integer 0/1 stored in i64. */
    int64_t        i64;
    double         f64;
    const char    *data;
    size_t         len;
} SqlValue;

typedef struct SqlColumn {
    char     name[128];
    uint32_t type_oid;
    int      type_len;        /* server-reported length; -1 = variable */
    int      flags;           /* driver-specific bitmask */
} SqlColumn;

typedef struct SqlRow {
    SqlValue *cells;          /* ncols entries, lifetime = arena */
    int       ncols;
    /* Backing storage for the row's text/blob bytes, freed by the
     * driver when the next row is fetched or the result is released. */
    void     *arena;
} SqlRow;

/* =========================================================================
 * Param value (script -> driver)
 * ===================================================================== */

typedef enum SqlParamKind {
    SQL_PARAM_NULL    = 0,
    SQL_PARAM_BOOL    = 1,
    SQL_PARAM_INT64   = 2,
    SQL_PARAM_DOUBLE  = 3,
    SQL_PARAM_TEXT    = 4,
    SQL_PARAM_BLOB    = 5,
} SqlParamKind;

typedef struct SqlParam {
    SqlParamKind kind;
    int64_t      i64;
    double       f64;
    const char  *data;
    size_t       len;
} SqlParam;

/* =========================================================================
 * Driver tag
 * ===================================================================== */

typedef enum SqlDriverKind {
    SQL_DRIVER_POSTGRES = 1,
    SQL_DRIVER_MYSQL    = 2,
} SqlDriverKind;

#endif /* CANDO_SQL_DRIVER_H */
