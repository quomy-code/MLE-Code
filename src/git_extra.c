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
static int gx_cmd_log(cmd_context_t *ctx);
static int gx_cmd_blame(cmd_context_t *ctx);
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

static int parse_hunk(const char *p, int ln, long *old_start, long *old_count, long *new_start, long *new_count, const char **func, int *func_len) {
    const char *minus;
    const char *plus;
    const char *at;
    char *stop;
    int seen;
    if (ln < 4 || strncmp(p, "@@ ", 3) != 0) return 0;
    minus = memchr(p, '-', (size_t)ln);
    plus = memchr(p, '+', (size_t)ln);
    if (!minus || !plus) return 0;
    *old_start = strtol(minus + 1, &stop, 10);
    *old_count = 1;
    if (stop && *stop == ',') *old_count = strtol(stop + 1, &stop, 10);
    *new_start = strtol(plus + 1, &stop, 10);
    *new_count = 1;
    if (stop && *stop == ',') *new_count = strtol(stop + 1, &stop, 10);
    if (func) *func = NULL;
    if (func_len) *func_len = 0;
    at = p;
    seen = 0;
    while (at + 1 < p + ln) {
        if (at[0] == '@' && at[1] == '@') {
            seen += 1;
            if (seen == 2) {
                const char *f = at + 2;
                int n;
                if (f < p + ln && *f == ' ') f += 1;
                n = (int)((p + ln) - f);
                while (n > 0 && (f[n - 1] == ' ' || f[n - 1] == '\r')) n -= 1;
                if (func) *func = n > 0 ? f : NULL;
                if (func_len) *func_len = n > 0 ? n : 0;
                break;
            }
            at += 2;
            continue;
        }
        at += 1;
    }
    return 1;
}

static const char *next_line(const char *p, const char *end, int *len) {
    const char *nl;
    int n;
    if (!p || p >= end) {
        if (len) *len = 0;
        return NULL;
    }
    nl = memchr(p, '\n', (size_t)(end - p));
    n = nl ? (int)(nl - p) : (int)(end - p);
    if (n > 0 && p[n - 1] == '\r') n -= 1;
    if (len) *len = n;
    return p;
}

static int line_kind(const char *p, int n) {
    if (n < 1) return 0;
    if (p[0] == '+' && !(n >= 3 && strncmp(p, "+++", 3) == 0)) return '+';
    if (p[0] == '-' && !(n >= 3 && strncmp(p, "---", 3) == 0)) return '-';
    if (p[0] == ' ') return ' ';
    if (p[0] == '\\') return '\\';
    return 0;
}

static int hunk_has_body(const char *p, const char *end) {
    while (p && p < end && strncmp(p, "@@ ", 3) != 0 && strncmp(p, "diff ", 5) != 0) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        int kind;
        if (!line) break;
        kind = line_kind(line, n);
        if (kind == '+' || kind == '-' || kind == ' ') return 1;
        p = memchr(p, '\n', (size_t)(end - p));
        if (!p) break;
        p += 1;
    }
    return 0;
}

static const char *hunk_body_end(const char *p, const char *end) {
    while (p && p < end && strncmp(p, "@@ ", 3) != 0 && strncmp(p, "diff ", 5) != 0) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return end;
        p = nl + 1;
    }
    return p ? p : end;
}

static void mark_at(char *marks, bint_t marks_len, long line, char m) {
    long at = line - 1;
    if (!marks || at < 0 || at >= marks_len) return;
    if (m == '~' || marks[at] == ' ') marks[at] = m;
}

static void apply_header_marks(char *marks, bint_t marks_len, long old_count, long new_start, long new_count) {
    long i;
    if (!new_count && new_start >= 0 && new_start < marks_len) marks[new_start ? new_start - 1 : 0] = '-';
    else for (i = 0; i < new_count && new_start + i - 1 < marks_len; i++) {
        long at = new_start + i - 1;
        if (at >= 0) marks[at] = old_count ? '~' : '+';
    }
}

static void mark_body(char *marks, bint_t marks_len, const char *body, const char *end, long new_start, long new_count) {
    long nl = new_start > 0 ? new_start : 1;
    const char *p = body;
    while (p && p < end) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        int kind = line ? line_kind(line, n) : 0;
        const char *nlpos = memchr(p, '\n', (size_t)(end - p));
        if (kind == '-') {
            const char *q = nlpos ? nlpos + 1 : end;
            int qn = 0;
            int next_add = 0;
            if (q < end) {
                next_line(q, end, &qn);
                next_add = line_kind(q, qn) == '+';
            }
            if (next_add) {
                mark_at(marks, marks_len, nl, '~');
                nl += 1;
                p = q;
                nlpos = memchr(p, '\n', (size_t)(end - p));
                if (!nlpos) break;
                p = nlpos + 1;
                continue;
            }
            if (new_count == 0) mark_at(marks, marks_len, new_start > 0 ? new_start : 1, '-');
            else mark_at(marks, marks_len, nl > 0 ? nl : 1, '-');
        } else if (kind == '+') {
            mark_at(marks, marks_len, nl, '+');
            nl += 1;
        } else if (kind == ' ') {
            nl += 1;
        }
        if (!nlpos) break;
        p = nlpos + 1;
    }
}

int git_extra_debug_marks(const char *diff, char *marks, bint_t marks_len) {
    const char *p = diff;
    const char *end;
    int hunks = 0;
    if (!diff || !marks || marks_len < 1) return MLE_ERR;
    end = diff + strlen(diff);
    memset(marks, ' ', (size_t)marks_len);
    while (p < end && *p) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        int n = nl ? (int)(nl - p) : (int)(end - p);
        long old_start = 0, old_count = 1, new_start = 0, new_count = 1;
        if (parse_hunk(p, n, &old_start, &old_count, &new_start, &new_count, NULL, NULL)) {
            const char *body = nl ? nl + 1 : end;
            const char *body_end = hunk_body_end(body, end);
            if (hunk_has_body(body, end)) mark_body(marks, marks_len, body, body_end, new_start, new_count);
            else apply_header_marks(marks, marks_len, old_count, new_start, new_count);
            hunks += 1;
            p = body_end;
            continue;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return hunks;
}

static void pad_cell(char *dst, int width, long no, const char *text, int len, int present) {
    char tmp[160];
    int n = 0;
    int i;
    if (width < 1) {
        dst[0] = '\0';
        return;
    }
    if (!present) {
        memset(dst, ' ', (size_t)width);
        dst[width] = '\0';
        return;
    }
    if (no > 0) n = snprintf(tmp, sizeof(tmp), "%4ld | ", no);
    else n = snprintf(tmp, sizeof(tmp), "%s", "     | ");
    if (n < 0) n = 0;
    for (i = 0; i < len && n + 1 < (int)sizeof(tmp); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\t' || c < 32 || c == 127) c = ' ';
        if (c >= 0x80) {
            int need = 1;
            if ((c & 0xE0) == 0xC0) need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            if (i + need > len || n + need >= (int)sizeof(tmp)) break;
            memcpy(tmp + n, text + i, (size_t)need);
            n += need;
            i += need - 1;
            continue;
        }
        tmp[n++] = (char)c;
    }
    tmp[n] = '\0';
    if (n > width) {
        int cut = width;
        while (cut > 0 && ((unsigned char)tmp[cut] & 0xC0) == 0x80) cut -= 1;
        memcpy(dst, tmp, (size_t)cut);
        if (cut < width) memset(dst + cut, ' ', (size_t)(width - cut));
    } else {
        memcpy(dst, tmp, (size_t)n);
        memset(dst + n, ' ', (size_t)(width - n));
    }
    dst[width] = '\0';
}

static void emit_row(str_t *out, int *rows, int *truncated, const char *line) {
    if (*truncated) return;
    if (*rows >= 220) {
        str_append(out, "... more changes are below this cut\n");
        *truncated = 1;
        return;
    }
    str_append(out, (char *)line);
    *rows += 1;
}

static void take_name(const char *p, int n, char *name, int namen) {
    const char *s = p;
    int len = n;
    if (len >= 2 && s[0] == 'b' && s[1] == '/') {
        s += 2;
        len -= 2;
    }
    if (len >= 2 && s[0] == '"') {
        s += 1;
        len -= 1;
        if (len > 0 && s[len - 1] == '"') len -= 1;
    }
    if (len >= namen) len = namen - 1;
    if (len < 0) len = 0;
    memcpy(name, s, (size_t)len);
    name[len] = '\0';
}

static void section_name(const char *start, const char *end, char *name, int namen) {
    const char *p = start;
    name[0] = '\0';
    while (p < end) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        const char *nl;
        if (!line) break;
        if (n >= 4 && strncmp(line, "+++ ", 4) == 0) {
            take_name(line + 4, n - 4, name, namen);
            return;
        }
        nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        p = nl + 1;
    }
}

static void emit_hunk(str_t *out, int *rows, int *truncated, int *first, const char *header, int header_len, const char *body, const char *end) {
    long old_start = 0, old_count = 1, new_start = 0, new_count = 1;
    const char *func = NULL;
    int func_len = 0;
    long old_no, new_no;
    const char *p;
    char banner[160];
    int pure_add, pure_del;
    if (!parse_hunk(header, header_len, &old_start, &old_count, &new_start, &new_count, &func, &func_len)) return;
    pure_add = old_count == 0;
    pure_del = new_count == 0;
    if (pure_add) snprintf(banner, sizeof(banner), "added at line %ld%s%.*s\n", new_start > 0 ? new_start : 1, func_len ? "  " : "", func_len, func ? func : "");
    else if (pure_del) snprintf(banner, sizeof(banner), "removed at line %ld%s%.*s\n", old_start > 0 ? old_start : 1, func_len ? "  " : "", func_len, func ? func : "");
    else snprintf(banner, sizeof(banner), "line %ld%s%.*s\n", new_start > 0 ? new_start : 1, func_len ? "  " : "", func_len, func ? func : "");
    emit_row(out, rows, truncated, banner);
    if (!pure_add && !pure_del) emit_row(out, rows, truncated, "  before                                    after\n");
    old_no = old_start;
    new_no = new_start;
    p = body;
    while (p < end && !*truncated) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        int kind = line ? line_kind(line, n) : 0;
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (kind == ' ') {
            char left[48], right[64], row[140];
            const char *text = n > 0 ? line + 1 : "";
            int tlen = n > 0 ? n - 1 : 0;
            if (pure_add) {
                pad_cell(right, 56, new_no, text, tlen, 1);
                snprintf(row, sizeof(row), "  %s\n", right);
            } else if (pure_del) {
                pad_cell(left, 40, old_no, text, tlen, 1);
                snprintf(row, sizeof(row), "  %s\n", left);
            } else {
                pad_cell(left, 40, old_no, text, tlen, 1);
                pad_cell(right, 56, new_no, text, tlen, 1);
                snprintf(row, sizeof(row), "  %s  %s\n", left, right);
            }
            emit_row(out, rows, truncated, row);
            old_no += 1;
            new_no += 1;
        } else if (kind == '-' || kind == '+') {
            const char *ltext[48];
            const char *rtext[48];
            int llen[48], rlen[48];
            long lno[48], rno[48];
            int lc = 0, rc = 0, i, pairs;
            while (p < end && (kind == '-' || kind == '+')) {
                const char *text = n > 0 ? line + 1 : "";
                int tlen = n > 0 ? n - 1 : 0;
                if (kind == '-' && lc < 48) {
                    ltext[lc] = text;
                    llen[lc] = tlen;
                    lno[lc] = old_no;
                    lc += 1;
                    old_no += 1;
                } else if (kind == '+' && rc < 48) {
                    rtext[rc] = text;
                    rlen[rc] = tlen;
                    rno[rc] = new_no;
                    rc += 1;
                    new_no += 1;
                }
                if (!nl) {
                    p = end;
                    break;
                }
                p = nl + 1;
                if (p >= end) break;
                line = next_line(p, end, &n);
                kind = line ? line_kind(line, n) : 0;
                nl = memchr(p, '\n', (size_t)(end - p));
                if (kind != '-' && kind != '+') break;
            }
            pairs = lc > rc ? lc : rc;
            for (i = 0; i < pairs; i++) {
                char left[48], right[64], row[140];
                int has_l = i < lc;
                int has_r = i < rc;
                char mark = (has_l && has_r) ? '~' : (has_l ? '-' : '+');
                long shown = has_r ? rno[i] : (new_no > 0 ? new_no : new_start);
                if (first && *first == 0 && shown > 0) *first = (int)shown;
                if (pure_add && has_r) {
                    pad_cell(right, 56, rno[i], rtext[i], rlen[i], 1);
                    snprintf(row, sizeof(row), "+ %s\n", right);
                } else if (pure_del && has_l) {
                    pad_cell(left, 40, lno[i], ltext[i], llen[i], 1);
                    snprintf(row, sizeof(row), "- %s\n", left);
                } else {
                    pad_cell(left, 40, has_l ? lno[i] : 0, has_l ? ltext[i] : "", has_l ? llen[i] : 0, has_l);
                    pad_cell(right, 56, has_r ? rno[i] : 0, has_r ? rtext[i] : "", has_r ? rlen[i] : 0, has_r);
                    snprintf(row, sizeof(row), "%c %s  %s\n", mark, left, right);
                }
                emit_row(out, rows, truncated, row);
            }
            continue;
        }
        if (!nl) break;
        p = nl + 1;
    }
    emit_row(out, rows, truncated, "\n");
}

static int emit_section(str_t *out, const char *heading, const char *start, const char *end, int *first) {
    char name[PATH_MAX];
    const char *p;
    int adds = 0, rems = 0, rows = 0, truncated = 0, hunks = 0;
    section_name(start, end, name, (int)sizeof(name));
    p = start;
    while (p < end) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        int kind = line ? line_kind(line, n) : 0;
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (kind == '+') adds += 1;
        else if (kind == '-') rems += 1;
        if (!nl) break;
        p = nl + 1;
    }
    if (adds < 1 && rems < 1) return 0;
    if (heading && heading[0]) str_sprintf(out, "%s\n", heading);
    str_sprintf(out, "%s\n%d added, %d removed\n", name[0] ? name : "this file", adds, rems);
    str_append(out, "~ a line changed, + added, - removed. Left was, right is now.\n\n");
    p = start;
    while (p < end && !truncated) {
        int n = 0;
        const char *line = next_line(p, end, &n);
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        long dummy_old_s, dummy_old_c, dummy_new_s, dummy_new_c;
        if (line && parse_hunk(line, n, &dummy_old_s, &dummy_old_c, &dummy_new_s, &dummy_new_c, NULL, NULL)) {
            const char *body = nl ? nl + 1 : end;
            const char *body_end = hunk_body_end(body, end);
            emit_hunk(out, &rows, &truncated, first, line, n, body, body_end);
            hunks += 1;
            p = body_end;
            continue;
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!hunks) str_append(out, "the file changed, but there is no text hunk to show\n\n");
    return adds + rems;
}

int git_extra_format_compare(const char *diff, const char *heading, char **out, int *first_line) {
    str_t text = {0};
    const char *p;
    const char *end;
    int total = 0;
    int first = 0;
    int any = 0;
    if (out) *out = NULL;
    if (first_line) *first_line = 0;
    if (!diff || !diff[0]) return 0;
    if (strstr(diff, "Binary files ")) {
        str_sprintf(&text, "%s\nbinary file, git has no line-by-line comparison\n\n", heading && heading[0] ? heading : "binary");
        if (out) *out = text.data ? text.data : strdup("");
        if (first_line) *first_line = 0;
        return 1;
    }
    end = diff + strlen(diff);
    p = diff;
    if (strncmp(diff, "diff --git ", 11) == 0) {
        while (p < end) {
            const char *next = p + 1;
            const char *found = NULL;
            while (next < end) {
                if ((next == diff || next[-1] == '\n') && strncmp(next, "diff --git ", 11) == 0 && next != p) {
                    found = next;
                    break;
                }
                next += 1;
            }
            if (!found) found = end;
            {
                int n = emit_section(&text, any ? NULL : heading, p, found, &first);
                if (n > 0) {
                    total += n;
                    any = 1;
                }
            }
            p = found;
        }
    } else {
        total = emit_section(&text, heading, diff, end, &first);
        any = total > 0;
    }
    if (!any) {
        str_free(&text);
        return 0;
    }
    if (out) *out = text.data ? text.data : strdup("");
    else str_free(&text);
    if (first_line) *first_line = first;
    return total;
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
    char *unstaged;
    char *staged;
    char *work = NULL;
    char *index = NULL;
    str_t menu = {0};
    int line = 0;
    int line2 = 0;
    int nwork;
    int nindex;
    if (!bv || !bv->buffer || !bv->buffer->path) return MLE_OK;
    unstaged = gx_git_diff(ctx->editor, bv->buffer->path, 0);
    staged = gx_git_diff(ctx->editor, bv->buffer->path, 1);
    nwork = git_extra_format_compare(unstaged, "work tree", &work, &line);
    nindex = git_extra_format_compare(staged, "staged", &index, &line2);
    if (!line) line = line2;
    if (work) str_append(&menu, work);
    if (index) str_append(&menu, index);
    if (nwork < 1 && nindex < 1) str_append(&menu, "  Nothing changed against HEAD\n");
    editor_menu(ctx->editor, NULL, menu.data ? menu.data : "", menu.len, NULL, NULL);
    free(unstaged);
    free(staged);
    free(work);
    free(index);
    str_free(&menu);
    (void)line;
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

static int gx_show_git(cmd_context_t *ctx, const char *tail) {
    const char *root_raw;
    char *root;
    char *command = NULL;
    char *out;
    root_raw = project_tools_root(ctx->editor);
    if (!root_raw || !root_raw[0]) {
        MLE_SET_ERR(ctx->editor, "%s", "git: no project root");
        return MLE_OK;
    }
    root = util_escape_shell_arg((char *)root_raw, strlen(root_raw));
    if (root) asprintf(&command, "git -C %s %s 2>&1", root, tail);
    out = command ? gx_capture(command, 1024 * 1024, NULL) : NULL;
    editor_menu(ctx->editor, NULL, out && out[0] ? out : "  git produced no output\n", out ? (int)strlen(out) : 0, NULL, NULL);
    free(root);
    free(command);
    free(out);
    return MLE_OK;
}

static int gx_cmd_log(cmd_context_t *ctx) {
    return gx_show_git(ctx, "log --decorate --graph --oneline -n 80");
}

static int gx_cmd_blame(cmd_context_t *ctx) {
    bview_t *bv = ctx->editor->active_edit;
    char *arg;
    char *tail = NULL;
    int rc;
    if (!bv || !bv->buffer || !bv->buffer->path) {
        MLE_SET_ERR(ctx->editor, "%s", "blame needs a saved file");
        return MLE_OK;
    }
    arg = util_escape_shell_arg(bv->buffer->path, strlen(bv->buffer->path));
    if (arg) asprintf(&tail, "blame --date=short -- %s", arg);
    rc = tail ? gx_show_git(ctx, tail) : MLE_ERR;
    free(arg);
    free(tail);
    return rc;
}

int git_extra_register_commands(editor_t *editor) {
    int rc = MLE_OK;
    rc |= gx_register(editor, "cmd_git_lines", gx_cmd_lines);
    rc |= gx_register(editor, "cmd_git_diff", gx_cmd_diff);
    rc |= gx_register(editor, "cmd_git_stage_hunk", gx_cmd_stage_hunk);
    rc |= gx_register(editor, "cmd_git_conflicts", gx_cmd_conflicts);
    rc |= gx_register(editor, "cmd_git_log", gx_cmd_log);
    rc |= gx_register(editor, "cmd_git_blame", gx_cmd_blame);
    return rc;
}
