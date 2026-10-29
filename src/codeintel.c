#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mle.h"

typedef struct fold_s {
    buffer_t *buffer;
    bint_t start;
    bint_t end;
    struct fold_s *next;
} fold_t;

struct codeintel_s {
    fold_t *folds;
    bview_t *completion_view;
    mark_t *completion_start;
    observer_t *move_observer;
};

static int ci_cmd_complete(cmd_context_t *ctx);
static int ci_cmd_snippets(cmd_context_t *ctx);
static int ci_cmd_fold(cmd_context_t *ctx);
static int ci_cmd_lsp(cmd_context_t *ctx);
static int ci_cmd_brackets(cmd_context_t *ctx);
static int ci_complete_cb(cmd_context_t *ctx);
static int ci_snippet_cb(cmd_context_t *ctx);
static int ci_lsp_cb(cmd_context_t *ctx);
static int ci_location_cb(cmd_context_t *ctx);

static int ci_register(editor_t *editor, const char *name, cmd_func_t fn) {
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.name = (char *)name;
    cmd.func = fn;
    return editor_register_cmd(editor, &cmd);
}

static char *ci_line(cmd_context_t *ctx) {
    bline_t *line = ctx->bview->active_cursor->mark->bline;
    return strndup(line->data, line->data_len);
}

static int ci_word_ch(unsigned char c) {
    return isalnum(c) || c == '_';
}

static fold_t *ci_fold_at(codeintel_t *ci, buffer_t *buffer, bint_t line, int containing) {
    fold_t *f;
    for (f = ci ? ci->folds : NULL; f; f = f->next) {
        if (f->buffer != buffer) continue;
        if ((!containing && f->start == line) || (containing && line >= f->start && line <= f->end)) return f;
    }
    return NULL;
}

static int ci_indent(bline_t *line) {
    int n = 0;
    bint_t i;
    for (i = 0; i < line->data_len; i++) {
        if (line->data[i] == ' ') n++;
        else if (line->data[i] == '\t') n += 4;
        else break;
    }
    return n;
}

static int ci_find_fold(bline_t *start, bint_t *end) {
    bline_t *line;
    int braces = 0;
    int seen = 0;
    bint_t i;
    int base_indent;
    int colon = 0;
    for (i = 0; i < start->data_len; i++) {
        if (start->data[i] == '{' || start->data[i] == '[' || start->data[i] == '(') { braces++; seen = 1; }
        else if ((start->data[i] == '}' || start->data[i] == ']' || start->data[i] == ')') && braces > 0) braces--;
    }
    if (seen && braces > 0) {
        for (line = start->next; line; line = line->next) {
            for (i = 0; i < line->data_len; i++) {
                if (line->data[i] == '{' || line->data[i] == '[' || line->data[i] == '(') braces++;
                else if (line->data[i] == '}' || line->data[i] == ']' || line->data[i] == ')') braces--;
            }
            if (braces <= 0) { *end = line->line_index; return *end > start->line_index ? MLE_OK : MLE_ERR; }
        }
    }
    for (i = start->data_len; i > 0; i--) {
        char c = start->data[i - 1];
        if (isspace((unsigned char)c)) continue;
        colon = c == ':';
        break;
    }
    base_indent = ci_indent(start);
    if (colon || start->next) {
        bint_t last = start->line_index;
        for (line = start->next; line; line = line->next) {
            int indent;
            if (!line->data_len) { if (last > start->line_index) last = line->line_index; continue; }
            indent = ci_indent(line);
            if (indent <= base_indent) break;
            last = line->line_index;
        }
        if (last > start->line_index) { *end = last; return MLE_OK; }
    }
    return MLE_ERR;
}

int codeintel_debug_fold_end(buffer_t *buffer, bint_t line, bint_t *end) {
    bline_t *start = NULL;
    if (!buffer || !end || buffer_get_bline(buffer, line, &start) != MLBUF_OK || !start) return MLE_ERR;
    return ci_find_fold(start, end);
}

static int ci_move_observer(char *event, void *event_data, void *udata) {
    cmd_context_t *ctx = event_data;
    codeintel_t *ci = udata;
    fold_t *f;
    (void)event;
    if (!ctx || !ctx->bview || !ctx->cursor || !(f = ci_fold_at(ci, ctx->buffer, ctx->cursor->mark->bline->line_index, 1))) return MLE_OK;
    if (ctx->cursor->mark->bline->line_index > f->start) mark_move_to(ctx->cursor->mark, f->start, 0);
    return MLE_OK;
}

int codeintel_init(editor_t *editor) {
    codeintel_t *ci;
    if (!editor || editor->headless_mode) return MLE_OK;
    ci = calloc(1, sizeof(*ci));
    if (!ci) return MLE_ERR;
    editor->codeintel = ci;
    editor_register_observer(editor, "cmd_move_*:after", ci, ci_move_observer, &ci->move_observer);
    return MLE_OK;
}

void codeintel_shutdown(editor_t *editor) {
    codeintel_t *ci;
    fold_t *f;
    fold_t *next;
    if (!editor || !(ci = editor->codeintel)) return;
    if (ci->move_observer) editor_destroy_observer(editor, ci->move_observer);
    if (ci->completion_start) mark_destroy(ci->completion_start);
    for (f = ci->folds; f; f = next) { next = f->next; free(f); }
    free(ci);
    editor->codeintel = NULL;
}

bline_t *codeintel_next_visible_line(editor_t *editor, bview_t *bview, bline_t *line) {
    fold_t *f;
    bline_t *next;
    if (!line) return NULL;
    f = editor && editor->codeintel ? ci_fold_at(editor->codeintel, bview->buffer, line->line_index, 0) : NULL;
    if (!f) return line->next;
    next = line;
    while (next && next->line_index <= f->end) next = next->next;
    return next;
}

bline_t *codeintel_screen_line(editor_t *editor, bview_t *bview, bline_t *start, int rows) {
    fold_t *inside;
    if (editor && editor->codeintel && start
        && (inside = ci_fold_at(editor->codeintel, bview->buffer, start->line_index, 1))
        && start->line_index > inside->start) {
        buffer_get_bline(bview->buffer, inside->start, &start);
    }
    while (start && rows-- > 0) start = codeintel_next_visible_line(editor, bview, start);
    return start;
}

int codeintel_visible_distance(editor_t *editor, bview_t *bview, bline_t *from, bline_t *to) {
    int distance = 0;
    bline_t *line;
    if (!from || !to) return INT_MAX;
    if (from->line_index > to->line_index) return -codeintel_visible_distance(editor, bview, to, from);
    for (line = from; line && line != to; line = codeintel_next_visible_line(editor, bview, line)) distance++;
    return line == to ? distance : INT_MAX;
}

char codeintel_fold_mark(editor_t *editor, bview_t *bview, bline_t *line) {
    fold_t *f;
    bint_t end;
    if (!editor || !editor->codeintel || !bview || !line) return ' ';
    f = ci_fold_at(editor->codeintel, bview->buffer, line->line_index, 0);
    if (f) return '>';
    return ci_find_fold(line, &end) == MLE_OK ? 'v' : ' ';
}

static int ci_cmd_fold(cmd_context_t *ctx) {
    codeintel_t *ci = ctx->editor->codeintel;
    bint_t line = ctx->cursor->mark->bline->line_index;
    fold_t *f = ci_fold_at(ci, ctx->buffer, line, 1);
    bint_t end;
    if (f) {
        fold_t **p = &ci->folds;
        while (*p && *p != f) p = &(*p)->next;
        if (*p) *p = f->next;
        free(f);
        MLE_SET_INFO(ctx->editor, "%s", "fold expanded");
        return MLE_OK;
    }
    if (ci_find_fold(ctx->cursor->mark->bline, &end) != MLE_OK) {
        MLE_SET_ERR(ctx->editor, "%s", "no foldable block on this line");
        return MLE_OK;
    }
    f = calloc(1, sizeof(*f));
    if (!f) return MLE_ERR;
    f->buffer = ctx->buffer;
    f->start = line;
    f->end = end;
    f->next = ci->folds;
    ci->folds = f;
    mark_move_to(ctx->cursor->mark, line, 0);
    MLE_SET_INFO(ctx->editor, "folded lines %" PRIdMAX "-%" PRIdMAX, line + 2, end + 1);
    return MLE_OK;
}

static int ci_candidate_exists(char words[][128], int n, const char *word) {
    int i;
    for (i = 0; i < n; i++) if (strcmp(words[i], word) == 0) return 1;
    return 0;
}

static int ci_cmd_complete(cmd_context_t *ctx) {
    codeintel_t *ci = ctx->editor->codeintel;
    bline_t *cur = ctx->cursor->mark->bline;
    bint_t col = ctx->cursor->mark->col;
    bint_t start = col;
    int start_byte;
    int end_byte;
    char prefix[128];
    char words[256][128];
    int nwords = 0;
    bview_t *bv;
    str_t menu = {0};
    MLBUF_BLINE_ENSURE_CHARS(cur);
    while (start > 0 && ci_word_ch((unsigned char)cur->chars[start - 1].ch)) start--;
    start_byte = start < cur->char_count ? cur->chars[start].index : cur->data_len;
    end_byte = col < cur->char_count ? cur->chars[col].index : cur->data_len;
    if (end_byte - start_byte <= 0 || end_byte - start_byte >= (int)sizeof(prefix)) return MLE_OK;
    memcpy(prefix, cur->data + start_byte, end_byte - start_byte);
    prefix[end_byte - start_byte] = '\0';
    CDL_FOREACH2(ctx->editor->all_bviews, bv, all_next) {
        bline_t *line;
        if (!MLE_BVIEW_IS_EDIT(bv) || !bv->buffer) continue;
        for (line = bv->buffer->first_line; line && nwords < 256; line = line->next) {
            bint_t i = 0;
            while (i < line->data_len && nwords < 256) {
                bint_t begin;
                int len;
                while (i < line->data_len && !ci_word_ch((unsigned char)line->data[i])) i++;
                begin = i;
                while (i < line->data_len && ci_word_ch((unsigned char)line->data[i])) i++;
                len = i - begin;
                if (len > (int)strlen(prefix) && len < 128 && strncmp(line->data + begin, prefix, strlen(prefix)) == 0) {
                    char word[128];
                    memcpy(word, line->data + begin, len); word[len] = '\0';
                    if (!ci_candidate_exists(words, nwords, word)) snprintf(words[nwords++], 128, "%s", word);
                }
            }
        }
    }
    if (!nwords) { MLE_SET_INFO(ctx->editor, "%s", "no completion candidates"); return MLE_OK; }
    if (ci->completion_start) mark_destroy(ci->completion_start);
    ci->completion_start = buffer_add_mark(ctx->buffer, cur, start);
    ci->completion_view = ctx->bview;
    str_sprintf(&menu, "  Complete '%s' - Enter inserts\n\n", prefix);
    for (int i = 0; i < nwords; i++) str_sprintf(&menu, "  %s\n", words[i]);
    editor_menu(ctx->editor, ci_complete_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    return MLE_OK;
}

static int ci_complete_cb(cmd_context_t *ctx) {
    codeintel_t *ci = ctx->editor->codeintel;
    char *line = ci_line(ctx);
    char *word = line;
    while (*word && isspace((unsigned char)*word)) word++;
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (*word && ci->completion_view && ci->completion_start) {
        mark_t *end = ci->completion_view->active_cursor->mark;
        mark_delete_between(ci->completion_start, end);
        mark_insert_before(end, word, strlen(word));
        editor_set_active(ctx->editor, ci->completion_view);
    }
    if (ci->completion_start) { mark_destroy(ci->completion_start); ci->completion_start = NULL; }
    ci->completion_view = NULL;
    free(line);
    return MLE_OK;
}

static char *ci_decode_snippet(const char *input, int *cursor_offset) {
    str_t out = {0};
    const char *p;
    *cursor_offset = -1;
    for (p = input; *p; p++) {
        if (strncmp(p, "${cursor}", 9) == 0) { *cursor_offset = out.len; p += 8; continue; }
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') str_append_len(&out, "\n", 1);
            else if (*p == 't') str_append_len(&out, "\t", 1);
            else str_append_len(&out, (char *)p, 1);
        } else str_append_len(&out, (char *)p, 1);
    }
    return out.data ? out.data : strdup("");
}

static int ci_cmd_snippets(cmd_context_t *ctx) {
    char path[PATH_MAX + 32];
    FILE *fp;
    char line[2048];
    str_t menu = {0};
    str_append(&menu, "  Snippets - Enter inserts. ${cursor} sets final cursor.\n\n");
    str_append(&menu, "  if        if (${cursor}) {\\n\\t\\n}\n");
    str_append(&menu, "  for       for (${cursor}) {\\n\\t\\n}\n");
    str_append(&menu, "  function  function ${cursor}() {\\n\\t\\n}\n");
    snprintf(path, sizeof(path), "%s/.mle-project", project_tools_root(ctx->editor));
    fp = fopen(path, "rb");
    while (fp && fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "snippet.", 8) == 0) {
            char *eq = strchr(line + 8, '=');
            if (eq) { *eq++ = '\0'; eq[strcspn(eq, "\r\n")] = '\0'; str_sprintf(&menu, "  %-9s %s\n", line + 8, eq); }
        }
    }
    if (fp) fclose(fp);
    editor_menu(ctx->editor, ci_snippet_cb, menu.data, menu.len, NULL, NULL);
    str_free(&menu);
    return MLE_OK;
}

static int ci_snippet_cb(cmd_context_t *ctx) {
    char *line = ci_line(ctx);
    char *body = line;
    char *decoded;
    int cursor_offset;
    bview_t *target = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
    while (*body && isspace((unsigned char)*body)) body++;
    while (*body && !isspace((unsigned char)*body)) body++;
    while (*body && isspace((unsigned char)*body)) body++;
    decoded = ci_decode_snippet(body, &cursor_offset);
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (target && decoded) {
        mark_t *mark = target->active_cursor->mark;
        mark_insert_before(mark, decoded, strlen(decoded));
        if (cursor_offset >= 0) mark_move_by(mark, -((bint_t)strlen(decoded) - cursor_offset));
        editor_set_active(ctx->editor, target);
    }
    free(decoded); free(line);
    return MLE_OK;
}

static char *ci_config_value(editor_t *editor, const char *key) {
    char path[PATH_MAX + 32];
    FILE *fp;
    char line[2048];
    size_t n = strlen(key);
    snprintf(path, sizeof(path), "%s/.mle-project", project_tools_root(editor));
    fp = fopen(path, "rb");
    while (fp && fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, n) == 0 && line[n] == '=') {
            char *v = line + n + 1;
            v[strcspn(v, "\r\n")] = '\0';
            fclose(fp);
            return strdup(v);
        }
    }
    if (fp) fclose(fp);
    return NULL;
}

static int ci_cmd_lsp(cmd_context_t *ctx) {
    static const char menu[] =
        "  Language server - uses lsp.command in .mle-project\n"
        "  Adapter receives: ACTION FILE LINE COLUMN WORD\n\n"
        "  definition    Go to definition\n"
        "  references    Find references\n"
        "  rename        Rename symbol\n"
        "  diagnostics   Show diagnostics\n";
    editor_menu(ctx->editor, ci_lsp_cb, (char *)menu, sizeof(menu) - 1, NULL, NULL);
    return MLE_OK;
}

static char *ci_capture(const char *command) {
    FILE *fp = popen(command, "r");
    str_t out = {0};
    char buf[4096];
    size_t n;
    if (!fp) return NULL;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0 && out.len < 4 * 1024 * 1024) str_append_len(&out, buf, n);
    pclose(fp);
    return out.data ? out.data : strdup("");
}

static int ci_lsp_cb(cmd_context_t *ctx) {
    char *line = ci_line(ctx);
    const char *action = strstr(line, "definition") ? "definition" : strstr(line, "references") ? "references" : strstr(line, "rename") ? "rename" : strstr(line, "diagnostics") ? "diagnostics" : NULL;
    bview_t *target = ctx->editor->active_edit_last ? ctx->editor->active_edit_last : ctx->editor->active_edit;
    char *adapter = ci_config_value(ctx->editor, "lsp.command");
    char *file = NULL;
    char *word = NULL;
    char *command = NULL;
    char *out = NULL;
    bint_t word_len = 0;
    if (target && target->buffer->path) file = util_escape_shell_arg(target->buffer->path, strlen(target->buffer->path));
    if (target && cursor_select_by(target->active_cursor, "word", 0) == MLE_OK) {
        mark_get_between(target->active_cursor->mark, target->active_cursor->anchor, &word, &word_len);
        cursor_toggle_anchor(target->active_cursor, 0);
    }
    editor_close_bview(ctx->editor, ctx->bview, NULL);
    if (!action || !adapter || !file) {
        MLE_SET_ERR(ctx->editor, "%s", "configure lsp.command=/path/to/lsp-adapter in .mle-project");
    } else {
        char *warg = util_escape_shell_arg(word ? word : "", word ? strlen(word) : 0);
        asprintf(&command, "%s %s %s %" PRIdMAX " %" PRIdMAX " %s 2>&1", adapter, action, file,
            target->active_cursor->mark->bline->line_index + 1, target->active_cursor->mark->col + 1, warg ? warg : "''");
        out = command ? ci_capture(command) : NULL;
        if (out && out[0]) editor_menu(ctx->editor, ci_location_cb, out, strlen(out), NULL, NULL);
        else MLE_SET_INFO(ctx->editor, "%s", "language server returned no results");
        free(warg);
    }
    free(line); free(adapter); free(file); free(word); free(command); free(out);
    return MLE_OK;
}

static int ci_location_cb(cmd_context_t *ctx) {
    char *line = ci_line(ctx);
    char *c = strchr(line, ':');
    if (c) {
        long lineno;
        bview_t *bv = NULL;
        *c++ = '\0'; lineno = strtol(c, NULL, 10);
        editor_close_bview(ctx->editor, ctx->bview, NULL);
        if (util_is_file(line, "rb", NULL)) editor_open_bview(ctx->editor, NULL, MLE_BVIEW_TYPE_EDIT, line, strlen(line), 1, lineno, 0, NULL, &bv);
    }
    free(line);
    return MLE_OK;
}

static int ci_cmd_brackets(cmd_context_t *ctx) {
    bline_t *line;
    struct { char ch; bint_t line; bint_t col; } stack[1024];
    int top = 0;
    str_t report = {0};
    for (line = ctx->buffer->first_line; line; line = line->next) {
        bint_t i;
        for (i = 0; i < line->data_len; i++) {
            char c = line->data[i];
            if (c == '(' || c == '[' || c == '{') { if (top < 1024) { stack[top].ch = c; stack[top].line = line->line_index; stack[top].col = i; top++; } }
            else if (c == ')' || c == ']' || c == '}') {
                char want = c == ')' ? '(' : c == ']' ? '[' : '{';
                if (!top || stack[top - 1].ch != want) str_sprintf(&report, "  line %" PRIdMAX ": unmatched %c\n", line->line_index + 1, c);
                else top--;
            }
        }
    }
    while (top) { top--; str_sprintf(&report, "  line %" PRIdMAX ": unmatched %c\n", stack[top].line + 1, stack[top].ch); }
    if (!report.len) MLE_SET_INFO(ctx->editor, "%s", "brackets are balanced");
    else { str_t menu = {0}; str_append(&menu, "  Bracket diagnostics\n\n"); str_append(&menu, report.data); editor_menu(ctx->editor, NULL, menu.data, menu.len, NULL, NULL); str_free(&menu); }
    str_free(&report);
    return MLE_OK;
}

int codeintel_register_commands(editor_t *editor) {
    int rc = MLE_OK;
    rc |= ci_register(editor, "cmd_complete", ci_cmd_complete);
    rc |= ci_register(editor, "cmd_snippets", ci_cmd_snippets);
    rc |= ci_register(editor, "cmd_fold", ci_cmd_fold);
    rc |= ci_register(editor, "cmd_lsp", ci_cmd_lsp);
    rc |= ci_register(editor, "cmd_bracket_check", ci_cmd_brackets);
    return rc;
}
