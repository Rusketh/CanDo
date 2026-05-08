/*
 * lib/process.c -- Subprocess management standard library for Cando.
 *
 * Surface:
 *   process.pid()                       -- current process ID
 *   process.ppid()                      -- parent process ID
 *   process.spawn(argv [, opts])        -- spawn a child process; returns
 *                                          a `proc` instance
 *
 * On a `proc`:
 *   proc:pid()                          -- child's pid
 *   proc:stdin()  / :stdout() / :stderr() -- streams for the requested
 *                                          piped fds (`null` if "inherit"
 *                                          or "null" was selected)
 *   proc:wait()                         -- waitpid; returns exit code
 *   proc:kill([sig])                    -- signal the child (default SIGTERM)
 *
 * Spawning is POSIX-only at the moment.  The Windows path returns an
 * error; a follow-up will add a CreateProcess implementation.
 *
 * Must compile with gcc -std=c11.
 */

#include "process.h"
#include "libutil.h"
#include "meta.h"
#include "stream.h"
#include "file.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"
#include "../core/thread_platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <fcntl.h>
extern char **environ;
#endif

/* =========================================================================
 * Pool
 * ===================================================================== */

#define PROC_MAX_INSTANCES 64

typedef struct ProcSlot {
    bool             in_use;
    int              pid;
    int              stdin_fd;        /* parent write end, -1 if not piped  */
    int              stdout_fd;       /* parent read end                    */
    int              stderr_fd;       /* parent read end                    */
    int              exit_code;
    bool             waited;
    /* Cached stream slot indices so :stdin/:stdout/:stderr return the same
     * underlying stream on repeated calls.  -1 means "not created yet". */
    int              stdin_stream_idx;
    int              stdout_stream_idx;
    int              stderr_stream_idx;
    /* Windows: HANDLE for the child process so wait/kill can address it.
     * Stored as void* to keep the struct identical across platforms. */
    void            *win_proc_handle;
} ProcSlot;

static ProcSlot      g_proc_pool[PROC_MAX_INSTANCES];
static cando_mutex_t g_proc_pool_mutex;
static _Atomic(int)  g_proc_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_proc_pool_inited, &expected, 1)) {
        cando_os_mutex_init(&g_proc_pool_mutex);
        for (int i = 0; i < PROC_MAX_INSTANCES; i++) {
            memset(&g_proc_pool[i], 0, sizeof(ProcSlot));
            g_proc_pool[i].stdin_fd          = -1;
            g_proc_pool[i].stdout_fd         = -1;
            g_proc_pool[i].stderr_fd         = -1;
            g_proc_pool[i].stdin_stream_idx  = -1;
            g_proc_pool[i].stdout_stream_idx = -1;
            g_proc_pool[i].stderr_stream_idx = -1;
        }
    }
}

static int proc_pool_alloc(void)
{
    ensure_pool_inited();
    cando_os_mutex_lock(&g_proc_pool_mutex);
    int idx = -1;
    for (int i = 0; i < PROC_MAX_INSTANCES; i++) {
        if (!g_proc_pool[i].in_use) {
            memset(&g_proc_pool[i], 0, sizeof(ProcSlot));
            g_proc_pool[i].in_use            = true;
            g_proc_pool[i].pid               = -1;
            g_proc_pool[i].stdin_fd          = -1;
            g_proc_pool[i].stdout_fd         = -1;
            g_proc_pool[i].stderr_fd         = -1;
            g_proc_pool[i].stdin_stream_idx  = -1;
            g_proc_pool[i].stdout_stream_idx = -1;
            g_proc_pool[i].stderr_stream_idx = -1;
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_proc_pool_mutex);
    return idx;
}

static ProcSlot *proc_pool_get(int idx)
{
    if (idx < 0 || idx >= PROC_MAX_INSTANCES) return NULL;
    if (!g_proc_pool[idx].in_use) return NULL;
    return &g_proc_pool[idx];
}

/* =========================================================================
 * Field helpers (same shape as socket.c)
 * ===================================================================== */

static bool get_int_field(CdoObject *obj, const char *name, int *out)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = (int)v.as.number;
    return true;
}

static void set_num_field(CdoObject *obj, const char *name, f64 n)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(n), FIELD_NONE);
    cdo_string_release(key);
}

static ProcSlot *proc_resolve_receiver(CandoVM *vm, CandoValue receiver)
{
    if (!cando_is_object(receiver)) return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(receiver));
    if (!obj) return NULL;
    int idx = -1;
    if (!get_int_field(obj, "__proc_id", &idx)) return NULL;
    return proc_pool_get(idx);
}

static CandoValue proc_create_instance(CandoVM *vm, int slot_idx)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));
    set_num_field(obj, "__proc_id", (f64)slot_idx);
    cando_lib_meta_attach(vm, obj, "proc");
    return val;
}

/* =========================================================================
 * Spawn
 * ===================================================================== */

typedef enum { STDIO_INHERIT = 0, STDIO_PIPE = 1, STDIO_NULL = 2 } StdioMode;

static StdioMode parse_stdio(CdoObject *opts, const char *key, StdioMode def)
{
    if (!opts) return def;
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue   v = cdo_null();
    bool       ok = cdo_object_rawget(opts, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_STRING || !v.as.string) return def;
    const char *s = v.as.string->data;
    if (strcmp(s, "pipe")    == 0) return STDIO_PIPE;
    if (strcmp(s, "null")    == 0) return STDIO_NULL;
    if (strcmp(s, "inherit") == 0) return STDIO_INHERIT;
    return def;
}

#if defined(_WIN32) || defined(_WIN64)
/* =========================================================================
 * Windows spawn: CreateProcess + anonymous pipes.
 *
 * The Windows command-line is one big string; we have to build it from
 * argv with the standard CommandLineToArgv quoting rules so the child
 * sees the same tokens.  Pipe ends are handed to the child via
 * STARTF_USESTDHANDLES with bInheritHandles=TRUE; the parent's ends
 * are explicitly marked non-inheritable so they don't leak into a
 * subsequent CreateProcess call.
 * ===================================================================== */

/* Quote one argv element per CommandLineToArgvW's escaping rules. */
static void win_append_quoted(char **buf, usize *len, usize *cap, const char *arg)
{
    bool need_quote = (*arg == '\0') ||
                      strchr(arg, ' ')  || strchr(arg, '\t') ||
                      strchr(arg, '"');
    usize alen = strlen(arg);

    /* Worst case: every char becomes \\ + " expansion = 4x.  Plus quotes. */
    usize need = (need_quote ? 2 : 0) + alen * 4;
    if (*len + need + 1 > *cap) {
        while (*len + need + 1 > *cap) *cap *= 2;
        *buf = (char *)cando_realloc(*buf, *cap);
    }

    if (!need_quote) {
        memcpy(*buf + *len, arg, alen);
        *len += alen;
        return;
    }

    (*buf)[(*len)++] = '"';
    int bs = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            bs++;
            (*buf)[(*len)++] = '\\';
        } else if (*p == '"') {
            for (int k = 0; k < bs; k++) (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = '"';
            bs = 0;
        } else {
            bs = 0;
            (*buf)[(*len)++] = *p;
        }
    }
    for (int k = 0; k < bs; k++) (*buf)[(*len)++] = '\\';
    (*buf)[(*len)++] = '"';
}

static char *win_build_cmdline(char *const argv[])
{
    usize cap = 256, len = 0;
    char *out = (char *)cando_alloc(cap);
    for (int i = 0; argv[i]; i++) {
        if (i > 0) {
            if (len + 1 >= cap) { cap *= 2; out = (char *)cando_realloc(out, cap); }
            out[len++] = ' ';
        }
        win_append_quoted(&out, &len, &cap, argv[i]);
    }
    if (len + 1 > cap) { cap += 1; out = (char *)cando_realloc(out, cap); }
    out[len] = '\0';
    return out;
}

static int spawn_windows(char *const argv[],
                         StdioMode in_mode, StdioMode out_mode, StdioMode err_mode,
                         const char *cwd,
                         char *err, usize errlen)
{
    HANDLE in_rd = NULL, in_wr = NULL;
    HANDLE out_rd = NULL, out_wr = NULL;
    HANDLE err_rd = NULL, err_wr = NULL;
    HANDLE null_in = INVALID_HANDLE_VALUE, null_out = INVALID_HANDLE_VALUE;
    int slot_idx = -1;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (in_mode == STDIO_PIPE) {
        if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) goto pipe_err;
        SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    }
    if (out_mode == STDIO_PIPE) {
        if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) goto pipe_err;
        SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    }
    if (err_mode == STDIO_PIPE) {
        if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) goto pipe_err;
        SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    }
    if (in_mode == STDIO_NULL) {
        null_in = CreateFileA("NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
    }
    if (out_mode == STDIO_NULL || err_mode == STDIO_NULL) {
        null_out = CreateFileA("NUL", GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, NULL);
    }

    slot_idx = proc_pool_alloc();
    if (slot_idx < 0) {
        snprintf(err, errlen, "process.spawn: pool exhausted");
        goto cleanup;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb        = sizeof(si);
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput  = (in_mode  == STDIO_PIPE) ? in_rd  :
                    (in_mode  == STDIO_NULL) ? null_in :
                    GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = (out_mode == STDIO_PIPE) ? out_wr :
                    (out_mode == STDIO_NULL) ? null_out :
                    GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = (err_mode == STDIO_PIPE) ? err_wr :
                    (err_mode == STDIO_NULL) ? null_out :
                    GetStdHandle(STD_ERROR_HANDLE);

    char *cmdline = win_build_cmdline(argv);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             0, NULL, cwd, &si, &pi);
    cando_free(cmdline);

    if (!ok) {
        snprintf(err, errlen, "process.spawn: CreateProcess failed: %lu",
                 (unsigned long)GetLastError());
        g_proc_pool[slot_idx].in_use = false;
        slot_idx = -1;
        goto cleanup;
    }

    /* Close the child ends in the parent so EOF propagates correctly when
     * the child exits. */
    if (in_rd)  { CloseHandle(in_rd);  in_rd  = NULL; }
    if (out_wr) { CloseHandle(out_wr); out_wr = NULL; }
    if (err_wr) { CloseHandle(err_wr); err_wr = NULL; }
    if (null_in  != INVALID_HANDLE_VALUE) { CloseHandle(null_in);  null_in  = INVALID_HANDLE_VALUE; }
    if (null_out != INVALID_HANDLE_VALUE) { CloseHandle(null_out); null_out = INVALID_HANDLE_VALUE; }

    ProcSlot *p = &g_proc_pool[slot_idx];
    p->pid = (int)pi.dwProcessId;
    p->win_proc_handle = pi.hProcess;
    if (in_mode == STDIO_PIPE)
        p->stdin_fd  = _open_osfhandle((intptr_t)in_wr,  _O_WRONLY | _O_BINARY);
    if (out_mode == STDIO_PIPE)
        p->stdout_fd = _open_osfhandle((intptr_t)out_rd, _O_RDONLY | _O_BINARY);
    if (err_mode == STDIO_PIPE)
        p->stderr_fd = _open_osfhandle((intptr_t)err_rd, _O_RDONLY | _O_BINARY);
    CloseHandle(pi.hThread);
    return slot_idx;

pipe_err:
    snprintf(err, errlen, "process.spawn: CreatePipe failed: %lu",
             (unsigned long)GetLastError());
cleanup:
    if (in_rd)  CloseHandle(in_rd);
    if (in_wr)  CloseHandle(in_wr);
    if (out_rd) CloseHandle(out_rd);
    if (out_wr) CloseHandle(out_wr);
    if (err_rd) CloseHandle(err_rd);
    if (err_wr) CloseHandle(err_wr);
    if (null_in  != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (null_out != INVALID_HANDLE_VALUE) CloseHandle(null_out);
    return slot_idx < 0 ? -1 : -1;
}
#else
/*
 * spawn_posix -- fork + exec on POSIX.  Returns the parent's slot index, or
 * -1 with `err` populated on failure.  argv must be NULL-terminated.
 */
static int spawn_posix(char *const argv[],
                       StdioMode in_mode, StdioMode out_mode, StdioMode err_mode,
                       const char *cwd,
                       char *err, usize errlen)
{
    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };

    if (in_mode  == STDIO_PIPE && pipe(in_pipe)  != 0) goto pipe_err;
    if (out_mode == STDIO_PIPE && pipe(out_pipe) != 0) goto pipe_err;
    if (err_mode == STDIO_PIPE && pipe(err_pipe) != 0) goto pipe_err;

    int slot_idx = proc_pool_alloc();
    if (slot_idx < 0) {
        snprintf(err, errlen, "process.spawn: pool exhausted");
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "process.spawn: fork failed: %s", strerror(errno));
        g_proc_pool[slot_idx].in_use = false;
        goto cleanup;
    }

    if (pid == 0) {
        /* Child.  Wire stdio. */
        if (in_mode == STDIO_PIPE) {
            dup2(in_pipe[0], 0);
            close(in_pipe[0]);
            close(in_pipe[1]);
        } else if (in_mode == STDIO_NULL) {
            int dn = open("/dev/null", O_RDONLY);
            if (dn >= 0) { dup2(dn, 0); close(dn); }
        }
        if (out_mode == STDIO_PIPE) {
            dup2(out_pipe[1], 1);
            close(out_pipe[0]);
            close(out_pipe[1]);
        } else if (out_mode == STDIO_NULL) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, 1); close(dn); }
        }
        if (err_mode == STDIO_PIPE) {
            dup2(err_pipe[1], 2);
            close(err_pipe[0]);
            close(err_pipe[1]);
        } else if (err_mode == STDIO_NULL) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, 2); close(dn); }
        }
        if (cwd && *cwd) {
            if (chdir(cwd) != 0) _exit(127);
        }
        execvp(argv[0], argv);
        /* exec failed. */
        _exit(127);
    }

    /* Parent. */
    ProcSlot *p = &g_proc_pool[slot_idx];
    p->pid = pid;
    if (in_mode  == STDIO_PIPE) { close(in_pipe[0]);  p->stdin_fd  = in_pipe[1];  }
    if (out_mode == STDIO_PIPE) { close(out_pipe[1]); p->stdout_fd = out_pipe[0]; }
    if (err_mode == STDIO_PIPE) { close(err_pipe[1]); p->stderr_fd = err_pipe[0]; }
    return slot_idx;

pipe_err:
    snprintf(err, errlen, "process.spawn: pipe failed: %s", strerror(errno));
cleanup:
    if (in_pipe[0]  >= 0) close(in_pipe[0]);
    if (in_pipe[1]  >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    return -1;
}
#endif

/* process.spawn(argv [, opts]) -> proc */
static int process_spawn_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm, "process.spawn: argv (array) required");
        return -1;
    }
    CdoObject *argv_obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    if (!argv_obj) {
        cando_vm_error(vm, "process.spawn: argv is invalid");
        return -1;
    }
    u32 nargs = cdo_array_len(argv_obj);
    if (nargs == 0) {
        cando_vm_error(vm, "process.spawn: argv is empty");
        return -1;
    }

    /* Materialise argv into a NULL-terminated char* array.  We dup each
     * string so the child has stable storage independent of script GC. */
    char **argv = (char **)cando_alloc(sizeof(char *) * (nargs + 1));
    for (u32 i = 0; i < nargs; i++) {
        CdoValue v = cdo_null();
        cdo_array_rawget_idx(argv_obj, i, &v);
        if (v.tag != CDO_STRING || !v.as.string) {
            for (u32 j = 0; j < i; j++) cando_free(argv[j]);
            cando_free(argv);
            cando_vm_error(vm, "process.spawn: argv[%u] must be a string", i);
            return -1;
        }
        usize len = v.as.string->length;
        argv[i] = (char *)cando_alloc(len + 1);
        memcpy(argv[i], v.as.string->data, len);
        argv[i][len] = '\0';
    }
    argv[nargs] = NULL;

    StdioMode  in_mode  = STDIO_INHERIT;
    StdioMode  out_mode = STDIO_INHERIT;
    StdioMode  err_mode = STDIO_INHERIT;
    const char *cwd     = NULL;
    if (argc >= 2 && cando_is_object(args[1])) {
        CdoObject *opts = cando_bridge_resolve(vm, cando_as_handle(args[1]));
        in_mode  = parse_stdio(opts, "stdin",  STDIO_INHERIT);
        out_mode = parse_stdio(opts, "stdout", STDIO_INHERIT);
        err_mode = parse_stdio(opts, "stderr", STDIO_INHERIT);
        CdoString *kcwd = cdo_string_intern("cwd", 3);
        CdoValue   vcwd = cdo_null();
        bool ok = cdo_object_rawget(opts, kcwd, &vcwd);
        cdo_string_release(kcwd);
        if (ok && vcwd.tag == CDO_STRING && vcwd.as.string)
            cwd = vcwd.as.string->data;
    }

    char errbuf[256] = {0};
    int slot_idx;
#if defined(_WIN32) || defined(_WIN64)
    slot_idx = spawn_windows(argv, in_mode, out_mode, err_mode, cwd,
                             errbuf, sizeof(errbuf));
#else
    slot_idx = spawn_posix(argv, in_mode, out_mode, err_mode, cwd,
                           errbuf, sizeof(errbuf));
#endif

    for (u32 i = 0; i < nargs; i++) cando_free(argv[i]);
    cando_free(argv);

    if (slot_idx < 0) {
        cando_vm_error(vm, "%s", errbuf[0] ? errbuf : "process.spawn failed");
        return -1;
    }
    cando_vm_push(vm, proc_create_instance(vm, slot_idx));
    return 1;
}

/* =========================================================================
 * proc instance methods
 * ===================================================================== */

/* proc:pid() */
static int proc_pid_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_number(p ? (f64)p->pid : -1.0));
    return 1;
}

/* Build (or return cached) stream over one of the proc's piped fds. */
static int proc_stream_for(CandoVM *vm, ProcSlot *p, int *cache_idx,
                           int fd, const char *fmode, unsigned caps,
                           const char *method)
{
    if (!p) {
        cando_vm_error(vm, "%s: invalid receiver", method);
        return -1;
    }
    if (fd < 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    /* Reuse the cached stream slot if it's still alive. */
    if (*cache_idx >= 0) {
        StreamSlot *s = stream_pool_get(*cache_idx);
        if (s) {
            cando_vm_push(vm, stream_create_instance(vm, *cache_idx));
            return 1;
        }
        /* Cached slot was released — fall through and create a fresh one
         * IF we still have the fd.  But since file_stream's destroy
         * fclose's the underlying FILE* (which closes the fd), the fd is
         * already invalid in that case. */
        cando_vm_error(vm, "%s: stream was closed", method);
        return -1;
    }
    FILE *fp = fdopen(fd, fmode);
    if (!fp) {
        cando_vm_error(vm, "%s: fdopen failed: %s", method, strerror(errno));
        return -1;
    }
    /* From here the FILE* owns the fd; clear it on the slot so :wait()
     * doesn't try to close it again. */
    if (cache_idx == &p->stdin_stream_idx)  p->stdin_fd  = -1;
    if (cache_idx == &p->stdout_stream_idx) p->stdout_fd = -1;
    if (cache_idx == &p->stderr_stream_idx) p->stderr_fd = -1;

    CandoValue sv = cando_lib_file_stream_from_fp(vm, fp, caps);
    if (cando_is_null(sv)) {
        cando_vm_error(vm, "%s: too many active streams", method);
        return -1;
    }
    /* Recover the slot index from the returned object so we can cache it. */
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(sv));
    int idx = -1;
    get_int_field(obj, "__stream_id", &idx);
    *cache_idx = idx;
    cando_vm_push(vm, sv);
    return 1;
}

/* proc:stdin() -> writable stream | null */
static int proc_stdin_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    return proc_stream_for(vm, p, p ? &p->stdin_stream_idx : NULL,
                           p ? p->stdin_fd : -1,
                           "wb", STREAM_CAP_WRITABLE, "proc:stdin");
}

/* proc:stdout() -> readable stream | null */
static int proc_stdout_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    return proc_stream_for(vm, p, p ? &p->stdout_stream_idx : NULL,
                           p ? p->stdout_fd : -1,
                           "rb", STREAM_CAP_READABLE, "proc:stdout");
}

/* proc:stderr() -> readable stream | null */
static int proc_stderr_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    return proc_stream_for(vm, p, p ? &p->stderr_stream_idx : NULL,
                           p ? p->stderr_fd : -1,
                           "rb", STREAM_CAP_READABLE, "proc:stderr");
}

/* Close fds the user never promoted to a stream.  Stream-owned fds are
 * cleared from the slot at promotion time so this only frees orphans. */
static void proc_close_orphan_fds(ProcSlot *p)
{
#if defined(_WIN32) || defined(_WIN64)
    if (p->stdin_fd  >= 0) { _close(p->stdin_fd);  p->stdin_fd  = -1; }
    if (p->stdout_fd >= 0) { _close(p->stdout_fd); p->stdout_fd = -1; }
    if (p->stderr_fd >= 0) { _close(p->stderr_fd); p->stderr_fd = -1; }
#else
    if (p->stdin_fd  >= 0) { close(p->stdin_fd);  p->stdin_fd  = -1; }
    if (p->stdout_fd >= 0) { close(p->stdout_fd); p->stdout_fd = -1; }
    if (p->stderr_fd >= 0) { close(p->stderr_fd); p->stderr_fd = -1; }
#endif
}

/* proc:wait() -> exit code */
static int proc_wait_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!p) {
        cando_vm_error(vm, "proc:wait: invalid receiver");
        return -1;
    }
    if (p->waited) {
        cando_vm_push(vm, cando_number((f64)p->exit_code));
        return 1;
    }
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = (HANDLE)p->win_proc_handle;
    if (h == NULL) {
        cando_vm_error(vm, "proc:wait: process handle is invalid");
        return -1;
    }
    DWORD wr = WaitForSingleObject(h, INFINITE);
    if (wr != WAIT_OBJECT_0) {
        cando_vm_error(vm, "proc:wait: WaitForSingleObject failed: %lu",
                       (unsigned long)GetLastError());
        return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    p->win_proc_handle = NULL;
    p->exit_code       = (int)code;
    p->waited          = true;
    proc_close_orphan_fds(p);
    cando_vm_push(vm, cando_number((f64)code));
    return 1;
#else
    int status = 0;
    if (waitpid(p->pid, &status, 0) < 0) {
        cando_vm_error(vm, "proc:wait: waitpid failed: %s", strerror(errno));
        return -1;
    }
    int code = WIFEXITED(status)  ? WEXITSTATUS(status)
            : WIFSIGNALED(status) ? -WTERMSIG(status)
            : -1;
    p->exit_code = code;
    p->waited    = true;
    proc_close_orphan_fds(p);
    cando_vm_push(vm, cando_number((f64)code));
    return 1;
#endif
}

/* proc:kill([sig]) -- POSIX takes a signal; on Windows the signal arg is
 * ignored and the child is unconditionally terminated. */
static int proc_kill_fn(CandoVM *vm, int argc, CandoValue *args)
{
    ProcSlot *p = proc_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!p) {
        cando_vm_error(vm, "proc:kill: invalid receiver");
        return -1;
    }
#if defined(_WIN32) || defined(_WIN64)
    (void)argc;
    HANDLE h = (HANDLE)p->win_proc_handle;
    if (h == NULL) {
        cando_vm_error(vm, "proc:kill: process handle is invalid");
        return -1;
    }
    if (!TerminateProcess(h, 1)) {
        cando_vm_error(vm, "proc:kill: TerminateProcess failed: %lu",
                       (unsigned long)GetLastError());
        return -1;
    }
    cando_vm_push(vm, args[0]);
    return 1;
#else
    int sig = (int)libutil_arg_num_at(args, argc, 1, (f64)SIGTERM);
    if (kill(p->pid, sig) != 0) {
        cando_vm_error(vm, "proc:kill: %s", strerror(errno));
        return -1;
    }
    cando_vm_push(vm, args[0]);
    return 1;
#endif
}

/* =========================================================================
 * Module entry
 * ===================================================================== */

static int process_pid(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)getpid()));
    return 1;
}

static int process_ppid(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
#if defined(_WIN32) || defined(_WIN64)
    cando_vm_push(vm, cando_number(0.0));
#else
    cando_vm_push(vm, cando_number((f64)getppid()));
#endif
    return 1;
}

void cando_lib_process_register(CandoVM *vm)
{
    ensure_pool_inited();
    cando_lib_meta_register(vm);

    CandoValue proc_val = cando_bridge_new_object(vm);
    CdoObject *proc_obj = cando_bridge_resolve(vm, cando_as_handle(proc_val));

    libutil_set_method(vm, proc_obj, "pid",   process_pid);
    libutil_set_method(vm, proc_obj, "ppid",  process_ppid);
    libutil_set_method(vm, proc_obj, "spawn", process_spawn_fn);

    cando_vm_set_global(vm, "process", proc_val, true);

    /* Per-instance meta table for spawned children. */
    CdoObject *meta = cando_lib_meta_table(vm, "proc");
    if (meta) {
        cando_lib_meta_define(vm, meta, "pid",    proc_pid_fn);
        cando_lib_meta_define(vm, meta, "stdin",  proc_stdin_fn);
        cando_lib_meta_define(vm, meta, "stdout", proc_stdout_fn);
        cando_lib_meta_define(vm, meta, "stderr", proc_stderr_fn);
        cando_lib_meta_define(vm, meta, "wait",   proc_wait_fn);
        cando_lib_meta_define(vm, meta, "kill",   proc_kill_fn);
    }
}
