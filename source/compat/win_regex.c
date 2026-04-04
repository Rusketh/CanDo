/*
 * win_regex.c — Minimal POSIX ERE for Windows/MinGW builds.
 *
 * Provides regcomp / regexec / regfree.
 *
 * Supported syntax:
 *   .           any non-NUL character
 *   *  +  ?     greedy repetition
 *   [abc]       character class
 *   [^abc]      negated character class
 *   [a-z]       range inside class
 *   ^  $        line anchors
 *   (...)       grouping and capture (up to RE_MAXGROUPS groups)
 *   |           alternation
 *   \d \D       digit / non-digit
 *   \w \W       word char (\w = [A-Za-z0-9_]) / non-word
 *   \s \S       space / non-space
 *   \n \t \r    escape sequences
 *   \x          literal x  (any other char after backslash)
 *
 * Not supported: {n,m}  lookahead/lookbehind  backreferences
 */

#include "win_regex.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RE_MAXGROUPS 32
#define RE_MAXDEPTH  200

/* =========================================================================
 * Pattern scanning utilities
 * ======================================================================= */

/* Return pointer past the atom at *p (not including any following quantifier).
 * An atom is: a char, \\x, [class], or (group). */
static const char *skip_atom(const char *p)
{
    if (!*p) return p;
    if (p[0] == '\\') return p[1] ? p + 2 : p + 1;
    if (p[0] == '[') {
        p++;
        if (*p == '^') p++;
        if (*p == ']') p++;          /* literal ] at start of class */
        while (*p && *p != ']') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return *p == ']' ? p + 1 : p;
    }
    if (p[0] == '(') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '[') {
                p++;
                if (*p == '^') p++;
                if (*p == ']') p++;
                while (*p && *p != ']') {
                    if (*p == '\\' && p[1]) p++;
                    p++;
                }
                if (*p == ']') p++;
                continue;
            }
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    return p + 1;
}

/* Find the first top-level '|' in [p, pend).  Returns NULL if none found. */
static const char *find_top_alt(const char *p, const char *pend)
{
    int depth = 0;
    for (; p < pend; p++) {
        if (*p == '\\') { if (p + 1 < pend) p++; continue; }
        if (*p == '[') {
            p++;
            if (p < pend && *p == '^') p++;
            if (p < pend && *p == ']') p++;
            while (p < pend && *p != ']') {
                if (*p == '\\' && p + 1 < pend) p++;
                p++;
            }
            continue;
        }
        if (*p == '(') { depth++; continue; }
        if (*p == ')') { if (depth > 0) depth--; continue; }
        if (*p == '|' && depth == 0) return p;
    }
    return NULL;
}

/* Count '(' in the pattern (for nsub). */
static int count_groups(const char *p)
{
    int n = 0;
    for (; *p; p++) {
        if (*p == '\\') { if (p[1]) p++; continue; }
        if (*p == '[') {
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            continue;
        }
        if (*p == '(') n++;
    }
    return n;
}

/* Return the 1-based capture group index for the '(' at position p_open
 * within the pattern starting at pat_start. */
static int group_index(const char *pat_start, const char *p_open)
{
    int n = 0;
    for (const char *q = pat_start; q < p_open; ) {
        if (*q == '\\') { q += (q[1] ? 2 : 1); continue; }
        if (*q == '[') {
            q++;
            if (*q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') { if (*q == '\\' && q[1]) q++; q++; }
            if (*q == ']') q++;
            continue;
        }
        if (*q == '(') { n++; q++; continue; }
        q++;
    }
    return n + 1;
}

/* =========================================================================
 * Character matching
 * ======================================================================= */

/* Match character c against the atom beginning at p.
 * Returns 1 on match, 0 on failure.
 * p must point to '.', '\\x', '[class]', or a literal char. */
static int char_matches(const char *p, char c)
{
    if (!c) return 0;
    if (*p == '.') return 1;
    if (*p == '\\') {
        char e = p[1];
        switch (e) {
            case 'd': return isdigit((unsigned char)c);
            case 'D': return !isdigit((unsigned char)c);
            case 'w': return isalnum((unsigned char)c) || c == '_';
            case 'W': return !(isalnum((unsigned char)c) || c == '_');
            case 's': return isspace((unsigned char)c);
            case 'S': return !isspace((unsigned char)c);
            case 'n': return c == '\n';
            case 't': return c == '\t';
            case 'r': return c == '\r';
            default:  return (unsigned char)c == (unsigned char)e;
        }
    }
    if (*p == '[') {
        const char *q = p + 1;
        int neg = (*q == '^');
        if (neg) q++;
        int found = 0;
        /* literal ] at start of class */
        if (*q == ']') { if (c == ']') found = 1; q++; }
        while (*q && *q != ']') {
            char lo, hi;
            if (*q == '\\' && q[1]) {
                char e = q[1]; q += 2;
                lo = hi = (e == 'n') ? '\n' : (e == 't') ? '\t' :
                          (e == 'r') ? '\r' : e;
            } else {
                lo = *q++;
                if (*q == '-' && q[1] && q[1] != ']') {
                    q++;
                    if (*q == '\\' && q[1]) {
                        char e2 = q[1]; q += 2;
                        hi = (e2 == 'n') ? '\n' : (e2 == 't') ? '\t' :
                             (e2 == 'r') ? '\r' : e2;
                    } else {
                        hi = *q++;
                    }
                } else {
                    hi = lo;
                }
            }
            if ((unsigned char)c >= (unsigned char)lo &&
                (unsigned char)c <= (unsigned char)hi)
                found = 1;
        }
        return neg ? !found : found;
    }
    return (unsigned char)c == (unsigned char)*p;
}

/* =========================================================================
 * Match state
 * ======================================================================= */

typedef struct {
    const char *text;               /* start of full input text */
    const char *pat;                /* start of full pattern */
    regoff_t    so[RE_MAXGROUPS];   /* capture start offsets (1-indexed) */
    regoff_t    eo[RE_MAXGROUPS];   /* capture end offsets */
    int         nsub;               /* number of capture groups */
    int         depth;              /* recursion depth guard */
} MS;

/* =========================================================================
 * Recursive matcher
 * ======================================================================= */

/* Forward declaration */
static const char *m_alt(MS *ms, const char *p, const char *pend, const char *s);

/* m_quant: match atom at p (pointing past any quantifier) preceded by
 * quantifier character qc ('*', '+', '?', or 0 for no quantifier),
 * then continue matching [next_p, pend) at whatever s we land on.
 *
 * atom_p: pointer to the atom
 * atom_end: pointer past the atom (= next_p when no parens, else may differ)
 * next_p: pointer to first char after the atom+quantifier
 * pend: exclusive end of the current alternative
 */

/* m_atom_here: match the atom at p at string position s WITHOUT quantifier
 * handling.  Returns new s position or NULL. */
static const char *m_atom_here(MS *ms, const char *p, const char *s)
{
    if (ms->depth > RE_MAXDEPTH) return NULL;
    if (*p == '^') {
        /* BOL: match if s is at start of text or after a newline */
        return (s == ms->text || *(s - 1) == '\n') ? s : NULL;
    }
    if (*p == '$') {
        return (*s == '\0' || *s == '\n') ? s : NULL;
    }
    if (*p == '(') {
        int grp = group_index(ms->pat, p);
        const char *inner  = p + 1;
        const char *ae     = skip_atom(p);
        const char *inner_end = ae - 1; /* points to ')' */

        /* Save old captures so we can restore on failure */
        regoff_t old_so = (grp <= ms->nsub) ? ms->so[grp] : -2;
        regoff_t old_eo = (grp <= ms->nsub) ? ms->eo[grp] : -2;

        if (grp <= ms->nsub) ms->so[grp] = (regoff_t)(s - ms->text);

        ms->depth++;
        const char *result = m_alt(ms, inner, inner_end, s);
        ms->depth--;

        if (result) {
            if (grp <= ms->nsub) ms->eo[grp] = (regoff_t)(result - ms->text);
            return result;
        }
        /* Restore on failure */
        if (grp <= ms->nsub) { ms->so[grp] = old_so; ms->eo[grp] = old_eo; }
        return NULL;
    }
    /* Regular char-matching atom (char, ., \\x, [...]) */
    if (!*s) return NULL;
    return char_matches(p, *s) ? s + 1 : NULL;
}

/* m_seq: match the sequence [p, pend) (no alternation at top level) at s.
 * Handles one atom at a time, applying any quantifier, then recurses for
 * the rest of the sequence. */
static const char *m_seq(MS *ms, const char *p, const char *pend,
                          const char *s)
{
    if (ms->depth > RE_MAXDEPTH) return NULL;

    /* Skip to end: empty pattern matches */
    if (p >= pend) return s;

    /* Anchor atoms don't consume text: handle specially */
    if (*p == '^' || *p == '$') {
        const char *ns = m_atom_here(ms, p, s);
        if (!ns) return NULL;
        return m_seq(ms, p + 1, pend, ns);
    }

    const char *ae  = skip_atom(p);    /* pointer past the atom */
    const char *qp  = ae;              /* pointer to quantifier char */
    char        qc  = *qp;
    const char *next_p;

    if (qc == '*' || qc == '+' || qc == '?') {
        next_p = qp + 1;
    } else {
        qc     = 0;
        next_p = ae;
    }

    if (qc == '?') {
        /* Try with one occurrence, then without */
        MS saved = *ms;
        const char *ns = m_atom_here(ms, p, s);
        if (ns) {
            const char *r = m_seq(ms, next_p, pend, ns);
            if (r) return r;
        }
        *ms = saved;
        return m_seq(ms, next_p, pend, s);
    }

    if (qc == '*' || qc == '+') {
        /* Greedy: try maximum repetitions first, then backtrack */
        /* Collect all possible match positions */
        const char *positions[4096];
        int         npos = 0;

        if (qc == '*') {
            positions[npos++] = s;   /* zero repetitions */
        }

        MS tmp = *ms;
        const char *cur = s;
        while (npos < 4096) {
            const char *ns = m_atom_here(&tmp, p, cur);
            if (!ns || ns == cur) break;   /* no progress */
            positions[npos++] = ns;
            cur = ns;
        }

        /* Try from longest match downward */
        for (int i = npos - 1; i >= 0; i--) {
            MS trial = *ms;
            const char *r = m_seq(&trial, next_p, pend, positions[i]);
            if (r) { *ms = trial; return r; }
        }
        return NULL;
    }

    /* No quantifier: match exactly once */
    const char *ns = m_atom_here(ms, p, s);
    if (!ns) return NULL;
    return m_seq(ms, next_p, pend, ns);
}

/* m_alt: match [p, pend) handling top-level '|' alternation at s. */
static const char *m_alt(MS *ms, const char *p, const char *pend,
                          const char *s)
{
    if (ms->depth > RE_MAXDEPTH) return NULL;

    const char *bar = find_top_alt(p, pend);
    if (!bar) {
        /* No alternation: just match the sequence */
        ms->depth++;
        const char *r = m_seq(ms, p, pend, s);
        ms->depth--;
        return r;
    }

    /* Try left alternative */
    MS saved = *ms;
    ms->depth++;
    const char *r = m_seq(ms, p, bar, s);
    ms->depth--;
    if (r) return r;

    /* Try right alternative(s) */
    *ms = saved;
    ms->depth++;
    r = m_alt(ms, bar + 1, pend, s);
    ms->depth--;
    return r;
}

/* =========================================================================
 * Public API
 * ======================================================================= */

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    (void)cflags;
    if (!preg || !pattern) return REG_BADPAT;

    preg->pattern = (char *)malloc(strlen(pattern) + 1);
    if (!preg->pattern) return REG_ESPACE;
    strcpy(preg->pattern, pattern);
    preg->nsub = count_groups(pattern);
    return 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)eflags;
    if (!preg || !string) return REG_NOMATCH;

    const char *pat    = preg->pattern;
    const char *pend   = pat + strlen(pat);
    int         nsub   = preg->nsub;

    /* Try starting the match at each position in the string */
    for (const char *s = string; ; s++) {

        MS ms;
        ms.text = string;
        ms.pat  = pat;
        ms.nsub = (nsub < RE_MAXGROUPS - 1) ? nsub : RE_MAXGROUPS - 1;
        ms.depth = 0;
        for (int i = 0; i < RE_MAXGROUPS; i++) ms.so[i] = ms.eo[i] = -1;

        const char *end = m_alt(&ms, pat, pend, s);
        if (end) {
            /* Fill in pmatch[] */
            if (nmatch > 0 && pmatch) {
                pmatch[0].rm_so = (regoff_t)(s   - string);
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            for (size_t i = 1; i < nmatch; i++) {
                if ((int)i <= ms.nsub) {
                    pmatch[i].rm_so = ms.so[i];
                    pmatch[i].rm_eo = ms.eo[i];
                } else {
                    pmatch[i].rm_so = pmatch[i].rm_eo = -1;
                }
            }
            return 0;  /* success */
        }

        if (!*s) break;  /* end of string, no match found */
    }

    return REG_NOMATCH;
}

void regfree(regex_t *preg)
{
    if (preg && preg->pattern) {
        free(preg->pattern);
        preg->pattern = NULL;
    }
}
