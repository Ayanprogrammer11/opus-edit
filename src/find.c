/*
 * find.c – Incremental text search with match highlighting.
 */

#include "find.h"
#include "editor.h"
#include "buffer.h"
#include "input.h"
#include "output.h"

#include <stdlib.h>
#include <string.h>

/* ── State preserved across incremental callbacks ─────────── */
static int  saved_cx;
static int  saved_cy;
static int  saved_coloff;
static int  saved_rowoff;
static int  last_match;    /* row index of last match, -1 = none */
static int  direction;     /* 1 = forward, -1 = backward         */

/* ── Restore saved highlight for the last-matched row ─────── */
static int  saved_hl_line = -1;
static unsigned char *saved_hl = NULL;

static void restore_highlight(void)
{
    if (saved_hl) {
        if (saved_hl_line >= 0 && saved_hl_line < E.numrows) {
            if (E.row[saved_hl_line].hl) {
                memcpy(E.row[saved_hl_line].hl, saved_hl,
                       (size_t)E.row[saved_hl_line].rsize);
            }
        }
        free(saved_hl);
        saved_hl      = NULL;
        saved_hl_line = -1;
    }
}

/* ── Incremental search callback ──────────────────────────── */

static void find_callback(const char *query, int key)
{
    restore_highlight();

    if (key == '\r' || key == '\x1b') {
        /* Confirm or cancel – reset search state */
        last_match = -1;
        direction  = 1;
        return;
    }

    if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction  = 1;
    }

    if (last_match == -1) direction = 1;

    int current = last_match;

    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1)         current = E.numrows - 1;
        if (current == E.numrows)  current = 0;

        erow *row = &E.row[current];
        if (!row->render) continue;
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = buffer_rx_to_cx(row, (int)(match - row->render));
            /* Place the match at the top of the screen */
            E.rowoff = output_total_render_rows();

            /* Save and override highlight for the matching region */
            saved_hl_line = current;
            if (row->hl) {
                saved_hl = malloc((size_t)row->rsize);
                if (saved_hl) {
                    memcpy(saved_hl, row->hl, (size_t)row->rsize);
                }
            }
            if (row->hl) {
                int qlen = (int)strlen(query);
                int mstart = (int)(match - row->render);
                memset(&row->hl[mstart], HL_MATCH,
                       qlen < row->rsize - mstart ? (size_t)qlen
                                                  : (size_t)(row->rsize - mstart));
            }
            break;
        }
    }
}

/* ── Entry point: open the search prompt ──────────────────── */

void find(void)
{
    saved_cx     = E.cx;
    saved_cy     = E.cy;
    saved_coloff = E.coloff;
    saved_rowoff = E.rowoff;

    char *query = editor_prompt(
        "Search: %s (ESC=cancel, Arrows=prev/next)", find_callback);

    if (!query) {
        /* Cancelled – restore original cursor position */
        E.cx     = saved_cx;
        E.cy     = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
    free(query);
}

/* ── Find and replace ─────────────────────────────────────── */

static void clamp_cursor_to_row(int row, int *cx)
{
    if (row < 0 || row >= E.numrows) return;
    if (*cx > E.row[row].size) *cx = E.row[row].size;
    if (*cx < 0) *cx = 0;
}

static int replace_range_in_row(int row_idx, int start_cx, int end_cx,
                                const char *replacement, int repl_len)
{
    if (row_idx < 0 || row_idx >= E.numrows) return 0;
    erow *row = &E.row[row_idx];
    if (!row) return 0;

    if (start_cx < 0) start_cx = 0;
    if (end_cx < start_cx) return 0;
    if (end_cx > row->size) end_cx = row->size;

    int remove_len = end_cx - start_cx;
    int new_size = row->size - remove_len + repl_len;
    if (new_size > OPUSEDIT_MAX_ROW_SIZE) {
        return 0;
    }

    E.cy = row_idx;
    E.cx = start_cx;

    for (int i = 0; i < remove_len; i++) {
        if (start_cx >= row->size) break;
        int deleted = row->chars[start_cx];
        undo_push(UNDO_DELETE_CHAR, row_idx, start_cx, deleted);
        buffer_row_delete_char(row, start_cx);
    }

    for (int i = 0; i < repl_len; i++) {
        undo_push(UNDO_INSERT_CHAR, row_idx, start_cx + i,
                  (unsigned char)replacement[i]);
        buffer_row_insert_char(row, start_cx + i,
                               (unsigned char)replacement[i]);
    }

    E.cx = start_cx + repl_len;
    return 1;
}

static int replace_all(const char *query, const char *replacement)
{
    if (!query || !*query) return 0;
    int total = 0;
    int qlen = (int)strlen(query);
    int repl_len = replacement ? (int)strlen(replacement) : 0;

    for (int row_idx = 0; row_idx < E.numrows; row_idx++) {
        erow *row = &E.row[row_idx];
        if (!row->render || row->rsize <= 0) continue;

        int rpos = 0;
        while (rpos <= row->rsize - qlen) {
            char *match = strstr(row->render + rpos, query);
            if (!match) break;

            int match_rx = (int)(match - row->render);
            int start_cx = buffer_rx_to_cx(row, match_rx);
            int end_cx = buffer_rx_to_cx(row, match_rx + qlen);
            if (end_cx < start_cx) break;

            int remove_len = end_cx - start_cx;
            int new_size = row->size - remove_len + repl_len;
            if (new_size > OPUSEDIT_MAX_ROW_SIZE) {
                editor_set_status_message(
                    "Replacement would make a line too long.");
                rpos = match_rx + qlen;
                continue;
            }

            if (!replace_range_in_row(row_idx, start_cx, end_cx,
                                      replacement, repl_len)) {
                rpos = match_rx + qlen;
                continue;
            }
            total++;

            int after_cx = start_cx + repl_len;
            clamp_cursor_to_row(row_idx, &after_cx);
            rpos = buffer_cx_to_rx(row, after_cx);
        }
    }

    return total;
}

void find_replace(void)
{
    saved_cx     = E.cx;
    saved_cy     = E.cy;
    saved_coloff = E.coloff;
    saved_rowoff = E.rowoff;

    char *query = editor_prompt(
        "Find: %s (ESC=cancel)", find_callback);
    restore_highlight();

    if (!query) {
        E.cx     = saved_cx;
        E.cy     = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
        return;
    }

    char *replacement = editor_prompt_allow_empty(
        "Replace with: %s (ESC=cancel)", NULL);

    if (!replacement) {
        E.cx     = saved_cx;
        E.cy     = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
        free(query);
        return;
    }

    int replaced = replace_all(query, replacement);

    E.cx     = saved_cx;
    E.cy     = saved_cy;
    clamp_cursor_to_row(E.cy, &E.cx);
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;

    editor_set_status_message("Replaced %d occurrence%s.",
                              replaced, replaced == 1 ? "" : "s");

    free(query);
    free(replacement);
}
