/*
 * buffer.c – Text-buffer row management and editing operations.
 */

#include "buffer.h"
#include "editor.h"
#include "output.h"
#include "undo.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Coordinate helpers ───────────────────────────────────── */

int buffer_cx_to_rx(const erow *row, int cx)
{
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
    size_t render_len = (size_t)row->size + tabs * (size_t)(OPUSEDIT_TAB_STOP - 1);
    if (render_len > (size_t)INT_MAX - 1) {
        row->render = NULL;
        row->rsize = 0;
        output_update_syntax(row);
        return;
    }

    row->render = malloc(render_len + 1);
    if (!row->render) {
        row->rsize = 0;
        return;
    }

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

void buffer_insert_row(int at, const char *s, size_t len)
{
    if (at < 0 || at > E.numrows) return;
    if (len > (size_t)OPUSEDIT_MAX_ROW_SIZE) {
        editor_set_status_message("Line too long to insert.");
        return;
    }
    if (len > SIZE_MAX - 1) return;

    char *chars = malloc(len + 1);
    if (!chars) return;
    memcpy(chars, s, len);
    chars[len] = '\0';

    erow *newrows = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    if (!newrows) {
        free(chars);
        return;
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

    buffer_update_row(&E.row[at]);
    E.numrows++;
    E.dirty++;
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
    E.dirty++;
}

/* ── Row-level character operations ───────────────────────── */

void buffer_row_insert_char(erow *row, int at, int c)
{
    if (!row) return;
    if (row->size < 0 || row->size >= OPUSEDIT_MAX_ROW_SIZE) {
        editor_set_status_message("Line too long to insert.");
        return;
    }
    if (at < 0 || at > row->size) at = row->size;

    char *tmp = realloc(row->chars, (size_t)(row->size + 2));
    if (!tmp) return;
    row->chars = tmp;

    memmove(&row->chars[at + 1], &row->chars[at],
            (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;

    buffer_update_row(row);
    E.dirty++;
}

void buffer_row_delete_char(erow *row, int at)
{
    if (!row) return;
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1],
            (size_t)(row->size - at));
    row->size--;

    buffer_update_row(row);
    E.dirty++;
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
    E.dirty++;
    return 1;
}

/* ── Editor-level text operations ─────────────────────────── */

void buffer_insert_char(int c)
{
    if (E.cy == E.numrows) {
        buffer_insert_row(E.numrows, "", 0);
    }
    undo_push(UNDO_INSERT_CHAR, E.cy, E.cx, c);
    buffer_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void buffer_delete_char(void)
{
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        int deleted = row->chars[E.cx - 1];
        undo_push(UNDO_DELETE_CHAR, E.cy, E.cx - 1, deleted);
        buffer_row_delete_char(row, E.cx - 1);
        E.cx--;
    } else {
        /* Merge with previous line */
        int merge_col = E.row[E.cy - 1].size;
        if (!buffer_row_append_string(&E.row[E.cy - 1],
                                      row->chars, (size_t)row->size)) {
            return;
        }
        undo_push(UNDO_DELETE_NEWLINE, E.cy - 1, merge_col, 0);
        buffer_delete_row(E.cy);
        E.cy--;
        E.cx = merge_col;
    }
}

void buffer_insert_newline(void)
{
    undo_push(UNDO_INSERT_NEWLINE, E.cy, E.cx, 0);
    if (E.cx == 0) {
        buffer_insert_row(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        buffer_insert_row(E.cy + 1,
                          &row->chars[E.cx],
                          (size_t)(row->size - E.cx));
        /* After insert_row, E.row may have been realloc'd */
        row = &E.row[E.cy];
        row->size  = E.cx;
        row->chars[row->size] = '\0';
        buffer_update_row(row);
    }
    E.cy++;
    E.cx = 0;
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
        size_t add = (size_t)E.row[j].size + 1; /* +1 for '\n' */
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
        *p = '\n';
        p++;
    }
    return buf;
}
