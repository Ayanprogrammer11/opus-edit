/*
 * output.h – Screen rendering, scrolling, and syntax highlighting.
 *
 * All output is double-buffered through an `abuf` to avoid flicker.
 * Syntax highlighting state is computed per-row and cached.
 */

#ifndef OPUSEDIT_OUTPUT_H
#define OPUSEDIT_OUTPUT_H

#include "editor.h"

/* ── Rendering ────────────────────────────────────────────── */

/* Recalculate scroll offsets so the cursor stays visible. */
void output_scroll(void);

/* Invalidate/free derived soft-wrap row mappings. */
void output_invalidate_wrap_cache(void);
void output_free_wrap_cache(void);

/* Render the full screen: rows + status bar + message bar. */
void output_refresh_screen(void);

/* Effective text width (screen columns minus gutters). */
int  output_text_width(void);

/*
 * Soft-wrap helpers: treat each file row as one or more visual rows.
 * These utilities map between file rows and wrapped render rows.
 */
int  output_total_render_rows(void);
int  output_row_render_index(int row_idx);
void output_render_row_to_file(int render_row, int *row_idx, int *row_line);

/* ── Syntax highlighting ──────────────────────────────────── */

/* Map a highlight token (HL_*) to an ANSI color code. */
int output_syntax_to_color(int hl);

/* Recompute the `hl` array for a single row. */
void output_update_syntax(erow *row);

/*
 * Select the syntax definition that matches the current filename
 * extension and store it in E.syntax.
 */
void output_select_syntax_highlight(void);

#endif /* OPUSEDIT_OUTPUT_H */
