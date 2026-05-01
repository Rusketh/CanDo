/*
 * lib/include.c -- Native include(path) function for Cando.
 *
 * include(path)
 *
 *   Loads a script (.cdo), binary extension (.so/.dylib/.dll), JSON
 *   document (.json), CSV document (.csv) or YAML document (.yaml/.yml)
 *   .csv files are parsed with the first row treated as the header row, so
 *   the result is an array of objects keyed by the header names.
 *   by path.  The first load executes/parses the module and caches the
 *   result; every subsequent call with the same canonical path returns
 *   the cached value without re-running the loader (Node.js require()
 *   semantics).
 *
 * Path resolution
 *   - Absolute paths  ("/…")        → used as-is then canonicalised.
 *   - Relative paths  ("./…", "…") → resolved by starting in the calling
 *     script's directory and walking up parent directories until the file
 *     is found, or until the process working directory (cwd) has been
 *     searched.  cwd is the upper bound — directories above it are never
 *     searched.  If no calling script is on the stack, the search starts
 *     from cwd.  The caller's directory must lie within the cwd tree;
 *     otherwise resolution fails.  At each directory level, all candidate
 *     extensions are probed before moving to the parent.
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
 *   cando_lib_csv_parse_buffer() (default comma delimiter, header row
 *   on) and the resulting array of objects keyed by header names is
 *   returned.  .yaml / .yml files are parsed with
 *   cando_lib_yaml_parse_buffer() and the resulting Cando value is
 *   returned.
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

static bool path_is_absolute(const char *p)
{
#if defined(_WIN32) || defined(_WIN64)
    return (p[0] == '/' || p[0] == '\\'
            || (p[0] && p[1] == ':'));
#else
    return (p[0] == '/');
#endif
}

static bool is_sep(char c)
{
#if defined(_WIN32) || defined(_WIN64)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* True if `path` is exactly `prefix` or sits underneath it.  Both arguments
 * are expected to be canonical (no trailing separator except when prefix is
 * the filesystem root). */
static bool is_within_dir(const char *path, const char *prefix)
{
    size_t pn = strlen(prefix);
    if (pn == 0) return false;
    if (strncmp(path, prefix, pn) != 0) return false;
    if (path[pn] == '\0') return true;
    /* Root prefix ("/" on POSIX, "X:\\" or "/" on Windows): every absolute
     * path beneath it qualifies. */
    if (is_sep(prefix[pn - 1])) return true;
    return is_sep(path[pn]);
}

/* Replace `dir` with its parent directory in place.  Returns false when
 * `dir` already names the filesystem root (no parent exists). */
static bool dir_to_parent(char *dir)
{
    size_t len = strlen(dir);
    if (len == 0) return false;

    /* Strip trailing separators except a leading root one. */
    while (len > 1 && is_sep(dir[len - 1])) dir[--len] = '\0';

    if (len == 1 && is_sep(dir[0])) return false; /* "/" */

    char *slash = strrchr(dir, '/');
#if defined(_WIN32) || defined(_WIN64)
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (!slash) return false;
    if (slash == dir) {
        dir[1] = '\0'; /* parent is root */
    } else {
        *slash = '\0';
    }
    return true;
}

/* Try to locate `raw_path` inside `dir`.  When `has_ext` is false, candidate
 * extensions (.so, .dylib, .dll, .cdo) are probed in order.  On success
 * writes the canonical path to `out` and returns true. */
static bool try_resolve_in(const char *dir, const char *raw_path,
                           bool has_ext, char out[PATH_MAX])
{
    char joined[PATH_MAX];
    int n = snprintf(joined, PATH_MAX, "%s/%s", dir, raw_path);
    if (n < 0 || n >= PATH_MAX) return false;

    if (has_ext) {
        if (!file_exists(joined)) return false;
        return realpath(joined, out) != NULL;
    }

    static const char *const candidates[] = {
        ".so", ".dylib", ".dll", ".cdo", NULL
    };
    char probe[PATH_MAX];
    for (int i = 0; candidates[i]; i++) {
        int m = snprintf(probe, PATH_MAX, "%s%s", joined, candidates[i]);
        if (m < 0 || m >= PATH_MAX) continue;
        if (!file_exists(probe)) continue;
        return realpath(probe, out) != NULL;
    }
    return false;
}

/*
 * resolve_path -- turn raw_path into a canonical absolute path stored in
 * out (at least PATH_MAX bytes).  When raw_path has no file extension at
 * all, candidate extensions are probed in order (.so, .dylib, .dll, .cdo)
 * and the first existing candidate is used.
 *
 * Relative paths are resolved by walking up from the calling script's
 * directory toward the process working directory (inclusive), trying each
 * level in turn.  cwd is a hard ceiling -- directories above it are never
 * searched, and the caller's directory must lie within it.
 *
 * Returns true on success, false if no candidate exists, the caller is
 * outside cwd, or canonicalisation fails.  Does NOT set a VM error -- the
 * caller does that.
 */
static bool resolve_path(CandoVM *vm, const char *raw_path,
                         char out[PATH_MAX])
{
    bool has_ext = has_any_extension(raw_path);

    /* --- Absolute path: use as-is, with extension probing if needed --- */
    if (path_is_absolute(raw_path)) {
        if (has_ext) {
            return realpath(raw_path, out) != NULL;
        }
        static const char *const candidates[] = {
            ".so", ".dylib", ".dll", ".cdo", NULL
        };
        char probe[PATH_MAX];
        for (int i = 0; candidates[i]; i++) {
            int n = snprintf(probe, PATH_MAX, "%s%s", raw_path, candidates[i]);
            if (n < 0 || n >= PATH_MAX) continue;
            if (!file_exists(probe)) continue;
            return realpath(probe, out) != NULL;
        }
        return false;
    }

    /* --- Establish cwd as the upper bound --- */
    char cwd_buf[PATH_MAX];
    if (!getcwd(cwd_buf, PATH_MAX)) return false;
    char cwd_real[PATH_MAX];
    if (!realpath(cwd_buf, cwd_real)) return false;

    /* --- Determine starting directory (caller's dir, or cwd) --- */
    const char *caller_file = NULL;
    for (int i = (int)vm->frame_count - 1; i >= 0; i--) {
        const char *name = vm->frames[i].closure->chunk->name;
        if (!name) continue;
        if (path_is_absolute(name)) { caller_file = name; break; }
    }

    char start_dir[PATH_MAX];
    if (caller_file) {
        size_t len = strlen(caller_file);
        if (len >= PATH_MAX) return false;
        memcpy(start_dir, caller_file, len + 1);
        char *slash = strrchr(start_dir, '/');
#if defined(_WIN32) || defined(_WIN64)
        char *bslash = strrchr(start_dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash && slash != start_dir) *slash = '\0';
        else { start_dir[0] = '/'; start_dir[1] = '\0'; }
    } else {
        size_t len = strlen(cwd_real);
        memcpy(start_dir, cwd_real, len + 1);
    }

    char current[PATH_MAX];
    if (!realpath(start_dir, current)) return false;

    /* Caller must live within the cwd tree -- include() never escapes it. */
    if (!is_within_dir(current, cwd_real)) return false;

    /* --- Walk upward: caller's dir, then each parent, stopping at cwd --- */
    for (;;) {
        if (try_resolve_in(current, raw_path, has_ext, out)) return true;
        if (strcmp(current, cwd_real) == 0) return false;
        if (!dir_to_parent(current)) return false;
        if (!is_within_dir(current, cwd_real)) return false;
    }
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
        cando_parser_free(&parser);
        cando_chunk_free(chunk);
        return false;
    }
    cando_parser_free(&parser);
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
    bool ok = cando_lib_csv_parse_buffer(vm, src, len, ',', true,
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
    const char *raw_path = libutil_require_cstr_at(vm, args, argc, 0, "include");
    if (!raw_path) return -1;

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
