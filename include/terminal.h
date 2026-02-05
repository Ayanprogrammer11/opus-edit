/*
 * terminal.h – Raw-mode terminal management for OpusEdit.
 *
 * Handles enabling / disabling raw mode, querying terminal dimensions,
 * and installing signal handlers (SIGWINCH, SIGINT, SIGTERM).
 */

#ifndef OPUSEDIT_TERMINAL_H
#define OPUSEDIT_TERMINAL_H

/* Enter raw mode, saving original terminal attributes. */
void terminal_enable_raw_mode(void);

/* Restore the terminal to its original (cooked) mode. */
void terminal_disable_raw_mode(void);

/*
 * Query the terminal size via ioctl, falling back to cursor-probe
 * escape if ioctl fails.  Returns 0 on success, -1 on error.
 */
int terminal_get_window_size(int *rows, int *cols);

/*
 * Install signal handlers for:
 *   SIGWINCH  – update editor dimensions on terminal resize.
 *   SIGINT    – graceful Ctrl-C handling (ignored in raw mode).
 *   SIGTERM   – graceful shutdown.
 */
void terminal_install_signal_handlers(void);

#endif /* OPUSEDIT_TERMINAL_H */
