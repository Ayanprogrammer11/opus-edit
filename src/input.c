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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
            if (seq[1] == '<') {
                char buf[32];
                int idx = 0;
                while (idx < (int)sizeof(buf) - 1) {
                    if (read(STDIN_FILENO, &buf[idx], 1) != 1) return '\x1b';
                    if (buf[idx] == 'm' || buf[idx] == 'M') {
                        idx++;
                        break;
                    }
                    idx++;
                }
                buf[idx] = '\0';
                int b = 0, x = 0, y = 0;
                char type = '\0';
                if (sscanf(buf, "%d;%d;%d%c", &b, &x, &y, &type) == 4) {
                    (void)x;
                    (void)y;
                    if (b == 64) return MOUSE_SCROLL_UP;
                    if (b == 65) return MOUSE_SCROLL_DOWN;
                }
                return '\x1b';
            }
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
                    if (seq[3] == '5') {
                        switch (seq[4]) {
                            case 'A': return CTRL_ARROW_UP;
                            case 'B': return CTRL_ARROW_DOWN;
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                        }
                    }
                    /* Fallback: treat as normal arrows */
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

static void editor_move_render_rows(int delta)
{
    int width = output_text_width();
    int rx = 0;
    if (E.cy < E.numrows)
        rx = buffer_cx_to_rx(&E.row[E.cy], E.cx);

    int base = (E.cy < E.numrows)
        ? output_row_render_index(E.cy)
        : output_total_render_rows();
    int cursor_render = base + (rx / width);

    int total_render = output_total_render_rows();
    int target = cursor_render + delta;
    if (target < 0) target = 0;
    if (target > total_render) target = total_render;

    int target_row = E.numrows;
    int target_line = 0;
    output_render_row_to_file(target, &target_row, &target_line);

    if (target_row >= E.numrows) {
        E.cy = E.numrows;
        E.cx = 0;
    } else {
        int target_rx = target_line * width + (rx % width);
        if (target_rx > E.row[target_row].rsize)
            target_rx = E.row[target_row].rsize;
        E.cy = target_row;
        E.cx = buffer_rx_to_cx(&E.row[target_row], target_rx);
    }
}

/* ── Selection helpers ───────────────────────────────────── */

static int selection_active(void)
{
    return E.sel_mode != SEL_NONE;
}

static void selection_start(editor_sel_mode mode)
{
    E.sel_mode = mode;
    E.sel_sx = E.cx;
    E.sel_sy = E.cy;
}

static void selection_clear(void)
{
    editor_clear_selection();
}

static void selection_normalize(int *sy, int *sx, int *ey, int *ex)
{
    *sy = E.sel_sy;
    *sx = E.sel_sx;
    *ey = E.cy;
    *ex = E.cx;
    if (*sy > *ey || (*sy == *ey && *sx > *ex)) {
        int ty = *sy;
        int tx = *sx;
        *sy = *ey;
        *sx = *ex;
        *ey = ty;
        *ex = tx;
    }
}

static int selection_get_copy_bounds(int *sy, int *sx, int *ey, int *ex,
                                     int *linewise)
{
    if (!selection_active())
        return 0;
    if (E.numrows <= 0)
        return 0;

    selection_normalize(sy, sx, ey, ex);
    if (*sy < 0) *sy = 0;
    if (*ey < 0) *ey = 0;
    if (*sy >= E.numrows) *sy = E.numrows - 1;
    if (*ey >= E.numrows) *ey = E.numrows - 1;
    *linewise = (E.sel_mode == SEL_LINE);

    if (*linewise) {
        *sx = 0;
        *ex = (*ey < E.numrows) ? E.row[*ey].size : 0;
        return 1;
    }

    if (*sy == *ey) {
        int rowlen = (*sy < E.numrows) ? E.row[*sy].size : 0;
        if (*ex < rowlen) (*ex)++;
        else *ex = rowlen;
    } else {
        int rowlen = (*ey < E.numrows) ? E.row[*ey].size : 0;
        if (*ex < rowlen) (*ex)++;
        else *ex = rowlen;
    }
    return 1;
}

/* ── Clipboard helpers ───────────────────────────────────── */

static void clipboard_clear(void)
{
    free(E.clipboard);
    E.clipboard = NULL;
    E.clipboard_len = 0;
    E.clipboard_linewise = 0;
}

static void clipboard_set(const char *data, int len, int linewise)
{
    if (!data || len <= 0) {
        clipboard_clear();
        return;
    }
    char *tmp = malloc((size_t)len + 1);
    if (!tmp) {
        editor_set_status_message("Copy failed: out of memory.");
        return;
    }
    memcpy(tmp, data, (size_t)len);
    tmp[len] = '\0';
    free(E.clipboard);
    E.clipboard = tmp;
    E.clipboard_len = len;
    E.clipboard_linewise = linewise;
}

/* ── Multi-cursor helpers ───────────────────────────────── */

typedef struct cursor_pos {
    int cx;
    int cy;
    int primary;
} cursor_pos;

static int mcursor_ensure_capacity(int needed)
{
    if (needed <= E.mcursor_capacity) return 1;
    int newcap = (E.mcursor_capacity == 0) ? 8 : E.mcursor_capacity * 2;
    while (newcap < needed) {
        if (newcap > INT_MAX / 2) {
            newcap = needed;
            break;
        }
        newcap *= 2;
    }
    if (newcap < needed) return 0;
    if ((size_t)newcap > SIZE_MAX / sizeof(editor_cursor)) return 0;
    editor_cursor *tmp = realloc(E.mcursors,
                                 (size_t)newcap * sizeof(editor_cursor));
    if (!tmp) return 0;
    E.mcursors = tmp;
    E.mcursor_capacity = newcap;
    return 1;
}

static int mcursor_index_of(int cx, int cy)
{
    for (int i = 0; i < E.mcursor_count; i++) {
        if (E.mcursors[i].cx == cx && E.mcursors[i].cy == cy)
            return i;
    }
    return -1;
}

static int mcursors_active(void)
{
    return E.mcursor_count > 0;
}

static void mcursor_add(int cx, int cy)
{
    if (cy < 0 || cy >= E.numrows) return;
    if (cx < 0) cx = 0;
    if (cx > E.row[cy].size) cx = E.row[cy].size;
    if (cx == E.cx && cy == E.cy) return;
    if (mcursor_index_of(cx, cy) >= 0) return;
    if (!mcursor_ensure_capacity(E.mcursor_count + 1)) {
        editor_set_status_message("Multi-cursor: out of memory.");
        return;
    }
    E.mcursors[E.mcursor_count].cx = cx;
    E.mcursors[E.mcursor_count].cy = cy;
    E.mcursor_count++;
}

static cursor_pos *mcursors_collect(int *out_count)
{
    int total = 1 + E.mcursor_count;
    cursor_pos *list = malloc((size_t)total * sizeof(cursor_pos));
    if (!list) return NULL;
    list[0].cx = E.cx;
    list[0].cy = E.cy;
    list[0].primary = 1;
    for (int i = 0; i < E.mcursor_count; i++) {
        list[i + 1].cx = E.mcursors[i].cx;
        list[i + 1].cy = E.mcursors[i].cy;
        list[i + 1].primary = 0;
    }
    *out_count = total;
    return list;
}

static int cursor_pos_cmp_desc(const void *a, const void *b)
{
    const cursor_pos *pa = a;
    const cursor_pos *pb = b;
    if (pa->cy != pb->cy) return (pb->cy - pa->cy);
    if (pa->cx != pb->cx) return (pb->cx - pa->cx);
    return pb->primary - pa->primary;
}

static void mcursors_normalize_from_list(cursor_pos *list, int count)
{
    int primary_idx = -1;
    for (int i = 0; i < count; i++) {
        if (list[i].primary) {
            primary_idx = i;
            break;
        }
    }
    if (primary_idx >= 0) {
        E.cx = list[primary_idx].cx;
        E.cy = list[primary_idx].cy;
    }

    E.mcursor_count = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].primary) continue;
        if (list[i].cy < 0 || list[i].cy >= E.numrows) continue;
        int cx = list[i].cx;
        if (cx < 0) cx = 0;
        if (cx > E.row[list[i].cy].size)
            cx = E.row[list[i].cy].size;
        if (cx == E.cx && list[i].cy == E.cy) continue;
        if (mcursor_index_of(cx, list[i].cy) >= 0) continue;
        if (!mcursor_ensure_capacity(E.mcursor_count + 1)) break;
        E.mcursors[E.mcursor_count].cx = cx;
        E.mcursors[E.mcursor_count].cy = list[i].cy;
        E.mcursor_count++;
    }
}

static void mcursors_add_vertical(int delta)
{
    int total = 0;
    cursor_pos *list = mcursors_collect(&total);
    if (!list) return;
    for (int i = 0; i < total; i++) {
        int row = list[i].cy + delta;
        if (row < 0 || row >= E.numrows) continue;
        int rx = buffer_cx_to_rx(&E.row[list[i].cy], list[i].cx);
        int cx = buffer_rx_to_cx(&E.row[row], rx);
        mcursor_add(cx, row);
    }
    free(list);
}

/* ── Multi-cursor edit primitives ───────────────────────── */

static void mcursors_shift_after_insert(cursor_pos *list, int count,
                                        int row, int col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row && list[i].cx >= col)
            list[i].cx++;
    }
}

static void mcursors_shift_after_delete(cursor_pos *list, int count,
                                        int row, int col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row && list[i].cx > col)
            list[i].cx--;
    }
}

static void mcursors_shift_after_newline(cursor_pos *list, int count,
                                         int row, int col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row && list[i].cx >= col) {
            list[i].cy++;
            list[i].cx -= col;
        } else if (list[i].cy > row) {
            list[i].cy++;
        }
    }
}

static void mcursors_shift_after_merge_prev(cursor_pos *list, int count,
                                            int row, int merge_col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row) {
            list[i].cy = row - 1;
            list[i].cx += merge_col;
        } else if (list[i].cy > row) {
            list[i].cy--;
        }
    }
}

static void mcursors_shift_after_merge_next(cursor_pos *list, int count,
                                            int row, int merge_col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row + 1) {
            list[i].cy = row;
            list[i].cx += merge_col;
        } else if (list[i].cy > row + 1) {
            list[i].cy--;
        }
    }
}

static void mcursors_shift_after_indent(cursor_pos *list, int count,
                                        int row, int indent)
{
    if (indent <= 0) return;
    for (int i = 0; i < count; i++) {
        if (list[i].cy == row)
            list[i].cx += indent;
    }
}

static void mcursors_apply_insert_char(int c)
{
    if (!mcursors_active()) {
        buffer_insert_char(c);
        return;
    }

    int count = 0;
    cursor_pos *list = mcursors_collect(&count);
    if (!list) return;
    qsort(list, (size_t)count, sizeof(cursor_pos), cursor_pos_cmp_desc);

    for (int i = 0; i < count; i++) {
        int row = list[i].cy;
        int col = list[i].cx;
        if (row < 0) continue;
        if (row > E.numrows) continue;
        if (row == E.numrows) {
            buffer_insert_row(E.numrows, "", 0);
        }
        if (col < 0) col = 0;
        if (col > E.row[row].size) col = E.row[row].size;

        E.cy = row;
        E.cx = col;
        undo_push(UNDO_INSERT_CHAR, row, col, c);
        buffer_row_insert_char(&E.row[row], col, c);
        mcursors_shift_after_insert(list, count, row, col);
    }

    mcursors_normalize_from_list(list, count);
    free(list);
}

static void mcursors_apply_newline(void)
{
    if (!mcursors_active()) {
        (void)buffer_insert_newline();
        return;
    }

    int count = 0;
    cursor_pos *list = mcursors_collect(&count);
    if (!list) return;
    qsort(list, (size_t)count, sizeof(cursor_pos), cursor_pos_cmp_desc);

    for (int i = 0; i < count; i++) {
        int row = list[i].cy;
        int col = list[i].cx;
        if (row < 0 || row > E.numrows) continue;
        if (row == E.numrows) col = 0;
        if (col < 0) col = 0;
        if (col > E.row[row].size) col = E.row[row].size;

        E.cy = row;
        E.cx = col;
        int indent = buffer_insert_newline();
        mcursors_shift_after_newline(list, count, row, col);
        if (indent > 0)
            mcursors_shift_after_indent(list, count, row + 1, indent);
    }

    mcursors_normalize_from_list(list, count);
    free(list);
}

static void mcursors_apply_backspace(void)
{
    if (!mcursors_active()) {
        buffer_delete_char();
        return;
    }

    int count = 0;
    cursor_pos *list = mcursors_collect(&count);
    if (!list) return;
    qsort(list, (size_t)count, sizeof(cursor_pos), cursor_pos_cmp_desc);

    for (int i = 0; i < count; i++) {
        int row = list[i].cy;
        int col = list[i].cx;
        if (row < 0 || row >= E.numrows) continue;
        if (col < 0) col = 0;
        if (col > E.row[row].size) col = E.row[row].size;

        E.cy = row;
        E.cx = col;
        if (col > 0) {
            int del_col = col - 1;
            int deleted = E.row[row].chars[del_col];
            undo_push(UNDO_DELETE_CHAR, row, del_col, deleted);
            buffer_row_delete_char(&E.row[row], del_col);
            mcursors_shift_after_delete(list, count, row, del_col);
        } else if (row > 0) {
            int merge_col = E.row[row - 1].size;
            buffer_delete_char();
            if (E.cy == row - 1) {
                mcursors_shift_after_merge_prev(list, count, row, merge_col);
            }
        }
    }

    mcursors_normalize_from_list(list, count);
    free(list);
}

static void mcursors_apply_delete(void)
{
    if (!mcursors_active()) {
        editor_move_cursor(ARROW_RIGHT);
        buffer_delete_char();
        return;
    }

    int count = 0;
    cursor_pos *list = mcursors_collect(&count);
    if (!list) return;
    qsort(list, (size_t)count, sizeof(cursor_pos), cursor_pos_cmp_desc);

    for (int i = 0; i < count; i++) {
        int row = list[i].cy;
        int col = list[i].cx;
        if (row < 0 || row >= E.numrows) continue;
        if (col < 0) col = 0;
        if (col > E.row[row].size) col = E.row[row].size;

        E.cy = row;
        E.cx = col;
        if (col < E.row[row].size) {
            int deleted = E.row[row].chars[col];
            undo_push(UNDO_DELETE_CHAR, row, col, deleted);
            buffer_row_delete_char(&E.row[row], col);
            mcursors_shift_after_delete(list, count, row, col);
        } else if (row + 1 < E.numrows) {
            int merge_col = E.row[row].size;
            if (!buffer_row_append_string(&E.row[row],
                                          E.row[row + 1].chars,
                                          (size_t)E.row[row + 1].size)) {
                continue;
            }
            undo_push(UNDO_DELETE_NEWLINE, row, merge_col, 0);
            buffer_delete_row(row + 1);
            mcursors_shift_after_merge_next(list, count, row, merge_col);
        }
    }

    mcursors_normalize_from_list(list, count);
    free(list);
}

/* ── Selection copy/cut/paste ────────────────────────────── */

static void delete_range(int sy, int sx, int ey, int ex)
{
    if (sy < 0 || sy >= E.numrows) return;
    if (ey < sy) return;

    E.cy = ey;
    E.cx = ex;
    while (E.cy > sy || E.cx > sx) {
        buffer_delete_char();
    }
}

static int selection_or_line_bounds(int *sy, int *sx, int *ey, int *ex,
                                    int *linewise)
{
    if (selection_get_copy_bounds(sy, sx, ey, ex, linewise))
        return 1;
    if (E.numrows == 0 || E.cy >= E.numrows)
        return 0;
    *sy = *ey = E.cy;
    *sx = 0;
    *ex = E.row[E.cy].size;
    *linewise = 1;
    return 1;
}

static void copy_selection_or_line(void)
{
    int sy, sx, ey, ex, linewise;
    if (!selection_or_line_bounds(&sy, &sx, &ey, &ex, &linewise)) {
        editor_set_status_message("Nothing to copy.");
        return;
    }

    size_t total = 0;
    if (linewise) {
        for (int row = sy; row <= ey; row++) {
            total += (size_t)E.row[row].size + 1;
        }
    } else if (sy == ey) {
        total += (size_t)(ex - sx);
    } else {
        total += (size_t)(E.row[sy].size - sx) + 1;
        for (int row = sy + 1; row < ey; row++)
            total += (size_t)E.row[row].size + 1;
        total += (size_t)ex;
    }

    if (total == 0) {
        editor_set_status_message("Nothing to copy.");
        selection_clear();
        return;
    }

    char *buf = malloc(total);
    if (!buf) {
        editor_set_status_message("Copy failed: out of memory.");
        selection_clear();
        return;
    }

    char *p = buf;
    if (linewise) {
        for (int row = sy; row <= ey; row++) {
            memcpy(p, E.row[row].chars, (size_t)E.row[row].size);
            p += E.row[row].size;
            *p++ = '\n';
        }
    } else if (sy == ey) {
        memcpy(p, &E.row[sy].chars[sx], (size_t)(ex - sx));
        p += ex - sx;
    } else {
        int first_len = E.row[sy].size - sx;
        memcpy(p, &E.row[sy].chars[sx], (size_t)first_len);
        p += first_len;
        *p++ = '\n';
        for (int row = sy + 1; row < ey; row++) {
            memcpy(p, E.row[row].chars, (size_t)E.row[row].size);
            p += E.row[row].size;
            *p++ = '\n';
        }
        memcpy(p, E.row[ey].chars, (size_t)ex);
        p += ex;
    }

    clipboard_set(buf, (int)total, linewise);
    free(buf);
    selection_clear();
    editor_set_status_message("Copied %d byte%s.",
                              (int)total, total == 1 ? "" : "s");
}

static void cut_selection_or_line(void)
{
    int sy, sx, ey, ex, linewise;
    if (!selection_or_line_bounds(&sy, &sx, &ey, &ex, &linewise)) {
        editor_set_status_message("Nothing to cut.");
        return;
    }

    copy_selection_or_line();

    int dsy = sy, dsx = sx, dey = ey, dex = ex;
    if (linewise) {
        dsx = 0;
        if (dey + 1 < E.numrows) {
            dey = dey + 1;
            dex = 0;
        } else {
            dex = (dey < E.numrows) ? E.row[dey].size : 0;
        }
    }

    delete_range(dsy, dsx, dey, dex);
    E.cy = sy;
    E.cx = sx;
    selection_clear();
}

static void paste_linewise(void)
{
    if (!E.clipboard || E.clipboard_len <= 0) return;

    if (E.numrows == 0 || E.cy >= E.numrows) {
        E.cy = E.numrows;
        E.cx = 0;
    }

    if (!mcursors_active()) {
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
    } else {
        for (int i = 0; i < E.mcursor_count; i++) {
            int row = E.mcursors[i].cy;
            if (row >= 0 && row < E.numrows)
                E.mcursors[i].cx = E.row[row].size;
        }
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
    }

    mcursors_apply_newline();
    for (int i = 0; i < E.clipboard_len; i++) {
        if (E.clipboard[i] == '\n') {
            if (i == E.clipboard_len - 1) break;
            mcursors_apply_newline();
        } else {
            mcursors_apply_insert_char(E.clipboard[i]);
        }
    }
}

static void paste_charwise(void)
{
    if (!E.clipboard || E.clipboard_len <= 0) return;
    for (int i = 0; i < E.clipboard_len; i++) {
        if (E.clipboard[i] == '\n')
            mcursors_apply_newline();
        else
            mcursors_apply_insert_char(E.clipboard[i]);
    }
}

static void paste_clipboard(void)
{
    if (!E.clipboard || E.clipboard_len <= 0) {
        editor_set_status_message("Clipboard empty.");
        return;
    }
    if (E.clipboard_linewise)
        paste_linewise();
    else
        paste_charwise();
}

static void delete_active_selection_only(void)
{
    if (!selection_active()) return;
    int sy, sx, ey, ex, linewise;
    if (!selection_get_copy_bounds(&sy, &sx, &ey, &ex, &linewise))
        return;

    int dsy = sy, dsx = sx, dey = ey, dex = ex;
    if (linewise) {
        dsx = 0;
        if (dey + 1 < E.numrows) {
            dey = dey + 1;
            dex = 0;
        } else {
            dex = (dey < E.numrows) ? E.row[dey].size : 0;
        }
    }

    delete_range(dsy, dsx, dey, dex);
    E.cy = sy;
    E.cx = sx;
    selection_clear();
}
/* ── Command dispatch ─────────────────────────────────────── */

void input_process_keypress(void)
{
    static int quit_times = OPUSEDIT_QUIT_TIMES;
    static int close_times = OPUSEDIT_QUIT_TIMES;
    int c = input_read_key();
    int handled = 0;

    switch (c) {
        /* ── Quit (Ctrl-Q) ─────────────────────────────────── */
        case CTRL_KEY('q'):
            if (editor_any_buffer_dirty() && quit_times > 0) {
                editor_set_status_message(
                    "WARNING! Unsaved changes in buffers. "
                    "Press Ctrl-Q %d more time(s) to quit.",
                    quit_times);
                quit_times--;
                close_times = OPUSEDIT_QUIT_TIMES;
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
            handled = 1;
            break;

        /* ── New buffer (Ctrl-N) ───────────────────────────── */
        case CTRL_KEY('n'):
            editor_buffer_new();
            handled = 1;
            break;

        /* ── Open file in new buffer (Ctrl-O) ──────────────── */
        case CTRL_KEY('o'):
            editor_buffer_open_prompt();
            handled = 1;
            break;

        /* ── Find (Ctrl-F) ─────────────────────────────────── */
        case CTRL_KEY('f'):
            find();
            handled = 1;
            break;

        /* ── Replace (Ctrl-R) ─────────────────────────────── */
        case CTRL_KEY('r'):
            find_replace();
            handled = 1;
            break;

        /* ── Next buffer (Ctrl-B) ─────────────────────────── */
        case CTRL_KEY('b'):
            editor_buffer_next();
            handled = 1;
            break;

        /* ── Previous buffer (Ctrl-P) ─────────────────────── */
        case CTRL_KEY('p'):
            editor_buffer_prev();
            handled = 1;
            break;

        /* ── Close buffer (Ctrl-W) ────────────────────────── */
        case CTRL_KEY('w'):
            if (E.dirty && close_times > 0) {
                editor_set_status_message(
                    "WARNING! Buffer has unsaved changes. "
                    "Press Ctrl-W %d more time(s) to close.",
                    close_times);
                close_times--;
                quit_times = OPUSEDIT_QUIT_TIMES;
                return;
            }
            editor_buffer_close();
            handled = 1;
            break;

        /* ── Undo (Ctrl-Z) ─────────────────────────────────── */
        case CTRL_KEY('z'):
            undo_perform_undo();
            handled = 1;
            break;

        /* ── Redo (Ctrl-Y) ─────────────────────────────────── */
        case CTRL_KEY('y'):
            undo_perform_redo();
            handled = 1;
            break;

        /* ── Mouse wheel scrolling ────────────────────────── */
        case MOUSE_SCROLL_UP:
            editor_move_render_rows(-3);
            handled = 1;
            break;
        case MOUSE_SCROLL_DOWN:
            editor_move_render_rows(3);
            handled = 1;
            break;

        /* ── Go to line (Ctrl-G) ──────────────────────────── */
        case CTRL_KEY('g'):
            editor_goto_line_prompt();
            handled = 1;
            break;

        /* ── Duplicate line (Ctrl-D) ──────────────────────── */
        case CTRL_KEY('d'):
            selection_clear();
            editor_clear_mcursors();
            buffer_duplicate_line();
            handled = 1;
            break;

        /* ── Copy / Cut / Paste (Ctrl-C/X/V) ───────────────── */
        case CTRL_KEY('c'):
            copy_selection_or_line();
            handled = 1;
            break;
        case CTRL_KEY('x'):
            cut_selection_or_line();
            handled = 1;
            break;
        case CTRL_KEY('v'):
            if (selection_active())
                delete_active_selection_only();
            paste_clipboard();
            handled = 1;
            break;

        /* ── Toggle line numbers (Ctrl-L) ─────────────────── */
        case CTRL_KEY('l'):
            E.show_line_numbers = !E.show_line_numbers;
            editor_set_status_message("Line numbers %s.",
                                      E.show_line_numbers ? "on" : "off");
            handled = 1;
            break;
    }

    if (handled) {
        quit_times = OPUSEDIT_QUIT_TIMES;
        close_times = OPUSEDIT_QUIT_TIMES;
        return;
    }

    if (c == CTRL_ARROW_UP) {
        selection_clear();
        mcursors_add_vertical(-1);
        quit_times = OPUSEDIT_QUIT_TIMES;
        close_times = OPUSEDIT_QUIT_TIMES;
        return;
    } else if (c == CTRL_ARROW_DOWN) {
        selection_clear();
        mcursors_add_vertical(1);
        quit_times = OPUSEDIT_QUIT_TIMES;
        close_times = OPUSEDIT_QUIT_TIMES;
        return;
    }

    switch (E.mode) {
        case MODE_INSERT:
            if (c == '\x1b') {
                E.mode = MODE_NORMAL;
                selection_clear();
                editor_clear_mcursors();
                break;
            }
            switch (c) {
                /* ── Enter ─────────────────────────────────── */
                case '\r':
                    mcursors_apply_newline();
                    break;

                /* ── Home / End ────────────────────────────── */
                case HOME_KEY:
                    E.cx = 0;
                    break;

                case END_KEY:
                    if (E.cy < E.numrows)
                        E.cx = E.row[E.cy].size;
                    break;

                /* ── Backspace / Ctrl-H / Delete ───────────── */
                case KEY_BACKSPACE:
                case CTRL_KEY('h'):
                    mcursors_apply_backspace();
                    break;

                case DEL_KEY:
                    mcursors_apply_delete();
                    break;

                /* ── Page Up / Down ───────────────────────── */
                case PAGE_UP:
                case PAGE_DOWN:
                    editor_move_render_rows(
                        c == PAGE_UP ? -E.screenrows : E.screenrows);
                    break;

                /* ── Arrow keys ────────────────────────────── */
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                    editor_move_cursor(c);
                    break;

                /* ── Escape – ignore ───────────────────────── */
                case '\x1b':
                    break;

                /* ── Default: insert character ─────────────── */
                default:
                    mcursors_apply_insert_char(c);
                    break;
            }
            break;

        case MODE_NORMAL:
            switch (c) {
                case 'i':
                    selection_clear();
                    E.mode = MODE_INSERT;
                    break;

                case ':':
                    editor_command_prompt();
                    break;

                case 'v':
                    if (selection_active() && E.sel_mode == SEL_CHAR) {
                        selection_clear();
                    } else {
                        editor_clear_mcursors();
                        selection_start(SEL_CHAR);
                    }
                    break;

                case 'V':
                    if (selection_active() && E.sel_mode == SEL_LINE) {
                        selection_clear();
                    } else {
                        editor_clear_mcursors();
                        selection_start(SEL_LINE);
                    }
                    break;

                case 'y':
                    copy_selection_or_line();
                    break;

                case 'd':
                    cut_selection_or_line();
                    break;

                case 'p':
                    if (selection_active())
                        delete_active_selection_only();
                    paste_clipboard();
                    break;

                case '\r':
                    editor_move_cursor(ARROW_DOWN);
                    break;

                case HOME_KEY:
                    E.cx = 0;
                    break;

                case END_KEY:
                    if (E.cy < E.numrows)
                        E.cx = E.row[E.cy].size;
                    break;

                case PAGE_UP:
                case PAGE_DOWN:
                    editor_move_render_rows(
                        c == PAGE_UP ? -E.screenrows : E.screenrows);
                    break;

                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                    editor_move_cursor(c);
                    break;

                case '\x1b':
                    selection_clear();
                    editor_clear_mcursors();
                default:
                    break;
            }
            break;

        case MODE_COMMAND:
        default:
            E.mode = MODE_NORMAL;
            break;
    }

    quit_times = OPUSEDIT_QUIT_TIMES;
    close_times = OPUSEDIT_QUIT_TIMES;
}
