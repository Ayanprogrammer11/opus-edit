/*
 * buffer.h – Text-buffer data structures and row operations.
 *
 * The buffer is a dynamic array of `erow` structs (defined in editor.h).
 * This module owns all insertion, deletion, and mutation logic for text.
 */

#ifndef OPUSEDIT_BUFFER_H
#define OPUSEDIT_BUFFER_H

#include "editor.h"
#include <stddef.h>

/* ── Row-level operations ─────────────────────────────────── */

/*
 * Recompute the `render` and `rsize` fields of a row after its
 * `chars` content has changed (tab expansion, etc.).
 */
void buffer_update_row(erow *row);

/*
 * Insert a new row at position `at` with content `s` of length `len`.
 * Adjusts indices of all subsequent rows.
 */
void buffer_insert_row(int at, const char *s, size_t len);

/* Free all heap allocations inside a row (does not free the struct). */
void buffer_free_row(erow *row);

/* Delete the row at position `at`, shifting subsequent rows up. */
void buffer_delete_row(int at);

/* Insert character `c` into `row` at column `at`. */
void buffer_row_insert_char(erow *row, int at, int c);

/* Delete the character at column `at` in `row`. */
void buffer_row_delete_char(erow *row, int at);

/* Append string `s` (length `len`) to the end of `row`. */
void buffer_row_append_string(erow *row, const char *s, size_t len);

/* ── Editor-level text operations ─────────────────────────── */

/* Insert character `c` at the current cursor position. */
void buffer_insert_char(int c);

/* Delete the character before the cursor (Backspace). */
void buffer_delete_char(void);

/* Insert a newline at the cursor, splitting the current row. */
void buffer_insert_newline(void);

/* ── Coordinate helpers ───────────────────────────────────── */

/* Convert a chars-index (cx) to a render-index (rx) for `row`. */
int buffer_cx_to_rx(const erow *row, int cx);

/* Convert a render-index (rx) back to a chars-index (cx). */
int buffer_rx_to_cx(const erow *row, int rx);

/*
 * Serialise all rows into a single heap-allocated string.
 * Caller must free(). `*buflen` receives total byte count.
 */
char *buffer_rows_to_string(int *buflen);

#endif /* OPUSEDIT_BUFFER_H */
