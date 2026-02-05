/*
 * output.c – Screen rendering, double-buffering, and syntax highlighting.
 */

#include "output.h"
#include "editor.h"
#include "buffer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Syntax database ──────────────────────────────────────── */

static const char *C_HL_extensions[] = {
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hxx", NULL
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
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ── Separator predicate ──────────────────────────────────── */

static int is_separator(int c)
{
    return isspace(c) || c == '\0'
        || strchr(",.()+-/*=~%<>[];:!&|^{}#?", c) != NULL;
}

/* ── Syntax highlighting per row ──────────────────────────── */

void output_update_syntax(erow *row)
{
    row->hl = realloc(row->hl, (size_t)row->rsize);
    if (!row->hl) return;
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    if (!E.syntax) return;

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

    int i = 0;
    while (i < row->rsize) {
        char  c       = row->render[i];
        int   prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
        (void)prev_hl;

        /* ── Multi-line comment end ───────────────────────── */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (mce_len > 0 && !strncmp(&row->render[i], mce, (size_t)mce_len)) {
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
        if (scs_len > 0 && !strncmp(&row->render[i], scs, (size_t)scs_len)) {
            memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
            break;
        }

        /* ── Multi-line comment start ─────────────────────── */
        if (mcs_len > 0 && !strncmp(&row->render[i], mcs, (size_t)mcs_len)) {
            memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
            i += mcs_len;
            in_comment = 1;
            prev_sep   = 0;
            continue;
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
            if ((isdigit(c) && (prev_sep || row->hl[i > 0 ? i - 1 : 0] == HL_NUMBER))
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

                if (!strncmp(&row->render[i], keywords[k], (size_t)klen)
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

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    /* Propagate change to following rows */
    if (changed && row->idx + 1 < E.numrows)
        output_update_syntax(&E.row[row->idx + 1]);
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

/* ── Scrolling ────────────────────────────────────────────── */

void output_scroll(void)
{
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = buffer_cx_to_rx(&E.row[E.cy], E.cx);

    /* Vertical scroll */
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;

    /* Horizontal scroll */
    if (E.rx < E.coloff)
        E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
}

/* ── Draw text rows ───────────────────────────────────────── */

static void output_draw_rows(abuf *ab)
{
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if (filerow >= E.numrows) {
            /* Draw '~' gutter for lines past end of file */
            if (E.numrows == 0 && y == E.screenrows / 3) {
                /* Welcome message */
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "OpusEdit -- version %s", OPUSEDIT_VERSION);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
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
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;

            const char    *c  = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];

            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (iscntrl((unsigned char)c[j])) {
                    /* Render control chars as inverted letter */
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    ab_append(ab, "\x1b[7m", 4);
                    ab_append(ab, &sym, 1);
                    ab_append(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf),
                                            "\x1b[%dm", current_color);
                        ab_append(ab, cbuf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        ab_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    ab_append(ab, &c[j], 1);
                } else {
                    int color = output_syntax_to_color(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf),
                                            "\x1b[%dm", color);
                        ab_append(ab, cbuf, clen);
                    }
                    ab_append(ab, &c[j], 1);
                }
            }
            ab_append(ab, "\x1b[39m", 5);  /* reset colour */
        }

        ab_append(ab, "\x1b[K", 3);  /* clear to end of line */
        ab_append(ab, "\r\n", 2);
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
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d  Col %d ",
                        E.syntax ? E.syntax->filetype : "no ft",
                        E.cy + 1, E.numrows, E.cx + 1);

    if (len > E.screencols) len = E.screencols;
    ab_append(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append(ab, rstatus, rlen);
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
        ab_append(ab, E.statusmsg, msglen);
}

/* ── Full screen refresh ──────────────────────────────────── */

void output_refresh_screen(void)
{
    output_scroll();

    abuf ab;
    ab_init(&ab);

    ab_append(&ab, "\x1b[?25l", 6);  /* hide cursor          */
    ab_append(&ab, "\x1b[H",    3);  /* cursor to top-left   */

    output_draw_rows(&ab);
    output_draw_status_bar(&ab);
    output_draw_message_bar(&ab);

    /* Position the cursor */
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       (E.cy - E.rowoff) + 1,
                       (E.rx - E.coloff) + 1);
    ab_append(&ab, buf, len);

    ab_append(&ab, "\x1b[?25h", 6);  /* show cursor */

    /* Single write — flicker-free */
    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    ab_free(&ab);
}
