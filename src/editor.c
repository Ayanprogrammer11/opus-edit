/*
 * editor.c – Shared editor state, utilities, and lifecycle.
 */

#include "editor.h"
#include "terminal.h"
#include "input.h"
#include "output.h"
#include "undo.h"
#include "buffer.h"
#include "file_io.h"
#include "find.h"
#include "git.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
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

int editor_any_buffer_dirty(void)
{
    if (E.dirty) return 1;
    for (int i = 0; i < E.buffer_count; i++) {
        if (i == E.current_buffer) continue;
        if (E.buffers[i].dirty) return 1;
    }
    return 0;
}

void editor_clear_selection(void)
{
    E.sel_mode = SEL_NONE;
    E.sel_sx = 0;
    E.sel_sy = 0;
}

void editor_clear_mcursors(void)
{
    E.mcursor_count = 0;
}

/* ── Prompt (used by Save-As, Find, Replace) ──────────────── */

static char *editor_prompt_internal(const char *prompt,
                                    void (*callback)(const char *, int),
                                    int allow_empty)
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
            if (allow_empty || buflen > 0) {
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (c < 128 && !iscntrl((unsigned char)c)) {
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

char *editor_prompt(const char *prompt, void (*callback)(const char *, int))
{
    return editor_prompt_internal(prompt, callback, 0);
}

char *editor_prompt_allow_empty(const char *prompt,
                                void (*callback)(const char *, int))
{
    return editor_prompt_internal(prompt, callback, 1);
}

/* ── Go to line ──────────────────────────────────────────── */

static int editor_parse_line_number(const char *arg, int *out_line)
{
    if (!arg) return 0;
    while (*arg && isspace((unsigned char)*arg)) arg++;
    if (*arg == '\0') return 0;

    char *end = NULL;
    long val = strtol(arg, &end, 10);
    if (end == arg) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;

    if (val < 1) val = 1;
    if (val > INT_MAX) val = INT_MAX;
    *out_line = (int)val;
    return 1;
}

static int editor_goto_line(int line)
{
    if (E.numrows <= 0) {
        E.cy = 0;
        E.cx = 0;
        editor_clear_selection();
        editor_clear_mcursors();
        return 0;
    }
    if (line < 1) line = 1;
    if (line > E.numrows) line = E.numrows;
    E.cy = line - 1;
    E.cx = 0;
    editor_clear_selection();
    editor_clear_mcursors();
    return line;
}

void editor_goto_line_prompt(void)
{
    char *line = editor_prompt("Go to line: %s (ESC to cancel)", NULL);
    if (!line) return;

    int target = 0;
    if (!editor_parse_line_number(line, &target)) {
        editor_set_status_message("Invalid line number.");
        free(line);
        return;
    }
    int actual = editor_goto_line(target);
    if (actual == 0) {
        editor_set_status_message("Buffer is empty.");
    } else {
        editor_set_status_message("Line %d of %d.", actual, E.numrows);
    }
    free(line);
}

/* ── Command prompt ───────────────────────────────────────── */

static void editor_quit_now(void)
{
    terminal_write_all("\x1b[2J", 4);
    terminal_write_all("\x1b[H",  3);
    editor_cleanup();
    exit(0);
}

static char *trim_whitespace(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

void editor_command_prompt(void)
{
    E.mode = MODE_COMMAND;
    char *line = editor_prompt(":%s", NULL);
    E.mode = MODE_NORMAL;
    if (!line) return;

    char *cmdline = trim_whitespace(line);
    if (*cmdline == '\0') {
        free(line);
        return;
    }

    char *arg = cmdline;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    if (*arg) {
        *arg = '\0';
        arg++;
        arg = trim_whitespace(arg);
        if (*arg == '\0') arg = NULL;
    } else {
        arg = NULL;
    }

    int force = 0;
    size_t cmdlen = strlen(cmdline);
    if (cmdlen > 0 && cmdline[cmdlen - 1] == '!') {
        force = 1;
        cmdline[cmdlen - 1] = '\0';
    }

    if (strcasecmp(cmdline, "w") == 0) {
        if (arg && *arg)
            file_save_as(arg);
        else
            file_save();
    } else if (strcasecmp(cmdline, "q") == 0) {
        if (force || !editor_any_buffer_dirty()) {
            free(line);
            editor_quit_now();
        } else {
            editor_set_status_message("Unsaved changes. Use :q! to quit.");
        }
    } else if (strcasecmp(cmdline, "wq") == 0) {
        if (arg && *arg)
            file_save_as(arg);
        else
            file_save();
        if (!editor_any_buffer_dirty()) {
            free(line);
            editor_quit_now();
        } else {
            editor_set_status_message(
                "Unsaved changes remain. Use :q! to quit.");
        }
    } else if (strcasecmp(cmdline, "e") == 0) {
        if (!arg || !*arg) {
            editor_set_status_message("Usage: :e <path>");
        } else if (!force && E.dirty) {
            editor_set_status_message(
                "Buffer has unsaved changes. Use :e! to discard.");
        } else {
            file_open(arg);
        }
    } else if (strcasecmp(cmdline, "bnext") == 0 ||
               strcasecmp(cmdline, "bn") == 0) {
        editor_buffer_next();
    } else if (strcasecmp(cmdline, "bprev") == 0 ||
               strcasecmp(cmdline, "bp") == 0) {
        editor_buffer_prev();
    } else if (strcasecmp(cmdline, "bd") == 0) {
        if (!force && E.dirty) {
            editor_set_status_message(
                "Buffer has unsaved changes. Use :bd! to close.");
        } else {
            editor_buffer_close();
        }
    } else if (strcasecmp(cmdline, "help") == 0) {
        editor_set_status_message(
            "NORMAL: i insert, : command | INSERT: Esc normal | "
            ":w :q :wq :e :bnext :bprev :bd :find :replace :goto "
            ":set :trim :dup");
    } else if (strcasecmp(cmdline, "find") == 0) {
        find();
    } else if (strcasecmp(cmdline, "replace") == 0) {
        find_replace();
    } else if (strcasecmp(cmdline, "goto") == 0 ||
               strcasecmp(cmdline, "line") == 0) {
        if (arg && *arg) {
            int target = 0;
            if (!editor_parse_line_number(arg, &target)) {
                editor_set_status_message("Invalid line number.");
            } else {
                int actual = editor_goto_line(target);
                if (actual == 0)
                    editor_set_status_message("Buffer is empty.");
                else
                    editor_set_status_message("Line %d of %d.",
                                              actual, E.numrows);
            }
        } else {
            editor_goto_line_prompt();
        }
    } else if (strcasecmp(cmdline, "set") == 0) {
        if (!arg || !*arg) {
            editor_set_status_message(
                "Usage: :set number|nonumber|autoindent|noautoindent|"
                "gitgutter|nogitgutter");
        } else if (strcasecmp(arg, "number") == 0 ||
                   strcasecmp(arg, "nu") == 0) {
            E.show_line_numbers = 1;
            editor_set_status_message("Line numbers on.");
        } else if (strcasecmp(arg, "nonumber") == 0 ||
                   strcasecmp(arg, "nonu") == 0) {
            E.show_line_numbers = 0;
            editor_set_status_message("Line numbers off.");
        } else if (strcasecmp(arg, "autoindent") == 0 ||
                   strcasecmp(arg, "ai") == 0) {
            E.auto_indent = 1;
            editor_set_status_message("Auto-indent on.");
        } else if (strcasecmp(arg, "noautoindent") == 0 ||
                   strcasecmp(arg, "noai") == 0) {
            E.auto_indent = 0;
            editor_set_status_message("Auto-indent off.");
        } else if (strcasecmp(arg, "gitgutter") == 0 ||
                   strcasecmp(arg, "gg") == 0) {
            E.show_git_gutter = 1;
            editor_set_status_message("Git gutter on.");
        } else if (strcasecmp(arg, "nogitgutter") == 0 ||
                   strcasecmp(arg, "nogg") == 0) {
            E.show_git_gutter = 0;
            editor_set_status_message("Git gutter off.");
        } else {
            editor_set_status_message("Unknown option: %s", arg);
        }
    } else if (strcasecmp(cmdline, "trim") == 0) {
        editor_clear_selection();
        editor_clear_mcursors();
        int removed = buffer_trim_trailing_whitespace();
        if (removed == 0) {
            editor_set_status_message("No trailing whitespace to trim.");
        } else {
            editor_set_status_message("Trimmed %d trailing whitespace char%s.",
                                      removed, removed == 1 ? "" : "s");
        }
    } else if (strcasecmp(cmdline, "dup") == 0 ||
               strcasecmp(cmdline, "duplicate") == 0) {
        editor_clear_selection();
        editor_clear_mcursors();
        buffer_duplicate_line();
    } else {
        editor_set_status_message("Unknown command: %s", cmdline);
    }

    free(line);
}

/* ── Buffer management ───────────────────────────────────── */

static void editor_buffer_init(editor_buffer *buf)
{
    if (!buf) return;
    buf->cx = 0;
    buf->cy = 0;
    buf->rx = 0;
    buf->rowoff = 0;
    buf->coloff = 0;
    buf->numrows = 0;
    buf->row = NULL;
    buf->dirty = 0;
    buf->filename = NULL;
    buf->syntax = NULL;
    buf->show_git_gutter = 1;
    buf->git_available = 0;
    buf->git_tracked = 0;
    buf->git_signs_dirty = 1;
    buf->git_root = NULL;
    buf->git_gitdir = NULL;
    buf->git_relpath = NULL;
    buf->git_base_lines = NULL;
    buf->git_base_count = 0;
    buf->git_signs = NULL;
    buf->git_signs_count = 0;
    undo_stack_init(&buf->undo);
    undo_stack_init(&buf->redo);
    buf->undo_recording = 1;
}

static void editor_buffer_free(editor_buffer *buf)
{
    if (!buf) return;
    for (int i = 0; i < buf->numrows; i++) {
        buffer_free_row(&buf->row[i]);
    }
    free(buf->row);
    free(buf->filename);
    free(buf->git_root);
    free(buf->git_gitdir);
    free(buf->git_relpath);
    if (buf->git_base_lines) {
        for (int i = 0; i < buf->git_base_count; i++)
            free(buf->git_base_lines[i]);
        free(buf->git_base_lines);
    }
    free(buf->git_signs);
    undo_stack_free(&buf->undo);
    undo_stack_free(&buf->redo);

    buf->row = NULL;
    buf->filename = NULL;
    buf->numrows = 0;
    buf->dirty = 0;
    buf->git_root = NULL;
    buf->git_gitdir = NULL;
    buf->git_relpath = NULL;
    buf->git_base_lines = NULL;
    buf->git_base_count = 0;
    buf->git_signs = NULL;
    buf->git_signs_count = 0;
}

static void editor_buffer_snapshot(editor_buffer *buf)
{
    if (!buf) return;
    buf->cx = E.cx;
    buf->cy = E.cy;
    buf->rx = E.rx;
    buf->rowoff = E.rowoff;
    buf->coloff = E.coloff;
    buf->numrows = E.numrows;
    buf->row = E.row;
    buf->dirty = E.dirty;
    buf->filename = E.filename;
    buf->syntax = E.syntax;
    buf->show_git_gutter = E.show_git_gutter;
    buf->git_available = E.git_available;
    buf->git_tracked = E.git_tracked;
    buf->git_signs_dirty = E.git_signs_dirty;
    buf->git_root = E.git_root;
    buf->git_gitdir = E.git_gitdir;
    buf->git_relpath = E.git_relpath;
    buf->git_base_lines = E.git_base_lines;
    buf->git_base_count = E.git_base_count;
    buf->git_signs = E.git_signs;
    buf->git_signs_count = E.git_signs_count;
    buf->undo = E.undo;
    buf->redo = E.redo;
    buf->undo_recording = E.undo_recording;
}

static void editor_buffer_restore(editor_buffer *buf)
{
    if (!buf) return;
    E.cx = buf->cx;
    E.cy = buf->cy;
    E.rx = buf->rx;
    E.rowoff = buf->rowoff;
    E.coloff = buf->coloff;
    E.numrows = buf->numrows;
    E.row = buf->row;
    E.dirty = buf->dirty;
    E.filename = buf->filename;
    E.syntax = buf->syntax;
    E.show_git_gutter = buf->show_git_gutter;
    E.git_available = buf->git_available;
    E.git_tracked = buf->git_tracked;
    E.git_signs_dirty = buf->git_signs_dirty;
    E.git_root = buf->git_root;
    E.git_gitdir = buf->git_gitdir;
    E.git_relpath = buf->git_relpath;
    E.git_base_lines = buf->git_base_lines;
    E.git_base_count = buf->git_base_count;
    E.git_signs = buf->git_signs;
    E.git_signs_count = buf->git_signs_count;
    E.undo = buf->undo;
    E.redo = buf->redo;
    E.undo_recording = buf->undo_recording;

    if (E.cy >= E.numrows)
        E.cy = E.numrows ? E.numrows - 1 : 0;
    if (E.cy < 0) E.cy = 0;
    if (E.cx < 0) E.cx = 0;
    if (E.cy < E.numrows && E.cx > E.row[E.cy].size)
        E.cx = E.row[E.cy].size;
}

static int editor_buffer_ensure_capacity(int needed)
{
    if (needed <= E.buffer_capacity) return 1;
    int newcap = (E.buffer_capacity == 0) ? 4 : E.buffer_capacity * 2;
    while (newcap < needed) {
        if (newcap > INT_MAX / 2) {
            newcap = needed;
            break;
        }
        newcap *= 2;
    }
    if (newcap < needed) return 0;
    if ((size_t)newcap > SIZE_MAX / sizeof(editor_buffer)) return 0;
    editor_buffer *tmp = realloc(E.buffers, (size_t)newcap * sizeof(editor_buffer));
    if (!tmp) return 0;
    E.buffers = tmp;
    E.buffer_capacity = newcap;
    return 1;
}

static int editor_buffer_create(void)
{
    if (!editor_buffer_ensure_capacity(E.buffer_count + 1))
        return -1;
    int idx = E.buffer_count++;
    editor_buffer_init(&E.buffers[idx]);
    return idx;
}

static void editor_buffer_switch_to(int idx)
{
    if (idx < 0 || idx >= E.buffer_count) return;
    if (idx == E.current_buffer) return;

    editor_buffer_snapshot(&E.buffers[E.current_buffer]);
    E.current_buffer = idx;
    editor_buffer_restore(&E.buffers[idx]);
    editor_clear_selection();
    editor_clear_mcursors();
}

int editor_buffer_count(void)
{
    return E.buffer_count;
}

int editor_buffer_index(void)
{
    return E.current_buffer;
}

void editor_buffer_new(void)
{
    int idx = editor_buffer_create();
    if (idx < 0) {
        editor_set_status_message("New buffer failed: out of memory.");
        return;
    }
    editor_buffer_switch_to(idx);
    editor_set_status_message("New buffer (%d/%d).",
                              E.current_buffer + 1, E.buffer_count);
}

void editor_buffer_open_prompt(void)
{
    char *filename = editor_prompt("Open: %s (ESC to cancel)", NULL);
    if (!filename) return;

    int idx = editor_buffer_create();
    if (idx < 0) {
        editor_set_status_message("Open failed: out of memory.");
        free(filename);
        return;
    }
    editor_buffer_switch_to(idx);
    if (!file_open(filename)) {
        char status[sizeof(E.statusmsg)];
        snprintf(status, sizeof(status), "%s", E.statusmsg);
        editor_buffer_close();
        editor_set_status_message("%s", status);
    }
    free(filename);
}

void editor_buffer_next(void)
{
    if (E.buffer_count <= 1) {
        editor_set_status_message("No other buffers.");
        return;
    }
    int next = E.current_buffer + 1;
    if (next >= E.buffer_count) next = 0;
    editor_buffer_switch_to(next);
    editor_set_status_message("Buffer %d/%d.",
                              E.current_buffer + 1, E.buffer_count);
}

void editor_buffer_prev(void)
{
    if (E.buffer_count <= 1) {
        editor_set_status_message("No other buffers.");
        return;
    }
    int prev = E.current_buffer - 1;
    if (prev < 0) prev = E.buffer_count - 1;
    editor_buffer_switch_to(prev);
    editor_set_status_message("Buffer %d/%d.",
                              E.current_buffer + 1, E.buffer_count);
}

void editor_buffer_close(void)
{
    if (E.buffer_count <= 0) return;

    if (E.buffer_count == 1) {
        editor_buffer_snapshot(&E.buffers[E.current_buffer]);
        editor_buffer_free(&E.buffers[0]);
        editor_buffer_init(&E.buffers[0]);
        editor_buffer_restore(&E.buffers[0]);
        editor_set_status_message("Buffer cleared.");
        return;
    }

    editor_buffer_snapshot(&E.buffers[E.current_buffer]);
    editor_buffer_free(&E.buffers[E.current_buffer]);

    int tail = E.buffer_count - E.current_buffer - 1;
    if (tail > 0) {
        memmove(&E.buffers[E.current_buffer],
                &E.buffers[E.current_buffer + 1],
                (size_t)tail * sizeof(editor_buffer));
    }
    E.buffer_count--;
    if (E.current_buffer >= E.buffer_count)
        E.current_buffer = E.buffer_count - 1;

    editor_buffer_restore(&E.buffers[E.current_buffer]);
    editor_set_status_message("Closed buffer. (%d/%d).",
                              E.current_buffer + 1, E.buffer_count);
}

/* ── Initialisation & teardown ────────────────────────────── */

void editor_init(void)
{
    E.statusmsg[0]    = '\0';
    E.statusmsg_time  = 0;
    E.mode = MODE_INSERT;
    E.sel_mode = SEL_NONE;
    E.sel_sx = 0;
    E.sel_sy = 0;
    E.show_line_numbers = 1;
    E.auto_indent = 1;
    E.show_git_gutter = 1;
    E.git_available = 0;
    E.git_tracked = 0;
    E.git_signs_dirty = 1;
    E.git_root = NULL;
    E.git_gitdir = NULL;
    E.git_relpath = NULL;
    E.git_base_lines = NULL;
    E.git_base_count = 0;
    E.git_signs = NULL;
    E.git_signs_count = 0;
    E.clipboard = NULL;
    E.clipboard_len = 0;
    E.clipboard_linewise = 0;
    E.mcursors = NULL;
    E.mcursor_count = 0;
    E.mcursor_capacity = 0;

    E.buffers = NULL;
    E.buffer_count = 0;
    E.buffer_capacity = 0;
    E.current_buffer = 0;

    if (terminal_get_window_size(&E.screenrows, &E.screencols) == -1) {
        /* Fallback – very conservative */
        E.screenrows = 24;
        E.screencols = 80;
    }
    /* Reserve two bottom rows: status bar + message bar */
    E.screenrows -= 2;

    int idx = editor_buffer_create();
    if (idx < 0) {
        /* OOM fallback: init transient empty buffer in-place */
        E.cx = 0;
        E.cy = 0;
        E.rx = 0;
        E.rowoff = 0;
        E.coloff = 0;
        E.numrows = 0;
        E.row = NULL;
        E.dirty = 0;
        E.filename = NULL;
        E.syntax = NULL;
        E.show_git_gutter = 1;
        E.git_available = 0;
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        E.git_root = NULL;
        E.git_gitdir = NULL;
        E.git_relpath = NULL;
        E.git_base_lines = NULL;
        E.git_base_count = 0;
        E.git_signs = NULL;
        E.git_signs_count = 0;
        undo_stack_init(&E.undo);
        undo_stack_init(&E.redo);
        E.undo_recording = 1;
        return;
    }
    E.current_buffer = idx;
    editor_buffer_restore(&E.buffers[idx]);
}

void editor_cleanup(void)
{
    if (E.buffer_count > 0) {
        editor_buffer_snapshot(&E.buffers[E.current_buffer]);
    }
    for (int i = 0; i < E.buffer_count; i++) {
        editor_buffer_free(&E.buffers[i]);
    }
    free(E.buffers);
    E.buffers = NULL;
    E.buffer_count = 0;
    E.buffer_capacity = 0;
    E.current_buffer = 0;

    E.row = NULL;
    E.filename = NULL;
    E.numrows = 0;
    free(E.clipboard);
    E.clipboard = NULL;
    E.clipboard_len = 0;
    E.clipboard_linewise = 0;
    free(E.mcursors);
    E.mcursors = NULL;
    E.mcursor_count = 0;
    E.mcursor_capacity = 0;
}
