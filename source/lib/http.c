#include "http.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

/* =========================================================================
 * Helper: Parse a URL into Host and Path
 * ======================================================================= */
static void parse_url(const char *url, char *host, char *path) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (slash) {
        strncpy(host, p, slash - p);
        host[slash - p] = '\0';
        strcpy(path, slash);
    } else {
        strcpy(host, p);
        strcpy(path, "/");
    }
}

/* =========================================================================
 * http.request(options) -> {status, body}
 * ======================================================================= */
static int http_request(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm, "http.request: expected options object");
        return -1;
    }

    CdoObject *opts = cando_bridge_resolve(vm, args[0].as.handle);
    CdoValue val;
    char host[256], path[1024], method[10] = "GET", url[1024] = "";

    // Extract URL
    if (cdo_object_get(opts, cdo_string_intern("url", 3), &val) && val.tag == CDO_STRING) {
        strcpy(url, val.as.string->data);
    } else {
        cando_vm_error(vm, "http.request: 'url' field is required");
        return -1;
    }

    // Extract Method
    if (cdo_object_get(opts, cdo_string_intern("method", 6), &val) && val.tag == CDO_STRING) {
        strncpy(method, val.as.string->data, 9);
    }

    parse_url(url, host, path);

    // Socket Setup
    struct hostent *server = gethostbyname(host);
    if (!server) { cando_vm_error(vm, "Could not resolve host"); return -1; }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr = { .sin_family = AF_INET, .sin_port = htons(80) };
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cando_vm_error(vm, "Connection failed");
        return -1;
    }

    // Build Request Header
    char request[4096];
    int len = sprintf(request, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, host);

    // Add Body if present (e.g. for POST)
    if (cdo_object_get(opts, cdo_string_intern("body", 4), &val) && val.tag == CDO_STRING) {
        len += sprintf(request + len, "Content-Length: %d\r\n\r\n%s", val.as.string->length, val.as.string->data);
    } else {
        len += sprintf(request + len, "\r\n");
    }

    write(sockfd, request, len);

    // Read Response
    char buffer[8192], response[16384] = "";
    int n, total = 0;
    while ((n = read(sockfd, buffer, sizeof(buffer)-1)) > 0) {
        memcpy(response + total, buffer, n);
        total += n;
    }
    close(sockfd);

    // Simple split of headers and body
    char *body_start = strstr(response, "\r\n\r\n");
    if (body_start) body_start += 4; else body_start = response;

    // Create return object {body: "..."}
    CandoValue res_val = cando_bridge_new_object(vm);
    CdoObject *res_obj = cando_bridge_resolve(vm, res_val.as.handle);
    cdo_object_rawset(res_obj, cdo_string_intern("body", 4), 
                      cdo_string_value(cdo_string_intern(body_start, strlen(body_start))), FIELD_NONE);

    cando_vm_push(vm, res_val);
    return 1;
}

void cando_lib_http_register(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, obj_val.as.handle);

    // Use set_method pattern from documentation
    CandoValue sentinel = cando_vm_add_native(vm, http_request);
    cdo_object_rawset(obj, cdo_string_intern("request", 7), 
                      cdo_number(sentinel.as.number), FIELD_STATIC);

    cando_vm_set_global(vm, "http", obj_val, true);
}