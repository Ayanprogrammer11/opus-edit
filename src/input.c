/*
 * input.c – Keypress reading, escape-sequence parsing, command dispatch.
 */

#include "input.h"
#include "editor.h"
#include "buffer.h"
#include "output.h"
#include "terminal.h"
#include "find.h"
#include "file_io.h"
#include "undo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── Low-level key reading ────────────────────────────────── */

int input_read_key(void)
{
    int nread;
    char c;

    for (;;) {
        if (terminal_exit_requested()) {
            editor_cleanup();
            exit(0);
        }

        nread = (int)read(STDIN_FILENO, &c, 1);
        if (nread == 1) {
            if (terminal_exit_requested()) {
                editor_cleanup();
                exit(0);
            }
            break;
        }

        if (nread == 0 || (nread == -1 && (errno == EAGAIN || errno == EINTR))) {
            if (terminal_apply_pending_resize()) {
                output_refresh_screen();
            }
            continue;
        }

        if (nread == -1) {
            /* Fatal read error */
            perror("read");
            exit(1);
        }
    }

    /* Escape sequence handling */
    if (c == '\x1b') {
        char seq[5];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                if (seq[2] == '~') {
                    /* \x1b[N~ sequences */
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[2] == ';') {
                    /* Modified keys: \x1b[1;5C etc. */
                    if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b';
                    /* We consume but treat as normal arrows for simplicity */
                    switch (seq[4]) {
                        case 'A': return ARROW_UP;
                        case 'B': return ARROW_DOWN;
                        case 'C': return ARROW_RIGHT;
                        case 'D': return ARROW_LEFT;
                    }
                }
            } else {
                /* \x1b[X single-char sequences */
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            /* \x1bOX sequences (some terminals) */
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }

    return (int)(unsigned char)c;
}

/* ── Cursor movement ──────────────────────────────────────── */

static void editor_move_cursor(int key)
{
    erow *row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;

    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                /* Wrap to end of previous line */
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;

        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                /* Wrap to start of next line */
                E.cy++;
                E.cx = 0;
            }
            break;

        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            break;

        case ARROW_DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
    }

    /* Snap cx to the end of the new row */
    row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* ── Command dispatch ─────────────────────────────────────── */

void input_process_keypress(void)
{
    static int quit_times = OPUSEDIT_QUIT_TIMES;
    int c = input_read_key();

    switch (c) {
        /* ── Enter ─────────────────────────────────────────── */
        case '\r':
            buffer_insert_newline();
            break;

        /* ── Quit (Ctrl-Q) ─────────────────────────────────── */
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editor_set_status_message(
                    "WARNING! File has unsaved changes. "
                    "Press Ctrl-Q %d more time(s) to quit.",
                    quit_times);
                quit_times--;
                return;
            }
            /* Clear screen and reposition cursor before exit */
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H",  3);
            editor_cleanup();
            exit(0);
            break;

        /* ── Save (Ctrl-S) ─────────────────────────────────── */
        case CTRL_KEY('s'):
            file_save();
            break;

        /* ── Find (Ctrl-F) ─────────────────────────────────── */
        case CTRL_KEY('f'):
            find();
            break;

        /* ── Undo (Ctrl-Z) ─────────────────────────────────── */
        case CTRL_KEY('z'):
            undo_perform_undo();
            break;

        /* ── Redo (Ctrl-Y) ─────────────────────────────────── */
        case CTRL_KEY('y'):
            undo_perform_redo();
            break;

        /* ── Home / End ────────────────────────────────────── */
        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        /* ── Backspace / Ctrl-H / Delete ───────────────────── */
        case KEY_BACKSPACE:
        case CTRL_KEY('h'):
            buffer_delete_char();
            break;

        case DEL_KEY:
            editor_move_cursor(ARROW_RIGHT);
            buffer_delete_char();
            break;

        /* ── Page Up / Down ────────────────────────────────── */
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }
                int times = E.screenrows;
                while (times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        /* ── Arrow keys ────────────────────────────────────── */
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;

        /* ── Refresh (Ctrl-L) / Escape – ignore ────────────── */
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        /* ── Default: insert character ─────────────────────── */
        default:
            buffer_insert_char(c);
            break;
    }

    quit_times = OPUSEDIT_QUIT_TIMES;
}
