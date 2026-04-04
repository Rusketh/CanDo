/*
 * lib/net.c -- Networking standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "net.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/array.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#endif

static int net_lookup(CandoVM *vm, int argc, CandoValue *args)
{
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    if (!host) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    struct hostent *server = gethostbyname(host);
    if (!server || server->h_addr_list[0] == NULL) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr     = cando_bridge_resolve(vm, arr_val.as.handle);

    for (int i = 0; server->h_addr_list[i] != NULL; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, server->h_addr_list[i], ip, INET_ADDRSTRLEN);
        CdoString *cs = cdo_string_new(ip, (u32)strlen(ip));
        cdo_array_push(arr, cdo_string_value(cs));
        cdo_string_release(cs);
    }

    cando_vm_push(vm, arr_val);
    return 1;
}

void cando_lib_net_register(CandoVM *vm)
{
#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    CandoValue net_val = cando_bridge_new_object(vm);
    CdoObject *net_obj = cando_bridge_resolve(vm, net_val.as.handle);

    libutil_set_method(vm, net_obj, "lookup", net_lookup);

    cando_vm_set_global(vm, "net", net_val, true);
}
