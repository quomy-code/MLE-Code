#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mle.h"

typedef struct watched_file_s {
    buffer_t *buffer;
    time_t reported_mtime;
} watched_file_t;

struct workspace_s {
    char state_dir[PATH_MAX + 1];
    char session_path[PATH_MAX + 1];
    char recovery_dir[PATH_MAX + 1];
    char recent_path[PATH_MAX + 1];
    time_t last_tick;
    time_t last_recovery;
    watched_file_t watched[64];
    int watched_len;
    int restored;
    int first_run;
    char status[40];
};

static int ws_cmd_session_save(cmd_context_t *ctx);
static int ws_cmd_session_restore(cmd_context_t *ctx);
static int ws_cmd_recovery(cmd_context_t *ctx);
static int ws_cmd_recent(cmd_context_t *ctx);
static int ws_cmd_reload(cmd_context_t *ctx);
static int ws_cmd_file_format(cmd_context_t *ctx);
static int ws_recovery_cb(cmd_context_t *ctx);
static int ws_recent_cb(cmd_context_t *ctx);
static int ws_format_cb(cmd_context_t *ctx);
static int ws_welcome_cb(cmd_context_t *ctx);

static int ws_register(editor_t *editor, const char *name, cmd_func_t fn) {
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.name = (char *)name;
    cmd.func = fn;
    return editor_register_cmd(editor, &cmd);
}

static int ws_mkdir(const char *path) {
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return MLE_OK;
    return MLE_ERR;
}

static int ws_paths(workspace_t *ws) {
    const char *home = getenv("HOME");
    char root[PATH_MAX + 1];
    if (!home || !home[0]) return MLE_ERR;
    if (snprintf(root, sizeof(root), "%s/.mle", home) >= (int)sizeof(root)) return MLE_ERR;
    if (snprintf(ws->state_dir, sizeof(ws->state_dir), "%s/state", root) >= (int)sizeof(ws->state_dir)) return MLE_ERR;
    if (snprintf(ws->session_path, sizeof(ws->session_path), "%s/session", ws->state_dir) >= (int)sizeof(ws->session_path)) return MLE_ERR;
    if (snprintf(ws->recovery_dir, sizeof(ws->recovery_dir), "%s/recovery", ws->state_dir) >= (int)sizeof(ws->recovery_dir)) return MLE_ERR;
    if (snprintf(ws->recent_path, sizeof(ws->recent_path), "%s/recent", ws->state_dir) >= (int)sizeof(ws->recent_path)) return MLE_ERR;
    if (ws_mkdir(root) != MLE_OK || ws_mkdir(ws->state_dir) != MLE_OK || ws_mkdir(ws->recovery_dir) != MLE_OK) return MLE_ERR;
    return MLE_OK;
}

static uint64_t ws_hash(const char *text) {
    uint64_t h = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)(text ? text : "untitled");
    while (*p) {
        h ^= *p++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static int ws_is_blank(editor_t *editor) {
    bview_t *bv = editor->active_edit;
    return editor_bview_edit_count(editor) == 1 && bv && bv->buffer
        && !bv->buffer->path && bv->buffer->byte_count == 0;
}

static void ws_remember_cwd(workspace_t *ws) {
    char cwd[PATH_MAX + 1];
    char tmp[PATH_MAX + 32];
    FILE *in;
    FILE *out;
    char line[PATH_MAX + 2];
    int copied = 0;
    if (!getcwd(cwd, sizeof(cwd))) return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", ws->recent_path) >= (int)sizeof(tmp)) return;
    out = fopen(tmp, "wb");
    if (!out) return;
    fprintf(out, "%s\n", cwd);
    in = fopen(ws->recent_path, "rb");
    while (in && copied < 19 && fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || strcmp(line, cwd) == 0) continue;
        fprintf(out, "%s\n", line);
        copied++;
    }
    if (in) fclose(in);
    fclose(out);
    rename(tmp, ws->recent_path);
}

static int ws_write_session(editor_t *editor) {
    workspace_t *ws = editor->workspace;
    bview_t *bv;
    char tmp[PATH_MAX + 32];
    FILE *fp;
    if (!ws || snprintf(tmp, sizeof(tmp), "%s.tmp", ws->session_path) >= (int)sizeof(tmp)) return MLE_ERR;
    fp = fopen(tmp, "wb");
    if (!fp) return MLE_ERR;
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        const char *path;
        if (!MLE_BVIEW_IS_EDIT(bv) || bv->is_menu || !bv->buffer) continue;
        path = bv->buffer->path;
        if (!path || strchr(path, '\t') || strchr(path, '\n')) continue;
        fprintf(fp, "%s\t%" PRIdMAX "\t%" PRIdMAX "\t%d\n", path,
            bv->active_cursor->mark->bline->line_index + 1,
            bv->active_cursor->mark->col,
            bv == editor->active_edit ? 1 : 0);
    }
    if (fclose(fp) != 0 || rename(tmp, ws->session_path) != 0) {
        unlink(tmp);
        return MLE_ERR;
    }
    return MLE_OK;
}

static int ws_restore_session(editor_t *editor) {
    workspace_t *ws = editor->workspace;
    FILE *fp;
    char line[PATH_MAX + 128];
    bview_t *active = NULL;
    bview_t *first = NULL;
    bview_t *blank = ws_is_blank(editor) ? editor->active_edit : NULL;
    int opened = 0;
    if (!ws || !(fp = fopen(ws->session_path, "rb"))) return MLE_ERR;
    while (fgets(line, sizeof(line), fp)) {
        char *path = line;
        char *a = strchr(path, '\t');
        char *b;
        char *c;
        long lineno;
        long col;
        bview_t *bv = NULL;
        if (!a) continue;
        *a++ = '\0';
        b = strchr(a, '\t');
        if (!b) continue;
        *b++ = '\0';
        c = strchr(b, '\t');
        if (!c) continue;
        *c++ = '\0';
        lineno = strtol(a, NULL, 10);
        col = strtol(b, NULL, 10);
        c[strcspn(c, "\r\n")] = '\0';
        if (!util_is_file(path, "rb", NULL)) continue;
        editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, path, strlen(path), 0, lineno, 0, NULL, &bv);
        if (!bv) continue;
        mark_move_col(bv->active_cursor->mark, col);
        if (!first) first = bv;
        if (atoi(c)) active = bv;
        opened++;
    }
    fclose(fp);
    if (opened && blank && blank != first) editor_close_bview(editor, blank, NULL);
    if (active) editor_set_active(editor, active);
    else if (first) editor_set_active(editor, first);
    ws->restored = opened > 0;
    return opened ? MLE_OK : MLE_ERR;
}

static int ws_write_recovery(editor_t *editor) {
    workspace_t *ws = editor->workspace;
    bview_t *bv;
    char manifest[PATH_MAX + 32];
    char manifest_tmp[PATH_MAX + 40];
    FILE *mf;
    int recovered = 0;
    if (!ws) return MLE_ERR;
    snprintf(manifest, sizeof(manifest), "%s/manifest", ws->recovery_dir);
    snprintf(manifest_tmp, sizeof(manifest_tmp), "%s.tmp", manifest);
    mf = fopen(manifest_tmp, "wb");
    if (!mf) return MLE_ERR;
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        char path[PATH_MAX + 64];
        char tmp[PATH_MAX + 72];
        const char *name;
        int fd;
        if (!MLE_BVIEW_IS_EDIT(bv) || bv->is_menu || !bv->buffer || !bv->buffer->is_unsaved) continue;
        name = bv->buffer->path ? bv->buffer->path : bv->path;
        if (strlen(ws->recovery_dir) + 26 >= sizeof(path)) continue;
        snprintf(path, sizeof(path), "%s/%016" PRIx64 ".recover", ws->recovery_dir, ws_hash(name));
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0) continue;
        if (buffer_write_to_fd(bv->buffer, fd, NULL) == MLBUF_OK) {
            fsync(fd);
            close(fd);
            if (rename(tmp, path) == 0) { fprintf(mf, "%s\t%s\n", path, name ? name : "untitled"); recovered++; }
        } else {
            close(fd);
            unlink(tmp);
        }
    }
    fclose(mf);
    if (recovered) rename(manifest_tmp, manifest);
    else unlink(manifest_tmp);
    return MLE_OK;
}

static void ws_update_status(editor_t *editor) {
    workspace_t *ws = editor->workspace;
    bview_t *bv = editor->active_edit;
    bline_t *line;
    int crlf = 0;
    int bom = 0;
    if (!ws || !bv || !bv->buffer) return;
    line = bv->buffer->first_line;
    if (line && line->data_len >= 3 && (unsigned char)line->data[0] == 0xef
        && (unsigned char)line->data[1] == 0xbb && (unsigned char)line->data[2] == 0xbf) bom = 1;
    for (line = bv->buffer->first_line; line && line->next; line = line->next) {
        if (line->data_len && line->data[line->data_len - 1] == '\r') { crlf = 1; break; }
    }
    snprintf(ws->status, sizeof(ws->status), "UTF-8%s %s", bom ? "+BOM" : "", crlf ? "CRLF" : "LF");
}

int workspace_init(editor_t *editor) {
    workspace_t *ws;
    char marker[PATH_MAX + 32];
    if (!editor || editor->headless_mode) return MLE_OK;
    ws = calloc(1, sizeof(*ws));
    if (!ws) return MLE_ERR;
    editor->workspace = ws;
    if (ws_paths(ws) != MLE_OK) return MLE_OK;
    snprintf(marker, sizeof(marker), "%s/welcome_seen", ws->state_dir);
    ws->first_run = access(marker, F_OK) != 0;
    ws_remember_cwd(ws);
    if (ws_is_blank(editor)) ws_restore_session(editor);
    ws_update_status(editor);
    return MLE_OK;
}

void workspace_shutdown(editor_t *editor) {
    workspace_t *ws;
    if (!editor || !(ws = editor->workspace)) return;
    ws_write_recovery(editor);
    ws_write_session(editor);
    free(ws);
    editor->workspace = NULL;
}

void workspace_tick(editor_t *editor) {
    workspace_t *ws;
    bview_t *bv;
    time_t now;
    if (!editor || !(ws = editor->workspace) || editor->headless_mode) return;
    now = time(NULL);
    if (now == ws->last_tick) return;
    ws->last_tick = now;
    ws_update_status(editor);
    if (now - ws->last_recovery >= 10) {
        ws_write_recovery(editor);
        ws->last_recovery = now;
    }
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        struct stat st;
        int i;
        watched_file_t *w = NULL;
        if (!MLE_BVIEW_IS_EDIT(bv) || !bv->buffer || !bv->buffer->path || bv->buffer->is_unsaved) continue;
        if (stat(bv->buffer->path, &st) != 0 || st.st_mtime <= bv->buffer->st.st_mtime) continue;
        for (i = 0; i < ws->watched_len; i++) if (ws->watched[i].buffer == bv->buffer) w = &ws->watched[i];
        if (!w && ws->watched_len < (int)(sizeof(ws->watched) / sizeof(ws->watched[0]))) {
            w = &ws->watched[ws->watched_len++];
            w->buffer = bv->buffer;
        }
        if (w && w->reported_mtime != st.st_mtime) {
            w->reported_mtime = st.st_mtime;
            MLE_SET_INFO(editor, "%s", "file changed outside mle; use palette: Reload changed file");
        }
    }
}

const char *workspace_status(editor_t *editor) {
    return editor && editor->workspace ? editor->workspace->status : "";
}

static char *ws_current_line(cmd_context_t *ctx) {
    bline_t *line = ctx->bview->active_cursor->mark->bline;
    return strndup(line->data, line->data_len);
}

static int ws_cmd_session_save(cmd_context_t *ctx) {
    if (ws_write_session(ctx->editor) == MLE_OK) MLE_SET_INFO(ctx->editor, "%s", "session saved");
    else MLE_SET_ERR(ctx->editor, "%s", "could not save session");
    return MLE_OK;
}

static int ws_cmd_session_restore(cmd_context_t *ctx) {
    if (ws_restore_session(ctx->editor) == MLE_OK) MLE_SET_INFO(ctx->editor, "%s", "session restored");
    else MLE_SET_ERR(ctx->editor, "%s", "no restorable session");
    return MLE_OK;
}

static int ws_cmd_reload(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    char *path;
    if (!bv || !bv->buffer || !bv->buffer->path) return MLE_OK;
    if (bv->buffer->is_unsaved) {
        MLE_SET_ERR(ctx->editor, "%s", "buffer has unsaved edits; save or discard them first");
        return MLE_OK;
    }
    path = strdup(bv->buffer->path);
    bview_open(bv, path, strlen(path));
    free(path);
    MLE_SET_INFO(ctx->editor, "%s", "file reloaded");
    return MLE_OK;
}

static int ws_cmd_recovery(cmd_context_t *ctx) {
    workspace_t *ws = ctx->editor->workspace;
    char path[PATH_MAX + 32];
    FILE *fp;
    str_t list = {0};
    char line[PATH_MAX * 2 + 4];
    snprintf(path, sizeof(path), "%s/manifest", ws->recovery_dir);
    fp = fopen(path, "rb");
    str_append(&list, "  Recovery files - Enter opens a recovered copy\n\n");
    while (fp && fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        tab[strcspn(tab, "\r\n")] = '\0';
        str_sprintf(&list, "  %s\t%s\n", tab, line);
    }
    if (fp) fclose(fp);
    if (list.len < 60) str_append(&list, "  No recovery files\n");
    editor_menu(ctx->editor, ws_recovery_cb, list.data, list.len, NULL, NULL);
    str_free(&list);
    return MLE_OK;
}

static int ws_recovery_cb(cmd_context_t *ctx) {
    char *line = ws_current_line(ctx);
    char *tab = strrchr(line, '\t');
    bview_t *bv = NULL;
    if (tab && util_is_file(tab + 1, "rb", NULL)) {
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        editor_open_bview(ctx->editor, NULL, MLE_BVIEW_TYPE_EDIT, tab + 1, strlen(tab + 1), 1, 0, 0, NULL, &bv);
        if (bv && bv->buffer) bv->buffer->is_unsaved = 1;
    }
    free(line);
    return MLE_OK;
}

static int ws_cmd_recent(cmd_context_t *ctx) {
    workspace_t *ws = ctx->editor->workspace;
    FILE *fp = fopen(ws->recent_path, "rb");
    str_t list = {0};
    char line[PATH_MAX + 2];
    str_append(&list, "  Recent projects - Enter opens the folder\n\n");
    while (fp && fgets(line, sizeof(line), fp)) str_append(&list, line);
    if (fp) fclose(fp);
    editor_menu(ctx->editor, ws_recent_cb, list.data, list.len, NULL, NULL);
    str_free(&list);
    return MLE_OK;
}

static int ws_recent_cb(cmd_context_t *ctx) {
    char *line = ws_current_line(ctx);
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (util_is_dir(p)) {
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        side_open_dir(ctx->editor, p);
    }
    free(line);
    return MLE_OK;
}

static void ws_set_eol(buffer_t *buffer, int crlf) {
    bline_t *line;
    for (line = buffer->first_line; line && line->next; line = line->next) {
        mark_t *m = buffer_add_mark(buffer, line, line->char_count);
        int has_cr = line->data_len > 0 && line->data[line->data_len - 1] == '\r';
        if (crlf && !has_cr) mark_insert_before(m, "\r", 1);
        else if (!crlf && has_cr) mark_delete_before(m, 1);
        mark_destroy(m);
    }
}

static void ws_set_bom(buffer_t *buffer, int bom) {
    bline_t *line = buffer->first_line;
    mark_t *m;
    int has = line && line->data_len >= 3 && (unsigned char)line->data[0] == 0xef
        && (unsigned char)line->data[1] == 0xbb && (unsigned char)line->data[2] == 0xbf;
    if (!line || has == bom) return;
    m = buffer_add_mark(buffer, line, 0);
    if (bom) mark_insert_before(m, "\xef\xbb\xbf", 3);
    else mark_delete_after(m, 3);
    mark_destroy(m);
}

static int ws_cmd_file_format(cmd_context_t *ctx) {
    str_t list = {0};
    str_sprintf(&list, "  File format                         current: %s\n\n", workspace_status(ctx->editor));
    str_append(&list, "  utf8      UTF-8 without BOM\n  bom       UTF-8 with BOM\n  lf        Unix LF endings\n  crlf      Windows CRLF endings\n");
    editor_menu(ctx->editor, ws_format_cb, list.data, list.len, NULL, NULL);
    str_free(&list);
    return MLE_OK;
}

static int ws_format_cb(cmd_context_t *ctx) {
    char *line = ws_current_line(ctx);
    buffer_t *buffer = ctx->editor->active_edit ? ctx->editor->active_edit->buffer : NULL;
    if (buffer) {
        if (strstr(line, "utf8")) ws_set_bom(buffer, 0);
        else if (strstr(line, "bom")) ws_set_bom(buffer, 1);
        else if (strstr(line, "crlf")) ws_set_eol(buffer, 1);
        else if (strstr(line, "lf")) ws_set_eol(buffer, 0);
    }
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    ws_update_status(ctx->editor);
    free(line);
    return MLE_OK;
}

void workspace_maybe_welcome(editor_t *editor) {
    workspace_t *ws;
    char marker[PATH_MAX + 32];
    FILE *fp;
    static const char welcome[] =
        "  MLE Code\n\n"
        "  open       Open a file\n"
        "  recent     Recent projects\n"
        "  restore    Restore previous session\n"
        "  recover    Recover unsaved work\n"
        "  palette    Command palette (F11)\n\n"
        "  F1 guide   F2 keys   F8 settings   F9 files   F10 git\n";
    if (!editor || !(ws = editor->workspace) || !ws->first_run || ws->restored || !ws_is_blank(editor)) return;
    snprintf(marker, sizeof(marker), "%s/welcome_seen", ws->state_dir);
    fp = fopen(marker, "wb");
    if (fp) { fputs("1\n", fp); fclose(fp); }
    editor_menu(editor, ws_welcome_cb, (char *)welcome, sizeof(welcome) - 1, NULL, NULL);
}

static int ws_welcome_cb(cmd_context_t *ctx) {
    char *line = ws_current_line(ctx);
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (strstr(line, "open")) cmd_open_file(ctx);
    else if (strstr(line, "recent")) ws_cmd_recent(ctx);
    else if (strstr(line, "restore")) ws_cmd_session_restore(ctx);
    else if (strstr(line, "recover")) ws_cmd_recovery(ctx);
    else if (strstr(line, "palette")) {
        cmd_t *cmd = NULL;
        HASH_FIND_STR(ctx->editor->cmd_map, "cmd_palette", cmd);
        if (cmd) cmd->func(ctx);
    }
    free(line);
    return MLE_OK;
}

int workspace_register_commands(editor_t *editor) {
    int rc = MLE_OK;
    rc |= ws_register(editor, "cmd_session_save", ws_cmd_session_save);
    rc |= ws_register(editor, "cmd_session_restore", ws_cmd_session_restore);
    rc |= ws_register(editor, "cmd_recovery", ws_cmd_recovery);
    rc |= ws_register(editor, "cmd_recent_projects", ws_cmd_recent);
    rc |= ws_register(editor, "cmd_reload_changed", ws_cmd_reload);
    rc |= ws_register(editor, "cmd_file_format", ws_cmd_file_format);
    return rc;
}
