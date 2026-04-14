/*
 * cando_lib.c -- High-level public embedding API for CanDo.
 *
 * Implements the convenience functions declared in include/cando.h:
 *
 *   cando_open / cando_close   — VM lifecycle with global ref-counting
 *   cando_openlibs             — register all standard libraries
 *   cando_open_*lib            — register individual standard libraries
 *   cando_dofile               — compile + execute a .cdo file
 *   cando_dostring             — compile + execute a source string
 *   cando_loadstring           — compile a source string (no execution)
 *   cando_errmsg               — retrieve the last error message
 *   cando_version / _num       — version queries
 *
 * Global singleton management
 * ───────────────────────────
 * cdo_object_init() initialises a process-global string intern table and
 * meta-key cache.  It is not thread-safe to call concurrently, and must be
 * balanced by exactly one call to cdo_object_destroy_globals().
 *
 * We maintain an atomic reference counter (g_state_count).  The first
 * cando_open() call initialises the singleton; the last cando_close() call
 * tears it down.  A static mutex (g_state_mutex) serialises the
 * init/destroy operations.
 *
 * Must compile with gcc -std=c11.
 */

#include "core/common.h"
#include "core/thread_platform.h"
#include "vm/vm.h"
#include "vm/chunk.h"
#include "vm/debug.h"
#include "parser/parser.h"
#include "object/object.h"
#include "natives.h"

/* Library registration headers */
#include "lib/math.h"
#include "lib/file.h"
#include "lib/eval.h"
#include "lib/string.h"
#include "lib/include.h"
#include "lib/json.h"
#include "lib/csv.h"
#include "lib/thread.h"
#include "lib/os.h"
#include "lib/datetime.h"
#include "lib/array.h"
#include "lib/object.h"
#include "lib/crypto.h"
#include "lib/process.h"
#include "lib/net.h"
#include "lib/http.h"
#include "lib/https.h"

/* cando.h error codes and CANDO_API */
#include "../include/cando.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

/* Windows realpath shim */
#if defined(CANDO_PLATFORM_WINDOWS)
#  include <direct.h>
static char *cando_realpath(const char *path, char *out) {
    return _fullpath(out, path, PATH_MAX);
}
#else
#  include <stdlib.h>
static char *cando_realpath(const char *path, char *out) {
    return realpath(path, out);
}
#endif

/* =========================================================================
 * Global singleton reference count
 * ====================================================================== */

static _Atomic(int)   g_state_count = 0;
static cando_mutex_t  g_state_mutex;
static bool           g_mutex_inited = false;

/* Initialise the mutex exactly once via a double-checked approach at program
 * start.  We rely on static storage zero-init so g_mutex_inited starts false.
 * On most platforms pthread_mutex_t zero-init is valid; we still call init
 * to be safe.  The first cando_open() call always wins the race on systems
 * where _Atomic guarantees are provided. */
static void ensure_global_mutex(void)
{
    /* There is a tiny window before the first VM is created; in practice
     * library users call cando_open() from a single thread during startup,
     * so this is safe without a separate once-flag. */
    if (!g_mutex_inited) {
        cando_os_mutex_init(&g_state_mutex);
        g_mutex_inited = true;
    }
}

/* =========================================================================
 * VM lifecycle
 * ====================================================================== */

CANDO_API CandoVM *cando_open(void)
{
    ensure_global_mutex();

    /* Initialise the global object layer on the first open. */
    cando_os_mutex_lock(&g_state_mutex);
    int prev = g_state_count;
    g_state_count = prev + 1;
    if (prev == 0) {
        cdo_object_init();
    }
    cando_os_mutex_unlock(&g_state_mutex);

    /* Allocate and initialise the VM. */
    CandoVM *vm = (CandoVM *)cando_alloc(sizeof(CandoVM));
    cando_vm_init(vm, NULL);

    /* Register core native functions (print, type, toString). */
    for (u32 i = 0; cando_native_names[i] && cando_native_table[i]; i++) {
        cando_vm_register_native(vm, cando_native_names[i],
                                 cando_native_table[i]);
    }

    return vm;
}

CANDO_API void cando_close(CandoVM *vm)
{
    if (!vm) return;

    /* Wait for any spawned threads before destroying the VM. */
    cando_vm_wait_all_threads(vm);
    cando_vm_destroy(vm);
    cando_free(vm);

    /* Tear down the global object layer when the last VM is closed. */
    cando_os_mutex_lock(&g_state_mutex);
    int remaining = --g_state_count;
    if (remaining <= 0) {
        g_state_count = 0;
        cdo_object_destroy_globals();
    }
    cando_os_mutex_unlock(&g_state_mutex);
}

/* =========================================================================
 * Standard library openers
 * ====================================================================== */

CANDO_API void cando_openlibs(CandoVM *vm)
{
    cando_lib_math_register(vm);
    cando_lib_file_register(vm);
    cando_lib_eval_register(vm);
    cando_lib_string_register(vm);
    cando_lib_include_register(vm);
    cando_lib_json_register(vm);
    cando_lib_csv_register(vm);
    cando_lib_thread_register(vm);
    cando_lib_os_register(vm);
    cando_lib_datetime_register(vm);
    cando_lib_array_register(vm);
    cando_lib_object_register(vm);
    cando_lib_crypto_register(vm);
    cando_lib_process_register(vm);
    cando_lib_net_register(vm);
    /* http/https must register after json so res.json(value) can look up
     * the json.stringify method on the child VM's shared globals. */
    cando_lib_http_register(vm);
    cando_lib_https_register(vm);
}

CANDO_API void cando_open_mathlib(CandoVM *vm)     { cando_lib_math_register(vm);     }
CANDO_API void cando_open_filelib(CandoVM *vm)     { cando_lib_file_register(vm);     }
CANDO_API void cando_open_stringlib(CandoVM *vm)   { cando_lib_string_register(vm);   }
CANDO_API void cando_open_arraylib(CandoVM *vm)    { cando_lib_array_register(vm);    }
CANDO_API void cando_open_objectlib(CandoVM *vm)   { cando_lib_object_register(vm);   }
CANDO_API void cando_open_jsonlib(CandoVM *vm)     { cando_lib_json_register(vm);     }
CANDO_API void cando_open_csvlib(CandoVM *vm)      { cando_lib_csv_register(vm);      }
CANDO_API void cando_open_threadlib(CandoVM *vm)   { cando_lib_thread_register(vm);   }
CANDO_API void cando_open_oslib(CandoVM *vm)       { cando_lib_os_register(vm);       }
CANDO_API void cando_open_datetimelib(CandoVM *vm) { cando_lib_datetime_register(vm); }
CANDO_API void cando_open_cryptolib(CandoVM *vm)   { cando_lib_crypto_register(vm);   }
CANDO_API void cando_open_processlib(CandoVM *vm)  { cando_lib_process_register(vm);  }
CANDO_API void cando_open_netlib(CandoVM *vm)      { cando_lib_net_register(vm);      }
CANDO_API void cando_open_evallib(CandoVM *vm)     { cando_lib_eval_register(vm);     }
CANDO_API void cando_open_includelib(CandoVM *vm)  { cando_lib_include_register(vm);  }
CANDO_API void cando_open_httplib(CandoVM *vm)     { cando_lib_http_register(vm);     }
CANDO_API void cando_open_httpslib(CandoVM *vm)    { cando_lib_https_register(vm);    }

/* =========================================================================
 * Internal: compile source into a chunk
 * ====================================================================== */

static int compile_source(CandoVM *vm,
                           const char *src, usize src_len,
                           const char *name,
                           CandoChunk **chunk_out)
{
    CandoChunk *chunk = cando_chunk_new(name ? name : "<string>", 0, false);
    CandoParser parser;
    cando_parser_init(&parser, src, src_len, chunk);

    if (!cando_parse(&parser)) {
        const char *err = cando_parser_error(&parser);
        snprintf(vm->error_msg, sizeof(vm->error_msg), "%s",
                 err ? err : "parse error");
        vm->has_error = true;
        cando_chunk_free(chunk);
        *chunk_out = NULL;
        return CANDO_ERR_PARSE;
    }

    *chunk_out = chunk;
    return CANDO_OK;
}

/* =========================================================================
 * Load and execute
 * ====================================================================== */

CANDO_API int cando_dofile(CandoVM *vm, const char *path)
{
    /* Read the source file. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "cannot open '%s'", path);
        vm->has_error = true;
        return CANDO_ERR_FILE;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "cannot seek in '%s'", path);
        vm->has_error = true;
        return CANDO_ERR_FILE;
    }

    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                 "cannot determine size of '%s'", path);
        vm->has_error = true;
        return CANDO_ERR_FILE;
    }
    rewind(f);

    char *source = (char *)cando_alloc((usize)(fsize + 1));
    usize nread  = fread(source, 1, (usize)fsize, f);
    source[nread] = '\0';
    fclose(f);

    /* Resolve the canonical path so include() resolves relative paths
     * relative to the script's directory, not the CWD. */
    char script_path[PATH_MAX];
    if (!cando_realpath(path, script_path)) {
        strncpy(script_path, path, PATH_MAX - 1);
        script_path[PATH_MAX - 1] = '\0';
    }

    /* Compile. */
    CandoChunk *chunk = NULL;
    int rc = compile_source(vm, source, nread, script_path, &chunk);
    cando_free(source);
    if (rc != CANDO_OK)
        return rc;

    /* Execute. */
    CandoVMResult result = cando_vm_exec(vm, chunk);
    cando_chunk_free(chunk);

    if (result == VM_RUNTIME_ERR)
        return CANDO_ERR_RUNTIME;

    return CANDO_OK;
}

CANDO_API int cando_dostring(CandoVM *vm, const char *src, const char *name)
{
    usize len = strlen(src);
    CandoChunk *chunk = NULL;
    int rc = compile_source(vm, src, len, name, &chunk);
    if (rc != CANDO_OK)
        return rc;

    CandoVMResult result = cando_vm_exec(vm, chunk);
    cando_chunk_free(chunk);

    if (result == VM_RUNTIME_ERR)
        return CANDO_ERR_RUNTIME;

    return CANDO_OK;
}

CANDO_API int cando_loadstring(CandoVM *vm, const char *src, const char *name,
                                CandoChunk **chunk_out)
{
    return compile_source(vm, src, strlen(src), name, chunk_out);
}

/* =========================================================================
 * Error inspection
 * ====================================================================== */

CANDO_API const char *cando_errmsg(const CandoVM *vm)
{
    if (!vm || !vm->has_error)
        return "";
    return vm->error_msg;
}

/* =========================================================================
 * Version
 * ====================================================================== */

CANDO_API const char *cando_version(void)
{
    return CANDO_VERSION;
}

CANDO_API int cando_version_num(void)
{
    return CANDO_VERSION_NUM;
}
