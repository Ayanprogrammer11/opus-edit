/*
 * undo.h – Undo/redo system for OpusEdit.
 *
 * Records atomic editing operations on a stack. Consecutive same-type
 * character edits at adjacent positions are grouped so that undo/redo
 * operates on logical "chunks" of typing rather than single characters.
 *
 * Architecture:
 *   - Every buffer mutation pushes an undo_op to the undo stack.
 *   - The redo stack is cleared on any new edit.
 *   - Undo pops a group from the undo stack, reverses each op, and
 *     pushes the inverse group onto the redo stack.
 *   - Redo does the symmetrical operation.
 *   - During undo/redo replay, E.undo_recording is set to 0 so
 *     buffer operations don't recursively record themselves.
 */

#ifndef OPUSEDIT_UNDO_H
#define OPUSEDIT_UNDO_H

/* ── Operation types ──────────────────────────────────────── */
enum undo_op_type {
    UNDO_INSERT_CHAR,       /* a character was inserted              */
    UNDO_DELETE_CHAR,       /* a character was deleted                */
    UNDO_INSERT_NEWLINE,    /* a newline was inserted (row split)    */
    UNDO_DELETE_NEWLINE,    /* a newline was deleted  (row merge)    */
};

/* ── Single atomic operation ──────────────────────────────── */
typedef struct undo_op {
    enum undo_op_type type;
    int row;                /* row where the operation occurred       */
    int col;                /* column where the operation occurred    */
    int c;                  /* character value (for char ops)         */
    int cursor_x;           /* cursor X before this operation         */
    int cursor_y;           /* cursor Y before this operation         */
    int group_id;           /* ops with same group_id undo together   */
} undo_op;

/* ── Operation stack ──────────────────────────────────────── */
typedef struct undo_stack {
    undo_op *ops;
    int      count;
    int      capacity;
} undo_stack;

/* ── API ──────────────────────────────────────────────────── */

/* Initialise / free a stack. */
void undo_stack_init(undo_stack *s);
void undo_stack_free(undo_stack *s);
void undo_stack_clear(undo_stack *s);

/*
 * Push an operation.  Grouping is automatic: consecutive same-type
 * char operations at adjacent positions share a group_id.
 * Pushing to the undo stack also clears the redo stack.
 */
void undo_push(enum undo_op_type type, int row, int col, int c);

/* Execute undo: pop one group, reverse it, push inverse to redo. */
void undo_perform_undo(void);

/* Execute redo: pop one group, replay it, push inverse to undo. */
void undo_perform_redo(void);

#endif /* OPUSEDIT_UNDO_H */
