/*
 * git.h – Lightweight Git integration for gutter signs.
 *
 * Parses .git directories directly (no libgit2) to compute
 * per-line diff signs against HEAD.
 */

#ifndef OPUSEDIT_GIT_H
#define OPUSEDIT_GIT_H

/* Re-scan repo metadata when the current filename changes. */
void git_on_file_change(void);

/* Mark the current buffer's git signs as stale. */
void git_mark_dirty(void);

/* Recompute signs for the current buffer if needed. */
void git_refresh_signs(void);

/* Return the sign for a line (row index), or ' ' if none. */
char git_sign_for_row(int row);

/* Whether to render the git sign gutter. */
int git_show_gutter(void);

#endif /* OPUSEDIT_GIT_H */
