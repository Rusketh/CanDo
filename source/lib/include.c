/*
 * lib/include.c -- Native include(path) function for Cando.
 *
 * include(path)
 *
 *   Loads a script (.cdo), binary extension (.so/.dylib/.dll), JSON
 *   document (.json), CSV document (.csv) or YAML document (.yaml/.yml)
 *   by path.  The first load executes/parses the module and caches the
 *   result; every subsequent call with the same canonical path returns
 *   the cached value without re-running the loader (Node.js require()
 *   semantics).
 *
 * Path resolution
 *   - Absolute paths  ("/…")        → used as-is then canonicalised.
 *   - Relative paths  ("./…", "…") → resolved relative to the calling
 *     script's directory by walking the call frame stack and finding the
 *     most recent frame whose chunk name is an absolute path.  Falls back
 *     to the process working directory if no such frame is found.
 *
 * Extension handling
 *   - If the path ends in a recognised extension (.cdo, .so, .dylib,
 *     .dll, .json, .csv, .yaml, .yml) the file is loaded with the
 *     matching loader and a missing file is an error.
 *   - If the path has no extension at all, include() probes the
 *     filesystem in order and uses the first existing match:
 *         <path>.so  →  <path>.dylib  →  <path>.dll  →  <path>.cdo
 *
 * Binary modules
 *   dlopen() loads the shared library.  The symbol
 *   cando_module_init(CandoVM *) → CandoValue must be exported; it is
 *   called once to register natives and return the module's export value.
 *
 * Data modules
 *   .json files are parsed with cando_lib_json_parse_buffer() and the
 *   resulting Cando value is returned.  .csv files are parsed with
 *   cando_lib_csv_parse_buffer() (default comma delimiter, no header
 *   row) and the resulting array of arrays is returned.  .yaml / .yml
 *   files are parsed with cando_lib_yaml_parse_buffer() and the
 *   resulting Cando value is returned.
 *
 * Must compile with gcc -std=c11.
 */

#include "include.h"
#include "libutil.h"
#include "json.h"
#include "csv.h"
#include "yaml.h"
#include "../parser/parser.h"
#include "../vm/chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

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

/* Forward declarations for path/extension helpers defined further down. */
static bool has_any_extension(const char *path);
static bool file_exists(const char *path);

/* =========================================================================
 * Path helpers
 * ======================================================================= */

/*
 * build_absolute -- combine `raw_path` with the calling script's directory
 * (if relative) into an absolute path stored in `out` (PATH_MAX bytes).
 * Does NOT call realpath(); the result may reference a non-existent file
 * so the caller can probe extension candidates.
 */
static bool build_absolute(CandoVM *vm, const char *raw_path,
                           char out[PATH_MAX])
{
#if defined(_WIN32) || defined(_WIN64)
    bool is_abs = (raw_path[0] == '/' || raw_path[0] == '\\'
                   || (raw_path[0] && raw_path[1] == ':'));
#else
    bool is_abs = (raw_path[0] == '/');
#endif
    if (is_abs) {
        size_t n = strlen(raw_path);
        if (n >= PATH_MAX) return false;
        memcpy(out, raw_path, n + 1);
        return true;
    }

    /* Relative path: find the calling script's directory. */
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
        size_t len = strlen(caller_file);
        if (len >= PATH_MAX) return false;
        memcpy(base_dir, caller_file, len + 1);
        char *slash = strrchr(base_dir, '/');
#if defined(_WIN32) || defined(_WIN64)
        char *bslash = strrchr(base_dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash && slash != base_dir) *slash = '\0';
        else { base_dir[0] = '.'; base_dir[1] = '\0'; }
    } else {
        if (!getcwd(base_dir, PATH_MAX)) return false;
    }

    int n = snprintf(out, PATH_MAX, "%s/%s", base_dir, raw_path);
    if (n < 0 || n >= PATH_MAX) return false;
    return true;
}

/*
 * resolve_path -- turn raw_path into a canonical absolute path stored in
 * out (at least PATH_MAX bytes).  When raw_path has no file extension at
 * all, candidate extensions are probed in order (.so, .dylib, .dll, .cdo)
 * and the first existing candidate is used.
 *
 * Returns true on success, false if no candidate exists or canonicalisation
 * fails.  Does NOT set a VM error -- the caller does that.
 */
static bool resolve_path(CandoVM *vm, const char *raw_path,
                         char out[PATH_MAX])
{
    char joined[PATH_MAX];
    if (!build_absolute(vm, raw_path, joined)) return false;

    if (has_any_extension(raw_path)) {
        /* Extension supplied -- use the path as given. */
        if (!realpath(joined, out)) return false;
        return true;
    }

    /* No extension: probe binary candidates first, then .cdo. */
    static const char *const candidates[] = {
        ".so", ".dylib", ".dll", ".cdo", NULL
    };
    char probe[PATH_MAX];
    for (int i = 0; candidates[i]; i++) {
        int n = snprintf(probe, PATH_MAX, "%s%s", joined, candidates[i]);
        if (n < 0 || n >= PATH_MAX) continue;
        if (!file_exists(probe)) continue;
        if (!realpath(probe, out)) return false;
        return true;
    }
    return false;
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
                                      CandoValue *values, u32 value_count,
                                      void *dl_handle,
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
    e->value_count = value_count;
    if (value_count > 0) {
        e->values = (CandoValue *)cando_alloc(value_count * sizeof(CandoValue));
        for (u32 i = 0; i < value_count; i++) {
            e->values[i] = cando_value_copy(values[i]);
        }
    } else {
        e->values = NULL;
    }
    e->dl_handle = dl_handle;
    e->closure   = closure; /* kept alive so OBJ_FUNCTION handles remain valid */
    e->chunk     = chunk;   /* kept alive as long as closure->chunk references it */
    return e;
}

/* =========================================================================
 * Script module loader
 * ======================================================================= */

static bool load_script(CandoVM *vm, const char *canonical_path,
                        CandoValue **results_out, u32 *result_count_out,
                        CandoClosure **closure_out, CandoChunk **chunk_out)
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

    CandoVMResult res = cando_vm_exec_eval_module(vm, chunk, results_out,
                                                  result_count_out,
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

/* Recognised module kinds. */
typedef enum {
    INC_KIND_NONE = 0,  /* no recognised extension */
    INC_KIND_CDO,
    INC_KIND_BINARY,
    INC_KIND_JSON,
    INC_KIND_CSV,
    INC_KIND_YAML,
} IncludeKind;

/* Case-sensitive suffix match. */
static bool ends_with(const char *s, size_t n, const char *suf)
{
    size_t sn = strlen(suf);
    return (n > sn) && (memcmp(s + n - sn, suf, sn) == 0);
}

static IncludeKind path_kind(const char *p)
{
    size_t n = strlen(p);
    if (ends_with(p, n, ".so"))    return INC_KIND_BINARY;
    if (ends_with(p, n, ".dylib")) return INC_KIND_BINARY;
    if (ends_with(p, n, ".dll"))   return INC_KIND_BINARY;
    if (ends_with(p, n, ".cdo"))   return INC_KIND_CDO;
    if (ends_with(p, n, ".json"))  return INC_KIND_JSON;
    if (ends_with(p, n, ".csv"))   return INC_KIND_CSV;
    if (ends_with(p, n, ".yaml"))  return INC_KIND_YAML;
    if (ends_with(p, n, ".yml"))   return INC_KIND_YAML;
    return INC_KIND_NONE;
}

/* Return true if the basename of `path` contains a '.' (i.e. has any
 * extension at all -- recognised or not).  Used to decide whether to
 * attempt the binary→cdo fallback search. */
static bool has_any_extension(const char *path)
{
    const char *base = path;
    for (const char *q = path; *q; q++) {
        if (*q == '/' || *q == '\\') base = q + 1;
    }
    return strchr(base, '.') != NULL;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read the entire file at `path` into a freshly-allocated, NUL-terminated
 * buffer.  *out_len is set to the byte length (not counting the NUL).
 * Returns true on success.  On failure sets a VM error and returns false. */
static bool read_whole_file(CandoVM *vm, const char *path,
                            char **out_data, usize *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        cando_vm_error(vm, "include: cannot open '%s'", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        cando_vm_error(vm, "include: cannot seek '%s'", path);
        return false;
    }
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 0) {
        fclose(f);
        cando_vm_error(vm, "include: cannot determine size of '%s'", path);
        return false;
    }
    char *buf = (char *)cando_alloc((usize)(fsize + 1));
    usize nread = fread(buf, 1, (usize)fsize, f);
    buf[nread] = '\0';
    fclose(f);
    *out_data = buf;
    *out_len  = nread;
    return true;
}

static bool load_json(CandoVM *vm, const char *canonical_path,
                      CandoValue *result_out)
{
    char  *src = NULL;
    usize  len = 0;
    if (!read_whole_file(vm, canonical_path, &src, &len)) return false;
    bool ok = cando_lib_json_parse_buffer(vm, src, len, "include", result_out);
    cando_free(src);
    return ok;
}

static bool load_csv(CandoVM *vm, const char *canonical_path,
                     CandoValue *result_out)
{
    char  *src = NULL;
    usize  len = 0;
    if (!read_whole_file(vm, canonical_path, &src, &len)) return false;
    bool ok = cando_lib_csv_parse_buffer(vm, src, len, ',', false,
                                         "include", result_out);
    cando_free(src);
    return ok;
}

static bool load_yaml(CandoVM *vm, const char *canonical_path,
                      CandoValue *result_out)
{
    char  *src = NULL;
    usize  len = 0;
    if (!read_whole_file(vm, canonical_path, &src, &len)) return false;
    bool ok = cando_lib_yaml_parse_buffer(vm, src, len, "include", result_out);
    cando_free(src);
    return ok;
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
        for (u32 i = 0; i < cached->value_count; i++) {
            cando_vm_push(vm, cando_value_copy(cached->values[i]));
        }
        vm->last_ret_count = (int)cached->value_count;
        return (int)cached->value_count;
    }

    /* --- Load the module --- */
    CandoValue    *results        = NULL;
    u32            result_count   = 0;
    void          *dl_handle      = NULL;
    CandoClosure  *module_closure = NULL;
    CandoChunk    *module_chunk   = NULL;
    bool           ok             = false;

    IncludeKind kind = path_kind(canonical);

    switch (kind) {
        case INC_KIND_BINARY: {
            CandoValue binary_res = cando_null();
            ok = load_binary(vm, canonical, &binary_res, &dl_handle);
            if (ok) {
                results = (CandoValue *)cando_alloc(sizeof(CandoValue));
                results[0]   = binary_res;
                result_count = 1;
            }
            break;
        }
        case INC_KIND_JSON: {
            CandoValue json_res = cando_null();
            ok = load_json(vm, canonical, &json_res);
            if (ok) {
                results = (CandoValue *)cando_alloc(sizeof(CandoValue));
                results[0]   = json_res;
                result_count = 1;
            }
            break;
        }
        case INC_KIND_CSV: {
            CandoValue csv_res = cando_null();
            ok = load_csv(vm, canonical, &csv_res);
            if (ok) {
                results = (CandoValue *)cando_alloc(sizeof(CandoValue));
                results[0]   = csv_res;
                result_count = 1;
            }
            break;
        }
        case INC_KIND_YAML: {
            CandoValue yaml_res = cando_null();
            ok = load_yaml(vm, canonical, &yaml_res);
            if (ok) {
                results = (CandoValue *)cando_alloc(sizeof(CandoValue));
                results[0]   = yaml_res;
                result_count = 1;
            }
            break;
        }
        case INC_KIND_CDO:
        case INC_KIND_NONE: /* fall-through: treat as script */
        default:
            ok = load_script(vm, canonical, &results, &result_count,
                             &module_closure, &module_chunk);
            break;
    }

    if (!ok) return -1; /* vm->has_error already set */

    /* --- Cache (closure + chunk kept alive so OBJ_FUNCTION values remain valid) --- */
    cache_insert(vm, canonical, results, result_count, dl_handle, module_closure, module_chunk);

    for (u32 i = 0; i < result_count; i++) {
        cando_vm_push(vm, cando_value_copy(results[i]));
    }
    vm->last_ret_count = (int)result_count;

    /* Cleanup temporary results array. */
    if (results) {
        for (u32 i = 0; i < result_count; i++) cando_value_release(results[i]);
        cando_free(results);
    }

    return (int)result_count;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_include_register(CandoVM *vm)
{
    cando_vm_register_native(vm, "include", native_include);
}
