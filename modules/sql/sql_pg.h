/*
 * modules/sql/sql_pg.h -- PostgreSQL driver public surface used by the
 * script-facing layer.  The implementation lives in sql_pg.c.
 *
 * A PgConn owns the TCP/TLS socket plus parameter status, backend key
 * data, and a small registry of named prepared statements.  Methods
 * mirror the action set the script layer needs to perform; errors are
 * surfaced in `last_err`.
 */

#ifndef CANDO_SQL_PG_H
#define CANDO_SQL_PG_H

#include "sql_driver.h"
#include "sql_net.h"

#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Connection
 * ===================================================================== */

typedef struct PgConn {
    SqlNet     net;
    SqlError   last_err;

    int        backend_pid;
    int        backend_secret_key;
    bool       in_failed_tx;       /* Z status == 'E' */
    bool       in_tx;              /* Z status == 'T' or 'E' */

    /* Most recently observed CommandComplete tag, e.g. "INSERT 0 1".
     * Used by exec / run to surface affected-row counts. */
    char       last_tag[64];

    /* Number of unique prepared-statement names handed out so far.
     * Each call to pg_prepare reserves a fresh "cdo_pg_<n>" name. */
    int        prepared_id_seq;
} PgConn;

/* Result set held in memory after pg_query / pg_execute_prepared. */
typedef struct PgResult {
    SqlColumn *columns;
    int        ncols;

    /* Rows are decoded eagerly: each row is a heap-allocated SqlValue[]
     * plus a single bytes-arena (text data is malloc'd into it).  The
     * row cells point into the arena, which is freed alongside the
     * row in pg_result_free. */
    struct PgRow *rows;
    int           nrows;

    /* CommandComplete tag and parsed affected-row count when the tag
     * encodes one. */
    char    cmd_tag[64];
    int64_t affected;
} PgResult;

typedef struct PgPreparedDesc {
    SqlColumn *columns;
    int        ncols;
} PgPreparedDesc;

/* =========================================================================
 * Lifecycle
 * ===================================================================== */

bool pg_connect(PgConn *c, const SqlConnectOpts *opts);
void pg_close(PgConn *c);

/* =========================================================================
 * Statement execution
 * ===================================================================== */

/*
 * pg_simple_exec -- send a Q (Simple Query) message; reads through
 * ReadyForQuery, accepting any number of subqueries and discarding
 * their result rows.  Used by exec() for DDL / multi-statement scripts.
 *
 * Returns true on success.  The CommandComplete tag of the *last*
 * subquery is left in c->last_tag.
 */
bool pg_simple_exec(PgConn *c, const char *sql);

/*
 * pg_query -- run `sql` via the simple-query protocol and capture
 * every result row in `out`.  Used by stmt:all() / stmt:get() when no
 * parameters are passed.  out must be released with pg_result_free.
 */
bool pg_query(PgConn *c, const char *sql, PgResult *out);

/*
 * pg_prepare -- Parse + Describe(S) + Sync.  Returns a fresh server-side
 * statement name.  Caller must free `*out_name` (heap-allocated) and
 * call pg_prepared_desc_free on `desc` once done.
 */
bool pg_prepare(PgConn *c, const char *sql,
                char **out_name, PgPreparedDesc *desc);

/*
 * pg_execute_prepared -- Bind + Execute + Sync against the named
 * statement.  Captures rows in `out` (release with pg_result_free).
 */
bool pg_execute_prepared(PgConn *c, const char *stmt_name,
                         const SqlParam *params, int nparams,
                         PgResult *out);

/*
 * pg_close_prepared -- send Close('S') + Sync.  Idempotent.
 */
bool pg_close_prepared(PgConn *c, const char *stmt_name);

/* =========================================================================
 * Result lifecycle
 * ===================================================================== */

void pg_result_init(PgResult *r);
void pg_result_free(PgResult *r);
void pg_prepared_desc_free(PgPreparedDesc *d);

/* =========================================================================
 * Iteration over a stored PgResult -- used by stmt:iterate / stmt:get
 * to surface one row at a time.  Returns NULL once exhausted.
 * ===================================================================== */
typedef struct PgRow {
    SqlValue *cells;
    int       ncols;
    /* row_arena is opaque storage for the row's heap text/blob bytes;
     * freed when the parent PgResult is released. */
    void     *row_arena;
} PgRow;

#endif /* CANDO_SQL_PG_H */
