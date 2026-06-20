/*
 * undo.c – Undo/redo system implementation.
 *
 * Each editing primitive (insert char, delete char, insert newline,
 * delete newline) pushes one undo_op.  Consecutive same-type char
 * operations at adjacent positions are assigned the same group_id
 * so they undo/redo as a single user-perceived action.
 */

#include "undo.h"
#include "editor.h"
#include "buffer.h"
#include "git.h"
#include "output.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ── Stack management ─────────────────────────────────────── */

void undo_stack_init(undo_stack *s)
{
    s->ops      = NULL;
    s->count    = 0;
    s->capacity = 0;
}

void undo_stack_free(undo_stack *s)
{
    free(s->ops);
    s->ops      = NULL;
    s->count    = 0;
    s->capacity = 0;
}

void undo_stack_clear(undo_stack *s)
{
    s->count = 0;
}

static int stack_push_raw(undo_stack *s, const undo_op *op)
{
    if (s->count < 0 || s->capacity < 0) return -1;
    if (s->count == INT_MAX) return -1;
    if (s->count >= s->capacity) {
        if (s->capacity > INT_MAX / 2) return -1;
        int newcap = (s->capacity == 0) ? 64 : s->capacity * 2;
        if (newcap < s->capacity) return -1;
        if ((size_t)newcap > SIZE_MAX / sizeof(undo_op)) return -1;
        undo_op *tmp = realloc(s->ops, (size_t)newcap * sizeof(undo_op));
        if (!tmp) return -1;
        s->ops      = tmp;
        s->capacity = newcap;
    }
    s->ops[s->count++] = *op;
    return 0;
}

/* ── Group-id generation ──────────────────────────────────── */

static int next_group_id = 1;
static int force_new_group = 0;
static int explicit_group_id = 0;
static int explicit_group_depth = 0;

static int undo_type_is_char_insert(enum undo_op_type type)
{
    return type == UNDO_INSERT_CHAR || type == UNDO_INSERT_CHAR_ROW;
}

/*
 * Determine whether a new operation should join the current group
 * (the top of the undo stack) or start a new one.
 *
 * Grouping rules:
 *   - Same type (INSERT_CHAR with INSERT_CHAR, etc.)
 *   - Character operations only (newlines always start a new group)
 *   - Same row
 *   - Adjacent column (±1 depending on direction)
 */
static int resolve_group_id(enum undo_op_type type, int row, int col)
{
    if (explicit_group_id)
        return explicit_group_id;

    if (force_new_group) {
        force_new_group = 0;
        return next_group_id++;
    }

    if (E.undo.count == 0)
        return next_group_id++;

    const undo_op *prev = &E.undo.ops[E.undo.count - 1];

    /* Newline ops always start a new group */
    if (type == UNDO_INSERT_NEWLINE || type == UNDO_DELETE_NEWLINE)
        return next_group_id++;

    if (type == UNDO_SET_FINAL_NEWLINE)
        return next_group_id++;

    if (undo_type_is_char_insert(type)) {
        if (!undo_type_is_char_insert(prev->type))
            return next_group_id++;
    } else if (prev->type != type) {
        return next_group_id++;
    }

    if (prev->row != row)
        return next_group_id++;

    /* For inserts: new col should be prev->col + 1 (typing forward) */
    if (undo_type_is_char_insert(type) && col == prev->col + 1)
        return prev->group_id;

    /* For deletes: backspace goes col = prev->col - 1, or same col (Del key) */
    if (type == UNDO_DELETE_CHAR &&
        (col == prev->col - 1 || col == prev->col))
        return prev->group_id;

    return next_group_id++;
}

/* ── Public push ──────────────────────────────────────────── */

void undo_push(enum undo_op_type type, int row, int col, int c)
{
    /* Don't record during undo/redo replay */
    if (!E.undo_recording) return;

    undo_op op;
    op.type     = type;
    op.row      = row;
    op.col      = col;
    op.c        = c;
    op.cursor_x = E.cx;
    op.cursor_y = E.cy;
    op.group_id = resolve_group_id(type, row, col);

    stack_push_raw(&E.undo, &op);

    /* Any new edit invalidates the redo stack */
    undo_stack_clear(&E.redo);
}

void undo_break_group(void)
{
    force_new_group = 1;
}

void undo_begin_group(void)
{
    if (!explicit_group_id) {
        explicit_group_id = next_group_id++;
        force_new_group = 0;
    }
    explicit_group_depth++;
}

void undo_end_group(void)
{
    if (explicit_group_depth > 0)
        explicit_group_depth--;
    if (explicit_group_depth == 0)
        explicit_group_id = 0;
}

void undo_push_final_newline(int old_value, int new_value)
{
    undo_push(UNDO_SET_FINAL_NEWLINE, old_value ? 1 : 0,
              new_value ? 1 : 0, 0);
}

/* ── Reverse a single operation ───────────────────────────── */

/*
 * Apply the inverse of `op` to the buffer.
 * Returns 1 on success, 0 on failure.
 * E.undo_recording is already 0 when this is called.
 */
static int apply_inverse(const undo_op *op)
{
    switch (op->type) {
        case UNDO_INSERT_CHAR:
        case UNDO_INSERT_CHAR_ROW:
            /* A char was inserted → delete it */
            if (op->row >= 0 && op->row < E.numrows) {
                if (!buffer_row_delete_char(&E.row[op->row], op->col))
                    return 0;
                if (op->type == UNDO_INSERT_CHAR_ROW
                    && op->row >= 0 && op->row < E.numrows
                    && E.row[op->row].size == 0)
                    buffer_delete_row(op->row);
                return 1;
            }
            return 0;

        case UNDO_DELETE_CHAR:
            /* A char was deleted → re-insert it */
            if (op->row >= 0 && op->row < E.numrows)
                return buffer_row_insert_char(&E.row[op->row], op->col, op->c);
            return 0;

        case UNDO_INSERT_NEWLINE:
            /* A newline was inserted (row was split) → merge them back */
            if (op->row >= 0 && op->row + 1 < E.numrows) {
                if (!buffer_row_append_string(
                        &E.row[op->row],
                        E.row[op->row + 1].chars,
                        (size_t)E.row[op->row + 1].size)) {
                    return 0;
                }
                buffer_delete_row(op->row + 1);
            } else if (op->row >= 0 && op->row < E.numrows
                       && op->col == 0 && E.row[op->row].size == 0) {
                buffer_delete_row(op->row);
            }
            return 1;

        case UNDO_DELETE_NEWLINE:
            /* A newline was deleted (rows were merged) → split again */
            if (op->row >= 0 && op->row < E.numrows) {
                erow *row = &E.row[op->row];
                if (op->col <= row->size) {
                    if (!buffer_insert_row(op->row + 1,
                                           &row->chars[op->col],
                                           (size_t)(row->size - op->col))) {
                        return 0;
                    }
                    /* row pointer may be stale after insert_row */
                    row = &E.row[op->row];
                    row->size = op->col;
                    row->chars[row->size] = '\0';
                    buffer_update_row(row);
                }
            }
            return 1;

        case UNDO_SET_FINAL_NEWLINE:
            E.ends_with_newline = op->row ? 1 : 0;
            editor_mark_dirty();
            git_mark_dirty();
            return 1;
    }
    return 1;
}

/* Apply the forward direction of `op` (used by redo). */
static int apply_forward(const undo_op *op)
{
    switch (op->type) {
        case UNDO_INSERT_CHAR:
        case UNDO_INSERT_CHAR_ROW:
            if ((op->type == UNDO_INSERT_CHAR_ROW
                 || (op->row == E.numrows && op->col == 0))
                && op->row == E.numrows) {
                if (!buffer_insert_row(E.numrows, "", 0))
                    return 0;
            }
            if (op->row >= 0 && op->row < E.numrows)
                return buffer_row_insert_char(&E.row[op->row], op->col, op->c);
            return 0;

        case UNDO_DELETE_CHAR:
            if (op->row >= 0 && op->row < E.numrows)
                return buffer_row_delete_char(&E.row[op->row], op->col);
            return 0;

        case UNDO_INSERT_NEWLINE:
            if (op->row >= 0 && op->row < E.numrows) {
                erow *row = &E.row[op->row];
                if (op->col <= row->size) {
                    if (!buffer_insert_row(op->row + 1,
                                           &row->chars[op->col],
                                           (size_t)(row->size - op->col))) {
                        return 0;
                    }
                    row = &E.row[op->row];
                    row->size = op->col;
                    row->chars[row->size] = '\0';
                    buffer_update_row(row);
                }
            } else if (op->row == E.numrows && op->col == 0) {
                if (!buffer_insert_row(op->row, "", 0))
                    return 0;
            }
            return 1;

        case UNDO_DELETE_NEWLINE:
            if (op->row >= 0 && op->row + 1 < E.numrows) {
                if (!buffer_row_append_string(
                        &E.row[op->row],
                        E.row[op->row + 1].chars,
                        (size_t)E.row[op->row + 1].size)) {
                    return 0;
                }
                buffer_delete_row(op->row + 1);
            }
            return 1;

        case UNDO_SET_FINAL_NEWLINE:
            E.ends_with_newline = op->col ? 1 : 0;
            editor_mark_dirty();
            git_mark_dirty();
            return 1;
    }
    return 1;
}

/* ── Undo ─────────────────────────────────────────────────── */

void undo_perform_undo(void)
{
    if (E.undo.count == 0) {
        editor_set_status_message("Nothing to undo.");
        return;
    }

    E.undo_recording = 0;

    int gid = E.undo.ops[E.undo.count - 1].group_id;

    /* Save cursor from the first op in the group for restoration */
    int restore_cx = -1, restore_cy = -1;
    int did_undo = 0;
    int failed = 0;

    /* Pop and reverse all ops in this group (back to front) */
    while (E.undo.count > 0 && E.undo.ops[E.undo.count - 1].group_id == gid) {
        undo_op op = E.undo.ops[--E.undo.count];

        if (!apply_inverse(&op)) {
            /* Requeue the op so it can be retried later */
            E.undo.ops[E.undo.count++] = op;
            failed = 1;
            break;
        }

        /* Remember cursor from the chronologically-earliest op */
        restore_cx = op.cursor_x;
        restore_cy = op.cursor_y;
        did_undo = 1;

        /* Keep the original operation so redo can replay it forward. */
        stack_push_raw(&E.redo, &op);
    }

    /* Restore cursor to where it was before the group was applied */
    if (restore_cx >= 0) {
        E.cx = restore_cx;
        E.cy = restore_cy;
    }

    E.undo_recording = 1;

    if (did_undo) {
        editor_refresh_dirty_from_saved();
        if (!failed)
            editor_set_status_message("Undo.");
    }
}

/* ── Redo ─────────────────────────────────────────────────── */

void undo_perform_redo(void)
{
    if (E.redo.count == 0) {
        editor_set_status_message("Nothing to redo.");
        return;
    }

    E.undo_recording = 0;

    int gid = E.redo.ops[E.redo.count - 1].group_id;

    int last_cx = -1, last_cy = -1;
    int did_redo = 0;
    int failed = 0;

    /* Pop and replay all ops in this group (back to front,
       but since they were pushed in reverse order during undo,
       this effectively replays them in original order) */
    while (E.redo.count > 0 && E.redo.ops[E.redo.count - 1].group_id == gid) {
        undo_op op = E.redo.ops[--E.redo.count];

        if (!apply_forward(&op)) {
            /* Requeue the op so it can be retried later */
            E.redo.ops[E.redo.count++] = op;
            failed = 1;
            break;
        }

        /* Compute where cursor should end up after this op */
        switch (op.type) {
            case UNDO_INSERT_CHAR:
            case UNDO_INSERT_CHAR_ROW:
                last_cx = op.col + 1;
                last_cy = op.row;
                break;
            case UNDO_DELETE_CHAR:
                last_cx = op.col;
                last_cy = op.row;
                break;
            case UNDO_INSERT_NEWLINE:
                last_cy = op.row + 1;
                last_cx = 0;
                break;
            case UNDO_DELETE_NEWLINE:
                last_cx = op.col;
                last_cy = op.row;
                break;
            case UNDO_SET_FINAL_NEWLINE:
                break;
        }

        /* Restore the original operation to the undo stack. */
        stack_push_raw(&E.undo, &op);
        did_redo = 1;
    }

    if (last_cx >= 0) {
        E.cx = last_cx;
        E.cy = last_cy;
    }

    E.undo_recording = 1;

    if (did_redo) {
        editor_refresh_dirty_from_saved();
        if (!failed)
            editor_set_status_message("Redo.");
    }
}
