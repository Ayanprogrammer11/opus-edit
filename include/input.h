/*
 * input.h – Low-level keypress reading and command dispatch.
 *
 * Parses VT100/xterm escape sequences into logical key constants
 * and maps them to editor actions.
 */

#ifndef OPUSEDIT_INPUT_H
#define OPUSEDIT_INPUT_H

/* ── Ctrl-key macro ───────────────────────────────────────── */
#define CTRL_KEY(k)  ((k) & 0x1f)

/* ── Logical key constants ────────────────────────────────── */
enum editor_key {
    KEY_BACKSPACE  = 127,
    /* Values above 0xFF avoid collision with normal bytes */
    ARROW_LEFT     = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    CTRL_ARROW_LEFT,
    CTRL_ARROW_RIGHT,
    CTRL_ARROW_UP,
    CTRL_ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    MOUSE_SCROLL_UP,
    MOUSE_SCROLL_DOWN,
    MOUSE_EVENT
};

/*
 * Read a single keypress from stdin, blocking until one arrives.
 * Multi-byte escape sequences (arrows, function keys) are collapsed
 * into the enum values above.
 */
int input_read_key(void);

/*
 * High-level command dispatcher. Reads a keypress and executes the
 * corresponding editor action (move, insert, delete, save, quit, etc.).
 */
void input_process_keypress(void);

#endif /* OPUSEDIT_INPUT_H */
