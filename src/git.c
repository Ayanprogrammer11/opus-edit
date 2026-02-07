/*
 * git.c – Minimal Git integration (no libgit2).
 *
 * Reads .git metadata and objects directly to compute a simple
 * line-based diff against HEAD for gutter signs.
 */

#include "git.h"
#include "editor.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define GIT_OID_HEX 40

/* ── Utility helpers ─────────────────────────────────────── */

static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static char *path_join(const char *a, const char *b)
{
    if (!a || !b) return NULL;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t need = alen + blen + 2;
    if (need > SIZE_MAX) return NULL;
    char *out = malloc(need);
    if (!out) return NULL;
    memcpy(out, a, alen);
    if (alen == 0 || a[alen - 1] != '/')
        out[alen++] = '/';
    memcpy(out + alen, b, blen);
    out[alen + blen] = '\0';
    return out;
}

static char *path_dirname(const char *path)
{
    if (!path || !*path) return strdup(".");
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    if (len == 0) return strdup("/");

    const char *slash = NULL;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '/') {
            slash = path + i - 1;
            break;
        }
    }
    if (!slash) return strdup(".");
    if (slash == path) return strdup("/");

    size_t out_len = (size_t)(slash - path);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    memcpy(out, path, out_len);
    out[out_len] = '\0';
    return out;
}

static int path_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int path_is_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

static char *read_text_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
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
    return buf;
}

static int read_file_bytes(const char *path, unsigned char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    unsigned char *buf = malloc((size_t)len);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    *out = buf;
    *out_len = n;
    return 1;
}

static char *abs_path_from(const char *path)
{
    if (!path) return NULL;
    if (path[0] == '/') return strdup(path);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return strdup(path);
    char *joined = path_join(cwd, path);
    if (!joined) return strdup(path);
    return joined;
}

static char *normalize_file_path(const char *path)
{
    char *abs = abs_path_from(path);
    if (!abs) return NULL;

    char *dir = path_dirname(abs);
    if (!dir) return abs;

    char *base = strrchr(abs, '/');
    const char *name = base ? base + 1 : abs;

    char resolved[PATH_MAX];
    if (realpath(dir, resolved)) {
        free(abs);
        free(dir);
        char *out = path_join(resolved, name);
        return out ? out : strdup(path);
    }

    free(dir);
    return abs;
}

/* ── Git path discovery ──────────────────────────────────── */

static int git_parse_gitdir_file(const char *path, char **out_gitdir)
{
    char *content = read_text_file(path);
    if (!content) return 0;
    char *p = content;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "gitdir:", 7) != 0) {
        free(content);
        return 0;
    }
    p += 7;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    if (*p == '\0') {
        free(content);
        return 0;
    }
    *out_gitdir = strdup(p);
    free(content);
    return *out_gitdir != NULL;
}

static int git_find_repo(const char *file_abs, char **out_root, char **out_gitdir)
{
    *out_root = NULL;
    *out_gitdir = NULL;
    char *dir = path_dirname(file_abs);
    if (!dir) return 0;

    for (;;) {
        char *dotgit = path_join(dir, ".git");
        if (dotgit) {
            if (path_is_dir(dotgit)) {
                *out_root = strdup(dir);
                *out_gitdir = dotgit;
                free(dir);
                return (*out_root && *out_gitdir);
            } else if (path_is_file(dotgit)) {
                char *gitdir = NULL;
                if (git_parse_gitdir_file(dotgit, &gitdir)) {
                    char *resolved = NULL;
                    if (gitdir[0] == '/') {
                        resolved = gitdir;
                    } else {
                        resolved = path_join(dir, gitdir);
                        free(gitdir);
                    }
                    *out_root = strdup(dir);
                    *out_gitdir = resolved;
                    free(dotgit);
                    free(dir);
                    return (*out_root && *out_gitdir);
                }
            }
            free(dotgit);
        }

        if (strcmp(dir, "/") == 0) break;
        char *parent = path_dirname(dir);
        if (!parent || strcmp(parent, dir) == 0) {
            free(parent);
            break;
        }
        free(dir);
        dir = parent;
    }

    free(dir);
    return 0;
}

static char *git_relpath_from_root(const char *root, const char *file_abs)
{
    if (!root || !file_abs) return NULL;
    size_t rlen = strlen(root);
    if (rlen == 0) return NULL;
    if (strncmp(root, file_abs, rlen) != 0) return NULL;
    if (file_abs[rlen] == '\0') return NULL;
    if (file_abs[rlen] != '/') return NULL;
    return strdup(file_abs + rlen + 1);
}

/* ── Git ref resolution ──────────────────────────────────── */

static int git_copy_oid(char out[GIT_OID_HEX + 1], const char *src)
{
    if (!src) return 0;
    for (int i = 0; i < GIT_OID_HEX; i++) {
        if (!is_hex_char(src[i])) return 0;
        out[i] = (char)tolower((unsigned char)src[i]);
    }
    out[GIT_OID_HEX] = '\0';
    return 1;
}

static int git_resolve_ref(const char *gitdir, const char *ref, char out[GIT_OID_HEX + 1])
{
    if (!gitdir || !ref) return 0;
    char *path = path_join(gitdir, ref);
    if (path) {
        char *txt = read_text_file(path);
        free(path);
        if (txt) {
            char *p = txt;
            while (*p && isspace((unsigned char)*p)) p++;
            int ok = git_copy_oid(out, p);
            free(txt);
            if (ok) return 1;
        }
    }

    char *packed = path_join(gitdir, "packed-refs");
    if (!packed) return 0;
    FILE *fp = fopen(packed, "rb");
    free(packed);
    if (!fp) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '^') continue;
        char oid[GIT_OID_HEX + 1];
        char refname[1024];
        if (sscanf(line, "%40s %1023s", oid, refname) == 2) {
            if (strcmp(refname, ref) == 0) {
                if (git_copy_oid(out, oid))
                    found = 1;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

static int git_read_head_oid(const char *gitdir, char out[GIT_OID_HEX + 1])
{
    char *path = path_join(gitdir, "HEAD");
    if (!path) return 0;
    char *txt = read_text_file(path);
    free(path);
    if (!txt) return 0;

    char *p = txt;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "ref:", 4) == 0) {
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) end--;
        *end = '\0';
        int ok = git_resolve_ref(gitdir, p, out);
        free(txt);
        return ok;
    }

    int ok = git_copy_oid(out, p);
    free(txt);
    return ok;
}

/* ── Object reading ──────────────────────────────────────── */

static int git_inflate(const unsigned char *in, size_t in_len,
                       unsigned char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) return 0;

    size_t cap = 8192;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        inflateEnd(&strm);
        return 0;
    }

    strm.next_in = (Bytef *)in;
    strm.avail_in = (uInt)in_len;
    size_t total = 0;

    for (;;) {
        if (total >= cap) {
            size_t newcap = cap * 2;
            if (newcap < cap || newcap > (size_t)INT_MAX) {
                free(buf);
                inflateEnd(&strm);
                return 0;
            }
            unsigned char *tmp = realloc(buf, newcap);
            if (!tmp) {
                free(buf);
                inflateEnd(&strm);
                return 0;
            }
            buf = tmp;
            cap = newcap;
        }

        strm.next_out = buf + total;
        strm.avail_out = (uInt)(cap - total);

        int ret = inflate(&strm, Z_NO_FLUSH);
        total = cap - (size_t)strm.avail_out;

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            free(buf);
            inflateEnd(&strm);
            return 0;
        }
    }

    inflateEnd(&strm);
    *out = buf;
    *out_len = total;
    return 1;
}

static int git_read_object(const char *gitdir, const char *oid,
                           char *out_type, size_t out_type_len,
                           unsigned char **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len = 0;
    if (!gitdir || !oid || strlen(oid) < GIT_OID_HEX) return 0;

    char dir[3] = { oid[0], oid[1], '\0' };
    const char *rest = oid + 2;

    char *objdir = path_join(gitdir, "objects");
    if (!objdir) return 0;
    char *objpath1 = path_join(objdir, dir);
    free(objdir);
    if (!objpath1) return 0;
    char *objpath = path_join(objpath1, rest);
    free(objpath1);
    if (!objpath) return 0;

    unsigned char *raw = NULL;
    size_t raw_len = 0;
    int ok = read_file_bytes(objpath, &raw, &raw_len);
    free(objpath);
    if (!ok) return 0;

    unsigned char *inflated = NULL;
    size_t inflated_len = 0;
    if (!git_inflate(raw, raw_len, &inflated, &inflated_len)) {
        free(raw);
        return 0;
    }
    free(raw);

    unsigned char *nul = memchr(inflated, '\0', inflated_len);
    if (!nul) {
        free(inflated);
        return 0;
    }
    size_t header_len = (size_t)(nul - inflated);
    char *header = malloc(header_len + 1);
    if (!header) {
        free(inflated);
        return 0;
    }
    memcpy(header, inflated, header_len);
    header[header_len] = '\0';

    char *space = strchr(header, ' ');
    if (!space) {
        free(header);
        free(inflated);
        return 0;
    }
    *space = '\0';
    strncpy(out_type, header, out_type_len - 1);
    out_type[out_type_len - 1] = '\0';
    char *size_str = space + 1;
    unsigned long declared = strtoul(size_str, NULL, 10);
    size_t data_len = inflated_len - header_len - 1;
    if (declared > data_len) {
        free(header);
        free(inflated);
        return 0;
    }

    unsigned char *data = malloc(data_len);
    if (!data) {
        free(header);
        free(inflated);
        return 0;
    }
    memcpy(data, nul + 1, data_len);
    free(header);
    free(inflated);

    *out_data = data;
    *out_len = data_len;
    return 1;
}

static void git_oid_to_hex(const unsigned char *oid, char out[GIT_OID_HEX + 1])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2] = hex[(oid[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[oid[i] & 0xf];
    }
    out[GIT_OID_HEX] = '\0';
}

static int git_commit_tree_oid(const char *gitdir, const char *commit_oid,
                               char out_tree[GIT_OID_HEX + 1])
{
    unsigned char *data = NULL;
    size_t len = 0;
    char type[16];
    if (!git_read_object(gitdir, commit_oid, type, sizeof(type), &data, &len))
        return 0;
    int ok = 0;
    if (strcmp(type, "commit") == 0) {
        char *p = (char *)data;
        char *end = (char *)data + len;
        while (p < end) {
            if (strncmp(p, "tree ", 5) == 0) {
                p += 5;
                ok = git_copy_oid(out_tree, p);
                break;
            }
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            p = nl + 1;
        }
    }
    free(data);
    return ok;
}

static int git_is_tree_mode(const char *mode, size_t len)
{
    return (len == 5 && memcmp(mode, "40000", 5) == 0)
        || (len == 6 && memcmp(mode, "040000", 6) == 0);
}

static int git_tree_find_blob_oid(const char *gitdir, const char *tree_oid,
                                  char **parts, int idx, int count,
                                  char out_blob[GIT_OID_HEX + 1])
{
    if (idx >= count) return 0;
    unsigned char *data = NULL;
    size_t len = 0;
    char type[16];
    if (!git_read_object(gitdir, tree_oid, type, sizeof(type), &data, &len))
        return 0;
    if (strcmp(type, "tree") != 0) {
        free(data);
        return 0;
    }

    size_t pos = 0;
    int found = 0;
    while (pos < len) {
        size_t mode_start = pos;
        while (pos < len && data[pos] != ' ') pos++;
        if (pos >= len) break;
        size_t mode_len = pos - mode_start;
        pos++; /* space */

        size_t name_start = pos;
        while (pos < len && data[pos] != '\0') pos++;
        if (pos >= len) break;
        size_t name_len = pos - name_start;
        pos++; /* NUL */

        if (pos + 20 > len) break;
        const unsigned char *oid = data + pos;
        pos += 20;

        if (name_len == strlen(parts[idx])
            && memcmp(data + name_start, parts[idx], name_len) == 0) {
            if (idx == count - 1) {
                if (!git_is_tree_mode((const char *)(data + mode_start), mode_len)) {
                    git_oid_to_hex(oid, out_blob);
                    found = 1;
                }
            } else {
                if (git_is_tree_mode((const char *)(data + mode_start), mode_len)) {
                    char next_tree[GIT_OID_HEX + 1];
                    git_oid_to_hex(oid, next_tree);
                    found = git_tree_find_blob_oid(gitdir, next_tree,
                                                   parts, idx + 1, count,
                                                   out_blob);
                }
            }
            break;
        }
    }
    free(data);
    return found;
}

static char **git_split_lines(const unsigned char *data, size_t len, int *out_count)
{
    *out_count = 0;
    if (!data || len == 0) return NULL;

    int count = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') count++;
    }
    if (len > 0 && data[len - 1] != '\n') count++;
    if (count == 0) return NULL;

    char **lines = calloc((size_t)count, sizeof(char *));
    if (!lines) return NULL;

    size_t start = 0;
    int idx = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            size_t line_len = i - start;
            if (line_len > 0 && data[start + line_len - 1] == '\r')
                line_len--;
            char *line = malloc(line_len + 1);
            if (!line) {
                for (int k = 0; k < idx; k++) free(lines[k]);
                free(lines);
                return NULL;
            }
            memcpy(line, data + start, line_len);
            line[line_len] = '\0';
            lines[idx++] = line;
            start = i + 1;
        }
    }

    *out_count = idx;
    return lines;
}

/* ── Diff computation ────────────────────────────────────── */

static void git_clear_signs(void)
{
    free(E.git_signs);
    E.git_signs = NULL;
    E.git_signs_count = 0;
}

static void git_mark_all(char mark)
{
    git_clear_signs();
    if (E.numrows <= 0) return;
    E.git_signs = malloc((size_t)E.numrows);
    if (!E.git_signs) return;
    for (int i = 0; i < E.numrows; i++)
        E.git_signs[i] = mark;
    E.git_signs_count = E.numrows;
}

static void git_compute_signs_lcs(void)
{
    git_clear_signs();
    if (E.numrows <= 0) return;

    int n = E.git_base_count;
    int m = E.numrows;
    if (n == 0) {
        git_mark_all('+');
        return;
    }

    size_t cells = (size_t)(n + 1) * (size_t)(m + 1);
    if (cells > 4000000U) {
        /* Fallback: if sizes are huge, mark all as modified. */
        git_mark_all('-');
        return;
    }

    int *dp = calloc(cells, sizeof(int));
    if (!dp) {
        git_mark_all('-');
        return;
    }

    for (int i = n - 1; i >= 0; i--) {
        for (int j = m - 1; j >= 0; j--) {
            int idx = i * (m + 1) + j;
            if (strcmp(E.git_base_lines[i], E.row[j].chars) == 0) {
                dp[idx] = dp[(i + 1) * (m + 1) + (j + 1)] + 1;
            } else {
                int a = dp[(i + 1) * (m + 1) + j];
                int b = dp[i * (m + 1) + (j + 1)];
                dp[idx] = (a > b) ? a : b;
            }
        }
    }

    char *signs = malloc((size_t)m);
    if (!signs) {
        free(dp);
        return;
    }
    for (int i = 0; i < m; i++) signs[i] = ' ';

    int *ins_idx = malloc((size_t)m * sizeof(int));
    int ins_count = 0;
    int hunk_has_del = 0;

    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && strcmp(E.git_base_lines[i], E.row[j].chars) == 0) {
            if (ins_count > 0) {
                char mark = hunk_has_del ? '-' : '+';
                for (int k = 0; k < ins_count; k++)
                    signs[ins_idx[k]] = mark;
                ins_count = 0;
                hunk_has_del = 0;
            }
            i++;
            j++;
        } else if (j < m && (i == n
                   || dp[i * (m + 1) + (j + 1)] >= dp[(i + 1) * (m + 1) + j])) {
            if (ins_idx && ins_count < m)
                ins_idx[ins_count++] = j;
            j++;
        } else {
            hunk_has_del = 1;
            i++;
        }
    }

    if (ins_count > 0) {
        char mark = hunk_has_del ? '-' : '+';
        for (int k = 0; k < ins_count; k++)
            signs[ins_idx[k]] = mark;
    }

    free(ins_idx);
    free(dp);

    E.git_signs = signs;
    E.git_signs_count = m;
}

/* ── Public API ──────────────────────────────────────────── */

void git_on_file_change(void)
{
    /* Clear any previous git state */
    free(E.git_root);
    free(E.git_gitdir);
    free(E.git_relpath);
    if (E.git_base_lines) {
        for (int i = 0; i < E.git_base_count; i++)
            free(E.git_base_lines[i]);
        free(E.git_base_lines);
    }
    git_clear_signs();

    E.git_root = NULL;
    E.git_gitdir = NULL;
    E.git_relpath = NULL;
    E.git_base_lines = NULL;
    E.git_base_count = 0;
    E.git_available = 0;
    E.git_tracked = 0;
    E.git_signs_dirty = 1;

    if (!E.filename || !*E.filename) return;

    char *abs = normalize_file_path(E.filename);
    if (!abs) return;

    char *root = NULL;
    char *gitdir = NULL;
    if (!git_find_repo(abs, &root, &gitdir)) {
        free(abs);
        return;
    }

    E.git_root = root;
    E.git_gitdir = gitdir;
    E.git_available = 1;

    E.git_relpath = git_relpath_from_root(root, abs);
    free(abs);
    if (!E.git_relpath) return;

    char head_oid[GIT_OID_HEX + 1];
    if (!git_read_head_oid(E.git_gitdir, head_oid)) {
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    char tree_oid[GIT_OID_HEX + 1];
    if (!git_commit_tree_oid(E.git_gitdir, head_oid, tree_oid)) {
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    char blob_oid[GIT_OID_HEX + 1];
    char *path_copy = strdup(E.git_relpath);
    if (!path_copy) {
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }
    int part_count = 0;
    for (char *p = path_copy; *p; p++) {
        if (*p == '/') part_count++;
    }
    part_count++; /* at least one component */

    char **parts = calloc((size_t)part_count, sizeof(char *));
    if (!parts) {
        free(path_copy);
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    int idx = 0;
    char *tok = strtok(path_copy, "/");
    while (tok && idx < part_count) {
        parts[idx++] = tok;
        tok = strtok(NULL, "/");
    }
    part_count = idx;

    if (!git_tree_find_blob_oid(E.git_gitdir, tree_oid,
                                parts, 0, part_count, blob_oid)) {
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        free(parts);
        free(path_copy);
        return;
    }
    free(parts);
    free(path_copy);

    unsigned char *blob = NULL;
    size_t blob_len = 0;
    char type[16];
    if (!git_read_object(E.git_gitdir, blob_oid, type, sizeof(type),
                         &blob, &blob_len)) {
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    if (strcmp(type, "blob") != 0) {
        free(blob);
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    E.git_base_lines = git_split_lines(blob, blob_len, &E.git_base_count);
    free(blob);
    if (!E.git_base_lines) {
        E.git_base_count = 0;
        E.git_tracked = 0;
        E.git_signs_dirty = 1;
        return;
    }

    E.git_tracked = 1;
    E.git_signs_dirty = 1;
}

void git_mark_dirty(void)
{
    E.git_signs_dirty = 1;
}

void git_refresh_signs(void)
{
    if (!E.show_git_gutter) return;
    if (!E.git_available) {
        git_clear_signs();
        return;
    }
    if (!E.git_signs_dirty) return;

    if (!E.git_tracked) {
        git_mark_all('+');
        E.git_signs_dirty = 0;
        return;
    }

    git_compute_signs_lcs();
    E.git_signs_dirty = 0;
}

char git_sign_for_row(int row)
{
    if (!E.git_signs || row < 0 || row >= E.git_signs_count) return ' ';
    return E.git_signs[row];
}

int git_show_gutter(void)
{
    return E.show_git_gutter && E.git_available;
}
