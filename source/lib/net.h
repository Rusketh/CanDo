/*
 * lib/net.h -- Networking standard library for Cando.
 *
 * Registers a global `net` object with:
 *
 *   net.lookup(host)         → array of IP strings
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_NET_H
#define CANDO_LIB_NET_H

#include "../vm/vm.h"

void cando_lib_net_register(CandoVM *vm);

#endif /* CANDO_LIB_NET_H */
