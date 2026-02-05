/*
 * buffer.c – Text-buffer row management and editing operations.
 */

#include "buffer.h"
#include "editor.h"
#include "output.h"

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
    /* Count tabs to know how much extra space we need */
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc((size_t)(row->size + tabs * (OPUSEDIT_TAB_STOP - 1) + 1));
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

    erow *newrows = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    if (!newrows) return;
    E.row = newrows;

    memmove(&E.row[at + 1], &E.row[at],
            sizeof(erow) * (size_t)(E.numrows - at));

    /* Update indices for rows that shifted down */
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx = j;

    E.row[at].idx  = at;
    E.row[at].size = (int)len;

    E.row[at].chars = malloc(len + 1);
    if (!E.row[at].chars) return;
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

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
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1],
            (size_t)(row->size - at));
    row->size--;

    buffer_update_row(row);
    E.dirty++;
}

void buffer_row_append_string(erow *row, const char *s, size_t len)
{
    char *tmp = realloc(row->chars, (size_t)(row->size) + len + 1);
    if (!tmp) return;
    row->chars = tmp;

    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';

    buffer_update_row(row);
    E.dirty++;
}

/* ── Editor-level text operations ─────────────────────────── */

void buffer_insert_char(int c)
{
    if (E.cy == E.numrows) {
        buffer_insert_row(E.numrows, "", 0);
    }
    buffer_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void buffer_delete_char(void)
{
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        buffer_row_delete_char(row, E.cx - 1);
        E.cx--;
    } else {
        /* Merge with previous line */
        E.cx = E.row[E.cy - 1].size;
        buffer_row_append_string(&E.row[E.cy - 1],
                                 row->chars, (size_t)row->size);
        buffer_delete_row(E.cy);
        E.cy--;
    }
}

void buffer_insert_newline(void)
{
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
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;   /* +1 for '\n' */

    *buflen = totlen;
    char *buf = malloc((size_t)totlen);
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
