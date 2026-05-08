/*
 * modules/smtp/dns.h -- DNS helpers used by the SMTP module.
 *
 * Provides MX, TXT, and reverse PTR lookups in a header-only,
 * cross-platform way:
 *
 *   - POSIX: libresolv (`res_query` + `dn_expand`).
 *   - Windows: dnsapi.lib (`DnsQuery_A`).
 *
 * The functions here populate caller-provided arrays of dns_*_record_t
 * up to a fixed cap.  Returning a fixed cap avoids any allocator
 * surprises on the hot path.
 *
 * Must compile with gcc -std=c11 (POSIX or MinGW).
 */

#ifndef SMTP_DNS_H
#define SMTP_DNS_H

#include "smtp_helpers.h"

#include <stdint.h>
#include <stddef.h>

#define DNS_MAX_MX   32
#define DNS_MAX_TXT  16
#define DNS_MAX_PTR   8
#define DNS_HOST_MAX 256

typedef struct {
    int  priority;
    char host[DNS_HOST_MAX];
} dns_mx_record_t;

typedef struct {
    char text[1024];
} dns_txt_record_t;

typedef struct {
    char host[DNS_HOST_MAX];
} dns_ptr_record_t;

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
/* ----------------------------------------------------------------------
 * Windows (DnsQuery_A from dnsapi.lib)
 *
 * Include winsock2.h before windows.h so the legacy winsock1 headers
 * pulled in by windows.h don't conflict with anything else in this
 * translation unit (sockutil.h, spf.h).
 * -------------------------------------------------------------------- */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

static int dns_lookup_mx(const char *name, dns_mx_record_t *out, int max)
{
    PDNS_RECORD rec = NULL;
    DNS_STATUS s = DnsQuery_A(name, DNS_TYPE_MX, DNS_QUERY_STANDARD,
                               NULL, &rec, NULL);
    if (s != 0) return -1;
    int n = 0;
    for (PDNS_RECORD p = rec; p && n < max; p = p->pNext) {
        if (p->wType != DNS_TYPE_MX) continue;
        out[n].priority = (int)p->Data.MX.wPreference;
        snprintf(out[n].host, DNS_HOST_MAX, "%s",
                 p->Data.MX.pNameExchange ? p->Data.MX.pNameExchange : "");
        n++;
    }
    DnsRecordListFree(rec, DnsFreeRecordList);
    return n;
}

static int dns_lookup_txt(const char *name, dns_txt_record_t *out, int max)
{
    PDNS_RECORD rec = NULL;
    DNS_STATUS s = DnsQuery_A(name, DNS_TYPE_TEXT, DNS_QUERY_STANDARD,
                               NULL, &rec, NULL);
    if (s != 0) return -1;
    int n = 0;
    for (PDNS_RECORD p = rec; p && n < max; p = p->pNext) {
        if (p->wType != DNS_TYPE_TEXT) continue;
        out[n].text[0] = '\0';
        size_t off = 0;
        for (DWORD i = 0; i < p->Data.TXT.dwStringCount; i++) {
            const char *s = p->Data.TXT.pStringArray[i];
            if (!s) continue;
            size_t L = strlen(s);
            if (off + L >= sizeof(out[n].text)) break;
            memcpy(out[n].text + off, s, L);
            off += L;
        }
        out[n].text[off] = '\0';
        n++;
    }
    DnsRecordListFree(rec, DnsFreeRecordList);
    return n;
}

static int dns_lookup_ptr(const char *ip, dns_ptr_record_t *out, int max)
{
    /* Build the in-addr.arpa name. */
    char arpa[256];
    /* Reverse-octet IPv4. */
    int a, b, c, d;
    if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    snprintf(arpa, sizeof(arpa), "%d.%d.%d.%d.in-addr.arpa", d, c, b, a);
    PDNS_RECORD rec = NULL;
    DNS_STATUS s = DnsQuery_A(arpa, DNS_TYPE_PTR, DNS_QUERY_STANDARD,
                               NULL, &rec, NULL);
    if (s != 0) return -1;
    int n = 0;
    for (PDNS_RECORD p = rec; p && n < max; p = p->pNext) {
        if (p->wType != DNS_TYPE_PTR) continue;
        snprintf(out[n].host, DNS_HOST_MAX, "%s",
                 p->Data.PTR.pNameHost ? p->Data.PTR.pNameHost : "");
        n++;
    }
    DnsRecordListFree(rec, DnsFreeRecordList);
    return n;
}

#else
/* ----------------------------------------------------------------------
 * POSIX (libresolv)
 * -------------------------------------------------------------------- */
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ns_initparse / ns_parserr are only declared when ns_msg is in scope
 * (NS_PACKETSZ etc).  Keep the guard simple. */

static int dns_lookup_mx(const char *name, dns_mx_record_t *out, int max)
{
    unsigned char buf[NS_PACKETSZ * 4];
    int len = res_query(name, ns_c_in, ns_t_mx, buf, sizeof(buf));
    if (len < 0) return -1;
    ns_msg msg;
    if (ns_initparse(buf, len, &msg) < 0) return -1;
    int n = 0;
    int total = ns_msg_count(msg, ns_s_an);
    for (int i = 0; i < total && n < max; i++) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != ns_t_mx) continue;
        const unsigned char *rd = ns_rr_rdata(rr);
        int prio = ns_get16(rd);
        char host[DNS_HOST_MAX];
        if (dn_expand(ns_msg_base(msg), ns_msg_end(msg), rd + 2,
                       host, sizeof(host)) < 0) continue;
        out[n].priority = prio;
        snprintf(out[n].host, DNS_HOST_MAX, "%s", host);
        n++;
    }
    /* Sort by priority (ascending). */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (out[j].priority > out[j+1].priority) {
                dns_mx_record_t t = out[j]; out[j] = out[j+1]; out[j+1] = t;
            }
        }
    }
    return n;
}

static int dns_lookup_txt(const char *name, dns_txt_record_t *out, int max)
{
    unsigned char buf[NS_PACKETSZ * 4];
    int len = res_query(name, ns_c_in, ns_t_txt, buf, sizeof(buf));
    if (len < 0) return -1;
    ns_msg msg;
    if (ns_initparse(buf, len, &msg) < 0) return -1;
    int n = 0;
    int total = ns_msg_count(msg, ns_s_an);
    for (int i = 0; i < total && n < max; i++) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != ns_t_txt) continue;
        const unsigned char *rd = ns_rr_rdata(rr);
        int rl = (int)ns_rr_rdlen(rr);
        size_t off = 0;
        out[n].text[0] = '\0';
        int p = 0;
        while (p < rl) {
            int L = rd[p++];
            if (p + L > rl) break;
            if (off + (size_t)L >= sizeof(out[n].text) - 1) break;
            memcpy(out[n].text + off, rd + p, L);
            off += L;
            p += L;
        }
        out[n].text[off] = '\0';
        n++;
    }
    return n;
}

static int dns_lookup_ptr(const char *ip, dns_ptr_record_t *out, int max)
{
    char arpa[256];
    int a, b, c, d;
    if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    snprintf(arpa, sizeof(arpa), "%d.%d.%d.%d.in-addr.arpa", d, c, b, a);

    unsigned char buf[NS_PACKETSZ * 4];
    int len = res_query(arpa, ns_c_in, ns_t_ptr, buf, sizeof(buf));
    if (len < 0) return -1;
    ns_msg msg;
    if (ns_initparse(buf, len, &msg) < 0) return -1;
    int n = 0;
    int total = ns_msg_count(msg, ns_s_an);
    for (int i = 0; i < total && n < max; i++) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != ns_t_ptr) continue;
        char host[DNS_HOST_MAX];
        if (dn_expand(ns_msg_base(msg), ns_msg_end(msg), ns_rr_rdata(rr),
                       host, sizeof(host)) < 0) continue;
        snprintf(out[n].host, DNS_HOST_MAX, "%s", host);
        n++;
    }
    return n;
}

#endif /* platform */

#endif /* SMTP_DNS_H */
