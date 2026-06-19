/*
 * output.c – Screen rendering, double-buffering, and syntax highlighting.
 */

#include "output.h"
#include "editor.h"
#include "buffer.h"
#include "git.h"
#include "terminal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Syntax database ──────────────────────────────────────── */

static const char *C_HL_extensions[] = {
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hxx", NULL
};

static const char *PY_HL_extensions[] = {
    ".py", ".pyw", NULL
};

/*
 * Keywords ending with '|' are drawn as "type" keywords (HL_KEYWORD2);
 * the rest are control / statement keywords (HL_KEYWORD1).
 */
static const char *C_HL_keywords[] = {
    /* Control flow & statements */
    "switch", "if", "while", "for", "break", "continue", "return",
    "else", "do", "goto", "case", "default",
    /* Declarations */
    "struct", "union", "typedef", "enum", "class", "namespace",
    "using", "try", "catch", "throw",
    /* Storage & qualifiers */
    "static", "extern", "const", "volatile", "register", "inline",
    "restrict", "_Atomic", "_Thread_local",
    /* Operators */
    "sizeof", "alignof", "_Alignof",
    /* Preprocessor */
    "#include", "#define", "#ifdef", "#ifndef", "#endif", "#else",
    "#elif", "#if", "#undef", "#pragma", "#error", "#warning",
    /* Type keywords (HL_KEYWORD2) — note trailing '|' */
    "int|", "long|", "double|", "float|", "char|",
    "unsigned|", "signed|", "void|", "short|", "auto|", "bool|",
    "size_t|", "ssize_t|", "ptrdiff_t|", "intptr_t|", "uintptr_t|",
    "uint8_t|", "uint16_t|", "uint32_t|", "uint64_t|",
    "int8_t|", "int16_t|", "int32_t|", "int64_t|",
    "FILE|", "NULL|", "true|", "false|",
    "pid_t|", "off_t|", "mode_t|",
    NULL
};

static const char *PY_HL_keywords[] = {
    /* Control flow & statements */
    "if", "elif", "else", "for", "while", "break", "continue",
    "return", "pass", "raise", "try", "except", "finally",
    "with", "as", "assert", "yield",
    /* Declarations */
    "def", "class", "lambda", "import", "from", "global", "nonlocal",
    /* Async */
    "async", "await",
    /* Builtins / literals (type-ish) */
    "True|", "False|", "None|",
    "int|", "float|", "bool|", "str|", "bytes|", "bytearray|",
    "list|", "dict|", "set|", "tuple|", "object|",
    NULL
};

static editor_syntax HLDB[] = {
    {
        "c/c++",
        C_HL_extensions,
        C_HL_keywords,
        "//",           /* single-line comment start */
        "/*",           /* multi-line comment start  */
        "*/",           /* multi-line comment end    */
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "python",
        PY_HL_extensions,
        PY_HL_keywords,
        "#",            /* single-line comment start */
        NULL,           /* multi-line comment start  */
        NULL,           /* multi-line comment end    */
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_TRIPLE_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ── Separator predicate ──────────────────────────────────── */

static int is_separator(int c)
{
    return isspace((unsigned char)c) || c == '\0'
        || strchr(",.()+-/*=~%<>[];:!&|^{}#?", (unsigned char)c) != NULL;
}

/* ── Append sanitized text (no control chars) ─────────────── */

static void ab_append_sanitized(abuf *ab, const char *s, int len)
{
    if (!s || len <= 0) return;

    int start = 0;
    for (int i = 0; i < len; i++) {
        if (iscntrl((unsigned char)s[i])) {
            /* Flush preceding clean run */
            if (i > start)
                ab_append(ab, &s[start], i - start);
            ab_append(ab, "?", 1);
            start = i + 1;
        }
    }
    /* Flush trailing clean run */
    if (len > start)
        ab_append(ab, &s[start], len - start);
}

/* ── Syntax highlighting per row ──────────────────────────── */

static void output_update_syntax_internal(erow *row, int propagate)
{
    if (!row->render) {
        free(row->hl);
        row->hl = NULL;
        row->hl_open_comment = 0;
        row->hl_open_string = 0;
        return;
    }

    if (row->rsize == 0) {
        free(row->hl);
        row->hl = NULL;
        int in_comment = (E.syntax && row->idx > 0
                          && E.row[row->idx - 1].hl_open_comment);
        int in_triple = 0;
        if (E.syntax && (E.syntax->flags & HL_HIGHLIGHT_TRIPLE_STRINGS)
            && row->idx > 0) {
            in_triple = E.row[row->idx - 1].hl_open_string;
        }
        int changed = (row->hl_open_comment != in_comment
                       || row->hl_open_string != in_triple);
        row->hl_open_comment = in_comment;
        row->hl_open_string = in_triple;
        if (propagate && changed && row->idx + 1 < E.numrows) {
            for (int next_idx = row->idx + 1; next_idx < E.numrows; next_idx++) {
                int prev_comment = E.row[next_idx].hl_open_comment;
                int prev_string = E.row[next_idx].hl_open_string;
                output_update_syntax_internal(&E.row[next_idx], 0);
                if (E.row[next_idx].hl_open_comment == prev_comment
                    && E.row[next_idx].hl_open_string == prev_string)
                    break;
            }
        }
        return;
    }

    unsigned char *tmp = realloc(row->hl, (size_t)row->rsize);
    if (!tmp) {
        /* Avoid keeping a stale highlight buffer whose size may no
         * longer match row->rsize. Rendering can safely fall back to
         * plain text when row->hl is NULL. */
        free(row->hl);
        row->hl = NULL;
        return;
    }
    row->hl = tmp;
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    if (!E.syntax) {
        row->hl_open_comment = 0;
        row->hl_open_string = 0;
        return;
    }

    const char **keywords = E.syntax->keywords;
    const char *scs  = E.syntax->singleline_comment_start;
    const char *mcs  = E.syntax->multiline_comment_start;
    const char *mce  = E.syntax->multiline_comment_end;

    int scs_len = scs ? (int)strlen(scs) : 0;
    int mcs_len = mcs ? (int)strlen(mcs) : 0;
    int mce_len = mce ? (int)strlen(mce) : 0;

    int prev_sep     = 1;       /* previous char was separator? */
    int in_string    = 0;       /* 0 or the quote character     */
    int in_comment   = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);
    int in_triple    = 0;
    if ((E.syntax->flags & HL_HIGHLIGHT_TRIPLE_STRINGS) && row->idx > 0)
        in_triple = E.row[row->idx - 1].hl_open_string;

    int i = 0;
    while (i < row->rsize) {
        char  c       = row->render[i];
        int   prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
        (void)prev_hl;

        int remaining = row->rsize - i;

        /* ── Multi-line comment end ───────────────────────── */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (mce_len > 0 && remaining >= mce_len
                && !strncmp(&row->render[i], mce, (size_t)mce_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, (size_t)mce_len);
                i += mce_len;
                in_comment = 0;
                prev_sep   = 1;
                continue;
            }
            i++;
            prev_sep = 0;
            continue;
        }

        /* ── Triple-quoted string (multiline) ─────────────── */
        if (in_triple) {
            row->hl[i] = HL_STRING;
            if (remaining >= 3
                && row->render[i] == in_triple
                && row->render[i + 1] == in_triple
                && row->render[i + 2] == in_triple) {
                memset(&row->hl[i], HL_STRING, 3);
                i += 3;
                in_triple = 0;
                prev_sep = 1;
                continue;
            }
            i++;
            prev_sep = 0;
            continue;
        }

        /* ── String ───────────────────────────────────────── */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (c == '\\' && i + 1 < row->rsize) {
                row->hl[i + 1] = HL_STRING;
                i += 2;
                prev_sep = 0;
                continue;
            }
            if (c == in_string) in_string = 0;
            i++;
            prev_sep = 1;
            continue;
        }

        /* ── Single-line comment ──────────────────────────── */
        if (scs_len > 0 && remaining >= scs_len
            && !strncmp(&row->render[i], scs, (size_t)scs_len)) {
            memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
            break;
        }

        /* ── Multi-line comment start ─────────────────────── */
        if (mcs_len > 0 && remaining >= mcs_len
            && !strncmp(&row->render[i], mcs, (size_t)mcs_len)) {
            memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
            i += mcs_len;
            in_comment = 1;
            prev_sep   = 0;
            continue;
        }

        /* ── Triple-quoted string start ───────────────────── */
        if ((E.syntax->flags & HL_HIGHLIGHT_TRIPLE_STRINGS)
            && remaining >= 3) {
            if (!strncmp(&row->render[i], "'''", 3)
                || !strncmp(&row->render[i], "\"\"\"", 3)) {
                in_triple = row->render[i];
                memset(&row->hl[i], HL_STRING, 3);
                i += 3;
                prev_sep = 0;
                continue;
            }
        }

        /* ── String start ─────────────────────────────────── */
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (c == '"' || c == '\'') {
                in_string  = c;
                row->hl[i] = HL_STRING;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        /* ── Numbers ──────────────────────────────────────── */
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit((unsigned char)c) && (prev_sep || row->hl[i > 0 ? i - 1 : 0] == HL_NUMBER))
                || (c == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        /* ── Keywords ─────────────────────────────────────── */
        if (prev_sep) {
            int matched = 0;
            for (int k = 0; keywords[k]; k++) {
                int klen = (int)strlen(keywords[k]);
                int kw2  = (keywords[k][klen - 1] == '|');
                if (kw2) klen--;

                if (i + klen <= row->rsize
                    && !strncmp(&row->render[i], keywords[k], (size_t)klen)
                    && is_separator(row->render[i + klen])) {
                    memset(&row->hl[i],
                           kw2 ? HL_KEYWORD2 : HL_KEYWORD1,
                           (size_t)klen);
                    i += klen;
                    matched = 1;
                    break;
                }
            }
            if (matched) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment
                   || row->hl_open_string != in_triple);
    row->hl_open_comment = in_comment;
    row->hl_open_string = in_triple;
    /* Propagate change to following rows */
    if (propagate && changed && row->idx + 1 < E.numrows) {
        for (int next_idx = row->idx + 1; next_idx < E.numrows; next_idx++) {
            int prev_comment = E.row[next_idx].hl_open_comment;
            int prev_string = E.row[next_idx].hl_open_string;
            output_update_syntax_internal(&E.row[next_idx], 0);
            if (E.row[next_idx].hl_open_comment == prev_comment
                && E.row[next_idx].hl_open_string == prev_string)
                break;
        }
    }
}

void output_update_syntax(erow *row)
{
    if (!row) return;
    output_update_syntax_internal(row, 1);
}

/* ── Highlight → ANSI color ───────────────────────────────── */

int output_syntax_to_color(int hl)
{
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;   /* cyan     */
        case HL_KEYWORD1:  return 33;   /* yellow   */
        case HL_KEYWORD2:  return 32;   /* green    */
        case HL_STRING:    return 35;   /* magenta  */
        case HL_NUMBER:    return 31;   /* red      */
        case HL_MATCH:     return 34;   /* blue     */
        default:           return 37;   /* white    */
    }
}

/* ── Select syntax based on filename extension ────────────── */

void output_select_syntax_highlight(void)
{
    E.syntax = NULL;
    if (!E.filename) return;

    const char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        editor_syntax *s = &HLDB[j];
        for (unsigned int i = 0; s->filematch[i]; i++) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i]))
                || (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                /* Re-highlight every row */
                for (int r = 0; r < E.numrows; r++)
                    output_update_syntax(&E.row[r]);
                return;
            }
        }
    }
}

/* ── Soft-wrap helpers ────────────────────────────────────── */

static int output_line_number_width(void)
{
    int rows = E.numrows;
    if (rows < 1) rows = 1;
    int width = 0;
    do {
        width++;
        rows /= 10;
    } while (rows);
    return width;
}

static void output_compute_gutter(int *git_width, int *num_width)
{
    int cols = (E.screencols > 0) ? E.screencols : 1;
    int gw = git_show_gutter() ? 2 : 0; /* sign + space */
    int nw = 0;
    if (E.show_line_numbers) {
        int digits = output_line_number_width();
        if (digits < 1) digits = 1;
        nw = digits + 1; /* trailing space */
    }

    if (gw + nw >= cols) {
        nw = 0;
        if (gw >= cols) gw = 0;
    }

    if (git_width) *git_width = gw;
    if (num_width) *num_width = nw;
}

static int output_gutter_width(void)
{
    int gw = 0;
    int nw = 0;
    output_compute_gutter(&gw, &nw);
    return gw + nw;
}

static int output_wrap_width(void)
{
    int cols = (E.screencols > 0) ? E.screencols : 1;
    int gutter = output_gutter_width();
    if (gutter >= cols) return 1;
    return cols - gutter;
}

int output_text_width(void)
{
    return output_wrap_width();
}

static int output_row_render_lines(const erow *row)
{
    if (!row || row->rsize <= 0) return 1;
    int width = output_wrap_width();
    if (width <= 0) width = 1;
    return (row->rsize - 1) / width + 1;
}

void output_invalidate_wrap_cache(void)
{
    E.render_prefix_valid = 0;
}

void output_free_wrap_cache(void)
{
    free(E.render_prefix);
    E.render_prefix = NULL;
    E.render_prefix_count = 0;
    E.render_prefix_width = 0;
    E.render_prefix_valid = 0;
    E.render_total_rows = 0;
}

static int output_ensure_wrap_cache(void)
{
    int width = output_wrap_width();
    if (width <= 0) width = 1;

    if (E.render_prefix_valid
        && E.render_prefix_width == width
        && E.render_prefix_count == E.numrows + 1
        && E.render_prefix) {
        return 1;
    }

    if (E.numrows < 0 || E.numrows == INT_MAX)
        return 0;

    size_t need = (size_t)(E.numrows + 1);
    if (need > SIZE_MAX / sizeof(int))
        return 0;

    int *prefix = realloc(E.render_prefix, need * sizeof(int));
    if (!prefix)
        return 0;

    E.render_prefix = prefix;
    E.render_prefix[0] = 0;
    int total = 0;
    for (int i = 0; i < E.numrows; i++) {
        int lines = output_row_render_lines(&E.row[i]);
        if (lines > INT_MAX - total) {
            total = INT_MAX;
        } else if (total != INT_MAX) {
            total += lines;
        }
        E.render_prefix[i + 1] = total;
    }

    E.render_total_rows = total;
    E.render_prefix_count = E.numrows + 1;
    E.render_prefix_width = width;
    E.render_prefix_valid = 1;
    return 1;
}

int output_total_render_rows(void)
{
    if (!output_ensure_wrap_cache())
        return E.numrows > 0 ? E.numrows : 0;
    return E.render_total_rows;
}

int output_row_render_index(int row_idx)
{
    if (row_idx <= 0) return 0;
    if (row_idx >= E.numrows) return output_total_render_rows();

    if (!output_ensure_wrap_cache())
        return row_idx;
    return E.render_prefix[row_idx];
}

void output_render_row_to_file(int render_row, int *row_idx, int *row_line)
{
    if (row_idx) *row_idx = E.numrows;
    if (row_line) *row_line = 0;

    int rr = render_row;
    if (rr < 0) rr = 0;

    if (!output_ensure_wrap_cache()) {
        if (rr < E.numrows) {
            if (row_idx) *row_idx = rr;
            if (row_line) *row_line = 0;
        }
        return;
    }

    if (rr >= E.render_total_rows)
        return;

    int lo = 0;
    int hi = E.numrows;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (E.render_prefix[mid + 1] <= rr)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (row_idx) *row_idx = lo;
    if (row_line) *row_line = rr - E.render_prefix[lo];
}

static int output_cursor_render_row(void)
{
    int base = (E.cy < E.numrows)
        ? output_row_render_index(E.cy)
        : output_total_render_rows();
    int width = output_wrap_width();
    int rx = E.rx;
    if (E.cy < E.numrows && E.cx == E.row[E.cy].size
        && rx > 0 && rx % width == 0) {
        rx--;
    }
    return base + (rx / width);
}

static int output_cursor_render_col(void)
{
    int width = output_wrap_width();
    int rx = E.rx;
    if (E.cy < E.numrows && E.cx == E.row[E.cy].size
        && rx > 0 && rx % width == 0) {
        rx--;
    }
    return (rx % width) + output_gutter_width();
}

/* ── Selection helpers ──────────────────────────────────── */

static int output_selection_bounds(int *sy, int *sx, int *ey, int *ex,
                                   int *linewise)
{
    if (E.sel_mode == SEL_NONE || E.numrows <= 0) return 0;

    *sy = E.sel_sy;
    *sx = E.sel_sx;
    *ey = E.cy;
    *ex = E.cx;

    if (*sy > *ey || (*sy == *ey && *sx > *ex)) {
        int ty = *sy;
        int tx = *sx;
        *sy = *ey;
        *sx = *ex;
        *ey = ty;
        *ex = tx;
    }

    if (*ey < 0) return 0;
    if (*sy < 0) *sy = 0;
    if (*sy >= E.numrows && *ey >= E.numrows) return 0;
    if (*sy >= E.numrows) *sy = E.numrows - 1;
    if (*ey >= E.numrows) *ey = E.numrows - 1;

    *linewise = (E.sel_mode == SEL_LINE);

    if (*linewise) {
        *sx = 0;
        *ex = E.row[*ey].size;
        return 1;
    }

    int rowlen = E.row[*ey].size;
    if (*ex < rowlen) (*ex)++;
    else *ex = rowlen;
    return 1;
}

static int output_mcursor_at_render(const erow *row, int row_idx, int render_idx)
{
    if (E.mcursor_count <= 0) return 0;
    if (!row) return 0;
    for (int i = 0; i < E.mcursor_count; i++) {
        if (E.mcursors[i].cy != row_idx) continue;
        int rx = buffer_cx_to_rx(row, E.mcursors[i].cx);
        if (rx == render_idx) return 1;
    }
    return 0;
}

/* ── Scrolling ────────────────────────────────────────────── */

void output_scroll(void)
{
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = buffer_cx_to_rx(&E.row[E.cy], E.cx);

    int screenrows = (E.screenrows > 0) ? E.screenrows : 1;
    int cursor_render = output_cursor_render_row();

    /* Vertical scroll in render-row space */
    if (cursor_render < E.rowoff)
        E.rowoff = cursor_render;
    if (cursor_render >= E.rowoff + screenrows)
        E.rowoff = cursor_render - screenrows + 1;

    int total_render = output_total_render_rows();
    int max_rowoff = total_render - screenrows;
    if (max_rowoff < 0) max_rowoff = 0;
    if (E.rowoff < 0) E.rowoff = 0;
    if (E.rowoff > max_rowoff) E.rowoff = max_rowoff;

    /* Soft-wrap disables horizontal scrolling */
    E.coloff = 0;
}

/* ── Draw text rows ───────────────────────────────────────── */

static void output_draw_rows(abuf *ab)
{
    int sel_active = 0;
    int sel_sy = 0, sel_sx = 0, sel_ey = 0, sel_ex = 0, sel_linewise = 0;
    sel_active = output_selection_bounds(&sel_sy, &sel_sx, &sel_ey, &sel_ex,
                                         &sel_linewise);
    int git_width = 0;
    int num_width = 0;
    output_compute_gutter(&git_width, &num_width);
    int show_git = git_width > 0;
    int show_numbers = num_width > 0;
    int number_width = show_numbers ? (num_width - 1) : 0;
    int text_width = output_wrap_width();

    int row_idx = 0;
    int row_line = 0;
    output_render_row_to_file(E.rowoff, &row_idx, &row_line);

    for (int y = 0; y < E.screenrows; y++) {
        /* Clear entire line to avoid stale gutter artifacts when toggling. */
        ab_append(ab, "\x1b[2K", 4);

        if (show_git) {
            char sign = ' ';
            if (row_idx < E.numrows && row_line == 0)
                sign = git_sign_for_row(row_idx);
            if (sign == '+') {
                ab_append(ab, "\x1b[32m", 5);
            } else if (sign == '-') {
                ab_append(ab, "\x1b[31m", 5);
            }
            ab_append(ab, &sign, 1);
            ab_append(ab, " ", 1);
            if (sign == '+' || sign == '-')
                ab_append(ab, "\x1b[m", 3);
        }

        if (show_numbers) {
            char lnbuf[32];
            int lnlen;
            if (row_idx < E.numrows && row_line == 0) {
                lnlen = snprintf(lnbuf, sizeof(lnbuf), "%*d ",
                                 number_width, row_idx + 1);
            } else {
                lnlen = snprintf(lnbuf, sizeof(lnbuf), "%*s ",
                                 number_width, "");
            }
            ab_append(ab, "\x1b[90m", 5);
            ab_append_sanitized(ab, lnbuf, lnlen);
            ab_append(ab, "\x1b[m", 3);
        }

        if (row_idx >= E.numrows) {
            /* Draw '~' gutter for lines past end of file */
            if (E.numrows == 0 && y == E.screenrows / 3) {
                /* Welcome message */
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "OpusEdit -- version %s", OPUSEDIT_VERSION);
                if (welcomelen < 0) welcomelen = 0;
                if ((size_t)welcomelen >= sizeof(welcome))
                    welcomelen = (int)sizeof(welcome) - 1;
                if (welcomelen > text_width)
                    welcomelen = text_width;

                int padding = (text_width - welcomelen) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding-- > 0)
                    ab_append(ab, " ", 1);

                ab_append(ab, welcome, welcomelen);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            /* Render a file row with syntax highlighting */
            erow *row = &E.row[row_idx];
            int rsize = row->rsize;
            const char *render = row->render;
            unsigned char *rowhl = row->hl;
            int width = text_width;
            int start = row_line * width;
            int eol_cursor = 0;
            if (E.mcursor_count > 0) {
                eol_cursor = output_mcursor_at_render(row, row_idx, rsize);
            }

            int sel_start_rx = -1;
            int sel_end_rx = -1;
            if (sel_active && row_idx >= sel_sy && row_idx <= sel_ey) {
                if (sel_linewise) {
                    sel_start_rx = 0;
                    sel_end_rx = rsize;
                } else if (sel_sy == sel_ey) {
                    int cs = sel_sx;
                    int ce = sel_ex;
                    if (cs < 0) cs = 0;
                    if (ce < 0) ce = 0;
                    if (cs > row->size) cs = row->size;
                    if (ce > row->size) ce = row->size;
                    sel_start_rx = buffer_cx_to_rx(row, cs);
                    sel_end_rx = buffer_cx_to_rx(row, ce);
                } else if (row_idx == sel_sy) {
                    int cs = sel_sx;
                    if (cs < 0) cs = 0;
                    if (cs > row->size) cs = row->size;
                    sel_start_rx = buffer_cx_to_rx(row, cs);
                    sel_end_rx = rsize;
                } else if (row_idx == sel_ey) {
                    int ce = sel_ex;
                    if (ce < 0) ce = 0;
                    if (ce > row->size) ce = row->size;
                    sel_start_rx = 0;
                    sel_end_rx = buffer_cx_to_rx(row, ce);
                } else {
                    sel_start_rx = 0;
                    sel_end_rx = rsize;
                }
                if (sel_end_rx < sel_start_rx) {
                    int tmp = sel_start_rx;
                    sel_start_rx = sel_end_rx;
                    sel_end_rx = tmp;
                }
                if (sel_end_rx == sel_start_rx) {
                    sel_start_rx = -1;
                    sel_end_rx = -1;
                }
            }

            int len = rsize - start;
            if (len < 0) len = 0;
            if (len > width) len = width;
            if (eol_cursor) {
                int eol_col = rsize - start;
                if (eol_col >= 0 && eol_col < width) {
                    int needed = eol_col + 1;
                    if (needed > len) len = needed;
                }
            }

            const char    *c  = (render && start < rsize) ? &render[start] : "";
            unsigned char *hl = (render && rowhl && start < rsize)
                                ? &rowhl[start] : NULL;

            int current_color = -1;
            int current_invert = 0;
            for (int j = 0; j < len; j++) {
                int render_idx = start + j;
                int is_virtual = (render_idx >= rsize);
                char ch = is_virtual ? ' ' : c[j];
                unsigned char hlval = (!is_virtual && hl) ? hl[j] : HL_NORMAL;
                int selected = (sel_start_rx >= 0
                                && render_idx >= sel_start_rx
                                && render_idx < sel_end_rx);
                if (!selected && output_mcursor_at_render(row, row_idx, render_idx))
                    selected = 1;
                int desired_invert = selected ? 1 : 0;

                if (!is_virtual && iscntrl((unsigned char)ch)) {
                    /* Render control chars as inverted letter */
                    char sym = (ch <= 26) ? '@' + ch : '?';
                    ab_append(ab, "\x1b[7m", 4);
                    ab_append(ab, &sym, 1);
                    ab_append(ab, "\x1b[m", 3);
                    current_color = -1;
                    current_invert = 0;
                    if (desired_invert) {
                        ab_append(ab, "\x1b[7m", 4);
                        current_invert = 1;
                    }
                    if (hlval != HL_NORMAL) {
                        int color = output_syntax_to_color(hlval);
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf),
                                            "\x1b[%dm", color);
                        ab_append(ab, cbuf, clen);
                        current_color = color;
                    }
                } else if (hlval == HL_NORMAL) {
                    if (desired_invert != current_invert) {
                        if (desired_invert) {
                            ab_append(ab, "\x1b[7m", 4);
                            current_invert = 1;
                        } else {
                            ab_append(ab, "\x1b[m", 3);
                            current_invert = 0;
                            current_color = -1;
                        }
                    }
                    if (current_color != -1) {
                        ab_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    ab_append(ab, &ch, 1);
                } else {
                    int color = output_syntax_to_color(hlval);
                    if (desired_invert != current_invert) {
                        if (desired_invert) {
                            ab_append(ab, "\x1b[7m", 4);
                            current_invert = 1;
                        } else {
                            ab_append(ab, "\x1b[m", 3);
                            current_invert = 0;
                            current_color = -1;
                        }
                    }
                    if (color != current_color) {
                        current_color = color;
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf),
                                            "\x1b[%dm", color);
                        ab_append(ab, cbuf, clen);
                    }
                    ab_append(ab, &ch, 1);
                }
            }
            ab_append(ab, "\x1b[m", 3);  /* reset attributes */
        }

        ab_append(ab, "\x1b[K", 3);  /* clear to end of line */
        ab_append(ab, "\r\n", 2);

        if (row_idx < E.numrows) {
            row_line++;
            if (row_line >= output_row_render_lines(&E.row[row_idx])) {
                row_idx++;
                row_line = 0;
            }
        }
    }
}

/* ── Status bar ───────────────────────────────────────────── */

static void output_draw_status_bar(abuf *ab)
{
    ab_append(ab, "\x1b[7m", 4);   /* inverse video */

    char status[256], rstatus[80];

    int len = snprintf(status, sizeof(status), " %.40s %s",
                       E.filename ? E.filename : "[No Name]",
                       E.dirty ? "(modified)" : "");
    int buf_idx = (E.buffer_count > 0) ? (E.current_buffer + 1) : 0;
    int buf_cnt = E.buffer_count;
    const char *mode = "NORMAL";
    if (E.mode == MODE_COMMAND) mode = "COMMAND";
    else if (E.mode == MODE_INSERT) mode = "INSERT";
    else if (E.sel_mode == SEL_CHAR) mode = "VISUAL";
    else if (E.sel_mode == SEL_LINE) mode = "V-LINE";
    int rlen = snprintf(rstatus, sizeof(rstatus),
                        "buf %d/%d | %s | %s | %d/%d  Col %d ",
                        buf_idx, buf_cnt,
                        mode,
                        E.syntax ? E.syntax->filetype : "no ft",
                        E.cy + 1, E.numrows, E.cx + 1);

    if (len < 0) len = 0;
    if (rlen < 0) rlen = 0;
    if ((size_t)len >= sizeof(status))
        len = (int)sizeof(status) - 1;
    if ((size_t)rlen >= sizeof(rstatus))
        rlen = (int)sizeof(rstatus) - 1;

    if (len > E.screencols) len = E.screencols;
    ab_append_sanitized(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append_sanitized(ab, rstatus, rlen);
            break;
        }
        ab_append(ab, " ", 1);
        len++;
    }

    ab_append(ab, "\x1b[m", 3);    /* reset */
    ab_append(ab, "\r\n", 2);
}

/* ── Message bar ──────────────────────────────────────────── */

static void output_draw_message_bar(abuf *ab)
{
    ab_append(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        ab_append_sanitized(ab, E.statusmsg, msglen);
}

/* ── Full screen refresh ──────────────────────────────────── */

void output_refresh_screen(void)
{
    git_refresh_signs();
    output_scroll();

    abuf ab;
    ab_init(&ab);

    ab_append(&ab, "\x1b[?25l", 6);  /* hide cursor          */
    ab_append(&ab, "\x1b[?7l", 6);   /* disable line wrap    */
    ab_append(&ab, "\x1b[H",    3);  /* cursor to top-left   */

    output_draw_rows(&ab);
    output_draw_status_bar(&ab);
    output_draw_message_bar(&ab);

    /* Position the cursor */
    char buf[32];
    int cursor_render = output_cursor_render_row();
    int cursor_col = output_cursor_render_col();
    int cursor_row = cursor_render - E.rowoff;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_col < 0) cursor_col = 0;
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       cursor_row + 1,
                       cursor_col + 1);
    ab_append(&ab, buf, len);

    ab_append(&ab, "\x1b[?7h", 6);  /* re-enable line wrap */
    ab_append(&ab, "\x1b[?25h", 6);  /* show cursor */

    /* Single complete write keeps refreshes flicker-free. */
    terminal_write_all(ab.b, (size_t)ab.len);
    ab_free(&ab);
}
