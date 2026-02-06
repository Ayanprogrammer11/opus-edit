/*
 * editor.h – Core data structures and shared state for OpusEdit.
 *
 * Every module includes this header. It owns the canonical definition
 * of the editor state, row representation, syntax database, and the
 * append-buffer used for flicker-free rendering.
 */

#ifndef OPUSEDIT_EDITOR_H
#define OPUSEDIT_EDITOR_H

#include <limits.h>
#include <stddef.h>
#include <termios.h>
#include <time.h>
#include "undo.h"

/* ── Build-wide constants ─────────────────────────────────── */
#define OPUSEDIT_VERSION   "1.1.0"
#define OPUSEDIT_TAB_STOP  4
#define OPUSEDIT_QUIT_TIMES 2
/*
 * Maximum row size we can safely store without overflowing render sizes.
 * Worst-case render expansion is every char = tab (size * TAB_STOP).
 */
#define OPUSEDIT_MAX_ROW_SIZE ((INT_MAX - 1) / OPUSEDIT_TAB_STOP)

/* ── Syntax highlight token types ─────────────────────────── */
enum editor_highlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

/* ── Syntax flags ─────────────────────────────────────────── */
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/* ── Syntax definition ────────────────────────────────────── */
typedef struct editor_syntax {
    const char  *filetype;
    const char **filematch;
    const char **keywords;           /* NULL-terminated; '|' suffix ⇒ type keyword */
    const char  *singleline_comment_start;
    const char  *multiline_comment_start;
    const char  *multiline_comment_end;
    int          flags;
} editor_syntax;

/* ── Editor row ───────────────────────────────────────────── */
typedef struct erow {
    int            idx;              /* index inside the file             */
    int            size;             /* length of chars (excl. '\0')      */
    int            rsize;            /* length of render (excl. '\0')     */
    char          *chars;            /* raw character data                */
    char          *render;           /* rendered characters (tabs → spaces) */
    unsigned char *hl;               /* per-character highlight tokens    */
    int            hl_open_comment;  /* row ends inside a '/ *' comment?  */
} erow;

/* ── Append buffer (double-buffering for output) ──────────── */
typedef struct abuf {
    char *b;
    int   len;
    int   cap;
} abuf;

void ab_init(abuf *ab);
void ab_append(abuf *ab, const char *s, int len);
void ab_free(abuf *ab);

/* ── Editor state ─────────────────────────────────────────── */
typedef struct editor_config {
    /* Cursor position in file coordinates */
    int cx, cy;
    /* Rendered x position (accounts for tabs) */
    int rx;
    /* Scroll offsets */
    int rowoff;
    int coloff;
    /* Terminal dimensions */
    int screenrows;
    int screencols;
    /* File content */
    int   numrows;
    erow *row;
    /* Status */
    int   dirty;
    char *filename;
    char  statusmsg[256];
    time_t statusmsg_time;
    /* Syntax */
    editor_syntax *syntax;
    /* Undo/redo */
    undo_stack undo;
    undo_stack redo;
    int        undo_recording;   /* 1 = normal, 0 = replaying undo/redo */
    /* Original terminal state for restoration */
    struct termios orig_termios;
} editor_config;

/* Global editor state — defined in editor.c */
extern editor_config E;

/* ── Editor-level utilities ───────────────────────────────── */
void  editor_init(void);
void  editor_cleanup(void);
void  editor_set_status_message(const char *fmt, ...);
char *editor_prompt(const char *prompt, void (*callback)(const char *, int));

#endif /* OPUSEDIT_EDITOR_H */
