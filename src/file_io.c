/*
 * file_io.c – Safe file reading and writing with permission handling.
 */

#include "file_io.h"
#include "editor.h"
#include "buffer.h"
#include "output.h"

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

static void file_clear_rows(void)
{
    for (int i = 0; i < E.numrows; i++)
        buffer_free_row(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
    E.dirty = 0;
}

void file_open(const char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);
    if (!E.filename) return;

    output_select_syntax_highlight();

    FILE *fp = fopen(filename, "r");
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
            file_clear_rows();
            return;
        }

        buffer_insert_row(E.numrows, line, (size_t)linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

/* ── Save file ────────────────────────────────────────────── */

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

    int len;
    char *buf = buffer_rows_to_string(&len);
    if (!buf) {
        if (len < 0) {
            editor_set_status_message("Save failed: file too large.");
        } else {
            editor_set_status_message("Save failed: out of memory.");
        }
        return;
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
    if (stat(E.filename, &st) == 0)
        mode = st.st_mode;

    int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, mode);
    if (fd == -1) {
        editor_set_status_message("Save failed: %s", strerror(errno));
        free(buf);
        return;
    }

    ssize_t nw = write(fd, buf, (size_t)len);
    if (nw != (ssize_t)len) {
        editor_set_status_message("Save failed: partial write (%s)",
                                  strerror(errno));
        close(fd);
        free(buf);
        return;
    }

    /* Flush to disk */
#ifdef __linux__
    fdatasync(fd);
#else
    fsync(fd);
#endif

    close(fd);
    free(buf);

    E.dirty = 0;
    editor_set_status_message("%d bytes written to %s", len, E.filename);
}
