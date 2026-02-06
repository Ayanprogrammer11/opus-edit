/*
 * terminal.c – Raw-mode management, window-size detection, signals.
 */

#include "terminal.h"
#include "editor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ── Signal state ─────────────────────────────────────────── */

static volatile sig_atomic_t pending_resize = 0;
static volatile sig_atomic_t pending_exit   = 0;

/* ── Raw mode ─────────────────────────────────────────────── */

void terminal_disable_raw_mode(void)
{
    /* Best-effort restore; nothing useful to do on failure. */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);

    /* Show cursor, exit alternate screen if desired */
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

void terminal_enable_raw_mode(void)
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        perror("tcgetattr");
        exit(1);
    }
    atexit(terminal_disable_raw_mode);

    struct termios raw = E.orig_termios;

    /*
     * Input flags:
     *   BRKINT  – break condition sends SIGINT → off
     *   ICRNL   – CR → NL translation → off  (we want raw '\r')
     *   INPCK   – parity checking → off
     *   ISTRIP  – strip 8th bit → off
     *   IXON    – Ctrl-S / Ctrl-Q flow control → off
     */
    raw.c_iflag &= ~((unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON));

    /*
     * Output flags:
     *   OPOST   – post-processing (NL → CR+NL) → off
     */
    raw.c_oflag &= ~((unsigned long)OPOST);

    /*
     * Control flags:
     *   CS8     – set character size to 8 bits
     */
    raw.c_cflag |= (unsigned long)CS8;

    /*
     * Local flags:
     *   ECHO    – echo input → off
     *   ICANON  – canonical mode → off (byte-at-a-time input)
     *   IEXTEN  – Ctrl-V literal → off
     *   ISIG    – SIGINT / SIGTSTP on Ctrl-C / Ctrl-Z → off
     */
    raw.c_lflag &= ~((unsigned long)(ECHO | ICANON | IEXTEN | ISIG));

    /*
     * read() behaviour:
     *   VMIN  = 0  – return as soon as any input is available
     *   VTIME = 1  – 100 ms timeout (so we don't spin)
     */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(1);
    }
}

/* ── Window size ──────────────────────────────────────────── */

static int terminal_get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    /* Ask the terminal for the current cursor position */
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;

    const char *p = &buf[2];
    char *end = NULL;
    errno = 0;
    unsigned long r = strtoul(p, &end, 10);
    if (errno || end == p || *end != ';') return -1;

    p = end + 1;
    errno = 0;
    unsigned long c = strtoul(p, &end, 10);
    if (errno || end == p || *end != '\0') return -1;

    if (r == 0 || c == 0 || r > (unsigned long)INT_MAX || c > (unsigned long)INT_MAX)
        return -1;

    *rows = (int)r;
    *cols = (int)c;
    return 0;
}

int terminal_get_window_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /*
         * Fallback: move the cursor to the bottom-right corner,
         * then query position.
         */
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return terminal_get_cursor_position(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ── Signal handlers ──────────────────────────────────────── */

static void handle_sigwinch(int sig)
{
    (void)sig;
    pending_resize = 1;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    pending_exit = 1;
}

int terminal_exit_requested(void)
{
    return pending_exit != 0;
}

int terminal_apply_pending_resize(void)
{
    if (!pending_resize) return 0;

    int rows, cols;
    if (terminal_get_window_size(&rows, &cols) == -1)
        return 0;

    pending_resize = 0;
    E.screenrows = (rows > 2) ? (rows - 2) : 1; /* status + message bars */
    E.screencols = cols;

    /* Clamp cursor position */
    if (E.cy >= E.numrows) E.cy = E.numrows ? E.numrows - 1 : 0;

    return 1;
}

void terminal_install_signal_handlers(void)
{
    struct sigaction sa;

    /* SIGWINCH – terminal resize */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigwinch;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    /* SIGINT – ignore (we handle Ctrl-C in the input layer) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);

    /* SIGTERM – graceful shutdown */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);

    /* SIGPIPE – ignore (can happen when piping output) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}
