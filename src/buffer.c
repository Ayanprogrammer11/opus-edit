/*
 * buffer.c – Text-buffer row management and editing operations.
 */

#include "buffer.h"
#include "editor.h"
#include "output.h"
#include "undo.h"
#include "git.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Coordinate helpers ───────────────────────────────────── */

int buffer_cx_to_rx(const erow *row, int cx)
{
    if (!row) return 0;
    if (cx < 0) cx = 0;
    if (cx > row->size) cx = row->size;

    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (OPUSEDIT_TAB_STOP - 1) - (rx % OPUSEDIT_TAB_STOP);
        rx++;
    }
    return rx;
}

int buffer_rx_to_cx(const erow *row, int rx)
{
    if (!row) return 0;
    if (rx < 0) return 0;

    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (OPUSEDIT_TAB_STOP - 1) - (cur_rx % OPUSEDIT_TAB_STOP);
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

/* ── Row rendering (expand tabs) ──────────────────────────── */

void buffer_update_row(erow *row)
{
    if (!row) return;
    output_invalidate_wrap_cache();
    if (row->size < 0 || row->size > OPUSEDIT_MAX_ROW_SIZE) {
        free(row->render);
        row->render = NULL;
        row->rsize = 0;
        output_update_syntax(row);
        return;
    }

    /* Count tabs to know how much extra space we need */
    size_t tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = NULL;
    size_t render_len = (size_t)row->size + tabs * (size_t)(OPUSEDIT_TAB_STOP - 1);
    if (render_len > (size_t)INT_MAX - 1) {
        row->render = NULL;
        row->rsize = 0;
        output_update_syntax(row);
        return;
    }

    char *render = malloc(render_len + 1);
    if (!render) {
        row->rsize = 0;
        output_update_syntax(row);
        return;
    }
    row->render = render;

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % OPUSEDIT_TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    output_update_syntax(row);
}

/* ── Row insertion ────────────────────────────────────────── */

int buffer_insert_row(int at, const char *s, size_t len)
{
    if (at < 0 || at > E.numrows) return 0;
    if (E.numrows == INT_MAX) return 0;
    if (len > 0 && !s) return 0;
    if (len > (size_t)OPUSEDIT_MAX_ROW_SIZE) {
        editor_set_status_message("Line too long to insert.");
        return 0;
    }
    if (len > SIZE_MAX - 1) return 0;

    char *chars = malloc(len + 1);
    if (!chars) return 0;
    if (len > 0) memcpy(chars, s, len);
    chars[len] = '\0';

    if ((size_t)(E.numrows + 1) > SIZE_MAX / sizeof(erow)) {
        free(chars);
        return 0;
    }
    erow *newrows = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    if (!newrows) {
        free(chars);
        return 0;
    }
    E.row = newrows;

    memmove(&E.row[at + 1], &E.row[at],
            sizeof(erow) * (size_t)(E.numrows - at));

    /* Update indices for rows that shifted down */
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx = j;

    E.row[at].idx  = at;
    E.row[at].size = (int)len;

    E.row[at].chars = chars;

    E.row[at].rsize          = 0;
    E.row[at].render         = NULL;
    E.row[at].hl             = NULL;
    E.row[at].hl_open_comment = 0;
    E.row[at].hl_open_string  = 0;

    E.numrows++;
    buffer_update_row(&E.row[at]);
    editor_mark_dirty();
    git_mark_dirty();
    return 1;
}

/* ── Row deletion ─────────────────────────────────────────── */

void buffer_free_row(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
    row->render = NULL;
    row->chars  = NULL;
    row->hl     = NULL;
}

void buffer_delete_row(int at)
{
    if (at < 0 || at >= E.numrows) return;

    buffer_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1],
            sizeof(erow) * (size_t)(E.numrows - at - 1));

    /* Update indices */
    for (int j = at; j < E.numrows - 1; j++)
        E.row[j].idx = j;

    E.numrows--;
    if (at < E.numrows)
        output_update_syntax(&E.row[at]);
    editor_mark_dirty();
    git_mark_dirty();
}

/* ── Row-level character operations ───────────────────────── */

int buffer_row_insert_char(erow *row, int at, int c)
{
    if (!row) return 0;
    if (c < 0 || c > UCHAR_MAX) return 0;
    if (row->size < 0 || row->size >= OPUSEDIT_MAX_ROW_SIZE) {
        editor_set_status_message("Line too long to insert.");
        return 0;
    }
    if (at < 0 || at > row->size) at = row->size;

    char *tmp = realloc(row->chars, (size_t)(row->size + 2));
    if (!tmp) return 0;
    row->chars = tmp;

    memmove(&row->chars[at + 1], &row->chars[at],
            (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;

    buffer_update_row(row);
    editor_mark_dirty();
    git_mark_dirty();
    return 1;
}

int buffer_row_delete_char(erow *row, int at)
{
    if (!row) return 0;
    if (at < 0 || at >= row->size) return 0;

    memmove(&row->chars[at], &row->chars[at + 1],
            (size_t)(row->size - at));
    row->size--;

    buffer_update_row(row);
    editor_mark_dirty();
    git_mark_dirty();
    return 1;
}

int buffer_row_append_string(erow *row, const char *s, size_t len)
{
    if (!row) return 0;
    if (len == 0) return 1;
    if (row->size < 0 || row->size > OPUSEDIT_MAX_ROW_SIZE) return 0;
    if (len > (size_t)(OPUSEDIT_MAX_ROW_SIZE - row->size)) {
        editor_set_status_message("Line too long to insert.");
        return 0;
    }
    char *tmp = realloc(row->chars, (size_t)(row->size) + len + 1);
    if (!tmp) {
        editor_set_status_message("Out of memory.");
        return 0;
    }
    row->chars = tmp;

    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';

    buffer_update_row(row);
    editor_mark_dirty();
    git_mark_dirty();
    return 1;
}

/* ── Editor-level text operations ─────────────────────────── */

void buffer_insert_char(int c)
{
    if (E.cy == E.numrows) {
        if (!buffer_insert_row(E.numrows, "", 0))
            return;
    }
    if (E.cy < 0 || E.cy >= E.numrows) return;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
    if (buffer_row_insert_char(&E.row[E.cy], E.cx, c)) {
        undo_push(UNDO_INSERT_CHAR, E.cy, E.cx, c);
        E.cx++;
    }
}

void buffer_delete_char(void)
{
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        int deleted = row->chars[E.cx - 1];
        if (buffer_row_delete_char(row, E.cx - 1)) {
            undo_push(UNDO_DELETE_CHAR, E.cy, E.cx - 1, deleted);
            E.cx--;
        }
    } else {
        /* Merge with previous line */
        int merge_col = E.row[E.cy - 1].size;
        if (!buffer_row_append_string(&E.row[E.cy - 1],
                                      row->chars, (size_t)row->size)) {
            return;
        }
        buffer_delete_row(E.cy);
        undo_push(UNDO_DELETE_NEWLINE, E.cy - 1, merge_col, 0);
        E.cy--;
        E.cx = merge_col;
    }
}

int buffer_insert_newline(void)
{
    if (E.cy < 0) E.cy = 0;
    if (E.cy > E.numrows) E.cy = E.numrows;
    if (E.cy < E.numrows) {
        if (E.cx < 0) E.cx = 0;
        if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
    } else {
        E.cx = 0;
    }

    char *indent = NULL;
    int indent_len = 0;
    int extra_spaces = 0;

    if (E.auto_indent && E.cy >= 0 && E.cy < E.numrows) {
        erow *row = &E.row[E.cy];
        int max = row->size;
        int leading = 0;
        while (leading < max &&
               (row->chars[leading] == ' ' || row->chars[leading] == '\t')) {
            leading++;
        }
        if (E.cx < leading) leading = E.cx;
        indent_len = leading;
        if (indent_len > 0) {
            indent = malloc((size_t)indent_len);
            if (indent) {
                memcpy(indent, row->chars, (size_t)indent_len);
            } else {
                indent_len = 0;
            }
        }

        int j = E.cx - 1;
        if (j >= row->size) j = row->size - 1;
        while (j >= 0 &&
               (row->chars[j] == ' ' || row->chars[j] == '\t')) {
            j--;
        }
        if (j >= 0) {
            char ch = row->chars[j];
            if (ch == '{' || ch == '[' || ch == '(')
                extra_spaces = OPUSEDIT_TAB_STOP;
        }
    }

    if (E.cx == 0) {
        if (!buffer_insert_row(E.cy, "", 0)) {
            free(indent);
            return -1;
        }
    } else {
        erow *row = &E.row[E.cy];
        if (!buffer_insert_row(E.cy + 1,
                               &row->chars[E.cx],
                               (size_t)(row->size - E.cx))) {
            free(indent);
            return -1;
        }
        /* After insert_row, E.row may have been realloc'd */
        row = &E.row[E.cy];
        row->size  = E.cx;
        row->chars[row->size] = '\0';
        buffer_update_row(row);
    }
    undo_push(UNDO_INSERT_NEWLINE, E.cy, E.cx, 0);
    E.cy++;
    E.cx = 0;

    int indent_total = 0;
    if (E.auto_indent && E.cy < E.numrows) {
        if (indent_len > 0 && indent) {
            for (int i = 0; i < indent_len; i++) {
                buffer_insert_char(indent[i]);
                indent_total++;
            }
        }
        for (int i = 0; i < extra_spaces; i++) {
            buffer_insert_char(' ');
            indent_total++;
        }
    }

    free(indent);
    return indent_total;
}

void buffer_duplicate_line(void)
{
    if (E.numrows <= 0) {
        int saved_auto = E.auto_indent;
        E.auto_indent = 0;
        E.cy = 0;
        E.cx = 0;
        if (buffer_insert_newline() < 0) {
            E.auto_indent = saved_auto;
            editor_set_status_message("Duplicate failed: out of memory.");
            return;
        }
        E.auto_indent = saved_auto;
        E.cy = 0;
        E.cx = 0;
        editor_set_status_message("Line duplicated.");
        return;
    }

    int row_idx = E.cy;
    if (row_idx >= E.numrows) row_idx = E.numrows - 1;
    if (row_idx < 0) row_idx = 0;

    erow *row = &E.row[row_idx];
    int len = row->size;
    char *copy = malloc(len > 0 ? (size_t)len : 1);
    if (!copy) {
        editor_set_status_message("Duplicate failed: out of memory.");
        return;
    }
    if (len > 0) memcpy(copy, row->chars, (size_t)len);

    int saved_cx = E.cx;
    int saved_auto = E.auto_indent;

    E.cy = row_idx;
    E.cx = row->size;
    E.auto_indent = 0;
    if (buffer_insert_newline() < 0) {
        E.auto_indent = saved_auto;
        free(copy);
        editor_set_status_message("Duplicate failed: out of memory.");
        return;
    }
    E.auto_indent = saved_auto;

    for (int i = 0; i < len; i++) {
        buffer_insert_char(copy[i]);
    }

    if (saved_cx > len) saved_cx = len;
    E.cy = row_idx + 1;
    E.cx = saved_cx;

    free(copy);
    editor_set_status_message("Line duplicated.");
}

int buffer_trim_trailing_whitespace(void)
{
    if (E.numrows <= 0) return 0;

    int removed = 0;
    int saved_cx = E.cx;
    int saved_cy = E.cy;

    for (int row_idx = 0; row_idx < E.numrows; row_idx++) {
        erow *row = &E.row[row_idx];
        while (row->size > 0) {
            int col = row->size - 1;
            char ch = row->chars[col];
            if (ch != ' ' && ch != '\t') break;
            E.cy = row_idx;
            E.cx = col + 1;
            if (buffer_row_delete_char(row, col)) {
                undo_push(UNDO_DELETE_CHAR, row_idx, col, ch);
                removed++;
            } else {
                break;
            }
        }
    }

    E.cy = saved_cy;
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.numrows) E.cy = E.numrows ? E.numrows - 1 : 0;
    if (E.cy < E.numrows) {
        int rowlen = E.row[E.cy].size;
        if (saved_cx > rowlen) saved_cx = rowlen;
    }
    E.cx = saved_cx;

    return removed;
}

/* ── Serialise buffer to a single string ──────────────────── */

char *buffer_rows_to_string(int *buflen)
{
    size_t totlen = 0;
    *buflen = 0;
    for (int j = 0; j < E.numrows; j++) {
        if (E.row[j].size < 0) {
            *buflen = -1;
            return NULL;
        }
        size_t add = (size_t)E.row[j].size;
        if (j + 1 < E.numrows || E.ends_with_newline)
            add++; /* newline separator or final newline */
        if (totlen > SIZE_MAX - add) {
            *buflen = -1;
            return NULL;
        }
        totlen += add;
    }

    if (totlen > (size_t)INT_MAX) {
        *buflen = -1;
        return NULL;
    }

    *buflen = (int)totlen;
    char *buf = malloc(totlen ? totlen : 1);
    if (!buf) return NULL;

    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, (size_t)E.row[j].size);
        p += E.row[j].size;
        if (j + 1 < E.numrows || E.ends_with_newline) {
            *p = '\n';
            p++;
        }
    }
    return buf;
}
