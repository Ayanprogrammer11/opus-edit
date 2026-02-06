/*
 * editor.c – Shared editor state, utilities, and lifecycle.
 */

#include "editor.h"
#include "terminal.h"
#include "input.h"
#include "output.h"
#include "undo.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Global editor state ──────────────────────────────────── */
editor_config E;

/* ── Append-buffer implementation ─────────────────────────── */

void ab_init(abuf *ab)
{
    ab->b   = NULL;
    ab->len = 0;
    ab->cap = 0;
}

void ab_append(abuf *ab, const char *s, int len)
{
    if (!s || len <= 0) return;
    if (ab->len < 0 || ab->cap < 0) return;
    if (len > INT_MAX - ab->len) return;

    int needed = ab->len + len;
    if (needed > ab->cap) {
        int newcap = (ab->cap == 0) ? 256 : ab->cap;
        while (newcap < needed) {
            if (newcap > INT_MAX / 2) {
                newcap = needed;
                break;
            }
            newcap *= 2;
        }
        if (newcap < needed) return;
        char *p = realloc(ab->b, (size_t)newcap);
        if (!p) return;           /* OOM: silently drop */
        ab->b   = p;
        ab->cap = newcap;
    }
    memcpy(ab->b + ab->len, s, (size_t)len);
    ab->len = needed;
}

void ab_free(abuf *ab)
{
    free(ab->b);
    ab->b   = NULL;
    ab->len = 0;
    ab->cap = 0;
}

/* ── Status message ───────────────────────────────────────── */

void editor_set_status_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ── Prompt (used by Save-As and Find) ────────────────────── */

char *editor_prompt(const char *prompt, void (*callback)(const char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    if (!buf) return NULL;

    size_t buflen = 0;
    buf[0] = '\0';

    for (;;) {
        editor_set_status_message(prompt, buf);
        output_refresh_screen();

        int c = input_read_key();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == KEY_BACKSPACE) {
            if (buflen > 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            /* Escape: cancel */
            editor_set_status_message("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            /* Enter: confirm */
            if (buflen > 0) {
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen + 1 >= bufsize) {
                if (bufsize > SIZE_MAX / 2) {
                    editor_set_status_message("Input too long.");
                    free(buf);
                    return NULL;
                }
                bufsize *= 2;
                char *tmp = realloc(buf, bufsize);
                if (!tmp) { free(buf); return NULL; }
                buf = tmp;
            }
            buf[buflen++] = (char)c;
            buf[buflen]   = '\0';
        }

        if (callback) callback(buf, c);
    }
}

/* ── Initialisation & teardown ────────────────────────────── */

void editor_init(void)
{
    E.cx       = 0;
    E.cy       = 0;
    E.rx       = 0;
    E.rowoff   = 0;
    E.coloff   = 0;
    E.numrows  = 0;
    E.row      = NULL;
    E.dirty    = 0;
    E.filename = NULL;
    E.statusmsg[0]    = '\0';
    E.statusmsg_time  = 0;
    E.syntax   = NULL;

    undo_stack_init(&E.undo);
    undo_stack_init(&E.redo);
    E.undo_recording = 1;

    if (terminal_get_window_size(&E.screenrows, &E.screencols) == -1) {
        /* Fallback – very conservative */
        E.screenrows = 24;
        E.screencols = 80;
    }
    /* Reserve two bottom rows: status bar + message bar */
    E.screenrows -= 2;
}

void editor_cleanup(void)
{
    for (int i = 0; i < E.numrows; i++) {
        free(E.row[i].chars);
        free(E.row[i].render);
        free(E.row[i].hl);
    }
    free(E.row);
    free(E.filename);

    undo_stack_free(&E.undo);
    undo_stack_free(&E.redo);

    E.row      = NULL;
    E.filename = NULL;
    E.numrows  = 0;
}
