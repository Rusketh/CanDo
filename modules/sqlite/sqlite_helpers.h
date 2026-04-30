/*
 * modules/sqlite/sqlite_helpers.h -- Pure-C helpers for the SQLite
 * binary module.  Kept header-only with `static inline` definitions so
 * the unit tests can include this file directly and exercise each
 * helper without linking against the rest of the module.
 */

#ifndef CANDO_SQLITE_HELPERS_H
#define CANDO_SQLITE_HELPERS_H

#include "vendor/sqlite3.h"

#include <stdint.h>
#include <string.h>

/* sqlite_errcode_name -- map a SQLite extended result code to its
 * symbolic name (e.g. SQLITE_BUSY, SQLITE_CONSTRAINT_UNIQUE).
 *
 * Returned strings are static literals; callers must not free them.
 * Returns "SQLITE_UNKNOWN" for codes the helper does not recognise.
 *
 * The implementation walks the primary code first (low byte), then
 * the extended code if one of the families that defines them.  This is
 * the same pattern sqlite3.c uses internally for sqlite3_errstr but
 * keyed on the symbolic name rather than the english message. */
static inline const char *sqlite_errcode_name(int code)
{
    switch (code & 0xff) {
        case SQLITE_OK:         return "SQLITE_OK";
        case SQLITE_ERROR:
            switch (code) {
                case SQLITE_ERROR_MISSING_COLLSEQ:  return "SQLITE_ERROR_MISSING_COLLSEQ";
                case SQLITE_ERROR_RETRY:            return "SQLITE_ERROR_RETRY";
                case SQLITE_ERROR_SNAPSHOT:         return "SQLITE_ERROR_SNAPSHOT";
                default:                            return "SQLITE_ERROR";
            }
        case SQLITE_INTERNAL:   return "SQLITE_INTERNAL";
        case SQLITE_PERM:       return "SQLITE_PERM";
        case SQLITE_ABORT:
            switch (code) {
                case SQLITE_ABORT_ROLLBACK:         return "SQLITE_ABORT_ROLLBACK";
                default:                            return "SQLITE_ABORT";
            }
        case SQLITE_BUSY:
            switch (code) {
                case SQLITE_BUSY_RECOVERY:          return "SQLITE_BUSY_RECOVERY";
                case SQLITE_BUSY_SNAPSHOT:          return "SQLITE_BUSY_SNAPSHOT";
                case SQLITE_BUSY_TIMEOUT:           return "SQLITE_BUSY_TIMEOUT";
                default:                            return "SQLITE_BUSY";
            }
        case SQLITE_LOCKED:
            switch (code) {
                case SQLITE_LOCKED_SHAREDCACHE:     return "SQLITE_LOCKED_SHAREDCACHE";
                case SQLITE_LOCKED_VTAB:            return "SQLITE_LOCKED_VTAB";
                default:                            return "SQLITE_LOCKED";
            }
        case SQLITE_NOMEM:      return "SQLITE_NOMEM";
        case SQLITE_READONLY:
            switch (code) {
                case SQLITE_READONLY_RECOVERY:      return "SQLITE_READONLY_RECOVERY";
                case SQLITE_READONLY_CANTLOCK:      return "SQLITE_READONLY_CANTLOCK";
                case SQLITE_READONLY_ROLLBACK:      return "SQLITE_READONLY_ROLLBACK";
                case SQLITE_READONLY_DBMOVED:       return "SQLITE_READONLY_DBMOVED";
                case SQLITE_READONLY_CANTINIT:      return "SQLITE_READONLY_CANTINIT";
                case SQLITE_READONLY_DIRECTORY:     return "SQLITE_READONLY_DIRECTORY";
                default:                            return "SQLITE_READONLY";
            }
        case SQLITE_INTERRUPT:  return "SQLITE_INTERRUPT";
        case SQLITE_IOERR:      return "SQLITE_IOERR";
        case SQLITE_CORRUPT:
            switch (code) {
                case SQLITE_CORRUPT_VTAB:           return "SQLITE_CORRUPT_VTAB";
                case SQLITE_CORRUPT_SEQUENCE:       return "SQLITE_CORRUPT_SEQUENCE";
                case SQLITE_CORRUPT_INDEX:          return "SQLITE_CORRUPT_INDEX";
                default:                            return "SQLITE_CORRUPT";
            }
        case SQLITE_NOTFOUND:   return "SQLITE_NOTFOUND";
        case SQLITE_FULL:       return "SQLITE_FULL";
        case SQLITE_CANTOPEN:
            switch (code) {
                case SQLITE_CANTOPEN_NOTEMPDIR:     return "SQLITE_CANTOPEN_NOTEMPDIR";
                case SQLITE_CANTOPEN_ISDIR:         return "SQLITE_CANTOPEN_ISDIR";
                case SQLITE_CANTOPEN_FULLPATH:      return "SQLITE_CANTOPEN_FULLPATH";
                case SQLITE_CANTOPEN_CONVPATH:      return "SQLITE_CANTOPEN_CONVPATH";
                case SQLITE_CANTOPEN_DIRTYWAL:      return "SQLITE_CANTOPEN_DIRTYWAL";
                case SQLITE_CANTOPEN_SYMLINK:       return "SQLITE_CANTOPEN_SYMLINK";
                default:                            return "SQLITE_CANTOPEN";
            }
        case SQLITE_PROTOCOL:   return "SQLITE_PROTOCOL";
        case SQLITE_EMPTY:      return "SQLITE_EMPTY";
        case SQLITE_SCHEMA:     return "SQLITE_SCHEMA";
        case SQLITE_TOOBIG:     return "SQLITE_TOOBIG";
        case SQLITE_CONSTRAINT:
            switch (code) {
                case SQLITE_CONSTRAINT_CHECK:       return "SQLITE_CONSTRAINT_CHECK";
                case SQLITE_CONSTRAINT_COMMITHOOK:  return "SQLITE_CONSTRAINT_COMMITHOOK";
                case SQLITE_CONSTRAINT_FOREIGNKEY:  return "SQLITE_CONSTRAINT_FOREIGNKEY";
                case SQLITE_CONSTRAINT_FUNCTION:    return "SQLITE_CONSTRAINT_FUNCTION";
                case SQLITE_CONSTRAINT_NOTNULL:     return "SQLITE_CONSTRAINT_NOTNULL";
                case SQLITE_CONSTRAINT_PRIMARYKEY:  return "SQLITE_CONSTRAINT_PRIMARYKEY";
                case SQLITE_CONSTRAINT_TRIGGER:     return "SQLITE_CONSTRAINT_TRIGGER";
                case SQLITE_CONSTRAINT_UNIQUE:      return "SQLITE_CONSTRAINT_UNIQUE";
                case SQLITE_CONSTRAINT_VTAB:        return "SQLITE_CONSTRAINT_VTAB";
                case SQLITE_CONSTRAINT_ROWID:       return "SQLITE_CONSTRAINT_ROWID";
                case SQLITE_CONSTRAINT_PINNED:      return "SQLITE_CONSTRAINT_PINNED";
                case SQLITE_CONSTRAINT_DATATYPE:    return "SQLITE_CONSTRAINT_DATATYPE";
                default:                            return "SQLITE_CONSTRAINT";
            }
        case SQLITE_MISMATCH:   return "SQLITE_MISMATCH";
        case SQLITE_MISUSE:     return "SQLITE_MISUSE";
        case SQLITE_NOLFS:      return "SQLITE_NOLFS";
        case SQLITE_AUTH:       return "SQLITE_AUTH";
        case SQLITE_FORMAT:     return "SQLITE_FORMAT";
        case SQLITE_RANGE:      return "SQLITE_RANGE";
        case SQLITE_NOTADB:     return "SQLITE_NOTADB";
        case SQLITE_NOTICE:     return "SQLITE_NOTICE";
        case SQLITE_WARNING:    return "SQLITE_WARNING";
        case SQLITE_ROW:        return "SQLITE_ROW";
        case SQLITE_DONE:       return "SQLITE_DONE";
        default:                return "SQLITE_UNKNOWN";
    }
}

#endif /* CANDO_SQLITE_HELPERS_H */
