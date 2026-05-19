/*
 * lib/file.c -- File I/O standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "file.h"
#include "libutil.h"
#include "stream.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP        '\\'
#  define PATH_SEP_STR    "\\"
#  define PATH_ALT_SEP    '/'
#else
#  include <unistd.h>
#  define PATH_SEP        '/'
#  define PATH_SEP_STR    "/"
#  define PATH_ALT_SEP    '/'   /* same -- no second separator on POSIX */
#endif

#if defined(_WIN32) || defined(_WIN64)
#  include <io.h>        /* _access */
#  include <direct.h>   /* _mkdir */
#  include <dirent.h>   /* MinGW-w64 provides this */
#  define access(p,m)  _access((p),(m))
#  define F_OK 0
/* getline is not available on Windows; provide a minimal replacement */
static ssize_t win_getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream) return -1;
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) return -1;
    }
    int c;
    size_t len = 0;
    while ((c = fgetc(stream)) != EOF) {
        if (len + 2 > *n) {
            size_t new_n = *n * 2;
            char *tmp = (char *)realloc(*lineptr, new_n);
            if (!tmp) return -1;
            *lineptr = tmp;
            *n = new_n;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0 && c == EOF) return -1;
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}
#  define getline win_getline
/* ssize_t is already provided by MinGW via corecrt.h */
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/* Get the mode string for fopen based on an optional encoding argument.
 * Returns "rb"/"wb"/"ab" for binary; "r"/"w"/"a" for text (utf8). */
static const char *fopen_mode(CandoValue enc_val, char rw)
{
    bool binary = false;
    const char *enc = libutil_arg_cstr(enc_val);
    if (enc && strcmp(enc, "binary") == 0) binary = true;
    if (rw == 'r') return binary ? "rb" : "r";
    if (rw == 'a') return binary ? "ab" : "a";
    return binary ? "wb" : "w";
}

/* =========================================================================
 * file.read(path, encoding?) → string | null
 * ======================================================================= */
static int file_read(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.read");
    if (!path) return -1;
    const char *mode = (argc >= 2) ? fopen_mode(args[1], 'r') : "r";

    FILE *f = fopen(path, mode);
    if (!f) { cando_vm_push(vm, cando_null()); return 1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); cando_vm_push(vm, cando_null()); return 1; }

    char *buf = (char *)malloc((usize)sz + 1);
    if (!buf)  { fclose(f); cando_vm_push(vm, cando_null()); return 1; }

    usize nread = fread(buf, 1, (usize)sz, f);
    fclose(f);
    buf[nread] = '\0';
    libutil_push_str(vm, buf, (u32)nread);
    free(buf);
    return 1;
}

/* =========================================================================
 * file.write(path, data, encoding?) → bool
 * ======================================================================= */
static int file_write(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.write");
    if (!path) return -1;
    CandoString *data_s = libutil_require_str_at(vm, args, argc, 1, "file.write");
    if (!data_s) return -1;
    const char *data = data_s->data;
    u32 len          = data_s->length;
    const char *mode = (argc >= 3) ? fopen_mode(args[2], 'w') : "w";

    FILE *f = fopen(path, mode);
    if (!f) { cando_vm_push(vm, cando_bool(false)); return 1; }

    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * file.append(path, data, encoding?) → bool
 * ======================================================================= */
static int file_append(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.append");
    if (!path) return -1;
    CandoString *data_s = libutil_require_str_at(vm, args, argc, 1, "file.append");
    if (!data_s) return -1;
    const char *data = data_s->data;
    u32 len          = data_s->length;
    const char *mode = (argc >= 3) ? fopen_mode(args[2], 'a') : "a";

    FILE *f = fopen(path, mode);
    if (!f) { cando_vm_push(vm, cando_bool(false)); return 1; }

    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * file.exists(path) → bool
 * ======================================================================= */
static int file_exists(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.exists");
    if (!path) return -1;
    bool exists = (access(path, F_OK) == 0);
    cando_vm_push(vm, cando_bool(exists));
    return 1;
}

/* =========================================================================
 * file.delete(path) → bool
 * ======================================================================= */
static int file_delete(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.delete");
    if (!path) return -1;
    cando_vm_push(vm, cando_bool(remove(path) == 0));
    return 1;
}

/* =========================================================================
 * file.copy(src, dst) → bool
 * ======================================================================= */
static int file_copy(CandoVM *vm, int argc, CandoValue *args)
{
    const char *src_path = libutil_require_cstr_at(vm, args, argc, 0, "file.copy");
    if (!src_path) return -1;
    const char *dst_path = libutil_require_cstr_at(vm, args, argc, 1, "file.copy");
    if (!dst_path) return -1;
    FILE *src = fopen(src_path, "rb");
    if (!src) { cando_vm_push(vm, cando_bool(false)); return 1; }

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); cando_vm_push(vm, cando_bool(false)); return 1; }

    char buf[4096];
    bool ok = true;
    usize n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    if (ferror(src)) ok = false;
    fclose(src);
    fclose(dst);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * file.move(src, dst) → bool
 * ======================================================================= */
static int file_move(CandoVM *vm, int argc, CandoValue *args)
{
    const char *src_path = libutil_require_cstr_at(vm, args, argc, 0, "file.move");
    if (!src_path) return -1;
    const char *dst_path = libutil_require_cstr_at(vm, args, argc, 1, "file.move");
    if (!dst_path) return -1;
    cando_vm_push(vm, cando_bool(rename(src_path, dst_path) == 0));
    return 1;
}

/* =========================================================================
 * file.size(path) → number | null
 * ======================================================================= */
static int file_size(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.size");
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cando_vm_push(vm, cando_number((f64)st.st_size));
    return 1;
}

/* =========================================================================
 * file.lines(path, encoding?) → array of strings | null
 * ======================================================================= */
static int file_lines(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.lines");
    if (!path) return -1;
    const char *mode = (argc >= 2) ? fopen_mode(args[1], 'r') : "r";

    FILE *f = fopen(path, mode);
    if (!f) { cando_vm_push(vm, cando_null()); return 1; }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr    = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    char   *line = NULL;
    usize   cap  = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        /* Strip trailing newline. */
        if (n > 0 && line[n - 1] == '\n') { line[--n] = '\0'; }
        if (n > 0 && line[n - 1] == '\r') { line[--n] = '\0'; }
        CdoString *s = cdo_string_new(line, (u32)n);
        CdoValue   sv = cdo_string_value(s);
        cdo_array_push(arr, sv);   /* push retains via cdo_value_copy */
        cdo_value_release(sv);     /* release our ref */
    }
    free(line);
    fclose(f);

    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * file.mkdir(path) → bool
 * ======================================================================= */
static int file_mkdir(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.mkdir");
    if (!path) return -1;
#if defined(_WIN32) || defined(_WIN64)
    bool ok = (_mkdir(path) == 0 || errno == EEXIST);
#else
    bool ok = (mkdir(path, 0755) == 0 || errno == EEXIST);
#endif
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * file.list(path) → array of strings | null
 * ======================================================================= */
static int file_list(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_require_cstr_at(vm, args, argc, 0, "file.list");
    if (!path) return -1;
    DIR *d = opendir(path);
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr    = cando_bridge_resolve(vm, cando_as_handle(arr_val));

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        u32 len        = (u32)strlen(ent->d_name);
        CdoString *s   = cdo_string_new(ent->d_name, len);
        CdoValue   sv  = cdo_string_value(s);
        cdo_array_push(arr, sv);
        cdo_value_release(sv);
    }
    closedir(d);

    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * file.open(path, mode) -- streaming adapter
 *
 * mode strings (chosen for least surprise vs. C's fopen):
 *   "r"  / "rb"   -- read-only
 *   "w"  / "wb"   -- truncate and write
 *   "a"  / "ab"   -- append (write-only, position at end)
 *   "r+" / "rb+"  -- read+write, file must exist
 *   "w+" / "wb+"  -- read+write, truncate
 *
 * Returns a stream backed by a `FILE *`.  The vtable wires read/write/seek/
 * tell/flush/close into stdio so the same script API works as for any
 * other adapter.
 * ======================================================================= */

typedef struct FileCtx {
    FILE       *fp;
    StreamSlot *owner;
} FileCtx;

static StreamCaps caps_from_mode(const char *m)
{
    /* Caps follow the standard fopen interpretation. */
    if (!m) return STREAM_CAP_READABLE;
    bool plus = strchr(m, '+') != NULL;
    if (plus) return STREAM_CAP_DUPLEX | STREAM_CAP_SEEKABLE;
    if (m[0] == 'r') return STREAM_CAP_READABLE | STREAM_CAP_SEEKABLE;
    return STREAM_CAP_WRITABLE | STREAM_CAP_SEEKABLE;
}

static StreamStatus file_stream_read(void *vctx, u8 *out, usize cap, usize *n_out)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc->fp) { *n_out = 0; return STREAM_EOF; }
    usize n = fread(out, 1, cap, fc->fp);
    *n_out  = n;
    if (n == 0) {
        if (feof(fc->fp))    return STREAM_EOF;
        if (ferror(fc->fp)) {
            stream_set_error(fc->owner, "file read error");
            return STREAM_ERR;
        }
    }
    return STREAM_OK;
}

static StreamStatus file_stream_write(void *vctx, const u8 *buf, usize len,
                                      usize *n_out)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc->fp) {
        *n_out = 0;
        stream_set_error(fc->owner, "file is closed");
        return STREAM_ERR;
    }
    usize n = fwrite(buf, 1, len, fc->fp);
    *n_out  = n;
    if (n < len) {
        stream_set_error(fc->owner, "file write error");
        return STREAM_ERR;
    }
    return STREAM_OK;
}

static StreamStatus file_stream_flush(void *vctx)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc->fp) return STREAM_OK;
    if (fflush(fc->fp) != 0) {
        stream_set_error(fc->owner, "file flush error");
        return STREAM_ERR;
    }
    return STREAM_OK;
}

static StreamStatus file_stream_seek(void *vctx, i64 off, int whence)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc->fp) return STREAM_ERR;
    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (fseek(fc->fp, (long)off, w) != 0) {
        stream_set_error(fc->owner, "file seek error");
        return STREAM_ERR;
    }
    return STREAM_OK;
}

static i64 file_stream_tell(void *vctx)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc->fp) return -1;
    long p = ftell(fc->fp);
    return (i64)p;
}

static void file_stream_destroy(void *vctx)
{
    FileCtx *fc = (FileCtx *)vctx;
    if (!fc) return;
    if (fc->fp) {
        fclose(fc->fp);
        fc->fp = NULL;
    }
    cando_free(fc);
}

static const StreamVTable g_file_stream_vt = {
    .read       = file_stream_read,
    .write      = file_stream_write,
    .flush      = file_stream_flush,
    .end        = NULL,
    .destroy    = file_stream_destroy,
    .seek       = file_stream_seek,
    .tell       = file_stream_tell,
    .kind_name  = "file",
};

static bool valid_mode(const char *m)
{
    if (!m || !*m) return false;
    if (m[0] != 'r' && m[0] != 'w' && m[0] != 'a') return false;
    /* Accept any combination of trailing 'b' and '+' in any order. */
    for (const char *p = m + 1; *p; p++) {
        if (*p != 'b' && *p != '+') return false;
    }
    return true;
}

CandoValue cando_lib_file_stream_from_fp(CandoVM *vm, FILE *fp, unsigned caps)
{
    if (!fp) return cando_null();
    FileCtx *fc = (FileCtx *)cando_alloc(sizeof(FileCtx));
    memset(fc, 0, sizeof(*fc));
    fc->fp = fp;
    int idx = stream_pool_alloc(&g_file_stream_vt, fc, (StreamCaps)caps);
    if (idx < 0) {
        /* Caller-owned fp: close it on failure to avoid a leak. */
        fclose(fp);
        cando_free(fc);
        return cando_null();
    }
    fc->owner = stream_pool_get(idx);
    return stream_create_instance(vm, idx);
}

static int file_open(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    if (!path) {
        cando_vm_error(vm, "file.open: path (string) required");
        return -1;
    }
    const char *mode = libutil_arg_cstr_at(args, argc, 1);
    if (!mode) mode = "r";
    if (!valid_mode(mode)) {
        cando_vm_error(vm, "file.open: invalid mode '%s'", mode);
        return -1;
    }
    FILE *fp = fopen(path, mode);
    if (!fp) {
        cando_vm_error(vm, "file.open: cannot open '%s'", path);
        return -1;
    }

    FileCtx *fc = (FileCtx *)cando_alloc(sizeof(FileCtx));
    memset(fc, 0, sizeof(*fc));
    fc->fp = fp;

    int idx = stream_pool_alloc(&g_file_stream_vt, fc, caps_from_mode(mode));
    if (idx < 0) {
        fclose(fp);
        cando_free(fc);
        cando_vm_error(vm, "file.open: too many active streams");
        return -1;
    }
    fc->owner = stream_pool_get(idx);
    cando_vm_push(vm, stream_create_instance(vm, idx));
    return 1;
}

/* =========================================================================
 * stat / isFile / isDir / isSymlink
 *
 * Each returns a script-visible object or boolean; pulls from stat(2) /
 * lstat(2) on POSIX and the Windows attributes equivalent.
 * ======================================================================= */

/* Internal helper: do a stat-like call and populate `out`.  Returns
 * true on success.  When `follow` is false, calls lstat (POSIX) to
 * avoid resolving symlinks. */
static bool do_stat(const char *path, bool follow, struct stat *out)
{
#if defined(_WIN32) || defined(_WIN64)
    (void)follow;     /* Windows does not distinguish stat/lstat the same way */
    return stat(path, out) == 0;
#else
    if (follow) return stat(path, out) == 0;
    return lstat(path, out) == 0;
#endif
}

static int file_stat(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    if (!path) { cando_vm_push(vm, cando_null()); return 1; }
    struct stat st;
    if (!do_stat(path, true, &st)) { cando_vm_push(vm, cando_null()); return 1; }

    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    CdoString *k;
#define SET_NUM(key, v) do { \
    k = cdo_string_intern(key, (u32)strlen(key)); \
    cdo_object_rawset(obj, k, cdo_number((f64)(v)), FIELD_NONE); \
    cdo_string_release(k); \
} while (0)
#define SET_BOOL(key, v) do { \
    k = cdo_string_intern(key, (u32)strlen(key)); \
    cdo_object_rawset(obj, k, cdo_bool(v), FIELD_NONE); \
    cdo_string_release(k); \
} while (0)
    SET_NUM("size",  (double)st.st_size);
    SET_NUM("mode",  (double)st.st_mode);
    SET_NUM("mtime", (double)st.st_mtime);
    SET_NUM("atime", (double)st.st_atime);
    SET_NUM("ctime", (double)st.st_ctime);
    SET_BOOL("isFile", S_ISREG(st.st_mode));
    SET_BOOL("isDir",  S_ISDIR(st.st_mode));
#if defined(S_ISLNK)
    /* lstat for the symlink-bit field. */
    struct stat lst;
    bool islnk = do_stat(path, false, &lst) && S_ISLNK(lst.st_mode);
    SET_BOOL("isSymlink", islnk);
#else
    SET_BOOL("isSymlink", false);
#endif
#undef SET_NUM
#undef SET_BOOL
    cando_vm_push(vm, obj_val);
    return 1;
}

static int file_isFile(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    struct stat st;
    cando_vm_push(vm, cando_bool(path && do_stat(path, true, &st) && S_ISREG(st.st_mode)));
    return 1;
}

static int file_isDir(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    struct stat st;
    cando_vm_push(vm, cando_bool(path && do_stat(path, true, &st) && S_ISDIR(st.st_mode)));
    return 1;
}

static int file_isSymlink(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
#if defined(S_ISLNK)
    struct stat st;
    cando_vm_push(vm, cando_bool(path && do_stat(path, false, &st) && S_ISLNK(st.st_mode)));
#else
    (void)path;
    cando_vm_push(vm, cando_bool(false));
#endif
    return 1;
}

/* =========================================================================
 * Path helpers -- pure string ops, no filesystem access.
 * ======================================================================= */

static bool is_sep(char c) { return c == PATH_SEP || c == PATH_ALT_SEP; }

static int file_basename(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    CandoString *ext = libutil_arg_str_at(args, argc, 1);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    u32 end = s->length;
    /* Strip any trailing separators (matches POSIX basename). */
    while (end > 0 && is_sep(s->data[end - 1])) end--;
    u32 start = end;
    while (start > 0 && !is_sep(s->data[start - 1])) start--;
    u32 base_len = end - start;
    /* Strip ext if given and present. */
    if (ext && ext->length > 0 && base_len >= ext->length
        && memcmp(s->data + end - ext->length, ext->data, ext->length) == 0) {
        base_len -= ext->length;
    }
    libutil_push_str(vm, s->data + start, base_len);
    return 1;
}

static int file_dirname(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s || s->length == 0) { libutil_push_str(vm, ".", 1); return 1; }
    u32 end = s->length;
    while (end > 0 && is_sep(s->data[end - 1])) end--;
    while (end > 0 && !is_sep(s->data[end - 1])) end--;
    while (end > 1 && is_sep(s->data[end - 1])) end--;
    if (end == 0) {
        /* Either an absolute root ("/") or no separator at all. */
        if (s->length > 0 && is_sep(s->data[0])) {
            libutil_push_str(vm, PATH_SEP_STR, 1);
        } else {
            libutil_push_str(vm, ".", 1);
        }
        return 1;
    }
    libutil_push_str(vm, s->data, end);
    return 1;
}

static int file_extname(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s || s->length == 0) { libutil_push_str(vm, "", 0); return 1; }
    for (i64 i = (i64)s->length - 1; i >= 0; i--) {
        if (is_sep(s->data[i])) break;
        if (s->data[i] == '.') {
            /* Leading dot (".bashrc") is not an extension. */
            if (i == 0 || is_sep(s->data[i - 1])) {
                libutil_push_str(vm, "", 0);
                return 1;
            }
            libutil_push_str(vm, s->data + i, s->length - (u32)i);
            return 1;
        }
    }
    libutil_push_str(vm, "", 0);
    return 1;
}

static int file_join(CandoVM *vm, int argc, CandoValue *args)
{
    /* Concatenate all string parts with PATH_SEP between, collapsing
     * doubled separators. */
    if (argc == 0) { libutil_push_str(vm, "", 0); return 1; }
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (!cando_is_string(args[i])) continue;
        total += cando_as_string(args[i])->length + 1;
    }
    char *out = (char *)cando_alloc(total + 1);
    size_t off = 0;
    for (int i = 0; i < argc; i++) {
        if (!cando_is_string(args[i])) continue;
        CandoString *s = cando_as_string(args[i]);
        u32 start = 0;
        u32 end   = s->length;
        if (end == 0) continue;
        /* Strip leading separators from non-first parts -- they're
         * redundant with the separator we'll insert ourselves. */
        if (off > 0) while (start < end && is_sep(s->data[start])) start++;
        if (start == end) continue;
        if (off > 0 && out[off - 1] != PATH_SEP && out[off - 1] != PATH_ALT_SEP)
            out[off++] = PATH_SEP;
        memcpy(out + off, s->data + start, end - start);
        off += end - start;
    }
    out[off] = '\0';
    libutil_push_str(vm, out, (u32)off);
    cando_free(out);
    return 1;
}

static int file_resolve(CandoVM *vm, int argc, CandoValue *args)
{
    /* Same as join + make absolute via realpath. */
    char *path = NULL;
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (!cando_is_string(args[i])) continue;
        total += cando_as_string(args[i])->length + 1;
    }
    path = (char *)cando_alloc(total + 1);
    size_t off = 0;
    for (int i = 0; i < argc; i++) {
        if (!cando_is_string(args[i])) continue;
        CandoString *s = cando_as_string(args[i]);
        if (s->length == 0) continue;
        if (off > 0 && path[off - 1] != PATH_SEP && path[off - 1] != PATH_ALT_SEP)
            path[off++] = PATH_SEP;
        memcpy(path + off, s->data, s->length);
        off += s->length;
    }
    path[off] = '\0';

#if defined(_WIN32) || defined(_WIN64)
    char abs[MAX_PATH];
    DWORD n = GetFullPathNameA(path, MAX_PATH, abs, NULL);
    if (n == 0 || n >= MAX_PATH) {
        libutil_push_str(vm, path, (u32)off);
    } else {
        libutil_push_str(vm, abs, (u32)n);
    }
#else
    char abs[PATH_MAX];
    /* realpath requires the file to exist; fall back to a manual prefix
     * if it doesn't (still useful for joining with cwd). */
    if (realpath(path, abs)) {
        libutil_push_str(vm, abs, (u32)strlen(abs));
    } else if (path[0] == '/') {
        libutil_push_str(vm, path, (u32)off);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            char joined[PATH_MAX * 2];
            snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
            libutil_push_str(vm, joined, (u32)strlen(joined));
        } else {
            libutil_push_str(vm, path, (u32)off);
        }
    }
#endif
    cando_free(path);
    return 1;
}

static int file_realpath(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    if (!path) { cando_vm_push(vm, cando_null()); return 1; }
#if defined(_WIN32) || defined(_WIN64)
    char abs[MAX_PATH];
    DWORD n = GetFullPathNameA(path, MAX_PATH, abs, NULL);
    if (n == 0 || n >= MAX_PATH) { cando_vm_push(vm, cando_null()); return 1; }
    libutil_push_str(vm, abs, (u32)n);
#else
    char abs[PATH_MAX];
    if (!realpath(path, abs)) { cando_vm_push(vm, cando_null()); return 1; }
    libutil_push_str(vm, abs, (u32)strlen(abs));
#endif
    return 1;
}

static int file_rmdir(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    if (!path) { cando_vm_push(vm, cando_bool(false)); return 1; }
#if defined(_WIN32) || defined(_WIN64)
    cando_vm_push(vm, cando_bool(_rmdir(path) == 0));
#else
    cando_vm_push(vm, cando_bool(rmdir(path) == 0));
#endif
    return 1;
}

static int file_chmod(CandoVM *vm, int argc, CandoValue *args)
{
    const char *path = libutil_arg_cstr_at(args, argc, 0);
    int mode = (int)libutil_arg_num_at(args, argc, 1, 0644);
    if (!path) { cando_vm_push(vm, cando_bool(false)); return 1; }
#if defined(_WIN32) || defined(_WIN64)
    cando_vm_push(vm, cando_bool(_chmod(path, mode) == 0));
#else
    cando_vm_push(vm, cando_bool(chmod(path, (mode_t)mode) == 0));
#endif
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_file_register(CandoVM *vm)
{
    CandoValue file_val = cando_bridge_new_object(vm);
    CdoObject *file_obj = cando_bridge_resolve(vm, cando_as_handle(file_val));

    libutil_set_method(vm, file_obj, "read",      file_read);
    libutil_set_method(vm, file_obj, "write",     file_write);
    libutil_set_method(vm, file_obj, "append",    file_append);
    libutil_set_method(vm, file_obj, "exists",    file_exists);
    libutil_set_method(vm, file_obj, "delete",    file_delete);
    libutil_set_method(vm, file_obj, "copy",      file_copy);
    libutil_set_method(vm, file_obj, "move",      file_move);
    libutil_set_method(vm, file_obj, "size",      file_size);
    libutil_set_method(vm, file_obj, "lines",     file_lines);
    libutil_set_method(vm, file_obj, "mkdir",     file_mkdir);
    libutil_set_method(vm, file_obj, "list",      file_list);
    libutil_set_method(vm, file_obj, "open",      file_open);

    /* New: stat / type checks. */
    libutil_set_method(vm, file_obj, "stat",       file_stat);
    libutil_set_method(vm, file_obj, "isFile",     file_isFile);
    libutil_set_method(vm, file_obj, "isDir",      file_isDir);
    libutil_set_method(vm, file_obj, "isSymlink",  file_isSymlink);

    /* New: path helpers. */
    libutil_set_method(vm, file_obj, "basename",   file_basename);
    libutil_set_method(vm, file_obj, "dirname",    file_dirname);
    libutil_set_method(vm, file_obj, "extname",    file_extname);
    libutil_set_method(vm, file_obj, "join",       file_join);
    libutil_set_method(vm, file_obj, "resolve",    file_resolve);
    libutil_set_method(vm, file_obj, "realpath",   file_realpath);

    /* New: directory + perms. */
    libutil_set_method(vm, file_obj, "rmdir",      file_rmdir);
    libutil_set_method(vm, file_obj, "chmod",      file_chmod);

    cando_vm_set_global(vm, "file", file_val, true);
}
