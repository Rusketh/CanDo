/*
 * lib/secure_socket.h -- TLS-wrapped TCP sockets for CanDo.
 *
 * Mirrors `socket` exactly with TLS layered on top:
 *   secure_socket.tcp([opts])                       -- unconnected TLS socket
 *   secure_socket.connect(host, port [, opts])      -- connected TLS client
 *   secure_socket.createServer(opts, callback)      -- TLS server (cert/key)
 *
 * Client opts (all optional unless noted):
 *   verifyPeer  bool   default true (safer default for the new API)
 *   ca          string PEM bundle of trust roots
 *   cert        string client certificate (mutual TLS)
 *   key         string private key matching cert
 *   serverName  string SNI override (defaults to host)
 *   timeout     number connect/read timeout in ms
 *   family      string "inet" / "inet6" / "any"
 *
 * Server opts:
 *   cert        string PEM certificate (REQUIRED)
 *   key         string PEM private key  (REQUIRED)
 *   verifyPeer  bool   default false; require client certificates
 *   ca          string PEM bundle of acceptable client CAs (with verifyPeer)
 *
 * Meta-tables:
 *   _meta.tls_socket  — connect/send/recv/close/... plus TLS introspection
 *   _meta.tls_server  — listen/close/localAddress/...
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_SECURE_SOCKET_H
#define CANDO_LIB_SECURE_SOCKET_H

#include "../vm/vm.h"

/*
 * cando_lib_secure_socket_register -- create the `secure_socket` global,
 * populate `_meta.tls_socket` and `_meta.tls_server`, and ensure sockutil
 * + the shared socket pool are initialised.
 */
CANDO_API void cando_lib_secure_socket_register(CandoVM *vm);

#endif /* CANDO_LIB_SECURE_SOCKET_H */
