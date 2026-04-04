/*
 * win_regex.h — Minimal POSIX ERE <regex.h> replacement for Windows/MinGW.
 *
 * Drop-in for #include <regex.h> when building with MinGW which lacks it.
 * Provides: regex_t, regmatch_t, regcomp, regexec, regfree.
 */

#ifndef WIN_REGEX_H
#define WIN_REGEX_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* regcomp flags */
#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8

/* regexec flags */
#define REG_NOTBOL    1
#define REG_NOTEOL    2

/* Error codes */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

typedef ptrdiff_t regoff_t;

typedef struct {
    regoff_t rm_so;  /* byte offset of start of match */
    regoff_t rm_eo;  /* byte offset of first char past end of match */
} regmatch_t;

typedef struct {
    char *pattern;  /* copy of the pattern string */
    int   nsub;     /* number of capture groups */
} regex_t;

int  regcomp(regex_t *preg, const char *pattern, int cflags);
int  regexec(const regex_t *preg, const char *string,
             size_t nmatch, regmatch_t pmatch[], int eflags);
void regfree(regex_t *preg);

#ifdef __cplusplus
}
#endif

#endif /* WIN_REGEX_H */
