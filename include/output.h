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

/* Render the full screen: rows + status bar + message bar. */
void output_refresh_screen(void);

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
