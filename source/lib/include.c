/*
 * lib/include.c -- Native include(path) function for Cando.
 *
 * include(path)
 *
 *   Loads a script (.cdo) or binary extension (.so/.dylib/.dll) module by
 *   path.  The first load executes the module and caches the result; every
 *   subsequent call with the same canonical path returns the cached value
 *   without re-executing (Node.js require() semantics).
 *
 * Path resolution
 *   - Absolute paths  ("/…")        → used as-is then canonicalised.
 *   - Relative paths  ("./…", "…") → resolved relative to the calling
 *     script's directory by walking the call frame stack and finding the
 *     most recent frame whose chunk name is an absolute path.  Falls back
 *     to the process working directory if no such frame is found.
 *
 * Binary modules
 *   dlopen() loads the shared library.  The symbol
 *   cando_module_init(CandoVM *) → CandoValue must be exported; it is
 *   called once to register natives and return the module's export value.
 *
 * Must compile with gcc -std=c11.
 */

#include "include.h"
#include "libutil.h"
#include "../parser/parser.h"
#include "../vm/chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  include <windows.h>
#  include <direct.h>   /* _getcwd */
/* Provide realpath via _fullpath */
static char *realpath(const char *path, char *out) {
    return _fullpath(out, path, PATH_MAX);
}
static char *getcwd_compat(char *buf, size_t sz) {
    return _getcwd(buf, (int)sz);
}
#  define getcwd getcwd_compat
#else
#  include <unistd.h>
#  include <dlfcn.h>
#endif

/* =========================================================================
 * Module init function type (binary modules must export this symbol)
 * ======================================================================= */
typedef CandoValue (*CandoModuleInitFn)(CandoVM *vm);

/* =========================================================================
 * Path helpers
 * ======================================================================= */

/*
 * resolve_path -- turn raw_path into a canonical absolute path stored in
 * out (at least PATH_MAX bytes).  The caller's directory is used as the
 * base when the path is relative.
 *
 * Returns true on success, false if realpath() or getcwd() fails.
 */
static bool resolve_path(CandoVM *vm, const char *raw_path,
                         char out[PATH_MAX])
{
    char joined[PATH_MAX];

#if defined(_WIN32) || defined(_WIN64)
    bool is_abs = (raw_path[0] == '/' || raw_path[0] == '\\'
                   || (raw_path[0] && raw_path[1] == ':'));
#else
    bool is_abs = (raw_path[0] == '/');
#endif
    if (is_abs) {
        /* Already absolute. */
        if (strlen(raw_path) >= PATH_MAX) return false;
        if (!realpath(raw_path, out)) return false;
        return true;
    }

    /* Relative path: find the calling script's directory by walking frames. */
    const char *caller_file = NULL;
    for (int i = (int)vm->frame_count - 1; i >= 0; i--) {
        const char *name = vm->frames[i].closure->chunk->name;
        if (!name) continue;
#if defined(_WIN32) || defined(_WIN64)
        bool frame_abs = (name[0] == '/' || name[0] == '\\'
                          || (name[0] && name[1] == ':'));
#else
        bool frame_abs = (name[0] == '/');
#endif
        if (frame_abs) { caller_file = name; break; }
    }

    char base_dir[PATH_MAX];
    if (caller_file) {
        /* dirname: copy up to the last slash. */
        size_t len = strlen(caller_file);
        if (len >= PATH_MAX) return false;
        memcpy(base_dir, caller_file, len + 1);
        char *slash = strrchr(base_dir, '/');
        if (slash && slash != base_dir) *slash = '\0';
        else { base_dir[0] = '.'; base_dir[1] = '\0'; }
    } else {
        /* No script frame with an absolute path — fall back to cwd. */
        if (!getcwd(base_dir, PATH_MAX)) return false;
    }

    int n = snprintf(joined, PATH_MAX, "%s/%s", base_dir, raw_path);
    if (n < 0 || n >= PATH_MAX) return false;
    if (!realpath(joined, out)) return false;
    return true;
}

/* =========================================================================
 * Module cache helpers
 * ======================================================================= */

/* Look up canonical_path in the cache; returns the entry or NULL. */
static CandoModuleEntry *cache_find(CandoVM *vm, const char *canonical_path)
{
    for (u32 i = 0; i < vm->module_cache_count; i++) {
        if (strcmp(vm->module_cache[i].path, canonical_path) == 0)
            return &vm->module_cache[i];
    }
    return NULL;
}

/* Append a new entry to the cache; returns a pointer to the slot. */
static CandoModuleEntry *cache_insert(CandoVM *vm, const char *canonical_path,
                                      CandoValue value, void *dl_handle,
                                      CandoClosure *closure, CandoChunk *chunk)
{
    if (vm->module_cache_count >= vm->module_cache_cap) {
        u32 new_cap = vm->module_cache_cap ? vm->module_cache_cap * 2 : 8;
        vm->module_cache = (CandoModuleEntry *)cando_realloc(
            vm->module_cache, new_cap * sizeof(CandoModuleEntry));
        vm->module_cache_cap = new_cap;
    }

    CandoModuleEntry *e = &vm->module_cache[vm->module_cache_count++];
    e->path      = strdup(canonical_path); /* owned by the cache */
    e->value     = cando_value_copy(value);
    e->dl_handle = dl_handle;
    e->closure   = closure; /* kept alive so OBJ_FUNCTION handles remain valid */
    e->chunk     = chunk;   /* kept alive as long as closure->chunk references it */
    return e;
}

/* =========================================================================
 * Script module loader
 * ======================================================================= */

static bool load_script(CandoVM *vm, const char *canonical_path,
                        CandoValue *result_out, CandoClosure **closure_out,
                        CandoChunk **chunk_out)
{
    /* Read file. */
    FILE *f = fopen(canonical_path, "r");
    if (!f) {
        cando_vm_error(vm, "include: cannot open '%s'", canonical_path);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        cando_vm_error(vm, "include: cannot seek '%s'", canonical_path);
        return false;
    }
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 0) {
        fclose(f);
        cando_vm_error(vm, "include: cannot determine size of '%s'",
                       canonical_path);
        return false;
    }

    char *source = (char *)cando_alloc((usize)(fsize + 1));
    usize nread  = fread(source, 1, (usize)fsize, f);
    source[nread] = '\0';
    fclose(f);

    /* Compile in eval mode (allows top-level RETURN). */
    CandoChunk *chunk = cando_chunk_new(canonical_path, 0, false);
    CandoParser parser;
    cando_parser_init(&parser, source, nread, chunk);
    parser.eval_mode = true;

    if (!cando_parse(&parser)) {
        cando_free(source);
        cando_vm_error(vm, "include parse error in '%s': %s",
                       canonical_path, cando_parser_error(&parser));
        cando_chunk_free(chunk);
        return false;
    }
    cando_free(source);

    *result_out = cando_null();
    CandoVMResult res = cando_vm_exec_eval_module(vm, chunk, result_out,
                                                  closure_out);
    if (res == VM_RUNTIME_ERR) {
        /* On failure the closure was already freed inside exec_eval_module. */
        cando_chunk_free(chunk);
        return false;
    }
    /* On success: transfer chunk ownership to the caller so it remains alive
     * as long as the module closure (which holds closure->chunk = chunk). */
    if (chunk_out) *chunk_out = chunk;
    else           cando_chunk_free(chunk);
    return true;
}

/* =========================================================================
 * Binary module loader
 * ======================================================================= */

static bool load_binary(CandoVM *vm, const char *canonical_path,
                        CandoValue *result_out, void **dl_handle_out)
{
#if defined(_WIN32) || defined(_WIN64)
    HMODULE handle = LoadLibraryA(canonical_path);
    if (!handle) {
        cando_vm_error(vm, "include: cannot load binary '%s'",
                       canonical_path);
        return false;
    }
    CandoModuleInitFn init_fn =
        (CandoModuleInitFn)(void *)GetProcAddress(handle, "cando_module_init");
    if (!init_fn) {
        FreeLibrary(handle);
        cando_vm_error(vm,
            "include: binary '%s' has no cando_module_init symbol",
            canonical_path);
        return false;
    }
    *result_out    = init_fn(vm);
    *dl_handle_out = (void *)handle;
    return true;
#else
    void *handle = dlopen(canonical_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        cando_vm_error(vm, "include: cannot load binary '%s': %s",
                       canonical_path, dlerror());
        return false;
    }
    /* Cast via uintptr_t to silence -Wpedantic on function-pointer casts. */
    CandoModuleInitFn init_fn =
        (CandoModuleInitFn)(uintptr_t)dlsym(handle, "cando_module_init");
    if (!init_fn) {
        dlclose(handle);
        cando_vm_error(vm,
            "include: binary '%s' has no cando_module_init symbol",
            canonical_path);
        return false;
    }
    *result_out    = init_fn(vm);
    *dl_handle_out = handle;
    return !vm->has_error;
#endif
}

/* =========================================================================
 * native_include
 * ======================================================================= */

static bool path_is_binary(const char *p)
{
    size_t n = strlen(p);
    if (n > 3  && strcmp(p + n - 3,  ".so")    == 0) return true;
    if (n > 6  && strcmp(p + n - 6,  ".dylib") == 0) return true;
    if (n > 4  && strcmp(p + n - 4,  ".dll")   == 0) return true;
    return false;
}

static int native_include(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "include: path must be a string");
        return -1;
    }
    const char *raw_path = args[0].as.string->data;

    /* --- Resolve to canonical absolute path --- */
    char canonical[PATH_MAX];
    if (!resolve_path(vm, raw_path, canonical)) {
        cando_vm_error(vm, "include: cannot resolve path '%s'", raw_path);
        return -1;
    }

    /* --- Check cache --- */
    CandoModuleEntry *cached = cache_find(vm, canonical);
    if (cached) {
        cando_vm_push(vm, cached->value);
        return 1;
    }

    /* --- Load the module --- */
    CandoValue    result         = cando_null();
    void         *dl_handle      = NULL;
    CandoClosure *module_closure = NULL;
    CandoChunk   *module_chunk   = NULL;
    bool          ok;

    if (path_is_binary(canonical))
        ok = load_binary(vm, canonical, &result, &dl_handle);
    else
        ok = load_script(vm, canonical, &result, &module_closure, &module_chunk);

    if (!ok) return -1; /* vm->has_error already set */

    /* --- Cache (closure + chunk kept alive so OBJ_FUNCTION values remain valid) --- */
    cache_insert(vm, canonical, result, dl_handle, module_closure, module_chunk);
    cando_vm_push(vm, result);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_include_register(CandoVM *vm)
{
    cando_vm_register_native(vm, "include", native_include);
}
