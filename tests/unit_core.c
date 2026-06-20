#include "buffer.h"
#include "editor.h"
#include "file_io.h"
#include "git.h"
#include "output.h"
#include "undo.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
    } \
} while (0)

#define CHECK_INT(actual, expected) do { \
    int a_ = (actual); \
    int e_ = (expected); \
    tests_run++; \
    if (a_ != e_) { \
        fprintf(stderr, "FAIL %s:%d: %s == %d, expected %d\n", \
                __FILE__, __LINE__, #actual, a_, e_); \
        tests_failed++; \
    } \
} while (0)

#define CHECK_STR(actual, expected) do { \
    const char *a_ = (actual); \
    const char *e_ = (expected); \
    tests_run++; \
    if (!a_ || strcmp(a_, e_) != 0) { \
        fprintf(stderr, "FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_); \
        tests_failed++; \
    } \
} while (0)

#define CHECK_BYTES(actual, actual_len, expected) do { \
    const char *a_ = (actual); \
    const char *e_ = (expected); \
    int alen_ = (actual_len); \
    int elen_ = (int)strlen(e_); \
    tests_run++; \
    if (!a_ || alen_ != elen_ || memcmp(a_, e_, (size_t)elen_) != 0) { \
        fprintf(stderr, "FAIL %s:%d: byte buffer mismatch for %s\n", \
                __FILE__, __LINE__, #actual); \
        tests_failed++; \
    } \
} while (0)

static void free_current_state(void)
{
    for (int i = 0; i < E.numrows; i++)
        buffer_free_row(&E.row[i]);
    free(E.row);
    free(E.filename);
    free(E.clipboard);
    free(E.mcursors);
    free(E.git_root);
    free(E.git_gitdir);
    free(E.git_relpath);
    if (E.git_base_lines) {
        for (int i = 0; i < E.git_base_count; i++)
            free(E.git_base_lines[i]);
        free(E.git_base_lines);
    }
    free(E.git_signs);
    undo_stack_free(&E.undo);
    undo_stack_free(&E.redo);

    for (int i = 0; i < E.buffer_count; i++) {
        editor_buffer *buf = &E.buffers[i];
        for (int r = 0; r < buf->numrows; r++)
            buffer_free_row(&buf->row[r]);
        free(buf->row);
        free(buf->filename);
        free(buf->git_root);
        free(buf->git_gitdir);
        free(buf->git_relpath);
        if (buf->git_base_lines) {
            for (int r = 0; r < buf->git_base_count; r++)
                free(buf->git_base_lines[r]);
            free(buf->git_base_lines);
        }
        free(buf->git_signs);
        undo_stack_free(&buf->undo);
        undo_stack_free(&buf->redo);
    }
    free(E.buffers);
    free(E.render_prefix);
}

static void reset_editor_state(void)
{
    free_current_state();
    memset(&E, 0, sizeof(E));
    E.screenrows = 22;
    E.screencols = 80;
    E.ends_with_newline = 1;
    E.saved_len = 0;
    E.saved_hash = 1469598103934665603ULL;
    E.mode = MODE_INSERT;
    E.show_line_numbers = 1;
    E.auto_indent = 1;
    E.show_git_gutter = 0;
    E.git_signs_dirty = 1;
    undo_stack_init(&E.undo);
    undo_stack_init(&E.redo);
    E.undo_recording = 1;
}

static char *rows_string(int *len)
{
    return buffer_rows_to_string(len);
}

static void insert_text(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '\n')
            (void)buffer_insert_newline();
        else
            buffer_insert_char((unsigned char)*p);
    }
}

static char *join_path(const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    char *out = malloc(dlen + nlen + 2);
    if (!out) return NULL;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return out;
}

static char *read_file(const char *path, size_t *out_len)
{
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static int dir_has_temp_for(const char *dir, const char *base)
{
    DIR *dp = opendir(dir);
    if (!dp) return 1;
    char prefix[256];
    snprintf(prefix, sizeof(prefix), ".%s.tmp.", base);
    size_t plen = strlen(prefix);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dp);
    return found;
}

static int write_file_exact(const char *path, const char *data)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, fp) == len;
    if (fclose(fp) != 0) ok = 0;
    return ok;
}

static void remove_tree(const char *path)
{
    DIR *dp = opendir(path);
    if (!dp) {
        (void)unlink(path);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char *child = join_path(path, ent->d_name);
        if (!child) continue;
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            (void)unlink(child);
        }
        free(child);
    }

    closedir(dp);
    (void)rmdir(path);
}

static int run_cmd(const char *cwd, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0)
            _exit(127);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int git_available_for_tests(void)
{
    char *const args[] = { "git", "--version", NULL };
    return run_cmd(NULL, args);
}

static int run_git(const char *cwd, char *const args[])
{
    return run_cmd(cwd, args);
}

static void test_row_rendering_and_coordinates(void)
{
    reset_editor_state();
    CHECK(buffer_insert_row(0, "a\tb", 3));
    CHECK_INT(E.numrows, 1);
    CHECK_INT(E.row[0].size, 3);
    CHECK_STR(E.row[0].render, "a   b");
    CHECK_INT(E.row[0].rsize, 5);
    CHECK_INT(buffer_cx_to_rx(&E.row[0], -10), 0);
    CHECK_INT(buffer_cx_to_rx(&E.row[0], 2), 4);
    CHECK_INT(buffer_cx_to_rx(&E.row[0], 99), 5);
    CHECK_INT(buffer_rx_to_cx(&E.row[0], -1), 0);
    CHECK_INT(buffer_rx_to_cx(&E.row[0], 4), 2);
    CHECK(!buffer_insert_row(99, "x", 1));
}

static void test_insert_delete_undo_redo_grouping(void)
{
    reset_editor_state();
    insert_text("abc");
    CHECK_INT(E.numrows, 1);
    CHECK_STR(E.row[0].chars, "abc");
    CHECK_INT(E.dirty, 1);
    editor_mark_saved();
    CHECK_INT(E.dirty, 0);

    insert_text("d");
    CHECK_STR(E.row[0].chars, "abcd");
    CHECK_INT(E.dirty, 1);

    undo_perform_undo();
    CHECK_INT(E.numrows, 1);
    CHECK_STR(E.row[0].chars, "abc");
    CHECK_INT(E.cx, 3);
    CHECK_INT(E.dirty, 0);

    undo_perform_redo();
    CHECK_STR(E.row[0].chars, "abcd");
    CHECK_INT(E.cx, 4);
    CHECK_INT(E.dirty, 1);

    buffer_delete_char();
    buffer_delete_char();
    CHECK_STR(E.row[0].chars, "ab");
    undo_perform_undo();
    CHECK_STR(E.row[0].chars, "abcd");
}

static void test_empty_newline_undo_redo(void)
{
    reset_editor_state();
    CHECK_INT(buffer_insert_newline(), 0);
    CHECK_INT(E.numrows, 1);
    CHECK_INT(E.cy, 1);
    undo_perform_undo();
    CHECK_INT(E.numrows, 0);
    CHECK_INT(E.cy, 0);
    undo_perform_redo();
    CHECK_INT(E.numrows, 1);
    CHECK_INT(E.cy, 1);
}

static void test_autoindent_duplicate_and_trim(void)
{
    reset_editor_state();
    CHECK(buffer_insert_row(0, "    if (x) {", 12));
    E.cy = 0;
    E.cx = E.row[0].size;
    CHECK_INT(buffer_insert_newline(), 8);
    CHECK_INT(E.numrows, 2);
    CHECK_INT(E.row[1].size, 8);
    CHECK_STR(E.row[1].chars, "        ");

    reset_editor_state();
    CHECK(buffer_insert_row(0, "abc  ", 5));
    CHECK(buffer_insert_row(1, "x\t", 2));
    CHECK_INT(buffer_trim_trailing_whitespace(), 3);
    CHECK_STR(E.row[0].chars, "abc");
    CHECK_STR(E.row[1].chars, "x");
    E.cy = 0;
    E.cx = 2;
    buffer_duplicate_line();
    CHECK_INT(E.numrows, 3);
    CHECK_STR(E.row[1].chars, "abc");
    CHECK_INT(E.cy, 1);
    CHECK_INT(E.cx, 2);
}

static void test_serialization_and_size_guards(void)
{
    reset_editor_state();
    CHECK(buffer_insert_row(0, "one", 3));
    CHECK(buffer_insert_row(1, "two", 3));
    int len = 0;
    char *s = rows_string(&len);
    CHECK_INT(len, 8);
    CHECK_BYTES(s, len, "one\ntwo\n");
    free(s);

    CHECK(!buffer_row_insert_char(&E.row[0], 0, UCHAR_MAX + 1));
    CHECK(!buffer_row_append_string(&E.row[0], "x",
                                    (size_t)OPUSEDIT_MAX_ROW_SIZE));
}

static void test_append_buffer_and_save_rejections(void)
{
    reset_editor_state();
    abuf ab;
    ab_init(&ab);
    ab_append(&ab, "hi", 2);
    ab_append(&ab, " there", 6);
    CHECK_INT(ab.len, 8);
    CHECK(ab.b != NULL);
    CHECK(memcmp(ab.b, "hi there", 8) == 0);
    ab_append(&ab, NULL, 10);
    ab_append(&ab, "ignored", -1);
    CHECK_INT(ab.len, 8);
    ab_free(&ab);
    CHECK(ab.b == NULL);
    CHECK_INT(ab.len, 0);
    CHECK_INT(ab.cap, 0);

    abuf huge = { NULL, INT_MAX - 1, INT_MAX - 1 };
    ab_append(&huge, "abc", 3);
    CHECK_INT(huge.len, INT_MAX - 1);
    CHECK(huge.b == NULL);

    char tmpl[] = "/tmp/opusedit-save-reject-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;

    char *path = join_path(dir, "ok.txt");
    CHECK(path != NULL);
    if (!path) {
        rmdir(dir);
        return;
    }

    CHECK(buffer_insert_row(0, "x", 1));
    CHECK_INT(E.dirty, 1);
    CHECK(!file_save_as(""));
    CHECK_INT(E.dirty, 1);
    CHECK(!file_save_as(dir));
    CHECK_INT(E.dirty, 1);
    CHECK(file_save_as(path));
    CHECK_INT(E.dirty, 0);

    unlink(path);
    rmdir(dir);
    free(path);
}

static void test_wrap_mapping_and_syntax(void)
{
    reset_editor_state();
    E.show_line_numbers = 0;
    E.screencols = 5;
    CHECK(buffer_insert_row(0, "abcdefghij", 10));
    CHECK_INT(output_text_width(), 5);
    CHECK_INT(output_total_render_rows(), 2);
    CHECK_INT(output_row_render_index(0), 0);
    int row = -1;
    int line = -1;
    output_render_row_to_file(1, &row, &line);
    CHECK_INT(row, 0);
    CHECK_INT(line, 1);

    reset_editor_state();
    E.filename = strdup("test.c");
    CHECK(E.filename != NULL);
    CHECK(buffer_insert_row(0, "int x = 42; // hi", 17));
    output_select_syntax_highlight();
    CHECK(E.syntax != NULL);
    CHECK(E.row[0].hl != NULL);
    CHECK_INT(E.row[0].hl[0], HL_KEYWORD2);
    CHECK_INT(E.row[0].hl[8], HL_NUMBER);
    CHECK_INT(E.row[0].hl[12], HL_COMMENT);
}

static void test_file_save_open_and_permissions(void)
{
    reset_editor_state();
    char tmpl[] = "/tmp/opusedit-unit-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;

    char *path = join_path(dir, "sample.txt");
    CHECK(path != NULL);
    if (!path) return;

    CHECK(buffer_insert_row(0, "alpha", 5));
    CHECK(file_save_as(path));

    size_t file_len = 0;
    char *contents = read_file(path, &file_len);
    CHECK_STR(contents, "alpha\n");
    CHECK_INT((int)file_len, 6);
    free(contents);
    CHECK(!dir_has_temp_for(dir, "sample.txt"));

    CHECK(chmod(path, 0600) == 0);
    buffer_row_insert_char(&E.row[0], E.row[0].size, '!');
    CHECK(file_save_as(path));
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK_INT((int)(st.st_mode & 0777), 0600);

    reset_editor_state();
    CHECK(file_open(path));
    CHECK_INT(E.numrows, 1);
    CHECK_STR(E.row[0].chars, "alpha!");

    reset_editor_state();
    CHECK(buffer_insert_row(0, "keep", 4));
    E.filename = strdup("current.txt");
    CHECK(E.filename != NULL);
    CHECK(!file_open(NULL));
    CHECK(!file_open(""));
    CHECK(!file_open(dir));
    CHECK_INT(E.numrows, 1);
    CHECK_STR(E.row[0].chars, "keep");
    CHECK_STR(E.filename, "current.txt");

    char *new_path = join_path(dir, "new.txt");
    CHECK(new_path != NULL);
    if (new_path) {
        CHECK(file_open(new_path));
        CHECK_INT(E.numrows, 0);
        CHECK_INT(E.ends_with_newline, 1);
        CHECK_INT(E.dirty, 0);
        CHECK_STR(E.filename, new_path);
        CHECK(!dir_has_temp_for(dir, "new.txt"));
        free(new_path);
    }

    unlink(path);
    rmdir(dir);
    free(path);
}

static void test_file_preserves_missing_final_newline(void)
{
    reset_editor_state();
    char tmpl[] = "/tmp/opusedit-nonewline-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;

    char *path = join_path(dir, "plain.txt");
    CHECK(path != NULL);
    if (!path) {
        rmdir(dir);
        return;
    }

    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp) {
        CHECK_INT((int)fwrite("plain", 1, 5, fp), 5);
        CHECK(fclose(fp) == 0);
    }

    CHECK(file_open(path));
    CHECK_INT(E.ends_with_newline, 0);
    CHECK(file_save_as(path));
    size_t file_len = 0;
    char *contents = read_file(path, &file_len);
    CHECK_BYTES(contents, (int)file_len, "plain");
    free(contents);

    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp) {
        CHECK_INT((int)fwrite("plain\n", 1, 6, fp), 6);
        CHECK(fclose(fp) == 0);
    }

    CHECK(file_open(path));
    CHECK_INT(E.ends_with_newline, 1);
    CHECK(file_save_as(path));
    contents = read_file(path, &file_len);
    CHECK_BYTES(contents, (int)file_len, "plain\n");
    free(contents);

    unlink(path);
    rmdir(dir);
    free(path);
}

static void test_file_save_symlink_umask_and_long_name(void)
{
    reset_editor_state();
    char tmpl[] = "/tmp/opusedit-save-edges-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;

    char *target = join_path(dir, "target.txt");
    char *link = join_path(dir, "link.txt");
    CHECK(target != NULL);
    CHECK(link != NULL);
    if (!target || !link) {
        free(target);
        free(link);
        rmdir(dir);
        return;
    }

    FILE *fp = fopen(target, "wb");
    CHECK(fp != NULL);
    if (fp) {
        CHECK_INT((int)fwrite("base\n", 1, 5, fp), 5);
        CHECK(fclose(fp) == 0);
    }

    CHECK(symlink(target, link) == 0);
    CHECK(file_open(link));
    buffer_insert_char('!');
    file_save();
    size_t file_len = 0;
    char *contents = read_file(target, &file_len);
    CHECK_BYTES(contents, (int)file_len, "!base\n");
    free(contents);
    struct stat st;
    CHECK(lstat(link, &st) == 0);
    CHECK(S_ISLNK(st.st_mode));

    reset_editor_state();
    char *umask_path = join_path(dir, "umask.txt");
    CHECK(umask_path != NULL);
    if (umask_path) {
        CHECK(buffer_insert_row(0, "secret", 6));
        mode_t old_mask = umask(0077);
        CHECK(file_save_as(umask_path));
        (void)umask(old_mask);
        CHECK(stat(umask_path, &st) == 0);
        CHECK_INT((int)(st.st_mode & 0777), 0600);
        unlink(umask_path);
        free(umask_path);
    }

    reset_editor_state();
    int name_len = NAME_MAX > 20 ? NAME_MAX - 5 : 20;
    char *name = malloc((size_t)name_len + 1);
    CHECK(name != NULL);
    if (name) {
        memset(name, 'a', (size_t)name_len);
        name[name_len] = '\0';
        char *long_path = join_path(dir, name);
        CHECK(long_path != NULL);
        if (long_path) {
            CHECK(buffer_insert_row(0, "long", 4));
            CHECK(file_save_as(long_path));
            contents = read_file(long_path, &file_len);
            CHECK_BYTES(contents, (int)file_len, "long\n");
            free(contents);
            unlink(long_path);
            free(long_path);
        }
        free(name);
    }

    unlink(link);
    unlink(target);
    rmdir(dir);
    free(link);
    free(target);
}

static void assert_clean_git_gutter_for_file(const char *path)
{
    reset_editor_state();
    CHECK(file_open(path));
    E.show_git_gutter = 1;
    git_refresh_signs();
    CHECK_INT(E.git_available, 1);
    CHECK_INT(E.git_tracked, 1);
    CHECK_INT(git_sign_for_row(0), ' ');
}

static void test_git_packed_objects_and_worktrees(void)
{
    if (!git_available_for_tests())
        return;

    reset_editor_state();
    char tmpl[] = "/tmp/opusedit-git-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;

    char *repo = join_path(dir, "repo");
    char *linked = join_path(dir, "linked");
    char *tracked = NULL;
    char *linked_tracked = NULL;
    char *untracked = NULL;
    int worktree_added = 0;

    CHECK(repo != NULL);
    CHECK(linked != NULL);
    if (!repo || !linked)
        goto cleanup;

    CHECK(mkdir(repo, 0700) == 0);
    tracked = join_path(repo, "tracked.txt");
    linked_tracked = join_path(linked, "tracked.txt");
    untracked = join_path(repo, "new.txt");
    CHECK(tracked != NULL);
    CHECK(linked_tracked != NULL);
    CHECK(untracked != NULL);
    if (!tracked || !linked_tracked || !untracked)
        goto cleanup;

    char *const init_args[] = { "git", "init", "-q", NULL };
    char *const email_args[] = {
        "git", "config", "user.email", "opusedit@example.invalid", NULL
    };
    char *const name_args[] = {
        "git", "config", "user.name", "OpusEdit Tests", NULL
    };
    char *const add_args[] = { "git", "add", "tracked.txt", NULL };
    char *const commit_args[] = {
        "git", "commit", "-q", "-m", "base", NULL
    };
    char *const repack_args[] = { "git", "repack", "-ad", NULL };
    char *const prune_packed_args[] = { "git", "prune-packed", NULL };

    CHECK(run_git(repo, init_args));
    CHECK(run_git(repo, email_args));
    CHECK(run_git(repo, name_args));
    CHECK(write_file_exact(tracked, "base\n"));
    CHECK(run_git(repo, add_args));
    CHECK(run_git(repo, commit_args));
    CHECK(run_git(repo, repack_args));
    CHECK(run_git(repo, prune_packed_args));

    assert_clean_git_gutter_for_file(tracked);

    char *const worktree_args[] = {
        "git", "worktree", "add", "-q", "-b", "opusedit-side", linked,
        "HEAD", NULL
    };
    worktree_added = run_git(repo, worktree_args);
    CHECK(worktree_added);
    if (worktree_added)
        assert_clean_git_gutter_for_file(linked_tracked);

    reset_editor_state();
    CHECK(write_file_exact(untracked, "new\n"));
    CHECK(file_open(untracked));
    E.show_git_gutter = 1;
    git_refresh_signs();
    CHECK_INT(E.git_available, 1);
    CHECK_INT(E.git_tracked, 0);
    CHECK_INT(git_sign_for_row(0), '+');

cleanup:
    if (worktree_added) {
        char *const remove_args[] = {
            "git", "worktree", "remove", "-f", linked, NULL
        };
        (void)run_git(repo, remove_args);
    }
    reset_editor_state();
    remove_tree(dir);
    free(untracked);
    free(linked_tracked);
    free(tracked);
    free(linked);
    free(repo);
}

int main(void)
{
    test_row_rendering_and_coordinates();
    test_insert_delete_undo_redo_grouping();
    test_empty_newline_undo_redo();
    test_autoindent_duplicate_and_trim();
    test_serialization_and_size_guards();
    test_append_buffer_and_save_rejections();
    test_wrap_mapping_and_syntax();
    test_file_save_open_and_permissions();
    test_file_preserves_missing_final_newline();
    test_file_save_symlink_umask_and_long_name();
    test_git_packed_objects_and_worktrees();
    reset_editor_state();

    if (tests_failed) {
        fprintf(stderr, "%d/%d assertions failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("unit_core: %d assertions passed\n", tests_run);
    return 0;
}
