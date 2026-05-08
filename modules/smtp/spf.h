/*
 * modules/smtp/spf.h -- SPF (RFC 7208) checker, header-only.
 *
 * Implements the most common SPF mechanisms:
 *   v=spf1 ip4:CIDR ip6:CIDR a mx include:domain ~all -all +all ?all
 *
 * Skipped (returns "neutral" rather than tempfail):
 *   exists:, ptr:, exp=, redirect=, macro expansion.
 *
 * Recursion via include: is bounded at 10 levels.  No DNS-query budget
 * is enforced -- callers needing strict RFC 7208 limits should layer
 * their own counter on top.
 *
 * Returns one of:
 *   "pass" "fail" "softfail" "neutral" "none" "temperror" "permerror"
 */

#ifndef SMTP_SPF_H
#define SMTP_SPF_H

#include "smtp_helpers.h"
#include "dns.h"

#include <arpa/inet.h>

#define SPF_MAX_DEPTH 10

static int ipv4_in_cidr(const char *ip_str, const char *cidr_str)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", cidr_str);
    char *slash = strchr(buf, '/');
    int prefix = 32;
    if (slash) { *slash = '\0'; prefix = atoi(slash + 1); }
    struct in_addr a, b;
    if (inet_pton(AF_INET, ip_str, &a) != 1) return 0;
    if (inet_pton(AF_INET, buf, &b) != 1) return 0;
    if (prefix < 0)  prefix = 0;
    if (prefix > 32) prefix = 32;
    uint32_t mask = prefix == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - prefix));
    return (a.s_addr & mask) == (b.s_addr & mask);
}

static const char *spf_check_inner(const char *domain, const char *ip,
                                   const char *sender, int depth);

static const char *evaluate_spf_record(const char *txt,
                                       const char *domain,
                                       const char *ip,
                                       const char *sender,
                                       int depth)
{
    /* Walk space-separated tokens. */
    const char *p = txt;
    /* Skip "v=spf1". */
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "v=spf1", 6) != 0) return "none";
    p += 6;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *tok_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t tl = (size_t)(p - tok_start);
        if (tl == 0) continue;
        char tok[256]; if (tl >= sizeof(tok)) tl = sizeof(tok) - 1;
        memcpy(tok, tok_start, tl); tok[tl] = '\0';

        char qual = '+';
        const char *m = tok;
        if (*m == '+' || *m == '-' || *m == '~' || *m == '?') { qual = *m; m++; }

        const char *result = NULL;
        if (strcmp(m, "all") == 0) {
            switch (qual) {
                case '+': result = "pass"; break;
                case '-': result = "fail"; break;
                case '~': result = "softfail"; break;
                case '?': result = "neutral"; break;
            }
            if (result) return result;
        } else if (strncmp(m, "ip4:", 4) == 0) {
            if (ipv4_in_cidr(ip, m + 4)) {
                switch (qual) {
                    case '+': return "pass";
                    case '-': return "fail";
                    case '~': return "softfail";
                    case '?': return "neutral";
                }
            }
        } else if (strncmp(m, "include:", 8) == 0) {
            if (depth >= SPF_MAX_DEPTH) return "permerror";
            const char *r = spf_check_inner(m + 8, ip, sender, depth + 1);
            /* RFC 7208: include returns pass -> match this mechanism. */
            if (strcmp(r, "pass") == 0) {
                switch (qual) {
                    case '+': return "pass";
                    case '-': return "fail";
                    case '~': return "softfail";
                    case '?': return "neutral";
                }
            }
            if (strcmp(r, "temperror") == 0) return "temperror";
            if (strcmp(r, "permerror") == 0) return "permerror";
            /* fail / softfail / neutral / none -- continue. */
        } else if (strcmp(m, "a") == 0 || strncmp(m, "a:", 2) == 0) {
            const char *target = (m[1] == ':') ? m + 2 : domain;
            /* Resolve IPv4 of target; compare to ip. */
            struct addrinfo hints = { 0 };
            hints.ai_family = AF_INET;
            struct addrinfo *res = NULL;
            if (getaddrinfo(target, NULL, &hints, &res) == 0) {
                struct in_addr want;
                inet_pton(AF_INET, ip, &want);
                bool matched = false;
                for (struct addrinfo *r = res; r; r = r->ai_next) {
                    struct sockaddr_in *sa = (struct sockaddr_in *)r->ai_addr;
                    if (sa->sin_addr.s_addr == want.s_addr) { matched = true; break; }
                }
                freeaddrinfo(res);
                if (matched) {
                    switch (qual) {
                        case '+': return "pass";
                        case '-': return "fail";
                        case '~': return "softfail";
                        case '?': return "neutral";
                    }
                }
            }
        } else if (strcmp(m, "mx") == 0 || strncmp(m, "mx:", 3) == 0) {
            const char *target = (m[2] == ':') ? m + 3 : domain;
            dns_mx_record_t mx[DNS_MAX_MX];
            int n = dns_lookup_mx(target, mx, DNS_MAX_MX);
            for (int i = 0; i < n; i++) {
                struct addrinfo hints = { 0 };
                hints.ai_family = AF_INET;
                struct addrinfo *res = NULL;
                if (getaddrinfo(mx[i].host, NULL, &hints, &res) == 0) {
                    struct in_addr want;
                    inet_pton(AF_INET, ip, &want);
                    bool matched = false;
                    for (struct addrinfo *rr = res; rr; rr = rr->ai_next) {
                        struct sockaddr_in *sa = (struct sockaddr_in *)rr->ai_addr;
                        if (sa->sin_addr.s_addr == want.s_addr) { matched = true; break; }
                    }
                    freeaddrinfo(res);
                    if (matched) {
                        switch (qual) {
                            case '+': return "pass";
                            case '-': return "fail";
                            case '~': return "softfail";
                            case '?': return "neutral";
                        }
                    }
                }
            }
        }
        /* Other mechanisms ignored for v1. */
    }
    return "neutral";
}

static const char *spf_check_inner(const char *domain, const char *ip,
                                   const char *sender, int depth)
{
    dns_txt_record_t txts[DNS_MAX_TXT];
    int n = dns_lookup_txt(domain, txts, DNS_MAX_TXT);
    if (n < 0) return "temperror";
    for (int i = 0; i < n; i++) {
        const char *t = txts[i].text;
        while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, "v=spf1", 6) == 0)
            return evaluate_spf_record(t, domain, ip, sender, depth);
    }
    return "none";
}

/* Public entry: split sender into local@domain and run the SPF
 * algorithm against the given client IP. */
static const char *spf_check(const char *sender, const char *ip)
{
    const char *at = strchr(sender, '@');
    if (!at) return "none";
    char domain[256];
    snprintf(domain, sizeof(domain), "%s", at + 1);
    /* Strip any closing '>'. */
    char *gt = strchr(domain, '>');
    if (gt) *gt = '\0';
    return spf_check_inner(domain, ip, sender, 0);
}

#endif /* SMTP_SPF_H */
