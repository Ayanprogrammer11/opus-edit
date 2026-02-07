/*
 * file_io.c – Safe file reading and writing with permission handling.
 */

#include "file_io.h"
#include "editor.h"
#include "buffer.h"
#include "output.h"
#include "undo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Platform-specific open flags ─────────────────────────── */
#ifdef __APPLE__
    /* macOS supports O_DSYNC since 10.12, but O_SYNC is always safe */
    #ifndef O_DSYNC
        #define O_DSYNC O_SYNC
    #endif
#endif

/* ── Open file ────────────────────────────────────────────── */

static void file_reset_buffer_state(void)
{
    for (int i = 0; i < E.numrows; i++)
        buffer_free_row(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
    E.dirty = 0;
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;

    undo_stack_free(&E.undo);
    undo_stack_free(&E.redo);
    undo_stack_init(&E.undo);
    undo_stack_init(&E.redo);
    E.undo_recording = 1;
}

void file_open(const char *filename)
{
    if (!filename || !*filename) {
        editor_set_status_message("Open failed: no filename.");
        return;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp && errno != ENOENT) {
        editor_set_status_message("Open failed: %s", strerror(errno));
        return;
    }

    char *copy = strdup(filename);
    if (!copy) {
        if (fp) fclose(fp);
        editor_set_status_message("Open failed: out of memory.");
        return;
    }

    file_reset_buffer_state();
    editor_clear_selection();
    editor_clear_mcursors();
    free(E.filename);
    E.filename = copy;

    output_select_syntax_highlight();

    if (!fp) {
        /* New file – that's fine, no rows to read */
        return;
    }

    char  *line    = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        /* Strip trailing newline / carriage-return */
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        if (linelen > (ssize_t)OPUSEDIT_MAX_ROW_SIZE) {
            editor_set_status_message("Open failed: line too long.");
            free(line);
            fclose(fp);
            file_reset_buffer_state();
            return;
        }

        buffer_insert_row(E.numrows, line, (size_t)linelen);
    }

    if (ferror(fp)) {
        editor_set_status_message("Open failed: %s", strerror(errno));
        free(line);
        fclose(fp);
        file_reset_buffer_state();
        return;
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

/* ── Save file ────────────────────────────────────────────── */

static int file_write_to_path(const char *path)
{
    if (!path || !*path) {
        editor_set_status_message("Save failed: no filename.");
        return 0;
    }
    int len;
    char *buf = buffer_rows_to_string(&len);
    if (!buf) {
        if (len < 0) {
            editor_set_status_message("Save failed: file too large.");
        } else {
            editor_set_status_message("Save failed: out of memory.");
        }
        return 0;
    }

    /*
     * Safe write strategy:
     *   1. Open / create with truncate.
     *   2. Write the whole buffer.
     *   3. fsync to ensure data is on disk.
     *   4. Close.
     *
     * We preserve existing permissions when the file already exists;
     * for new files we use 0644.
     */
    struct stat st;
    mode_t mode = 0644;
    if (stat(path, &st) == 0)
        mode = st.st_mode & 0777;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd == -1) {
        editor_set_status_message("Save failed: %s", strerror(errno));
        free(buf);
        return 0;
    }

    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t nw = write(fd, buf + total, (size_t)(len - total));
        if (nw < 0) {
            if (errno == EINTR) continue;
            editor_set_status_message("Save failed: %s", strerror(errno));
            close(fd);
            free(buf);
            return 0;
        }
        if (nw == 0) {
            editor_set_status_message("Save failed: write returned 0.");
            close(fd);
            free(buf);
            return 0;
        }
        total += nw;
    }

    /* Flush to disk */
#ifdef __linux__
    if (fdatasync(fd) == -1) {
        int saved = errno;
        close(fd);
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }
#else
    if (fsync(fd) == -1) {
        int saved = errno;
        close(fd);
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }
#endif

    if (close(fd) == -1) {
        int saved = errno;
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }
    free(buf);

    E.dirty = 0;
    editor_set_status_message("%d bytes written to %s", len, path);
    return 1;
}

void file_save(void)
{
    if (!E.filename) {
        E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (!E.filename) {
            editor_set_status_message("Save aborted.");
            return;
        }
        output_select_syntax_highlight();
    }
    file_write_to_path(E.filename);
}

int file_save_as(const char *path)
{
    if (!path || !*path) {
        editor_set_status_message("Save failed: no filename.");
        return 0;
    }

    if (!file_write_to_path(path)) {
        return 0;
    }

    char *copy = strdup(path);
    if (!copy) {
        editor_set_status_message(
            "Saved to %s (name not updated: OOM).", path);
        return 1;
    }
    free(E.filename);
    E.filename = copy;
    output_select_syntax_highlight();
    return 1;
}
