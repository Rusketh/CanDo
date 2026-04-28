/*
 * tests/test_sockutil.c -- Unit tests for the sockutil cross-platform
 *                           TCP and TLS primitives.
 *
 * Each test exercises a specific concern: address parsing, listener bind
 * + accept, blocking recv/send roundtrip on loopback, error paths.  The
 * tests do not require the VM or any standard library; they link directly
 * against sockutil.c and its dependencies.
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "thread_platform.h"
#include "sockutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if defined(CANDO_PLATFORM_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#endif

/* -----------------------------------------------------------------------
 * Minimal test harness (mirrors test_thread.c)
 * --------------------------------------------------------------------- */
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_run++; \
        if (cond) { g_passed++; } \
        else { g_failed++; \
               fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))

static void run_test(const char *name, void (*fn)(void))
{
    printf("  %-52s ", name);
    fflush(stdout);
    int before = g_failed;
    fn();
    printf("%s\n", g_failed == before ? "OK" : "FAILED");
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Discover the kernel-assigned port that `fd` is bound to. */
static int local_port(sockutil_socket_t fd)
{
    struct sockaddr_storage sa;
    socklen_t len = 0;
    if (!sockutil_get_local_addr(fd, &sa, &len)) return -1;
    int port = 0;
    char host[64];
    if (!sockutil_addr_to_string(&sa, len, host, sizeof(host), &port, NULL))
        return -1;
    return port;
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

TEST(test_one_time_init_is_idempotent)
{
    /* Call multiple times: the atomic compare-exchange should make all
     * subsequent calls no-ops without crashing. */
    sockutil_one_time_init();
    sockutil_one_time_init();
    sockutil_one_time_init();
    EXPECT_TRUE(true);  /* survival is the assertion */
}

TEST(test_resolve_localhost)
{
    char addrs[8][INET6_ADDRSTRLEN];
    int n = sockutil_resolve("127.0.0.1", AF_INET, addrs, 8);
    EXPECT_TRUE(n >= 1);
    if (n >= 1) EXPECT_EQ(0, strcmp(addrs[0], "127.0.0.1"));
}

TEST(test_resolve_invalid_host)
{
    char addrs[2][INET6_ADDRSTRLEN];
    int n = sockutil_resolve("definitely.not.a.real.tld.invalid",
                             AF_UNSPEC, addrs, 2);
    EXPECT_EQ(0, n);
}

TEST(test_listen_and_local_port_assigned)
{
    char err[160] = {0};
    sockutil_socket_t srv = sockutil_tcp_listen("127.0.0.1", 0, AF_INET,
                                                64, err, sizeof(err));
    EXPECT_NEQ(SOCKUTIL_INVALID_SOCKET, srv);
    if (srv == SOCKUTIL_INVALID_SOCKET) return;

    int port = local_port(srv);
    EXPECT_TRUE(port > 0);
    EXPECT_TRUE(port <= 65535);

    sockutil_close(srv);
}

TEST(test_connect_refused_on_unbound_port)
{
    /* Port 1 is reserved and almost certainly closed; connect should fail.
     * On systems where port 1 is somehow open this would spuriously pass —
     * the more important assertion is that the call returns cleanly without
     * crashing or hanging. */
    char err[160] = {0};
    sockutil_socket_t fd = sockutil_tcp_connect("127.0.0.1", 1, AF_INET,
                                                500, err, sizeof(err));
    EXPECT_EQ(SOCKUTIL_INVALID_SOCKET, fd);
    EXPECT_TRUE(err[0] != '\0');
}

/* Shared state for the loopback echo test. */
typedef struct EchoCtx {
    sockutil_socket_t listener;
    int               port;
    _Atomic(int)      connections_handled;
} EchoCtx;

static CANDO_THREAD_RETURN echo_server_thread(void *arg)
{
    EchoCtx *ctx = (EchoCtx *)arg;
    /* Accept exactly one connection, echo whatever arrives until EOF. */
    sockutil_socket_t cfd = sockutil_tcp_accept(ctx->listener, 5000,
                                                NULL, NULL, NULL, 0);
    if (cfd == SOCKUTIL_INVALID_SOCKET) return CANDO_THREAD_RETURN_VAL;

    char buf[1024];
    int n;
    while ((n = sockutil_recv_raw(cfd, buf, sizeof(buf))) > 0) {
        if (!sockutil_send_all(cfd, NULL, buf, (size_t)n)) break;
    }
    sockutil_close(cfd);
    atomic_fetch_add(&ctx->connections_handled, 1);
    return CANDO_THREAD_RETURN_VAL;
}

TEST(test_loopback_echo)
{
    EchoCtx ctx;
    char err[160] = {0};
    ctx.listener = sockutil_tcp_listen("127.0.0.1", 0, AF_INET,
                                       64, err, sizeof(err));
    EXPECT_NEQ(SOCKUTIL_INVALID_SOCKET, ctx.listener);
    if (ctx.listener == SOCKUTIL_INVALID_SOCKET) return;

    ctx.port = local_port(ctx.listener);
    EXPECT_TRUE(ctx.port > 0);
    atomic_store(&ctx.connections_handled, 0);

    cando_thread_t srv;
    EXPECT_TRUE(cando_os_thread_create(&srv, echo_server_thread, &ctx));

    sockutil_socket_t client = sockutil_tcp_connect("127.0.0.1", ctx.port,
                                                    AF_INET, 5000,
                                                    err, sizeof(err));
    EXPECT_NEQ(SOCKUTIL_INVALID_SOCKET, client);

    const char *payload = "hello-loopback";
    EXPECT_TRUE(sockutil_send_all(client, NULL, payload, strlen(payload)));

    /* Read exactly strlen(payload) bytes back. */
    char in[64] = {0};
    size_t got = 0;
    while (got < strlen(payload)) {
        int n = sockutil_recv_raw(client, in + got,
                                  (int)(sizeof(in) - 1 - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    EXPECT_EQ(strlen(payload), got);
    EXPECT_EQ(0, memcmp(in, payload, got));

    sockutil_close(client);
    cando_os_thread_join(srv);
    sockutil_close(ctx.listener);
    EXPECT_EQ(1, atomic_load(&ctx.connections_handled));
}

TEST(test_addr_to_string_ipv4)
{
    struct sockaddr_in sa4;
    memset(&sa4, 0, sizeof(sa4));
    sa4.sin_family = AF_INET;
    sa4.sin_port   = htons(8080);
    inet_pton(AF_INET, "192.0.2.42", &sa4.sin_addr);

    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    memcpy(&ss, &sa4, sizeof(sa4));

    char host[64] = {0};
    int  port = 0, family = 0;
    EXPECT_TRUE(sockutil_addr_to_string(&ss, sizeof(sa4),
                                        host, sizeof(host), &port, &family));
    EXPECT_EQ(0, strcmp(host, "192.0.2.42"));
    EXPECT_EQ(8080, port);
    EXPECT_EQ(AF_INET, family);
}

TEST(test_set_options_on_open_socket)
{
    /* Allocate a regular TCP socket directly so we can verify setsockopt
     * wrappers without involving DNS or a remote peer. */
    sockutil_one_time_init();
    sockutil_socket_t fd = (sockutil_socket_t)socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NEQ(SOCKUTIL_INVALID_SOCKET, fd);
    if (fd == SOCKUTIL_INVALID_SOCKET) return;

    EXPECT_TRUE(sockutil_set_reuseaddr(fd, true));
    EXPECT_TRUE(sockutil_set_keepalive(fd, true));
    EXPECT_TRUE(sockutil_set_nodelay(fd, true));
    EXPECT_TRUE(sockutil_set_recvbuf(fd, 16384));
    EXPECT_TRUE(sockutil_set_sendbuf(fd, 16384));
    sockutil_set_timeout(fd, 250);
    sockutil_set_timeout(fd, 0);  /* reset */

    sockutil_close(fd);
}

TEST(test_close_invalid_is_safe)
{
    /* All these should be no-ops on invalid inputs. */
    sockutil_close(SOCKUTIL_INVALID_SOCKET);
    sockutil_shutdown(SOCKUTIL_INVALID_SOCKET);
    sockutil_set_timeout(SOCKUTIL_INVALID_SOCKET, 100);
    EXPECT_FALSE(sockutil_set_blocking(SOCKUTIL_INVALID_SOCKET, true));
    EXPECT_FALSE(sockutil_set_reuseaddr(SOCKUTIL_INVALID_SOCKET, true));
    EXPECT_FALSE(sockutil_set_nodelay(SOCKUTIL_INVALID_SOCKET, true));
    EXPECT_FALSE(sockutil_set_keepalive(SOCKUTIL_INVALID_SOCKET, true));
    EXPECT_FALSE(sockutil_set_recvbuf(SOCKUTIL_INVALID_SOCKET, 1024));
    EXPECT_FALSE(sockutil_set_sendbuf(SOCKUTIL_INVALID_SOCKET, 1024));
    EXPECT_EQ(-1, sockutil_send_raw(SOCKUTIL_INVALID_SOCKET, "x", 1));
    char buf[1];
    EXPECT_EQ(-1, sockutil_recv_raw(SOCKUTIL_INVALID_SOCKET, buf, 1));
}

TEST(test_tls_client_ctx_default)
{
    /* Build a verify-disabled client context.  Should always succeed if
     * OpenSSL is linked correctly. */
    SockutilTlsClientOpts opts = { 0 };
    opts.verify_peer = false;
    char err[160] = {0};
    SSL_CTX *ctx = sockutil_build_client_ssl_ctx(&opts, err, sizeof(err));
    EXPECT_TRUE(ctx != NULL);
    if (ctx) SSL_CTX_free(ctx);
}

TEST(test_tls_server_ctx_rejects_garbage)
{
    char err[160] = {0};
    SSL_CTX *ctx = sockutil_build_server_ssl_ctx("not a pem", 9,
                                                 "still not a pem", 15,
                                                 NULL, err, sizeof(err));
    EXPECT_TRUE(ctx == NULL);
    EXPECT_TRUE(err[0] != '\0');
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void)
{
    printf("\n=== sockutil unit tests ===\n\n");
    printf("-- One-time init --\n");
    run_test("init is idempotent", test_one_time_init_is_idempotent);

    printf("\n-- Address resolution --\n");
    run_test("resolve localhost",  test_resolve_localhost);
    run_test("resolve invalid",    test_resolve_invalid_host);
    run_test("addr_to_string IPv4", test_addr_to_string_ipv4);

    printf("\n-- Listener / connect --\n");
    run_test("listen assigns port",          test_listen_and_local_port_assigned);
    run_test("connect refused on closed port", test_connect_refused_on_unbound_port);

    printf("\n-- Loopback I/O --\n");
    run_test("echo across thread", test_loopback_echo);

    printf("\n-- Socket options --\n");
    run_test("set options on open socket", test_set_options_on_open_socket);
    run_test("safe on invalid socket",      test_close_invalid_is_safe);

    printf("\n-- TLS contexts --\n");
    run_test("client ctx default", test_tls_client_ctx_default);
    run_test("server ctx rejects garbage", test_tls_server_ctx_rejects_garbage);

    printf("\n--------------------------\n");
    printf("Results: %d/%d passed", g_passed, g_run);
    if (g_failed) printf(", %d FAILED", g_failed);
    printf("\n");

    return g_failed ? 1 : 0;
}
