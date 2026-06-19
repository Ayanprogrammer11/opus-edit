/*
 * file_io.c – Safe file reading and writing with permission handling.
 */

#include "file_io.h"
#include "editor.h"
#include "buffer.h"
#include "output.h"
#include "undo.h"
#include "git.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

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
    E.ends_with_newline = 1;
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

static void file_free_loaded_rows(erow *rows, int count)
{
    if (!rows) return;
    for (int i = 0; i < count; i++)
        buffer_free_row(&rows[i]);
    free(rows);
}

static int file_make_loaded_row(erow *row, int idx, const char *s, size_t len)
{
    if (!row) return 0;
    if (len > 0 && !s) return 0;
    if (len > (size_t)OPUSEDIT_MAX_ROW_SIZE) return 0;

    memset(row, 0, sizeof(*row));
    row->idx = idx;
    row->size = (int)len;

    row->chars = malloc(len + 1);
    if (!row->chars) return 0;
    if (len > 0) memcpy(row->chars, s, len);
    row->chars[len] = '\0';

    size_t tabs = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\t') tabs++;
    }
    size_t render_len = len + tabs * (size_t)(OPUSEDIT_TAB_STOP - 1);
    if (render_len > (size_t)INT_MAX - 1) {
        buffer_free_row(row);
        return 0;
    }

    row->render = malloc(render_len + 1);
    if (!row->render) {
        buffer_free_row(row);
        return 0;
    }

    int ridx = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\t') {
            row->render[ridx++] = ' ';
            while (ridx % OPUSEDIT_TAB_STOP != 0)
                row->render[ridx++] = ' ';
        } else {
            row->render[ridx++] = s[i];
        }
    }
    row->render[ridx] = '\0';
    row->rsize = ridx;
    row->hl = NULL;
    row->hl_open_comment = 0;
    row->hl_open_string = 0;
    return 1;
}

static int file_append_loaded_row(erow **rows, int *count,
                                  const char *s, size_t len)
{
    if (!rows || !count) return 0;
    if (*count == INT_MAX) return 0;

    erow row;
    if (!file_make_loaded_row(&row, *count, s, len))
        return 0;

    if ((size_t)(*count + 1) > SIZE_MAX / sizeof(erow)) {
        buffer_free_row(&row);
        return 0;
    }
    erow *tmp = realloc(*rows, (size_t)(*count + 1) * sizeof(erow));
    if (!tmp) {
        buffer_free_row(&row);
        return 0;
    }

    *rows = tmp;
    (*rows)[*count] = row;
    (*count)++;
    return 1;
}

int file_open(const char *filename)
{
    if (!filename || !*filename) {
        editor_set_status_message("Open failed: no filename.");
        return 0;
    }

    struct stat st;
    int exists = 0;
    if (stat(filename, &st) == 0) {
        exists = 1;
        if (!S_ISREG(st.st_mode)) {
            editor_set_status_message("Open failed: not a regular file.");
            return 0;
        }
    } else if (errno != ENOENT) {
        editor_set_status_message("Open failed: %s", strerror(errno));
        return 0;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp && exists) {
        editor_set_status_message("Open failed: %s", strerror(errno));
        return 0;
    }

    char *copy = strdup(filename);
    if (!copy) {
        if (fp) fclose(fp);
        editor_set_status_message("Open failed: out of memory.");
        return 0;
    }

    erow *loaded_rows = NULL;
    int loaded_count = 0;
    int loaded_ends_with_newline = exists ? 0 : 1;
    char  *line    = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    if (fp) {
        while ((linelen = getline(&line, &linecap, fp)) != -1) {
            loaded_ends_with_newline = (linelen > 0 && line[linelen - 1] == '\n');
            /* Strip trailing newline / carriage-return */
            while (linelen > 0 &&
                   (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
                linelen--;

            if (linelen > (ssize_t)OPUSEDIT_MAX_ROW_SIZE) {
                editor_set_status_message("Open failed: line too long.");
                free(line);
                fclose(fp);
                free(copy);
                file_free_loaded_rows(loaded_rows, loaded_count);
                return 0;
            }

            if (!file_append_loaded_row(&loaded_rows, &loaded_count,
                                        line, (size_t)linelen)) {
                editor_set_status_message("Open failed: out of memory.");
                free(line);
                fclose(fp);
                free(copy);
                file_free_loaded_rows(loaded_rows, loaded_count);
                return 0;
            }
        }

        if (ferror(fp)) {
            editor_set_status_message("Open failed: %s", strerror(errno));
            free(line);
            fclose(fp);
            free(copy);
            file_free_loaded_rows(loaded_rows, loaded_count);
            return 0;
        }

        if (fclose(fp) == EOF) {
            editor_set_status_message("Open failed: %s", strerror(errno));
            free(line);
            free(copy);
            file_free_loaded_rows(loaded_rows, loaded_count);
            return 0;
        }
        free(line);
    }

    file_reset_buffer_state();
    editor_clear_selection();
    editor_clear_mcursors();
    free(E.filename);
    E.filename = copy;
    E.row = loaded_rows;
    E.numrows = loaded_count;
    E.ends_with_newline = loaded_ends_with_newline;
    E.dirty = 0;

    output_select_syntax_highlight();
    git_on_file_change();
    if (exists)
        git_mark_dirty();
    return 1;
}

/* ── Save file ────────────────────────────────────────────── */

static int file_sync_fd(int fd)
{
#ifdef __linux__
    return fdatasync(fd);
#else
    return fsync(fd);
#endif
}

static char *file_dirname_dup(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return strdup(".");
    if (slash == path) return strdup("/");

    size_t len = (size_t)(slash - path);
    char *dir = malloc(len + 1);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char *file_temp_template(const char *dir)
{
    static const char name[] = ".opusedit.XXXXXX";
    size_t dlen = strlen(dir);
    int need_sep = !(dlen == 1 && dir[0] == '/');
    size_t sep_len = need_sep ? 1U : 0U;

    if (dlen > SIZE_MAX - sep_len
        || dlen + sep_len > SIZE_MAX - sizeof(name)) {
        return NULL;
    }

    size_t len = dlen + sep_len + sizeof(name);
    char *tmpl = malloc(len);
    if (!tmpl) return NULL;

    char *p = tmpl;
    memcpy(p, dir, dlen);
    p += dlen;
    if (need_sep) *p++ = '/';
    memcpy(p, name, sizeof(name));
    return tmpl;
}

static mode_t file_default_create_mode(void)
{
    mode_t mask = umask(0);
    (void)umask(mask);
    return (mode_t)(0666 & ~mask);
}

static int file_sync_parent_dir(const char *dir)
{
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(dir, flags);
    if (fd == -1) return 0;
    int ok = (fsync(fd) == 0);
    int saved = errno;
    if (close(fd) == -1 && ok) {
        saved = errno;
        ok = 0;
    }
    errno = saved;
    return ok;
}

static int file_write_to_path(const char *path)
{
    if (!path || !*path) {
        editor_set_status_message("Save failed: no filename.");
        return 0;
    }
    size_t path_len = strlen(path);
    if (path[path_len - 1] == '/') {
        editor_set_status_message("Save failed: invalid filename.");
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
     *   1. Write to a same-directory temporary file.
     *   2. fsync the temporary file.
     *   3. Atomically rename it over the destination.
     *   4. fsync the parent directory so the rename is durable.
     */
    struct stat st;
    struct stat lst;
    char *resolved_path = NULL;
    const char *write_path = path;
    mode_t mode = file_default_create_mode();

    errno = 0;
    int lstat_result = lstat(path, &lst);
    if (lstat_result == 0 && S_ISLNK(lst.st_mode)) {
        resolved_path = realpath(path, NULL);
        if (!resolved_path) {
            editor_set_status_message("Save failed: %s", strerror(errno));
            free(buf);
            return 0;
        }
        write_path = resolved_path;
    } else if (lstat_result == -1 && errno != ENOENT) {
        editor_set_status_message("Save failed: %s", strerror(errno));
        free(resolved_path);
        free(buf);
        return 0;
    }

    if (stat(write_path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            editor_set_status_message("Save failed: not a regular file.");
            free(resolved_path);
            free(buf);
            return 0;
        }
        mode = st.st_mode & 0777;
    } else if (errno != ENOENT) {
        editor_set_status_message("Save failed: %s", strerror(errno));
        free(resolved_path);
        free(buf);
        return 0;
    }

    char *dir = file_dirname_dup(write_path);
    char *tmp_path = dir ? file_temp_template(dir) : NULL;
    if (!dir || !tmp_path) {
        editor_set_status_message("Save failed: out of memory.");
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        return 0;
    }

    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        editor_set_status_message("Save failed: %s", strerror(errno));
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        return 0;
    }

    if (fchmod(fd, mode) == -1) {
        int saved = errno;
        close(fd);
        unlink(tmp_path);
        editor_set_status_message("Save failed: %s", strerror(saved));
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        return 0;
    }

    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t nw = write(fd, buf + total, (size_t)(len - total));
        if (nw < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            editor_set_status_message("Save failed: %s", strerror(saved));
            close(fd);
            unlink(tmp_path);
            free(dir);
            free(tmp_path);
            free(resolved_path);
            free(buf);
            return 0;
        }
        if (nw == 0) {
            editor_set_status_message("Save failed: write returned 0.");
            close(fd);
            unlink(tmp_path);
            free(dir);
            free(tmp_path);
            free(resolved_path);
            free(buf);
            return 0;
        }
        total += nw;
    }

    if (file_sync_fd(fd) == -1) {
        int saved = errno;
        close(fd);
        unlink(tmp_path);
        free(dir);
        free(tmp_path);
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }

    if (close(fd) == -1) {
        int saved = errno;
        unlink(tmp_path);
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }

    if (rename(tmp_path, write_path) == -1) {
        int saved = errno;
        unlink(tmp_path);
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        editor_set_status_message("Save failed: %s", strerror(saved));
        return 0;
    }

    int dirsync_ok = file_sync_parent_dir(dir);
    int dirsync_errno = errno;

    if (!dirsync_ok && dirsync_errno != EINVAL && dirsync_errno != ENOTSUP) {
        free(dir);
        free(tmp_path);
        free(resolved_path);
        free(buf);
        editor_set_status_message("Save failed after rename: %s",
                                  strerror(dirsync_errno));
        return 0;
    }

    free(dir);
    free(tmp_path);
    free(resolved_path);
    free(buf);

    E.dirty = 0;
    editor_set_status_message("%d bytes written to %s", len, path);
    return 1;
}

void file_save(void)
{
    if (!E.filename) {
        char *filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (!filename) {
            editor_set_status_message("Save aborted.");
            return;
        }
        if (!file_save_as(filename)) {
            free(filename);
            return;
        }
        free(filename);
        return;
    }
    file_write_to_path(E.filename);
}

int file_save_as(const char *path)
{
    if (!path || !*path) {
        editor_set_status_message("Save failed: no filename.");
        return 0;
    }

    char *copy = strdup(path);
    if (!copy) {
        editor_set_status_message("Save failed: out of memory.");
        return 0;
    }

    if (!file_write_to_path(path)) {
        free(copy);
        return 0;
    }

    free(E.filename);
    E.filename = copy;
    output_select_syntax_highlight();
    git_on_file_change();
    return 1;
}
