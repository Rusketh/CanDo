/*
 * cando.h -- Public embedding API for the CanDo scripting language.
 *
 * This is the single header to include when embedding CanDo in a host
 * application, game engine, or platform framework.
 *
 *   #include <cando.h>
 *
 * Link with -lcando (shared) or -lcando_static (static).
 *
 * CanDo version:  1.0.0
 * Comparable to:  Lua 5.x embedding API
 *
 * Quick example
 * ─────────────
 *   CandoVM *vm = cando_open();
 *   cando_openlibs(vm);
 *   if (cando_dofile(vm, "game/main.cdo") != CANDO_OK)
 *       fprintf(stderr, "%s\n", cando_errmsg(vm));
 *   cando_close(vm);
 *
 * Must be compiled with a C11-capable compiler (gcc / clang / MSVC 2019+).
 */

#ifndef CANDO_H
#define CANDO_H

/* =========================================================================
 * Version
 * ====================================================================== */

#define CANDO_VERSION_MAJOR  1
#define CANDO_VERSION_MINOR  0
#define CANDO_VERSION_PATCH  0
#define CANDO_VERSION        "1.0.0"

/** Numeric version: major*10000 + minor*100 + patch.  Use for #if guards. */
#define CANDO_VERSION_NUM    10000

/* =========================================================================
 * Error codes returned by cando_dofile / cando_dostring / cando_loadstring
 * ====================================================================== */

/** No error — execution succeeded. */
#define CANDO_OK           0

/** File could not be opened or read. */
#define CANDO_ERR_FILE     1

/** Syntax or compilation error. */
#define CANDO_ERR_PARSE    2

/** Unhandled runtime error. */
#define CANDO_ERR_RUNTIME  3

/* =========================================================================
 * Low-level API headers
 *
 * These bring in the full VM, value, object, parser and bridge APIs for
 * advanced embedding use cases (native function registration, object
 * creation, direct stack manipulation, etc.).
 *
 * They are found on the include path set by the build system or pkg-config.
 * ====================================================================== */
#include "core/common.h"
#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "vm/chunk.h"
#include "vm/debug.h"
#include "object/object.h"
#include "object/array.h"
#include "object/string.h"
#include "parser/parser.h"
#include "natives.h"

/* Pull in all library registration headers so a host can selectively open
 * individual standard libraries.  See cando_openlibs() below. */
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

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * State lifecycle
 * ====================================================================== */

/**
 * cando_open — create and initialise a new CanDo state.
 *
 * Allocates a CandoVM, initialises the global object layer (thread-safe
 * reference-counted singleton), and registers the core native functions
 * (print, type, toString).  No standard libraries are loaded; call
 * cando_openlibs() or the individual cando_open_*lib() functions to enable
 * them.
 *
 * Returns a pointer to the new state, or NULL on allocation failure.
 *
 * Each call must be paired with exactly one call to cando_close().
 * Multiple VMs may coexist in the same process.
 */
CANDO_API CandoVM *cando_open(void);

/**
 * cando_close — destroy a CanDo state and release all resources.
 *
 * Waits for any threads spawned by the VM to finish, then destroys the VM
 * and frees its memory.  The last call to cando_close() in the process also
 * tears down the shared object layer (intern tables, meta-key strings).
 *
 * The pointer is invalid after this call.
 */
CANDO_API void cando_close(CandoVM *vm);

/* =========================================================================
 * Standard library openers
 *
 * These follow the luaopen_* / luaL_openlibs pattern: call them after
 * cando_open() to expose the desired standard library globals to scripts.
 * ====================================================================== */

/** Open all standard libraries (equivalent to calling each one below). */
CANDO_API void cando_openlibs(CandoVM *vm);

CANDO_API void cando_open_mathlib(CandoVM *vm);      /**< math.*            */
CANDO_API void cando_open_filelib(CandoVM *vm);      /**< file.*            */
CANDO_API void cando_open_stringlib(CandoVM *vm);    /**< string prototype  */
CANDO_API void cando_open_arraylib(CandoVM *vm);     /**< array prototype   */
CANDO_API void cando_open_objectlib(CandoVM *vm);    /**< object.*          */
CANDO_API void cando_open_jsonlib(CandoVM *vm);      /**< json.*            */
CANDO_API void cando_open_csvlib(CandoVM *vm);       /**< csv.*             */
CANDO_API void cando_open_threadlib(CandoVM *vm);    /**< thread.*          */
CANDO_API void cando_open_oslib(CandoVM *vm);        /**< os.*              */
CANDO_API void cando_open_datetimelib(CandoVM *vm);  /**< datetime.*        */
CANDO_API void cando_open_cryptolib(CandoVM *vm);    /**< crypto.*          */
CANDO_API void cando_open_processlib(CandoVM *vm);   /**< process.*         */
CANDO_API void cando_open_netlib(CandoVM *vm);       /**< net.*             */
CANDO_API void cando_open_evallib(CandoVM *vm);      /**< eval()            */
CANDO_API void cando_open_includelib(CandoVM *vm);   /**< include()         */
CANDO_API void cando_open_httplib(CandoVM *vm);      /**< http.* + fetch()  */
CANDO_API void cando_open_httpslib(CandoVM *vm);     /**< https.*           */

/* =========================================================================
 * Load and execute
 * ====================================================================== */

/**
 * cando_dofile — compile and execute a .cdo source file.
 *
 * Reads the file at `path`, compiles it, and runs it in `vm`.  The script's
 * directory is used as the base for relative include() calls.
 *
 * Returns CANDO_OK (0) on success, or one of:
 *   CANDO_ERR_FILE     — cannot open or read the file
 *   CANDO_ERR_PARSE    — syntax / compile error
 *   CANDO_ERR_RUNTIME  — unhandled runtime error
 *
 * On error call cando_errmsg(vm) to retrieve the human-readable description.
 */
CANDO_API int cando_dofile(CandoVM *vm, const char *path);

/**
 * cando_dostring — compile and execute a CanDo source string.
 *
 * `src`  is the NUL-terminated source text.
 * `name` is used in error messages as the "filename" (may be NULL → "<string>").
 *
 * Returns CANDO_OK, CANDO_ERR_PARSE, or CANDO_ERR_RUNTIME.
 */
CANDO_API int cando_dostring(CandoVM *vm, const char *src, const char *name);

/**
 * cando_loadstring — compile a source string without executing it.
 *
 * On success stores the compiled CandoChunk* in *chunk_out and returns
 * CANDO_OK.  The caller owns the chunk and must free it with
 * cando_chunk_free() when done.
 *
 * On failure returns CANDO_ERR_PARSE; *chunk_out is set to NULL.
 *
 * Use cando_vm_exec(vm, chunk) to run the compiled chunk.
 */
CANDO_API int cando_loadstring(CandoVM *vm, const char *src, const char *name,
                                CandoChunk **chunk_out);

/* =========================================================================
 * Error inspection
 * ====================================================================== */

/**
 * cando_errmsg — return the most recent error message from `vm`.
 *
 * Valid after any cando_dofile/cando_dostring/cando_loadstring call that returned non-zero.
 * The string is owned by the VM; it is valid until the next call that
 * modifies vm->error_msg.  Never returns NULL (returns "" on no error).
 */
CANDO_API const char *cando_errmsg(const CandoVM *vm);

/* =========================================================================
 * Version query
 * ====================================================================== */

/** cando_version — return the version string, e.g. "1.0.0". */
CANDO_API const char *cando_version(void);

/** cando_version_num — return CANDO_VERSION_NUM as a runtime value. */
CANDO_API int cando_version_num(void);

#ifdef __cplusplus
}
#endif

#endif /* CANDO_H */
