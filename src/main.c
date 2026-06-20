/*
 * main.c – OpusEdit entry point.
 *
 * Initialises the terminal, editor state, and enters the main loop.
 *
 * Usage:  opusedit [file ...]
 *
 * Key bindings:
 *   Ctrl-Q        Quit (press twice if unsaved changes exist)
 *   Ctrl-S        Save
 *   Ctrl-N        New buffer
 *   Ctrl-O        Open file in new buffer
 *   Ctrl-F        Incremental search
 *   Ctrl-R        Find and replace
 *   Ctrl-B        Next buffer
 *   Ctrl-P        Previous buffer
 *   Ctrl-W        Close buffer
 *   Ctrl-Z        Undo
 *   Ctrl-Y        Redo
 *   Ctrl-G        Go to line
 *   Ctrl-D        Duplicate line
 *   Ctrl-L        Toggle line numbers
 *   Ctrl-C/X/V    Copy/Cut/Paste (linewise if no selection)
 *   Ctrl-↑/↓      Add cursor above/below
 *   Esc           Normal mode
 *   i             Insert mode
 *   :             Command mode
 *   v / V         Visual select (char/line)
 *   y / d / p     Copy / Cut / Paste (Normal mode)
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

#include <stdio.h>
#include <stdlib.h>

static int main_current_buffer_is_empty_untitled(void)
{
    return E.buffer_count == 1 && E.filename == NULL
        && E.numrows == 0 && E.dirty == 0;
}

static void main_open_startup_file(const char *path, int *opened_any)
{
    int use_current = !*opened_any && main_current_buffer_is_empty_untitled();
    if (!use_current) {
        int before_count = editor_buffer_count();
        int before_index = editor_buffer_index();
        editor_buffer_new();
        if (editor_buffer_count() == before_count
            && editor_buffer_index() == before_index) {
            return;
        }
    }

    if (file_open(path)) {
        *opened_any = 1;
        return;
    }

    char status[sizeof(E.statusmsg)];
    snprintf(status, sizeof(status), "%s", E.statusmsg);
    if (!use_current)
        editor_buffer_close();
    editor_set_status_message("%s", status);
}

int main(int argc, char *argv[])
{
    terminal_enable_raw_mode();
    editor_init();
    terminal_install_signal_handlers();

    if (argc >= 2) {
        int opened_any = 0;
        for (int i = 1; i < argc; i++)
            main_open_startup_file(argv[i], &opened_any);
    }

    if (E.statusmsg[0] == '\0') {
        editor_set_status_message(
            "HELP: Esc=normal | i=insert | :=command | v/V=visual | y/d/p=copy/cut/paste "
            "| Ctrl-S=save | Ctrl-Q=quit | Ctrl-N=new | Ctrl-O=open | Ctrl-F=find "
            "| Ctrl-R=replace | Ctrl-B/P=next/prev buf | Ctrl-W=close "
            "| Ctrl-Arrow Up/Down=add cursor | Ctrl-Z=undo | Ctrl-Y=redo "
            "| Ctrl-G=goto | Ctrl-D=dup | Ctrl-L=lines");
    }

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
