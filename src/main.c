/*
 * main.c – OpusEdit entry point.
 *
 * Initialises the terminal, editor state, and enters the main loop.
 *
 * Usage:  opusedit [filename]
 *
 * Key bindings:
 *   Ctrl-Q        Quit (press twice if unsaved changes exist)
 *   Ctrl-S        Save
 *   Ctrl-F        Incremental search
 *   Ctrl-Z        Undo
 *   Ctrl-Y        Redo
 *   Arrows        Cursor movement
 *   Page Up/Down  Scroll by screenful
 *   Home / End    Beginning / end of line
 *   Backspace     Delete character before cursor
 *   Delete        Delete character at cursor
 *   Enter         Insert newline
 */

#include "editor.h"
#include "terminal.h"
#include "input.h"
#include "output.h"
#include "file_io.h"

#include <stdlib.h>

int main(int argc, char *argv[])
{
    terminal_enable_raw_mode();
    editor_init();
    terminal_install_signal_handlers();

    if (argc >= 2) {
        file_open(argv[1]);
    }

    editor_set_status_message(
        "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find "
        "| Ctrl-Z = undo | Ctrl-Y = redo");

    /* ── Main loop ────────────────────────────────────────── */
    for (;;) {
        if (terminal_exit_requested()) {
            editor_cleanup();
            exit(0);
        }
        terminal_apply_pending_resize();
        output_refresh_screen();
        input_process_keypress();
    }

    return 0;
}
