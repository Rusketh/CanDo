/*
 * lib/file.c -- File I/O standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "file.h"
#include "libutil.h"
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
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

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
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.read: path must be a string");
        return -1;
    }
    const char *path = libutil_arg_cstr(args[0]);
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
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "file.write: path and data must be strings");
        return -1;
    }
    const char *path = libutil_arg_cstr(args[0]);
    const char *data = libutil_arg_cstr(args[1]);
    u32 len          = args[1].as.string->length;
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
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "file.append: path and data must be strings");
        return -1;
    }
    const char *path = libutil_arg_cstr(args[0]);
    const char *data = libutil_arg_cstr(args[1]);
    u32 len          = args[1].as.string->length;
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
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.exists: path must be a string");
        return -1;
    }
    bool exists = (access(libutil_arg_cstr(args[0]), F_OK) == 0);
    cando_vm_push(vm, cando_bool(exists));
    return 1;
}

/* =========================================================================
 * file.delete(path) → bool
 * ======================================================================= */
static int file_delete(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.delete: path must be a string");
        return -1;
    }
    cando_vm_push(vm, cando_bool(remove(libutil_arg_cstr(args[0])) == 0));
    return 1;
}

/* =========================================================================
 * file.copy(src, dst) → bool
 * ======================================================================= */
static int file_copy(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "file.copy: src and dst must be strings");
        return -1;
    }
    FILE *src = fopen(libutil_arg_cstr(args[0]), "rb");
    if (!src) { cando_vm_push(vm, cando_bool(false)); return 1; }

    FILE *dst = fopen(libutil_arg_cstr(args[1]), "wb");
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
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "file.move: src and dst must be strings");
        return -1;
    }
    cando_vm_push(vm, cando_bool(rename(libutil_arg_cstr(args[0]),
                                         libutil_arg_cstr(args[1])) == 0));
    return 1;
}

/* =========================================================================
 * file.size(path) → number | null
 * ======================================================================= */
static int file_size(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.size: path must be a string");
        return -1;
    }
    struct stat st;
    if (stat(libutil_arg_cstr(args[0]), &st) != 0) {
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
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.lines: path must be a string");
        return -1;
    }
    const char *path = libutil_arg_cstr(args[0]);
    const char *mode = (argc >= 2) ? fopen_mode(args[1], 'r') : "r";

    FILE *f = fopen(path, mode);
    if (!f) { cando_vm_push(vm, cando_null()); return 1; }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr    = cando_bridge_resolve(vm, arr_val.as.handle);

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
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.mkdir: path must be a string");
        return -1;
    }
    /* 0755 permissions */
    bool ok = (mkdir(libutil_arg_cstr(args[0]), 0755) == 0 || errno == EEXIST);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

/* =========================================================================
 * file.list(path) → array of strings | null
 * ======================================================================= */
static int file_list(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "file.list: path must be a string");
        return -1;
    }
    DIR *d = opendir(libutil_arg_cstr(args[0]));
    if (!d) { cando_vm_push(vm, cando_null()); return 1; }

    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr    = cando_bridge_resolve(vm, arr_val.as.handle);

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
 * Registration
 * ======================================================================= */

void cando_lib_file_register(CandoVM *vm)
{
    CandoValue file_val = cando_bridge_new_object(vm);
    CdoObject *file_obj = cando_bridge_resolve(vm, file_val.as.handle);

    libutil_set_method(vm, file_obj, "read",   file_read);
    libutil_set_method(vm, file_obj, "write",  file_write);
    libutil_set_method(vm, file_obj, "append", file_append);
    libutil_set_method(vm, file_obj, "exists", file_exists);
    libutil_set_method(vm, file_obj, "delete", file_delete);
    libutil_set_method(vm, file_obj, "copy",   file_copy);
    libutil_set_method(vm, file_obj, "move",   file_move);
    libutil_set_method(vm, file_obj, "size",   file_size);
    libutil_set_method(vm, file_obj, "lines",  file_lines);
    libutil_set_method(vm, file_obj, "mkdir",  file_mkdir);
    libutil_set_method(vm, file_obj, "list",   file_list);

    cando_vm_set_global(vm, "file", file_val, true);
}
