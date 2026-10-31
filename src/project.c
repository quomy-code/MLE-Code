#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mle.h"

typedef struct task_s {
    char name[64];
    char command[512];
} task_t;

struct project_tools_s {
    char root[PATH_MAX + 1];
    task_t tasks[32];
    int task_len;
    char *replace_find;
    char *replace_with;
    char **replace_files;
    int replace_file_len;
};

typedef struct palette_item_s {
    const char *command;
    const char *label;
} palette_item_t;

static const palette_item_t palette_items[] = {
    {"cmd_open_file", "Open file"},
    {"cmd_recent_projects", "Recent projects"},
    {"cmd_session_save", "Save workspace session"},
    {"cmd_session_restore", "Restore workspace session"},
    {"cmd_recovery", "Recover unsaved work"},
    {"cmd_project_search", "Search across project"},
    {"cmd_project_replace", "Replace across project with preview"},
    {"cmd_tasks", "Run project task"},
    {"cmd_problems", "Show last task and problems"},
    {"cmd_outline", "Show symbols in current file"},
    {"cmd_complete", "Complete word"},
    {"cmd_snippets", "Insert snippet"},
    {"cmd_fold", "Toggle code fold"},
    {"cmd_lsp", "Language server actions"},
    {"cmd_git_lines", "Changed lines in current file"},
    {"cmd_git_diff", "Compare this file with git"},
    {"cmd_git_stage_hunk", "Stage current git hunk"},
    {"cmd_git_conflicts", "Resolve merge conflicts"},
    {"cmd_git_log", "Recent commits"},
    {"cmd_git_blame", "Blame the current file"},
    {"cmd_file_action", "Create, rename or remove a file"},
    {"cmd_reload_changed", "Reload changed file"},
    {"cmd_file_format", "Encoding and line endings"},
    {"cmd_options", "Editor settings"},
    {"cmd_show_help", "All key bindings"},
    {NULL, NULL}
};

static int pt_cmd_palette(cmd_context_t *ctx);
static int pt_cmd_search(cmd_context_t *ctx);
static int pt_cmd_replace(cmd_context_t *ctx);
static int pt_cmd_tasks(cmd_context_t *ctx);
static int pt_cmd_problems(cmd_context_t *ctx);
static int pt_cmd_outline(cmd_context_t *ctx);
static int pt_cmd_file_action(cmd_context_t *ctx);
static int pt_palette_cb(cmd_context_t *ctx);
static int pt_search_cb(cmd_context_t *ctx);
static int pt_replace_cb(cmd_context_t *ctx);
static int pt_task_cb(cmd_context_t *ctx);
static int pt_problem_cb(cmd_context_t *ctx);
static int pt_outline_cb(cmd_context_t *ctx);
static int pt_file_action_cb(cmd_context_t *ctx);

static int pt_register(editor_t *editor, const char *name, cmd_func_t fn) {
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.name = (char *)name;
    cmd.func = fn;
    return editor_register_cmd(editor, &cmd);
}

static int pt_contains_ci(const char *text, const char *needle) {
    size_t n;
    const char *p;
    if (!needle || !needle[0]) return 1;
    n = strlen(needle);
    for (p = text; *p; p++) if (strncasecmp(p, needle, n) == 0) return 1;
    return 0;
}

static char *pt_line(cmd_context_t *ctx) {
    bline_t *line = ctx->bview->active_cursor->mark->bline;
    return strndup(line->data, line->data_len);
}

static char *pt_capture(const char *command, size_t limit, int *status) {
    FILE *fp;
    str_t out = {0};
    char buf[4096];
    size_t n;
    int rc;
    fp = popen(command, "r");
    if (!fp) return NULL;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (out.len + n > limit) n = limit > out.len ? limit - out.len : 0;
        if (n) str_append_len(&out, buf, n);
        if (out.len >= limit) break;
    }
    rc = pclose(fp);
    if (status) *status = rc;
    if (!out.data) out.data = strdup("");
    return out.data;
}

static void pt_find_root(project_tools_t *pt) {
    char cwd[PATH_MAX + 1];
    char probe[PATH_MAX + 1];
    if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
    snprintf(probe, sizeof(probe), "%s", cwd);
    while (probe[0]) {
        char git[PATH_MAX + 1];
        char *slash;
        if (snprintf(git, sizeof(git), "%s/.git", probe) < (int)sizeof(git) && util_is_dir(git)) {
            snprintf(pt->root, sizeof(pt->root), "%s", probe);
            return;
        }
        slash = strrchr(probe, '/');
        if (!slash) break;
        if (slash == probe) { probe[1] = '\0'; if (strcmp(probe, "/") == 0) break; }
        else *slash = '\0';
    }
    snprintf(pt->root, sizeof(pt->root), "%s", cwd);
}

static void pt_load_tasks(project_tools_t *pt) {
    char path[PATH_MAX + 32];
    FILE *fp;
    char line[1024];
    pt->task_len = 0;
    if (snprintf(path, sizeof(path), "%s/.mle-project", pt->root) >= (int)sizeof(path)) return;
    fp = fopen(path, "rb");
    while (fp && fgets(line, sizeof(line), fp) && pt->task_len < 32) {
        char *eq;
        char *name;
        char *command;
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "task.", 5) != 0 || !(eq = strchr(line + 5, '='))) continue;
        *eq++ = '\0';
        name = line + 5;
        command = eq;
        while (*name && isspace((unsigned char)*name)) name++;
        while (*command && isspace((unsigned char)*command)) command++;
        if (!*name || !*command) continue;
        snprintf(pt->tasks[pt->task_len].name, sizeof(pt->tasks[pt->task_len].name), "%s", name);
        snprintf(pt->tasks[pt->task_len].command, sizeof(pt->tasks[pt->task_len].command), "%s", command);
        pt->task_len++;
    }
    if (fp) fclose(fp);
    if (!pt->task_len) {
        const char *names[] = {"build", "test", "run"};
        const char *cmds[] = {"make", "make test", "make run"};
        int i;
        for (i = 0; i < 3; i++) {
            snprintf(pt->tasks[i].name, sizeof(pt->tasks[i].name), "%s", names[i]);
            snprintf(pt->tasks[i].command, sizeof(pt->tasks[i].command), "%s", cmds[i]);
        }
        pt->task_len = 3;
    }
}

int project_tools_init(editor_t *editor) {
    project_tools_t *pt;
    if (!editor || editor->headless_mode) return MLE_OK;
    pt = calloc(1, sizeof(*pt));
    if (!pt) return MLE_ERR;
    editor->project_tools = pt;
    pt_find_root(pt);
    pt_load_tasks(pt);
    return MLE_OK;
}

static void pt_clear_replace(project_tools_t *pt) {
    int i;
    free(pt->replace_find);
    free(pt->replace_with);
    for (i = 0; i < pt->replace_file_len; i++) free(pt->replace_files[i]);
    free(pt->replace_files);
    pt->replace_find = NULL;
    pt->replace_with = NULL;
    pt->replace_files = NULL;
    pt->replace_file_len = 0;
}

void project_tools_shutdown(editor_t *editor) {
    if (!editor || !editor->project_tools) return;
    pt_clear_replace(editor->project_tools);
    free(editor->project_tools);
    editor->project_tools = NULL;
}

const char *project_tools_root(editor_t *editor) {
    return editor && editor->project_tools ? editor->project_tools->root : ".";
}

static int pt_cmd_palette(cmd_context_t *ctx) {
    char *query = NULL;
    str_t menu = {0};
    int i;
    cmd_t *cmd;
    cmd_t *tmp;
    editor_prompt(ctx->editor, "palette: filter (empty shows all)?", NULL, &query);
    if (!query) return MLE_OK;
    str_append(&menu, "  Command palette - Enter runs a command\n\n");
    for (i = 0; palette_items[i].command; i++) {
        if (pt_contains_ci(palette_items[i].label, query) || pt_contains_ci(palette_items[i].command, query))
            str_sprintf(&menu, "  %-42s [%s]\n", palette_items[i].label, palette_items[i].command);
    }
    HASH_ITER(hh, ctx->editor->cmd_map, cmd, tmp) {
        int known = 0;
        if (cmd->name[0] == '_' || !pt_contains_ci(cmd->name, query)) continue;
        for (i = 0; palette_items[i].command; i++) if (strcmp(cmd->name, palette_items[i].command) == 0) known = 1;
        if (!known) str_sprintf(&menu, "  %-42s [%s]\n", cmd->name + 4, cmd->name);
    }
    if (menu.len < 60) str_append(&menu, "  No matching commands\n");
    editor_menu(ctx->editor, pt_palette_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    free(query);
    return MLE_OK;
}

static int pt_palette_cb(cmd_context_t *ctx) {
    char *line = pt_line(ctx);
    char *open = strrchr(line, '[');
    char *close = strrchr(line, ']');
    cmd_t *cmd = NULL;
    if (open && close && close > open) {
        *close = '\0';
        HASH_FIND_STR(ctx->editor->cmd_map, open + 1, cmd);
    }
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (cmd && cmd->func != pt_cmd_palette) {
        ctx->cmd = cmd;
        ctx->bview = ctx->editor->active;
        ctx->buffer = ctx->bview->buffer;
        ctx->cursor = ctx->bview->active_cursor;
        ctx->static_param = NULL;
        cmd->func(ctx);
    }
    free(line);
    return MLE_OK;
}

static int pt_cmd_search(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char *query = NULL;
    char *arg;
    char *root;
    char *command = NULL;
    char *out;
    str_t menu = {0};
    editor_prompt(ctx->editor, "project search: text or regex?", NULL, &query);
    if (!query || !query[0]) { free(query); return MLE_OK; }
    arg = util_escape_shell_arg(query, strlen(query));
    root = util_escape_shell_arg(pt->root, strlen(pt->root));
    if (arg && root) asprintf(&command, "rg --line-number --column --color never --hidden --glob '!.git' -- %s %s 2>/dev/null", arg, root);
    out = command ? pt_capture(command, 4 * 1024 * 1024, NULL) : NULL;
    str_sprintf(&menu, "  Project search: %s\n\n", query);
    if (out && out[0]) str_append(&menu, out);
    else str_append(&menu, "  No matches\n");
    editor_menu(ctx->editor, pt_search_cb, menu.data, menu.len, NULL, NULL);
    free(query); free(arg); free(root); free(command); free(out); str_free(&menu);
    return MLE_OK;
}

static int pt_open_location(editor_t *editor, const char *text) {
    char *copy = strdup(text);
    char *c1 = strchr(copy, ':');
    char *c2;
    long line = 0;
    long col = 0;
    bview_t *bv = NULL;
    if (!c1) { free(copy); return MLE_ERR; }
    *c1++ = '\0';
    c2 = strchr(c1, ':');
    if (c2) { *c2++ = '\0'; col = strtol(c2, NULL, 10); }
    line = strtol(c1, NULL, 10);
    if (!util_is_file(copy, "rb", NULL)) { free(copy); return MLE_ERR; }
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, copy, strlen(copy), 1, line, 0, NULL, &bv);
    if (bv && col > 0) mark_move_col(bv->active_cursor->mark, col - 1);
    free(copy);
    return MLE_OK;
}

static int pt_search_cb(cmd_context_t *ctx) {
    char *line = pt_line(ctx);
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    pt_open_location(ctx->editor, line);
    free(line);
    return MLE_OK;
}

static void pt_add_replace_file(project_tools_t *pt, const char *path) {
    char **next = realloc(pt->replace_files, sizeof(char *) * (pt->replace_file_len + 1));
    if (!next) return;
    pt->replace_files = next;
    pt->replace_files[pt->replace_file_len++] = strdup(path);
}

static int pt_cmd_replace(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char *find = NULL;
    char *with = NULL;
    char *arg = NULL;
    char *root = NULL;
    char *command = NULL;
    char *out = NULL;
    char *scan;
    str_t menu = {0};
    editor_prompt(ctx->editor, "project replace: literal text?", NULL, &find);
    if (!find || !find[0]) { free(find); return MLE_OK; }
    editor_prompt(ctx->editor, "replace with?", NULL, &with);
    if (!with) { free(find); return MLE_OK; }
    pt_clear_replace(pt);
    pt->replace_find = find;
    pt->replace_with = with;
    arg = util_escape_shell_arg(find, strlen(find));
    root = util_escape_shell_arg(pt->root, strlen(pt->root));
    if (arg && root) asprintf(&command, "rg --files-with-matches --color never --hidden --glob '!.git' -F -- %s %s 2>/dev/null", arg, root);
    out = command ? pt_capture(command, 2 * 1024 * 1024, NULL) : NULL;
    for (scan = out; scan && *scan; ) {
        char *end = strchr(scan, '\n');
        if (end) *end = '\0';
        if (*scan) pt_add_replace_file(pt, scan);
        if (!end) break;
        scan = end + 1;
    }
    str_sprintf(&menu, "  Replace preview: '%s' -> '%s'\n  [apply] Apply to %d files\n\n", find, with, pt->replace_file_len);
    for (int i = 0; i < pt->replace_file_len; i++) str_sprintf(&menu, "  %s\n", pt->replace_files[i]);
    if (!pt->replace_file_len) str_append(&menu, "  No matching files\n");
    editor_menu(ctx->editor, pt_replace_cb, menu.data, menu.len, NULL, NULL);
    free(arg); free(root); free(command); free(out); str_free(&menu);
    return MLE_OK;
}

static int pt_replace_one(const char *path, const char *find, const char *with) {
    struct stat st;
    FILE *fp;
    char *data;
    long size;
    str_t out = {0};
    char *p;
    char tmp[PATH_MAX + 32];
    int fd;
    if (stat(path, &st) != 0 || st.st_size > 64 * 1024 * 1024 || !(fp = fopen(path, "rb"))) return MLE_ERR;
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return MLE_ERR; }
    data = malloc((size_t)size + 1);
    if (!data) { fclose(fp); return MLE_ERR; }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(data); return MLE_ERR; }
    fclose(fp); data[size] = '\0';
    p = data;
    while (*p) {
        char *hit = strstr(p, find);
        if (!hit) { str_append(&out, p); break; }
        str_append_len(&out, p, hit - p);
        str_append(&out, (char *)with);
        p = hit + strlen(find);
    }
    if (snprintf(tmp, sizeof(tmp), "%s.mle-tmp", path) >= (int)sizeof(tmp)) { free(data); str_free(&out); return MLE_ERR; }
    fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, st.st_mode & 0777);
    if (fd < 0 || write(fd, out.data, out.len) != (ssize_t)out.len) { if (fd >= 0) close(fd); unlink(tmp); free(data); str_free(&out); return MLE_ERR; }
    fsync(fd); close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); free(data); str_free(&out); return MLE_ERR; }
    free(data); str_free(&out); return MLE_OK;
}

static int pt_replace_cb(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char *line = pt_line(ctx);
    if (strstr(line, "[apply]")) {
        int ok = 0;
        int i;
        for (i = 0; i < pt->replace_file_len; i++) if (pt_replace_one(pt->replace_files[i], pt->replace_find, pt->replace_with) == MLE_OK) ok++;
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        MLE_SET_INFO(ctx->editor, "project replace updated %d files", ok);
    }
    free(line);
    return MLE_OK;
}

static int pt_cmd_tasks(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    str_t menu = {0};
    int i;
    pt_find_root(pt); pt_load_tasks(pt);
    str_sprintf(&menu, "  Project tasks - %s\n  Configure with task.NAME=command in .mle-project\n\n", pt->root);
    for (i = 0; i < pt->task_len; i++) str_sprintf(&menu, "  %-18s %s\n", pt->tasks[i].name, pt->tasks[i].command);
    editor_menu(ctx->editor, pt_task_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    return MLE_OK;
}

static int pt_task_cb(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char *line = pt_line(ctx);
    int i;
    for (i = 0; i < pt->task_len; i++) if (strstr(line, pt->tasks[i].name)) {
        char *root = util_escape_shell_arg(pt->root, strlen(pt->root));
        char *command = NULL;
        char *out;
        char state[PATH_MAX + 32];
        FILE *fp;
        if (root) asprintf(&command, "cd %s && (%s) 2>&1", root, pt->tasks[i].command);
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        out = command ? pt_capture(command, 8 * 1024 * 1024, NULL) : NULL;
        snprintf(state, sizeof(state), "%s/.mle-last-problems", pt->root);
        fp = fopen(state, "wb");
        if (fp) { if (out) fwrite(out, 1, strlen(out), fp); fclose(fp); }
        if (out && out[0]) editor_menu(ctx->editor, pt_problem_cb, out, strlen(out), NULL, NULL);
        else MLE_SET_INFO(ctx->editor, "task %s finished without output", pt->tasks[i].name);
        free(root); free(command); free(out);
        break;
    }
    free(line);
    return MLE_OK;
}

static int pt_cmd_problems(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char path[PATH_MAX + 32];
    FILE *fp;
    str_t out = {0};
    char buf[4096];
    size_t n;
    snprintf(path, sizeof(path), "%s/.mle-last-problems", pt->root);
    fp = fopen(path, "rb");
    while (fp && (n = fread(buf, 1, sizeof(buf), fp)) > 0 && out.len < 8 * 1024 * 1024) str_append_len(&out, buf, n);
    if (fp) fclose(fp);
    if (!out.len) str_append(&out, "  No task output yet. Run Project tasks first.\n");
    editor_menu(ctx->editor, pt_problem_cb, out.data, out.len, NULL, NULL);
    str_free(&out);
    return MLE_OK;
}

static int pt_problem_cb(cmd_context_t *ctx) {
    char *line = pt_line(ctx);
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (pt_open_location(ctx->editor, line) != MLE_OK) MLE_SET_INFO(ctx->editor, "%s", "selected output is not a file location");
    free(line);
    return MLE_OK;
}

static int pt_cmd_outline(cmd_context_t *ctx) {
    const char *path = ctx->editor->active_edit && ctx->editor->active_edit->buffer ? ctx->editor->active_edit->buffer->path : NULL;
    char *arg;
    char *command = NULL;
    char *out;
    str_t menu = {0};
    if (!path) { MLE_SET_ERR(ctx->editor, "%s", "save the file before opening Outline"); return MLE_OK; }
    arg = util_escape_shell_arg((char *)path, strlen(path));
    if (arg) asprintf(&command, "ctags -x --sort=no %s 2>/dev/null", arg);
    out = command ? pt_capture(command, 2 * 1024 * 1024, NULL) : NULL;
    str_append(&menu, "  Outline - Enter jumps to symbol\n\n");
    if (out && out[0]) str_append(&menu, out); else str_append(&menu, "  ctags unavailable or no symbols found\n");
    editor_menu(ctx->editor, pt_outline_cb, menu.data, menu.len, NULL, NULL);
    free(arg); free(command); free(out); str_free(&menu);
    return MLE_OK;
}

static int pt_outline_cb(cmd_context_t *ctx) {
    char *line = pt_line(ctx);
    char name[256];
    char kind[128];
    long lineno = 0;
    if (sscanf(line, "%255s %127s %ld", name, kind, &lineno) == 3 && lineno > 0) {
        bview_t *target = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        if (target) { editor_set_active(ctx->editor, target); mark_move_to(target->active_cursor->mark, lineno - 1, 0); bview_center_viewport_y(target); }
    }
    free(line);
    return MLE_OK;
}

static int pt_cmd_file_action(cmd_context_t *ctx) {
    static const char menu[] =
        "  File actions\n\n"
        "  create-file    Create a file\n"
        "  create-dir     Create a directory\n"
        "  rename         Rename the current file\n"
        "  remove         Remove the current file (asks for yes)\n";
    editor_menu(ctx->editor, pt_file_action_cb, (char *)menu, sizeof(menu) - 1, NULL, NULL);
    return MLE_OK;
}

static int pt_file_action_cb(cmd_context_t *ctx) {
    project_tools_t *pt = ctx->editor->project_tools;
    char *line = pt_line(ctx);
    bview_t *active = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (strstr(line, "create-file")) {
        char *path = NULL; editor_prompt(ctx->editor, "create file: path?", NULL, &path);
        if (path) { int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644); if (fd >= 0) { close(fd); editor_open_bview(ctx->editor, NULL, MLE_BVIEW_TYPE_EDIT, path, strlen(path), 1, 0, 0, NULL, NULL); } else MLE_SET_ERR(ctx->editor, "create failed: %s", strerror(errno)); free(path); }
    } else if (strstr(line, "create-dir")) {
        char *path = NULL; editor_prompt(ctx->editor, "create directory: path?", NULL, &path);
        if (path) { if (mkdir(path, 0755) != 0) MLE_SET_ERR(ctx->editor, "mkdir failed: %s", strerror(errno)); free(path); }
    } else if (strstr(line, "rename") && active && active->buffer->path) {
        char *path = NULL; char *old = strdup(active->buffer->path); editor_prompt(ctx->editor, "rename to?", NULL, &path);
        if (path && rename(old, path) == 0) { free(active->buffer->path); active->buffer->path = strdup(path); } else if (path) MLE_SET_ERR(ctx->editor, "rename failed: %s", strerror(errno));
        free(old); free(path);
    } else if (strstr(line, "remove") && active && active->buffer->path) {
        char *answer = NULL; editor_prompt(ctx->editor, "type yes to remove current file", NULL, &answer);
        if (answer && strcmp(answer, "yes") == 0) { if (unlink(active->buffer->path) == 0) MLE_SET_INFO(ctx->editor, "%s", "file removed; the open buffer is still available"); else MLE_SET_ERR(ctx->editor, "remove failed: %s", strerror(errno)); }
        free(answer);
    }
    (void)pt;
    free(line);
    return MLE_OK;
}

int project_tools_register_commands(editor_t *editor) {
    int rc = MLE_OK;
    rc |= pt_register(editor, "cmd_palette", pt_cmd_palette);
    rc |= pt_register(editor, "cmd_project_search", pt_cmd_search);
    rc |= pt_register(editor, "cmd_project_replace", pt_cmd_replace);
    rc |= pt_register(editor, "cmd_tasks", pt_cmd_tasks);
    rc |= pt_register(editor, "cmd_problems", pt_cmd_problems);
    rc |= pt_register(editor, "cmd_outline", pt_cmd_outline);
    rc |= pt_register(editor, "cmd_file_action", pt_cmd_file_action);
    return rc;
}
