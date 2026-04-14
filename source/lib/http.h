/*
 * lib/http.h -- HTTP standard library for Cando.
 *
 * Registers:
 *   - A global `http` object with:
 *       http.request(opts)              -- full-featured HTTP client
 *       http.get(url)                   -- simple GET convenience
 *       http.createServer(callback)     -- create an HTTP server
 *   - A global `fetch(url[, opts])` function (auto http/https).
 *
 * Server objects expose:
 *   server.listen(port [, host])        -- start accept loop on background thread
 *   server.close()                      -- stop the server
 *
 * Response objects passed to the request callback expose:
 *   res.status(code)                    -- set status code
 *   res.setHeader(name, value)          -- set a response header
 *   res.send(body)                      -- send the response
 *   res.json(value)                     -- JSON-stringify + send
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_HTTP_H
#define CANDO_LIB_HTTP_H

#include "../vm/vm.h"

/*
 * cando_lib_http_register -- create the `http` global object and register
 * all methods on it, plus register the global fetch() function.
 */
CANDO_API void cando_lib_http_register(CandoVM *vm);

/* -------------------------------------------------------------------------
 * Internal helpers shared with https.c.  These are NOT part of the public
 * CanDo API; they exist so the HTTPS library can reuse the server machinery
 * and response-context pool without duplicating code.
 * ---------------------------------------------------------------------- */

struct SSL_CTX_st;  /* forward — avoid including <openssl/ssl.h> here */

/*
 * http_do_request_native -- shared implementation for http.request,
 * https.request, and fetch.  args[0] must be either a string (URL) or an
 * options object.  is_tls_hint forces TLS if true; otherwise the scheme
 * in the URL decides.  Returns the normal native result convention.
 */
int http_do_request_native(CandoVM *vm, int argc, CandoValue *args,
                           bool is_tls_hint);

/*
 * http_create_server_native -- shared implementation that creates a server
 * object.  If ssl_ctx is non-NULL the server is TLS; the ctx is owned by
 * the server and freed on close.
 */
int http_create_server_native_impl(CandoVM *vm, CandoValue callback,
                                   struct SSL_CTX_st *ssl_ctx);

#endif /* CANDO_LIB_HTTP_H */
