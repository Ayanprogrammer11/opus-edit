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
            memcpy(E.row[saved_hl_line].hl, saved_hl,
                   (size_t)E.row[saved_hl_line].rsize);
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
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = buffer_rx_to_cx(row, (int)(match - row->render));
            /* Place the match at the top of the screen */
            E.rowoff = E.numrows;

            /* Save and override highlight for the matching region */
            saved_hl_line = current;
            saved_hl = malloc((size_t)row->rsize);
            if (saved_hl) {
                memcpy(saved_hl, row->hl, (size_t)row->rsize);
            }
            int qlen = (int)strlen(query);
            int mstart = (int)(match - row->render);
            memset(&row->hl[mstart], HL_MATCH,
                   qlen < row->rsize - mstart ? (size_t)qlen
                                              : (size_t)(row->rsize - mstart));
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
