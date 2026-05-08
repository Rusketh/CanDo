/*
 * modules/smtp/storage.h -- Maildir + mbox local-delivery, header-only.
 *
 * Maildir (D. J. Bernstein, qmail) uses tmp/ + new/ + cur/ subdirectories
 * with atomic rename for crash-safe delivery.  mbox is the older single-
 * file format; we use the simple "From " line separator with timestamp.
 *
 * Both writers create their parent directory tree on first delivery if
 * missing.  POSIX-only by design (Maildir doesn't really make sense on
 * Windows; mbox reading still works).
 */

#ifndef SMTP_STORAGE_H
#define SMTP_STORAGE_H

#include "smtp_helpers.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>      /* gethostname */
#  include <io.h>            /* _open, _write, _close, _unlink */
#  include <direct.h>        /* _mkdir */
#  include <process.h>       /* getpid */
#  ifndef open
#    define open  _open
#  endif
#  ifndef write
#    define write _write
#  endif
#  ifndef close
#    define close _close
#  endif
#  ifndef unlink
#    define unlink _unlink
#  endif
#  ifndef fsync
#    define fsync(fd) ((void)(fd))
#  endif
#  ifndef O_CREAT
#    define O_CREAT  _O_CREAT
#  endif
#  ifndef O_WRONLY
#    define O_WRONLY _O_WRONLY
#  endif
#  ifndef O_EXCL
#    define O_EXCL   _O_EXCL
#  endif
#else
#  include <unistd.h>
#endif

/* mkdir -p -- ignores EEXIST; creates all components in `path`. */
static int mkdirs(const char *path, int mode)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t L = strlen(tmp);
    if (L && tmp[L-1] == '/') tmp[L-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#if defined(_WIN32) || defined(_WIN64)
            (void)mode; if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
#endif
            *p = '/';
        }
    }
#if defined(_WIN32) || defined(_WIN64)
    if (mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

/* Atomic Maildir delivery: write to tmp/, fsync, rename to new/.
 *
 * Returns 0 on success, -1 on error (errno set). */
static int maildir_deliver(const char *maildir, const uint8_t *msg, size_t n)
{
    char tmp_dir[1024], new_dir[1024], cur_dir[1024];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", maildir);
    snprintf(new_dir, sizeof(new_dir), "%s/new", maildir);
    snprintf(cur_dir, sizeof(cur_dir), "%s/cur", maildir);
    if (mkdirs(tmp_dir, 0700) != 0) return -1;
    if (mkdirs(new_dir, 0700) != 0) return -1;
    if (mkdirs(cur_dir, 0700) != 0) return -1;

    char rnd[16]; random_hex(rnd, sizeof(rnd));
    char hostname[64];
#if defined(_WIN32) || defined(_WIN64)
    snprintf(hostname, sizeof(hostname), "windows");
#else
    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "host");
#endif
    /* Replace any '/' in hostname with '_'. */
    for (char *p = hostname; *p; p++) if (*p == '/') *p = '_';

    char tmp_path[1100], new_path[1100], filename[256];
    snprintf(filename, sizeof(filename), "%lld.M%.*s.%s",
             (long long)time(NULL), (int)sizeof(rnd), rnd, hostname);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", tmp_dir, filename);
    snprintf(new_path, sizeof(new_path), "%s/%s", new_dir, filename);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    size_t wrote = 0;
    while (wrote < n) {
        ssize_t k = write(fd, msg + wrote, n - wrote);
        if (k < 0) { close(fd); unlink(tmp_path); return -1; }
        wrote += (size_t)k;
    }
#if !(defined(_WIN32) || defined(_WIN64))
    fsync(fd);
#endif
    close(fd);
    if (rename(tmp_path, new_path) != 0) { unlink(tmp_path); return -1; }
    return 0;
}

/* Append a message to an mbox file with a "From " line + escape any
 * subsequent line that begins with "From " by prepending '>'. */
static int mbox_deliver(const char *mbox_path, const char *envelope_from,
                         const uint8_t *msg, size_t n)
{
    FILE *f = fopen(mbox_path, "ab");
    if (!f) return -1;
    char date[64]; rfc5322_date(date, sizeof(date));
    fprintf(f, "From %s %s\n",
            envelope_from && *envelope_from ? envelope_from : "MAILER-DAEMON",
            date);
    bool at_line_start = true;
    for (size_t i = 0; i < n; i++) {
        char c = (char)msg[i];
        if (at_line_start && i + 5 <= n && memcmp(msg + i, "From ", 5) == 0) {
            fputc('>', f);
        }
        fputc(c, f);
        at_line_start = (c == '\n');
    }
    if (n == 0 || msg[n-1] != '\n') fputc('\n', f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

#endif /* SMTP_STORAGE_H */
