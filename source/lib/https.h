/*
 * lib/https.h -- HTTPS standard library for Cando.
 *
 * Registers a global `https` object with:
 *   https.request(opts)           -- HTTPS client request (same API as http.request)
 *   https.get(url)                -- HTTPS convenience GET
 *   https.createServer(opts, cb)  -- HTTPS server; opts = { cert, key }
 *
 * Internally delegates to the shared HTTP plumbing in http.c / httputil.c.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_HTTPS_H
#define CANDO_LIB_HTTPS_H

#include "../vm/vm.h"

/*
 * cando_lib_https_register -- create the `https` global object and register
 * all methods on it.
 *
 * Must be called AFTER cando_lib_http_register() because https.request /
 * https.get internally use http_do_request_native (and the shared server
 * pools are initialised by the http lib).
 */
CANDO_API void cando_lib_https_register(CandoVM *vm);

#endif /* CANDO_LIB_HTTPS_H */
