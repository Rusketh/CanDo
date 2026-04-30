/*
 * modules/sql/sql_mysql.h -- MySQL / MariaDB driver public surface.
 *
 * MyConn owns the protocol state for one connection: the TCP/TLS
 * socket, the rolling sequence-id counter that frames every
 * client/server packet, the negotiated capability flags, and the
 * server-side prepared statement registry.
 */

#ifndef CANDO_SQL_MYSQL_H
#define CANDO_SQL_MYSQL_H

#include "sql_driver.h"
#include "sql_net.h"

#include <stdbool.h>
#include <stdint.h>

/* Capability flags (subset used by the driver). */
#define MY_CAP_LONG_PASSWORD         0x00000001
#define MY_CAP_FOUND_ROWS            0x00000002
#define MY_CAP_LONG_FLAG             0x00000004
#define MY_CAP_CONNECT_WITH_DB       0x00000008
#define MY_CAP_NO_SCHEMA             0x00000010
#define MY_CAP_COMPRESS              0x00000020
#define MY_CAP_LOCAL_FILES           0x00000080
#define MY_CAP_IGNORE_SPACE          0x00000100
#define MY_CAP_PROTOCOL_41           0x00000200
#define MY_CAP_INTERACTIVE           0x00000400
#define MY_CAP_SSL                   0x00000800
#define MY_CAP_TRANSACTIONS          0x00002000
#define MY_CAP_SECURE_CONNECTION     0x00008000
#define MY_CAP_MULTI_STATEMENTS      0x00010000
#define MY_CAP_MULTI_RESULTS         0x00020000
#define MY_CAP_PS_MULTI_RESULTS      0x00040000
#define MY_CAP_PLUGIN_AUTH           0x00080000
#define MY_CAP_CONNECT_ATTRS         0x00100000
#define MY_CAP_PLUGIN_AUTH_LENENC    0x00200000
#define MY_CAP_DEPRECATE_EOF         0x01000000

/* enum_field_types we care about for binary protocol decoding. */
#define MY_TYPE_DECIMAL      0
#define MY_TYPE_TINY         1
#define MY_TYPE_SHORT        2
#define MY_TYPE_LONG         3
#define MY_TYPE_FLOAT        4
#define MY_TYPE_DOUBLE       5
#define MY_TYPE_NULL         6
#define MY_TYPE_TIMESTAMP    7
#define MY_TYPE_LONGLONG     8
#define MY_TYPE_INT24        9
#define MY_TYPE_DATE         10
#define MY_TYPE_TIME         11
#define MY_TYPE_DATETIME     12
#define MY_TYPE_YEAR         13
#define MY_TYPE_NEWDATE      14
#define MY_TYPE_VARCHAR      15
#define MY_TYPE_BIT          16
#define MY_TYPE_TIMESTAMP2   17
#define MY_TYPE_DATETIME2    18
#define MY_TYPE_TIME2        19
#define MY_TYPE_JSON         245
#define MY_TYPE_NEWDECIMAL   246
#define MY_TYPE_ENUM         247
#define MY_TYPE_SET          248
#define MY_TYPE_TINY_BLOB    249
#define MY_TYPE_MEDIUM_BLOB  250
#define MY_TYPE_LONG_BLOB    251
#define MY_TYPE_BLOB         252
#define MY_TYPE_VAR_STRING   253
#define MY_TYPE_STRING       254
#define MY_TYPE_GEOMETRY     255

typedef struct MyConn {
    SqlNet     net;
    SqlError   last_err;

    uint8_t    seq;              /* next sequence id to use */
    uint32_t   server_caps;
    uint32_t   client_caps;
    uint32_t   max_packet;
    uint8_t    charset;
    int        protocol_version;
    char       server_version[64];
    int        server_thread_id;

    /* Affected-row counters mirrored from the most recent OK packet. */
    uint64_t   last_affected;
    uint64_t   last_insert_id;
    uint16_t   last_status;
    uint16_t   last_warnings;
} MyConn;

/* Result for text-protocol queries (COM_QUERY). */
typedef struct MyResult {
    SqlColumn *columns;
    int        ncols;
    struct MyRow *rows;
    int        nrows;
    uint64_t   affected;
    uint64_t   insert_id;
} MyResult;

typedef struct MyRow {
    SqlValue *cells;
    int       ncols;
    void     *row_arena;
} MyRow;

/* Prepared statement state (server-side handle). */
typedef struct MyStmt {
    uint32_t   stmt_id;
    uint16_t   nparams;
    uint16_t   ncols;
    SqlColumn *param_columns;        /* param descriptions (nparams) */
    SqlColumn *result_columns;       /* (ncols)                       */
} MyStmt;

bool my_connect(MyConn *c, const SqlConnectOpts *opts);
void my_close(MyConn *c);

/* Text-protocol query: COM_QUERY + result-set.  All cells are TEXT. */
bool my_simple_exec(MyConn *c, const char *sql);
bool my_query(MyConn *c, const char *sql, MyResult *out);

/* Binary-protocol prepared statements. */
bool my_prepare(MyConn *c, const char *sql, MyStmt *out);
bool my_execute(MyConn *c, MyStmt *st,
                const SqlParam *params, int nparams,
                MyResult *out);
bool my_close_stmt(MyConn *c, MyStmt *st);

void my_result_init(MyResult *r);
void my_result_free(MyResult *r);
void my_stmt_free(MyStmt *st);

#endif /* CANDO_SQL_MYSQL_H */
