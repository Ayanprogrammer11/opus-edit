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
#include <stdint.h>
#include <termios.h>
#include <time.h>
#include "undo.h"

/* ── Build-wide constants ─────────────────────────────────── */
#define OPUSEDIT_VERSION   "2.0.0"
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

/* ── Editor modes ────────────────────────────────────────── */
typedef enum editor_mode {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_COMMAND
} editor_mode;

/* ── Syntax flags ─────────────────────────────────────────── */
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)
#define HL_HIGHLIGHT_TRIPLE_STRINGS (1 << 2)

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
    int            hl_open_string;   /* row ends inside a triple string?  */
} erow;

/* ── Selection mode ──────────────────────────────────────── */
typedef enum editor_sel_mode {
    SEL_NONE = 0,
    SEL_CHAR,
    SEL_LINE
} editor_sel_mode;

/* ── Multi-cursor ────────────────────────────────────────── */
typedef struct editor_cursor {
    int cx;
    int cy;
} editor_cursor;

/* ── Editor buffer (one file) ─────────────────────────────── */
typedef struct editor_buffer {
    /* Cursor position in file coordinates */
    int cx, cy;
    /* Rendered x position (accounts for tabs) */
    int rx;
    /* Scroll offsets */
    int rowoff;
    int coloff;
    /* File content */
    int   numrows;
    erow *row;
    int   ends_with_newline;
    /* Status */
    int   dirty;
    size_t saved_len;
    uint64_t saved_hash;
    char *filename;
    /* Syntax */
    editor_syntax *syntax;
    /* Git gutter */
    int   show_git_gutter;
    int   git_available;
    int   git_tracked;
    int   git_signs_dirty;
    char *git_root;
    char *git_gitdir;
    char *git_relpath;
    char **git_base_lines;
    int   git_base_count;
    char *git_signs;
    int   git_signs_count;
    /* Undo/redo */
    undo_stack undo;
    undo_stack redo;
    int        undo_recording;   /* 1 = normal, 0 = replaying undo/redo */
} editor_buffer;

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
    int   ends_with_newline;
    /* Status */
    int   dirty;
    size_t saved_len;
    uint64_t saved_hash;
    char *filename;
    char  statusmsg[256];
    time_t statusmsg_time;
    /* Mode */
    editor_mode mode;
    /* Selection (anchor at sel_sx/sel_sy, end is current cursor) */
    editor_sel_mode sel_mode;
    int sel_sx, sel_sy;
    /* Display options */
    int show_line_numbers;
    int auto_indent;
    /* Clipboard */
    char *clipboard;
    int   clipboard_len;
    int   clipboard_linewise;
    /* Git gutter */
    int   show_git_gutter;
    int   git_available;
    int   git_tracked;
    int   git_signs_dirty;
    char *git_root;
    char *git_gitdir;
    char *git_relpath;
    char **git_base_lines;
    int   git_base_count;
    char *git_signs;
    int   git_signs_count;
    /* Multi-cursor */
    editor_cursor *mcursors;
    int            mcursor_count;
    int            mcursor_capacity;
    /* Mode */
    /* Syntax */
    editor_syntax *syntax;
    /* Undo/redo */
    undo_stack undo;
    undo_stack redo;
    int        undo_recording;   /* 1 = normal, 0 = replaying undo/redo */
    /* Buffer management */
    editor_buffer *buffers;
    int            buffer_count;
    int            buffer_capacity;
    int            current_buffer;
    /* Original terminal state for restoration */
    struct termios orig_termios;
} editor_config;

/* Global editor state — defined in editor.c */
extern editor_config E;

/* ── Editor-level utilities ───────────────────────────────── */
void  editor_init(void);
void  editor_cleanup(void);
void  editor_set_status_message(const char *fmt, ...);
int   editor_any_buffer_dirty(void);
void  editor_mark_saved(void);
void  editor_mark_dirty(void);
void  editor_refresh_dirty_from_saved(void);
void  editor_clear_selection(void);
void  editor_clear_mcursors(void);
char *editor_prompt(const char *prompt, void (*callback)(const char *, int));
char *editor_prompt_allow_empty(const char *prompt,
                                void (*callback)(const char *, int));
void  editor_command_prompt(void);
void  editor_goto_line_prompt(void);

/* ── Buffer management ───────────────────────────────────── */
int  editor_buffer_count(void);
int  editor_buffer_index(void);
void editor_buffer_new(void);
void editor_buffer_open_prompt(void);
void editor_buffer_next(void);
void editor_buffer_prev(void);
void editor_buffer_close(void);

#endif /* OPUSEDIT_EDITOR_H */
