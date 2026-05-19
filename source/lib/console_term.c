/*
 * lib/console_term.c -- Cross-platform terminal control implementation.
 *
 * POSIX path: termios for raw mode, ANSI escape sequences for cursor
 * and colour, select() over STDIN_FILENO for input polling, SIGWINCH
 * handler for resize, /dev/null for detach.
 *
 * Windows path: SetConsoleMode for raw + virtual-terminal-processing,
 * ReadConsoleInput for events (with WaitForSingleObject for polling),
 * GetConsoleWindow + ShowWindow for hide/show, FreeConsole +
 * AllocConsole for detach/attach.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include "console_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  define CT_PLATFORM_WINDOWS 1
#else
#  include <unistd.h>
#  include <termios.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#  define CT_PLATFORM_POSIX 1
#endif

/* =========================================================================
 * Globals
 * ======================================================================= */

static atomic_int  g_inited       = 0;
static atomic_int  g_resize_flag  = 0;
static atomic_bool g_detached     = false;

#if CT_PLATFORM_POSIX
static struct termios g_termios_saved;
static bool           g_termios_saved_valid = false;
static bool           g_raw_mode            = false;
static bool           g_echo                = true;
static bool           g_mouse_on            = false;
static bool           g_alt_screen_on       = false;
static bool           g_cursor_hidden       = false;
#endif

#if CT_PLATFORM_WINDOWS
static DWORD g_stdin_mode_saved   = 0;
static DWORD g_stdout_mode_saved  = 0;
static bool  g_modes_saved        = false;
static bool  g_raw_mode           = false;
static bool  g_mouse_on           = false;
#endif

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

#if CT_PLATFORM_POSIX
static void on_sigwinch(int sig)
{
    (void)sig;
    atomic_store(&g_resize_flag, 1);
}
#endif

static void restore_terminal_atexit(void)
{
    console_term_shutdown();
}

/* =========================================================================
 * Lifecycle
 * ======================================================================= */

void console_term_init(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_inited, &expected, 1)) return;

#if CT_PLATFORM_POSIX
    if (tcgetattr(STDIN_FILENO, &g_termios_saved) == 0) {
        g_termios_saved_valid = true;
    }
    /* SIGWINCH: install a one-line handler that sets the sticky flag
     * the dispatcher polls.  We use signal() for portability with
     * minimal flags. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
#endif

#if CT_PLATFORM_WINDOWS
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hin  != INVALID_HANDLE_VALUE) GetConsoleMode(hin,  &g_stdin_mode_saved);
    if (hout != INVALID_HANDLE_VALUE) GetConsoleMode(hout, &g_stdout_mode_saved);
    g_modes_saved = true;

    /* Enable VT processing on output so our ANSI sequences work on
     * Windows 10+.  No-op on older Windows but the function call
     * succeeds either way. */
    if (hout != INVALID_HANDLE_VALUE) {
        DWORD m = g_stdout_mode_saved | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */;
        SetConsoleMode(hout, m);
    }
#endif

    atexit(restore_terminal_atexit);
}

void console_term_shutdown(void)
{
#if CT_PLATFORM_POSIX
    /* Take the terminal out of every persistent state we may have
     * turned on, in order. */
    if (g_mouse_on)           console_term_set_mouse(false);
    if (g_alt_screen_on)      console_term_set_alternate_screen(false);
    if (g_cursor_hidden)      console_term_set_cursor_visible(true);
    if (g_termios_saved_valid && g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_termios_saved);
        g_raw_mode = false;
    }
#endif
#if CT_PLATFORM_WINDOWS
    if (g_modes_saved) {
        HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hin  != INVALID_HANDLE_VALUE) SetConsoleMode(hin,  g_stdin_mode_saved);
        if (hout != INVALID_HANDLE_VALUE) SetConsoleMode(hout, g_stdout_mode_saved);
    }
#endif
}

void console_term_detach(void)
{
    if (atomic_exchange(&g_detached, true)) return;
#if CT_PLATFORM_WINDOWS
    FreeConsole();
    /* Reopen the C standard streams onto NUL so any later fputs
     * doesn't crash the runtime. */
    FILE *unused;
    (void)freopen_s(&unused, "NUL", "w", stdout);
    (void)freopen_s(&unused, "NUL", "w", stderr);
    (void)freopen_s(&unused, "NUL", "r", stdin);
#else
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
    }
#endif
}

bool console_term_attach(void)
{
    atomic_store(&g_detached, false);
#if CT_PLATFORM_WINDOWS
    if (GetConsoleWindow() != NULL) return true;
    if (!AllocConsole()) return false;
    FILE *unused;
    (void)freopen_s(&unused, "CONOUT$", "w", stdout);
    (void)freopen_s(&unused, "CONOUT$", "w", stderr);
    (void)freopen_s(&unused, "CONIN$",  "r", stdin);
    return true;
#else
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) return false;
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    return true;
#endif
}

void console_term_hide(void)
{
#if CT_PLATFORM_WINDOWS
    HWND hw = GetConsoleWindow();
    if (hw) ShowWindow(hw, SW_HIDE);
#endif
}

void console_term_show(void)
{
#if CT_PLATFORM_WINDOWS
    HWND hw = GetConsoleWindow();
    if (hw) ShowWindow(hw, SW_SHOW);
#endif
}

bool console_term_exists(void)
{
#if CT_PLATFORM_WINDOWS
    return GetConsoleWindow() != NULL;
#else
    return isatty(STDOUT_FILENO) == 1;
#endif
}

void console_term_set_title(const char *title, size_t len)
{
    if (atomic_load(&g_detached)) return;
#if CT_PLATFORM_WINDOWS
    /* SetConsoleTitleA expects NUL-terminated; build a stack copy. */
    char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, title, len);
    buf[len] = '\0';
    SetConsoleTitleA(buf);
#else
    /* xterm OSC 0 sets icon + title. */
    fputs("\x1b]0;", stdout);
    fwrite(title, 1, len, stdout);
    fputs("\x07", stdout);
    fflush(stdout);
#endif
}

/* =========================================================================
 * Output
 * ======================================================================= */

bool console_term_write(const char *data, size_t len)
{
    if (atomic_load(&g_detached)) return false;
    if (len == 0) return true;
    /* fwrite on Windows now goes via VT-processing-enabled stdout; on
     * POSIX it's the terminal pty directly. */
    size_t wr = fwrite(data, 1, len, stdout);
    return wr == len;
}

void console_term_flush(void)
{
    if (atomic_load(&g_detached)) return;
    fflush(stdout);
}

/* =========================================================================
 * Mode control
 * ======================================================================= */

bool console_term_set_raw(bool on)
{
    console_term_init();
#if CT_PLATFORM_POSIX
    if (!g_termios_saved_valid) return false;
    if (on) {
        struct termios t = g_termios_saved;
        /* cfmakeraw-equivalent.  Some libcs lack cfmakeraw outright. */
        t.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR
                                  | IGNCR | ICRNL | IXON);
        t.c_oflag &= (tcflag_t)~OPOST;
        t.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
        t.c_cflag |= CS8;
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
        g_raw_mode = true;
    } else if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_termios_saved);
        g_raw_mode = false;
    }
    return true;
#endif
#if CT_PLATFORM_WINDOWS
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin == INVALID_HANDLE_VALUE) return false;
    DWORD m;
    if (!GetConsoleMode(hin, &m)) return false;
    if (on) {
        /* Turn off line / echo / processed input; turn on virtual-
         * terminal-input so arrow keys arrive as escape sequences,
         * and mouse-input + extended-flags so SetConsoleMode mouse
         * mode sticks. */
        m &= ~(0x0001 /*ENABLE_PROCESSED_INPUT*/
             | 0x0002 /*ENABLE_LINE_INPUT*/
             | 0x0004 /*ENABLE_ECHO_INPUT*/);
        m |=  (0x0010 /*ENABLE_MOUSE_INPUT*/
             | 0x0080 /*ENABLE_EXTENDED_FLAGS*/
             | 0x0200 /*ENABLE_VIRTUAL_TERMINAL_INPUT*/);
        g_raw_mode = true;
    } else if (g_modes_saved) {
        m = g_stdin_mode_saved;
        g_raw_mode = false;
    }
    return SetConsoleMode(hin, m) ? true : false;
#endif
}

bool console_term_set_echo(bool on)
{
#if CT_PLATFORM_POSIX
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
    if (on) t.c_lflag |=  ECHO;
    else    t.c_lflag &= (tcflag_t)~ECHO;
    g_echo = on;
    return tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
#endif
#if CT_PLATFORM_WINDOWS
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin == INVALID_HANDLE_VALUE) return false;
    DWORD m;
    if (!GetConsoleMode(hin, &m)) return false;
    if (on) m |=  0x0004 /*ENABLE_ECHO_INPUT*/;
    else    m &= ~(DWORD)0x0004;
    return SetConsoleMode(hin, m) ? true : false;
#endif
}

void console_term_set_cursor_visible(bool on)
{
    if (atomic_load(&g_detached)) return;
    if (on) console_term_write("\x1b[?25h", 6);
    else    console_term_write("\x1b[?25l", 6);
#if CT_PLATFORM_POSIX
    g_cursor_hidden = !on;
#endif
    console_term_flush();
}

void console_term_set_alternate_screen(bool on)
{
    if (atomic_load(&g_detached)) return;
    if (on) console_term_write("\x1b[?1049h", 8);
    else    console_term_write("\x1b[?1049l", 8);
#if CT_PLATFORM_POSIX
    g_alt_screen_on = on;
#endif
    console_term_flush();
}

void console_term_set_mouse(bool on)
{
    if (atomic_load(&g_detached)) return;
    /* SGR mouse mode (1006) handles coordinates above 223 cleanly.
     * 1000 = any-event reporting, 1006 = SGR encoding. */
    if (on) console_term_write("\x1b[?1000h\x1b[?1006h", 16);
    else    console_term_write("\x1b[?1000l\x1b[?1006l", 16);
#if CT_PLATFORM_POSIX
    g_mouse_on = on;
#endif
#if CT_PLATFORM_WINDOWS
    g_mouse_on = on;
#endif
    console_term_flush();
}

bool console_term_is_tty(void)
{
#if CT_PLATFORM_POSIX
    return isatty(STDOUT_FILENO) == 1;
#else
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD m;
    return GetConsoleMode(h, &m) != 0;
#endif
}

void console_term_size(int *cols, int *rows)
{
    if (cols) *cols = 0;
    if (rows) *rows = 0;
#if CT_PLATFORM_POSIX
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (cols) *cols = ws.ws_col;
        if (rows) *rows = ws.ws_row;
    }
#endif
#if CT_PLATFORM_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        if (cols) *cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        if (rows) *rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    }
#endif
}

/* =========================================================================
 * Resize signalling
 * ======================================================================= */

bool console_term_resize_pending(void)
{
    return atomic_load(&g_resize_flag) != 0;
}

void console_term_resize_clear(void)
{
    atomic_store(&g_resize_flag, 0);
}

/* =========================================================================
 * Input fd / handle access
 * ======================================================================= */

#if CT_PLATFORM_POSIX
int console_term_input_fd(void)
{
    return STDIN_FILENO;
}
#else
HANDLE console_term_input_handle(void)
{
    return GetStdHandle(STD_INPUT_HANDLE);
}
#endif

int console_term_read_input(unsigned char *buf, size_t cap, int timeout_ms)
{
    if (atomic_load(&g_detached) || cap == 0) return 0;
#if CT_PLATFORM_POSIX
    if (timeout_ms >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sr = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sr <= 0) return sr == 0 ? 0 : -1;
    }
    ssize_t rd = read(STDIN_FILENO, buf, cap);
    if (rd < 0) return errno == EINTR ? 0 : -1;
    return (int)rd;
#endif
#if CT_PLATFORM_WINDOWS
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin == INVALID_HANDLE_VALUE) return -1;
    /* Wait first (so we honour timeout_ms), then read. */
    DWORD wait = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    DWORD r = WaitForSingleObject(hin, wait);
    if (r == WAIT_TIMEOUT)  return 0;
    if (r == WAIT_FAILED)   return -1;
    /* ReadConsoleInput returns INPUT_RECORDs; convert key events to
     * raw VT bytes so the same decoder works on both platforms.
     * Mouse / resize events from ReadConsoleInput are dropped here
     * and re-derived from the bytes plus the global resize flag --
     * keeping the cross-platform path narrow.  For full mouse on
     * Windows, the dispatcher uses ReadConsoleInput directly via
     * console_input_*.  For now the byte read suffices. */
    INPUT_RECORD recs[16];
    DWORD got = 0;
    if (!ReadConsoleInputA(hin, recs, 16, &got)) return -1;
    size_t out = 0;
    for (DWORD i = 0; i < got && out < cap; i++) {
        INPUT_RECORD *r2 = &recs[i];
        if (r2->EventType == KEY_EVENT && r2->Event.KeyEvent.bKeyDown) {
            CHAR ch = r2->Event.KeyEvent.uChar.AsciiChar;
            if (ch != 0) {
                buf[out++] = (unsigned char)ch;
                continue;
            }
            /* Virtual key without an AsciiChar -- emit a VT escape so
             * the decoder can identify arrows, F-keys, etc. */
            WORD vk = r2->Event.KeyEvent.wVirtualKeyCode;
            const char *seq = NULL;
            switch (vk) {
                case VK_UP:     seq = "\x1b[A"; break;
                case VK_DOWN:   seq = "\x1b[B"; break;
                case VK_RIGHT:  seq = "\x1b[C"; break;
                case VK_LEFT:   seq = "\x1b[D"; break;
                case VK_HOME:   seq = "\x1b[H"; break;
                case VK_END:    seq = "\x1b[F"; break;
                case VK_PRIOR:  seq = "\x1b[5~"; break;
                case VK_NEXT:   seq = "\x1b[6~"; break;
                case VK_INSERT: seq = "\x1b[2~"; break;
                case VK_DELETE: seq = "\x1b[3~"; break;
                default: break;
            }
            if (seq) {
                size_t sl = strlen(seq);
                if (out + sl <= cap) {
                    memcpy(buf + out, seq, sl);
                    out += sl;
                }
            }
        } else if (r2->EventType == WINDOW_BUFFER_SIZE_EVENT) {
            atomic_store(&g_resize_flag, 1);
        }
    }
    return (int)out;
#endif
}

/* =========================================================================
 * Public detach API (shared with vm.h)
 * ======================================================================= */

void cando_console_detach(void)
{
    console_term_detach();
}
