#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mle.h"

struct git_extra_s {
    buffer_t *buffer;
    char *marks;
    bint_t marks_len;
    time_t refreshed_at;
};

static int gx_cmd_lines(cmd_context_t *ctx);
static int gx_cmd_diff(cmd_context_t *ctx);
static int gx_cmd_stage_hunk(cmd_context_t *ctx);
static int gx_cmd_conflicts(cmd_context_t *ctx);
static int gx_line_cb(cmd_context_t *ctx);
static int gx_conflict_cb(cmd_context_t *ctx);

static int gx_register(editor_t *editor, const char *name, cmd_func_t fn) {
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.name = (char *)name;
    cmd.func = fn;
    return editor_register_cmd(editor, &cmd);
}

static char *gx_capture(const char *command, size_t limit, int *status) {
    FILE *fp = popen(command, "r");
    str_t out = {0};
    char buf[4096];
    size_t n;
    int rc;
    if (!fp) return NULL;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (out.len + n > limit) n = limit > out.len ? limit - out.len : 0;
        if (n) str_append_len(&out, buf, n);
        if (out.len >= limit) break;
    }
    rc = pclose(fp);
    if (status) *status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return out.data ? out.data : strdup("");
}

static char *gx_git_diff(editor_t *editor, const char *path, int cached) {
    char *root = util_escape_shell_arg((char *)project_tools_root(editor), strlen(project_tools_root(editor)));
    char *arg = path ? util_escape_shell_arg((char *)path, strlen(path)) : NULL;
    char *command = NULL;
    char *out;
    if (root && arg) asprintf(&command, "git -C %s diff %s--no-ext-diff --no-color --unified=3 -- %s 2>/dev/null", root, cached ? "--cached " : "", arg);
    out = command ? gx_capture(command, 8 * 1024 * 1024, NULL) : NULL;
    free(root); free(arg); free(command);
    return out;
}

int git_extra_debug_marks(const char *diff, char *marks, bint_t marks_len) {
    const char *p = diff;
    int hunks = 0;
    if (!diff || !marks || marks_len < 1) return MLE_ERR;
    memset(marks, ' ', marks_len);
    while (p && *p) {
        const char *end = strchr(p, '\n');
        long old_start = 0, old_count = 1, new_start = 0, new_count = 1;
        if (strncmp(p, "@@ ", 3) == 0) {
            char *minus = strchr((char *)p, '-');
            char *plus = strchr((char *)p, '+');
            if (minus) {
                old_start = strtol(minus + 1, &minus, 10);
                if (*minus == ',') old_count = strtol(minus + 1, NULL, 10);
            }
            if (plus) {
                new_start = strtol(plus + 1, &plus, 10);
                if (*plus == ',') new_count = strtol(plus + 1, NULL, 10);
            }
            if (!new_count && new_start >= 0 && new_start < marks_len) marks[new_start ? new_start - 1 : 0] = '-';
            else for (long i = 0; i < new_count && new_start + i - 1 < marks_len; i++) {
                long at = new_start + i - 1;
                if (at >= 0) marks[at] = old_count ? '~' : '+';
            }
            (void)old_start;
            hunks++;
        }
        if (!end) break;
        p = end + 1;
    }
    return hunks;
}

static void gx_refresh(editor_t *editor, bview_t *bview) {
    git_extra_t *gx = editor->git_extra;
    char *diff;
    if (!gx || !bview || !bview->buffer || !bview->buffer->path) return;
    free(gx->marks);
    gx->marks_len = bview->buffer->line_count + 1;
    gx->marks = malloc(gx->marks_len);
    if (!gx->marks) { gx->marks_len = 0; return; }
    gx->buffer = bview->buffer;
    gx->refreshed_at = time(NULL);
    diff = gx_git_diff(editor, bview->buffer->path, 0);
    git_extra_debug_marks(diff ? diff : "", gx->marks, gx->marks_len);
    free(diff);
}

int git_extra_init(editor_t *editor) {
    if (!editor || editor->headless_mode) return MLE_OK;
    editor->git_extra = calloc(1, sizeof(*editor->git_extra));
    return editor->git_extra ? MLE_OK : MLE_ERR;
}

void git_extra_shutdown(editor_t *editor) {
    if (!editor || !editor->git_extra) return;
    free(editor->git_extra->marks);
    free(editor->git_extra);
    editor->git_extra = NULL;
}

char git_extra_line_mark(editor_t *editor, bview_t *bview, bint_t line) {
    git_extra_t *gx;
    if (!editor || !(gx = editor->git_extra) || !bview || bview->is_menu || !bview->buffer || !bview->buffer->path) return ' ';
    if (gx->buffer != bview->buffer || time(NULL) - gx->refreshed_at >= 3) gx_refresh(editor, bview);
    return gx->buffer == bview->buffer && line >= 0 && line < gx->marks_len ? gx->marks[line] : ' ';
}

static int gx_cmd_lines(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    git_extra_t *gx = ctx->editor->git_extra;
    str_t menu = {0};
    bline_t *line;
    int count = 0;
    if (!bv || !bv->buffer || !bv->buffer->path) return MLE_OK;
    gx_refresh(ctx->editor, bv);
    str_append(&menu, "  Changed lines - + added, ~ changed, - deletion nearby\n\n");
    for (line = bv->buffer->first_line; line; line = line->next) {
        char mark = line->line_index < gx->marks_len ? gx->marks[line->line_index] : ' ';
        if (mark != ' ') {
            str_sprintf(&menu, "  %c line %-6" PRIdMAX " %.*s\n", mark, line->line_index + 1,
                (int)MLE_MIN(line->data_len, 100), line->data);
            count++;
        }
    }
    if (!count) str_append(&menu, "  No unstaged line changes\n");
    editor_menu(ctx->editor, gx_line_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    return MLE_OK;
}

static int gx_line_cb(cmd_context_t *ctx) {
    bline_t *line = ctx->bview->active_cursor->mark->bline;
    char *text = strndup(line->data, line->data_len);
    char *where = strstr(text, "line ");
    long lineno = where ? strtol(where + 5, NULL, 10) : 0;
    bview_t *target = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (target && lineno > 0) { editor_set_active(ctx->editor, target); mark_move_to(target->active_cursor->mark, lineno - 1, 0); bview_center_viewport_y(target); }
    free(text);
    return MLE_OK;
}

static int gx_cmd_diff(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    char *root;
    char *path;
    char *command = NULL;
    char *spec = NULL;
    char *spec_arg = NULL;
    char *out;
    str_t menu = {0};
    if (!bv || !bv->buffer || !bv->buffer->path) return MLE_OK;
    root = util_escape_shell_arg((char *)project_tools_root(ctx->editor), strlen(project_tools_root(ctx->editor)));
    path = util_escape_shell_arg(bv->buffer->path, strlen(bv->buffer->path));
    {
        const char *repo = project_tools_root(ctx->editor);
        const char *rel = bv->buffer->path;
        size_t rn = strlen(repo);
        if (strncmp(rel, repo, rn) == 0 && rel[rn] == '/') rel += rn + 1;
        if (asprintf(&spec, "HEAD:%s", rel) >= 0 && spec) spec_arg = util_escape_shell_arg(spec, strlen(spec));
    }
    if (root && path && spec_arg) asprintf(&command,
        "cd %s && old=$(mktemp) && git show %s >\"$old\" 2>/dev/null || :; diff -y --suppress-common-lines --width=180 \"$old\" %s 2>/dev/null; rm -f \"$old\"",
        root, spec_arg, path);
    out = command ? gx_capture(command, 8 * 1024 * 1024, NULL) : NULL;
    str_append(&menu, "  Side-by-side diff: HEAD | working tree\n\n");
    if (out && out[0]) str_append(&menu, out); else str_append(&menu, "  No differences or diff utility unavailable\n");
    editor_menu(ctx->editor, NULL, menu.data, menu.len, NULL, NULL);
    free(root); free(path); free(spec); free(spec_arg); free(command); free(out); str_free(&menu);
    return MLE_OK;
}

static int gx_extract_hunk(const char *diff, bint_t current, str_t *patch) {
    const char *p = diff;
    const char *headers_end = NULL;
    const char *chosen = NULL;
    const char *chosen_end = NULL;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p + 1) : strlen(p);
        if (strncmp(p, "@@ ", 3) == 0) {
            char *plus = strchr((char *)p, '+');
            long start = plus ? strtol(plus + 1, &plus, 10) : 0;
            long count = 1;
            if (plus && *plus == ',') count = strtol(plus + 1, NULL, 10);
            if (!headers_end) headers_end = p;
            if (current + 1 >= start && current + 1 <= start + (count ? count - 1 : 0)) chosen = p;
            else if (chosen && !chosen_end) { chosen_end = p; break; }
        }
        if (!end) break;
        p += len;
    }
    if (!chosen || !headers_end) return MLE_ERR;
    if (!chosen_end) chosen_end = diff + strlen(diff);
    str_append_len(patch, (char *)diff, headers_end - diff);
    str_append_len(patch, (char *)chosen, chosen_end - chosen);
    return MLE_OK;
}

static int gx_cmd_stage_hunk(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    char *diff;
    str_t patch = {0};
    char tmp[] = "/tmp/mle-hunk-XXXXXX";
    int fd;
    char *root;
    char *arg;
    char *command = NULL;
    int status = -1;
    char *out;
    if (!bv || !bv->buffer || !bv->buffer->path) return MLE_OK;
    if (bv->buffer->is_unsaved) { MLE_SET_ERR(ctx->editor, "%s", "save the file before staging a hunk"); return MLE_OK; }
    diff = gx_git_diff(ctx->editor, bv->buffer->path, 0);
    if (!diff || gx_extract_hunk(diff, bv->active_cursor->mark->bline->line_index, &patch) != MLE_OK) {
        MLE_SET_ERR(ctx->editor, "%s", "cursor is not inside an unstaged hunk"); free(diff); return MLE_OK;
    }
    fd = mkstemp(tmp);
    if (fd < 0 || write(fd, patch.data, patch.len) != (ssize_t)patch.len) {
        if (fd >= 0) close(fd);
        unlink(tmp);
        free(diff);
        str_free(&patch);
        return MLE_ERR;
    }
    close(fd);
    root = util_escape_shell_arg((char *)project_tools_root(ctx->editor), strlen(project_tools_root(ctx->editor)));
    arg = util_escape_shell_arg(tmp, strlen(tmp));
    if (root && arg) asprintf(&command, "git -C %s apply --cached --unidiff-zero %s 2>&1", root, arg);
    out = command ? gx_capture(command, 1024 * 1024, &status) : NULL;
    unlink(tmp);
    if (status == 0) { MLE_SET_INFO(ctx->editor, "%s", "current hunk staged"); gx_refresh(ctx->editor, bv); }
    else MLE_SET_ERR(ctx->editor, "could not stage hunk: %s", out && out[0] ? out : "git apply failed");
    free(diff); str_free(&patch); free(root); free(arg); free(command); free(out);
    return MLE_OK;
}

static int gx_cmd_conflicts(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    bline_t *line;
    str_t menu = {0};
    int count = 0;
    if (!bv || !bv->buffer) return MLE_OK;
    str_append(&menu, "  Merge conflicts - Enter chooses a resolution\n\n");
    for (line = bv->buffer->first_line; line; line = line->next) {
        if (line->data_len >= 7 && strncmp(line->data, "<<<<<<<", 7) == 0) {
            str_sprintf(&menu, "  conflict at line %" PRIdMAX "  %.*s\n", line->line_index + 1, (int)line->data_len, line->data);
            count++;
        }
    }
    if (!count) str_append(&menu, "  No conflict markers in this buffer\n");
    editor_menu(ctx->editor, gx_conflict_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    return MLE_OK;
}

static int gx_conflict_cb(cmd_context_t *ctx) {
    char *line_text;
    char *where;
    long start_no;
    bview_t *target = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
    bline_t *start = NULL;
    bline_t *middle = NULL;
    bline_t *end = NULL;
    bline_t *line;
    char *answer = NULL;
    str_t ours = {0};
    str_t theirs = {0};
    line_text = strndup(ctx->bview->active_cursor->mark->bline->data, ctx->bview->active_cursor->mark->bline->data_len);
    where = strstr(line_text, "line ");
    start_no = where ? strtol(where + 5, NULL, 10) : 0;
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (!target || start_no < 1 || buffer_get_bline(target->buffer, start_no - 1, &start) != MLBUF_OK) goto done;
    for (line = start->next; line; line = line->next) {
        if (!middle && line->data_len >= 7 && strncmp(line->data, "=======", 7) == 0) middle = line;
        else if (middle && line->data_len >= 7 && strncmp(line->data, ">>>>>>>", 7) == 0) { end = line; break; }
        else if (!middle) { str_append_len(&ours, line->data, line->data_len); str_append_len(&ours, "\n", 1); }
        else { str_append_len(&theirs, line->data, line->data_len); str_append_len(&theirs, "\n", 1); }
    }
    if (!middle || !end) goto done;
    editor_prompt(ctx->editor, "resolve conflict: ours, theirs, or both?", NULL, &answer);
    if (answer && (!strcmp(answer, "ours") || !strcmp(answer, "theirs") || !strcmp(answer, "both"))) {
        mark_t *a = buffer_add_mark(target->buffer, start, 0);
        mark_t *b = buffer_add_mark(target->buffer, end, end->char_count);
        mark_delete_between(a, b);
        if (!strcmp(answer, "ours") || !strcmp(answer, "both")) mark_insert_before(a, ours.data ? ours.data : "", ours.len);
        if (!strcmp(answer, "theirs") || !strcmp(answer, "both")) mark_insert_before(a, theirs.data ? theirs.data : "", theirs.len);
        mark_destroy(a); mark_destroy(b);
        MLE_SET_INFO(ctx->editor, "%s", "conflict resolved in buffer; review and save");
    }
done:
    free(answer); free(line_text); str_free(&ours); str_free(&theirs);
    return MLE_OK;
}

int git_extra_register_commands(editor_t *editor) {
    int rc = MLE_OK;
    rc |= gx_register(editor, "cmd_git_lines", gx_cmd_lines);
    rc |= gx_register(editor, "cmd_git_diff", gx_cmd_diff);
    rc |= gx_register(editor, "cmd_git_stage_hunk", gx_cmd_stage_hunk);
    rc |= gx_register(editor, "cmd_git_conflicts", gx_cmd_conflicts);
    return rc;
}
