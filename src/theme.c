#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mle.h"

#define THEME_CAP 16

static mle_theme_t g_themes[THEME_CAP];
static int g_nthemes;
static int g_ready;
static char g_active[24];

static void set_name(mle_theme_t *t, const char *name) {
    size_t i;
    if (!name) name = "theme";
    for (i = 0; name[i] && i + 1 < sizeof(t->name); i++) {
        char c = name[i];
        if (c == ' ' || c == '\t') c = '-';
        t->name[i] = c;
    }
    t->name[i] = '\0';
    if (!t->name[0]) snprintf(t->name, sizeof(t->name), "%s", "theme");
}

static void add_builtin(const char *name,
    uint16_t text_fg, uint16_t text_bg,
    uint16_t comment, uint16_t string, uint16_t number,
    uint16_t keyword, uint16_t preproc, uint16_t punct,
    uint16_t sel_fg, uint16_t sel_bg,
    uint16_t file_fg, uint16_t file_bg,
    uint16_t pos_fg, uint16_t pos_bg,
    uint16_t git_fg, uint16_t git_bg,
    uint16_t accent_fg, uint16_t accent_bg,
    uint16_t grip_fg, uint16_t grip_bg,
    uint16_t line_bg
) {
    mle_theme_t *t;
    if (g_nthemes >= THEME_CAP) return;
    t = &g_themes[g_nthemes++];
    memset(t, 0, sizeof(*t));
    set_name(t, name);
    t->user = 0;
    t->text_fg = text_fg;
    t->text_bg = text_bg;
    t->comment = comment;
    t->string = string;
    t->number = number;
    t->keyword = keyword;
    t->preproc = preproc;
    t->punct = punct;
    t->sel_fg = sel_fg;
    t->sel_bg = sel_bg;
    t->file_fg = file_fg;
    t->file_bg = file_bg;
    t->pos_fg = pos_fg;
    t->pos_bg = pos_bg;
    t->git_fg = git_fg;
    t->git_bg = git_bg;
    t->accent_fg = accent_fg;
    t->accent_bg = accent_bg;
    t->muted_fg = text_fg ? text_fg : TB_WHITE;
    t->muted_bg = text_bg ? text_bg : TB_BLACK;
    t->line_fg = text_fg;
    t->line_bg = line_bg;
    t->grip_fg = grip_fg;
    t->grip_bg = grip_bg;
    t->err_fg = TB_WHITE | TB_BOLD;
    t->err_bg = TB_RED;
    t->bar_fg = TB_BLACK | TB_BOLD;
    t->bar_bg = TB_WHITE;
    if (text_bg == TB_WHITE) t->bar_bg = TB_CYAN;
    if (text_fg == 0 && text_bg == 0) {
        t->caption_fg = TB_BOLD;
        t->caption_bg = TB_BLUE;
        t->dir_fg = TB_CYAN | TB_BOLD;
        t->dir_bg = TB_BLACK;
    } else {
        t->caption_fg = TB_WHITE | TB_BOLD;
        t->caption_bg = accent_bg ? accent_bg : TB_BLUE;
        t->dir_fg = keyword ? keyword : (TB_CYAN | TB_BOLD);
        t->dir_bg = t->muted_bg;
    }
}

static void load_builtins(void) {
    g_nthemes = 0;
    add_builtin("mle",
        0, 0,
        TB_CYAN, TB_YELLOW | TB_BOLD, TB_BLUE | TB_BOLD,
        TB_GREEN, TB_MAGENTA | TB_BOLD, TB_RED | TB_BOLD,
        TB_BLACK, TB_CYAN,
        TB_BLACK | TB_BOLD, TB_CYAN,
        TB_BLACK, TB_YELLOW,
        TB_BLACK | TB_BOLD, TB_GREEN,
        TB_BLACK | TB_BOLD, TB_CYAN,
        TB_BLACK, TB_BLUE,
        TB_BLACK | TB_BRIGHT);
    add_builtin("paper",
        TB_BLACK, TB_WHITE,
        TB_BLUE, TB_RED, TB_MAGENTA,
        TB_BLUE | TB_BOLD, TB_MAGENTA | TB_BOLD, TB_BLACK,
        TB_WHITE, TB_BLUE,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_BLACK, TB_CYAN,
        TB_WHITE | TB_BOLD, TB_GREEN,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_WHITE, TB_BLUE,
        TB_CYAN);
    add_builtin("night",
        TB_WHITE, TB_BLACK,
        TB_BLUE | TB_BOLD, TB_YELLOW, TB_MAGENTA | TB_BOLD,
        TB_CYAN | TB_BOLD, TB_BLUE | TB_BOLD, TB_WHITE,
        TB_BLACK, TB_CYAN,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_BLACK, TB_CYAN,
        TB_BLACK | TB_BOLD, TB_GREEN,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_WHITE, TB_BLUE,
        TB_BLUE);
    add_builtin("forest",
        TB_GREEN, TB_BLACK,
        TB_GREEN, TB_YELLOW, TB_CYAN,
        TB_GREEN | TB_BOLD, TB_YELLOW | TB_BOLD, TB_GREEN,
        TB_BLACK, TB_GREEN,
        TB_BLACK | TB_BOLD, TB_GREEN,
        TB_BLACK, TB_YELLOW,
        TB_WHITE | TB_BOLD, TB_GREEN,
        TB_BLACK | TB_BOLD, TB_GREEN,
        TB_BLACK, TB_GREEN,
        TB_GREEN);
    add_builtin("ember",
        TB_YELLOW, TB_BLACK,
        TB_RED, TB_YELLOW | TB_BOLD, TB_WHITE | TB_BOLD,
        TB_RED | TB_BOLD, TB_YELLOW | TB_BOLD, TB_YELLOW,
        TB_BLACK, TB_RED,
        TB_BLACK | TB_BOLD, TB_RED,
        TB_BLACK, TB_YELLOW,
        TB_BLACK | TB_BOLD, TB_YELLOW,
        TB_WHITE | TB_BOLD, TB_RED,
        TB_BLACK, TB_RED,
        TB_RED);
    add_builtin("ocean",
        TB_CYAN, TB_BLACK,
        TB_BLUE | TB_BOLD, TB_CYAN | TB_BOLD, TB_WHITE,
        TB_BLUE | TB_BOLD, TB_MAGENTA | TB_BOLD, TB_CYAN,
        TB_BLACK, TB_CYAN,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_BLACK, TB_CYAN,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_WHITE | TB_BOLD, TB_BLUE,
        TB_WHITE, TB_BLUE,
        TB_BLUE);
}

static int parse_one_color(const char *s, uint16_t *out) {
    uint16_t v = 0;
    int any = 0;
    char buf[64];
    char *tok;
    char *save;
    size_t n;
    if (!s || !s[0]) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        *out = (uint16_t)strtoul(s, NULL, 16);
        return 0;
    }
    n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    tok = strtok_r(buf, " \t", &save);
    while (tok) {
        if (!strcmp(tok, "bold")) v |= TB_BOLD;
        else if (!strcmp(tok, "black")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_BLACK); any = 1; }
        else if (!strcmp(tok, "red")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_RED); any = 1; }
        else if (!strcmp(tok, "green")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_GREEN); any = 1; }
        else if (!strcmp(tok, "yellow")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_YELLOW); any = 1; }
        else if (!strcmp(tok, "blue")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_BLUE); any = 1; }
        else if (!strcmp(tok, "magenta")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_MAGENTA); any = 1; }
        else if (!strcmp(tok, "cyan")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_CYAN); any = 1; }
        else if (!strcmp(tok, "white")) { v = (uint16_t)((v & (uint16_t)~0xff) | TB_WHITE); any = 1; }
        else if (!strcmp(tok, "default")) { v = (uint16_t)(v & (uint16_t)~0xff); any = 1; }
        else return -1;
        tok = strtok_r(NULL, " \t", &save);
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

static int role_slot(mle_theme_t *t, const char *role, uint16_t **a, uint16_t **b) {
    *a = NULL;
    *b = NULL;
    if (!strcmp(role, "text")) { *a = &t->text_fg; return 1; }
    if (!strcmp(role, "background")) { *a = &t->text_bg; return 1; }
    if (!strcmp(role, "comment")) { *a = &t->comment; return 1; }
    if (!strcmp(role, "string")) { *a = &t->string; return 1; }
    if (!strcmp(role, "number")) { *a = &t->number; return 1; }
    if (!strcmp(role, "keyword")) { *a = &t->keyword; return 1; }
    if (!strcmp(role, "preproc")) { *a = &t->preproc; return 1; }
    if (!strcmp(role, "punct")) { *a = &t->punct; return 1; }
    if (!strcmp(role, "selection")) { *a = &t->sel_fg; *b = &t->sel_bg; return 1; }
    if (!strcmp(role, "file")) { *a = &t->file_fg; *b = &t->file_bg; return 1; }
    if (!strcmp(role, "position")) { *a = &t->pos_fg; *b = &t->pos_bg; return 1; }
    if (!strcmp(role, "git")) { *a = &t->git_fg; *b = &t->git_bg; return 1; }
    if (!strcmp(role, "accent")) { *a = &t->accent_fg; *b = &t->accent_bg; return 1; }
    if (!strcmp(role, "grip")) { *a = &t->grip_fg; *b = &t->grip_bg; return 1; }
    if (!strcmp(role, "error")) { *a = &t->err_fg; *b = &t->err_bg; return 1; }
    if (!strcmp(role, "line")) { *a = &t->line_fg; *b = &t->line_bg; return 1; }
    if (!strcmp(role, "bar")) { *a = &t->bar_fg; *b = &t->bar_bg; return 1; }
    if (!strcmp(role, "caption")) { *a = &t->caption_fg; *b = &t->caption_bg; return 1; }
    if (!strcmp(role, "dir")) { *a = &t->dir_fg; *b = &t->dir_bg; return 1; }
    return 0;
}

static void apply_role_line(mle_theme_t *t, const char *role, const char *val) {
    uint16_t *a;
    uint16_t *b;
    char left[64];
    char right[64];
    const char *slash;
    size_t n;
    if (!role_slot(t, role, &a, &b) || !a || !val) return;
    slash = strchr(val, '/');
    if (slash && b) {
        n = (size_t)(slash - val);
        if (n >= sizeof(left)) n = sizeof(left) - 1;
        memcpy(left, val, n);
        left[n] = '\0';
        snprintf(right, sizeof(right), "%s", slash + 1);
        if (parse_one_color(left, a) != 0) return;
        parse_one_color(right, b);
        return;
    }
    if (parse_one_color(val, a) != 0) return;
    if (b && !slash) *b = *a;
}

static int find_name(const char *name) {
    int i;
    for (i = 0; i < g_nthemes; i++) {
        if (!strcmp(g_themes[i].name, name)) return i;
    }
    return -1;
}

static int path_for(char *dst, size_t n, int directory) {
    const char *home;
    const char *tail;
    size_t i, j;
    home = getenv("HOME");
    if (!home || !home[0] || n < 8) return -1;
    tail = directory ? "/.mle" : "/.mle/themes";
    i = 0;
    for (j = 0; home[j] && i + 1 < n; j++) dst[i++] = home[j];
    if (home[j]) return -1;
    for (j = 0; tail[j] && i + 1 < n; j++) dst[i++] = tail[j];
    if (tail[j]) return -1;
    dst[i] = '\0';
    return 0;
}

static const char *color_word(uint16_t c) {
    switch (c & 0xff) {
        case TB_BLACK: return "black";
        case TB_RED: return "red";
        case TB_GREEN: return "green";
        case TB_YELLOW: return "yellow";
        case TB_BLUE: return "blue";
        case TB_MAGENTA: return "magenta";
        case TB_CYAN: return "cyan";
        case TB_WHITE: return "white";
        default: return "default";
    }
}

static void write_color(FILE *fp, const char *role, uint16_t v) {
    if ((v & (uint16_t)~(0xff | TB_BOLD)) != 0) {
        fprintf(fp, "%s 0x%04x\n", role, (unsigned)v);
        return;
    }
    fprintf(fp, "%s %s%s\n", role, (v & TB_BOLD) ? "bold " : "", color_word(v));
}

static void write_pair(FILE *fp, const char *role, uint16_t fg, uint16_t bg) {
    int fb = (fg & TB_BOLD) != 0;
    int bb = (bg & TB_BOLD) != 0;
    if ((fg & (uint16_t)~(0xff | TB_BOLD)) || (bg & (uint16_t)~(0xff | TB_BOLD))) {
        fprintf(fp, "%s 0x%04x/0x%04x\n", role, (unsigned)fg, (unsigned)bg);
        return;
    }
    fprintf(fp, "%s %s%s/%s%s\n", role,
        fb ? "bold " : "", color_word(fg),
        bb ? "bold " : "", color_word(bg));
}

static int save_file(void) {
    char dir[512];
    char path[512];
    FILE *fp;
    int i;
    if (path_for(dir, sizeof(dir), 1) != 0) return -1;
    mkdir(dir, 0755);
    if (path_for(path, sizeof(path), 0) != 0) return -1;
    fp = fopen(path, "w");
    if (!fp) return -1;
    if (g_active[0]) fprintf(fp, "active %s\n\n", g_active);
    for (i = 0; i < g_nthemes; i++) {
        mle_theme_t *t = &g_themes[i];
        if (!t->user) continue;
        fprintf(fp, "[%s]\n", t->name);
        write_color(fp, "text", t->text_fg);
        write_color(fp, "background", t->text_bg);
        write_color(fp, "comment", t->comment);
        write_color(fp, "string", t->string);
        write_color(fp, "number", t->number);
        write_color(fp, "keyword", t->keyword);
        write_color(fp, "preproc", t->preproc);
        write_color(fp, "punct", t->punct);
        write_pair(fp, "selection", t->sel_fg, t->sel_bg);
        write_pair(fp, "file", t->file_fg, t->file_bg);
        write_pair(fp, "position", t->pos_fg, t->pos_bg);
        write_pair(fp, "git", t->git_fg, t->git_bg);
        write_pair(fp, "accent", t->accent_fg, t->accent_bg);
        write_pair(fp, "grip", t->grip_fg, t->grip_bg);
        write_pair(fp, "error", t->err_fg, t->err_bg);
        write_pair(fp, "line", t->line_fg, t->line_bg);
        write_pair(fp, "bar", t->bar_fg, t->bar_bg);
        write_pair(fp, "caption", t->caption_fg, t->caption_bg);
        write_pair(fp, "dir", t->dir_fg, t->dir_bg);
        fprintf(fp, "\n");
    }
    fclose(fp);
    return 0;
}

static void load_file(void) {
    char path[512];
    FILE *fp;
    char line[160];
    mle_theme_t *cur = NULL;
    if (path_for(path, sizeof(path), 0) != 0) return;
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *role;
        char *val;
        size_t n;
        while (*p == ' ' || *p == '\t') p++;
        n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) {
            p[--n] = '\0';
        }
        if (!p[0] || p[0] == '#') continue;
        if (!strncmp(p, "active ", 7) && !cur) {
            snprintf(g_active, sizeof(g_active), "%s", p + 7);
            continue;
        }
        if (p[0] == '[') {
            char name[24];
            char *end = strchr(p, ']');
            int idx;
            if (!end || end == p + 1) { cur = NULL; continue; }
            *end = '\0';
            snprintf(name, sizeof(name), "%s", p + 1);
            idx = find_name(name);
            if (idx >= 0 && !g_themes[idx].user) { cur = NULL; continue; }
            if (idx >= 0) cur = &g_themes[idx];
            else if (g_nthemes < THEME_CAP && g_nthemes > 0) {
                cur = &g_themes[g_nthemes++];
                *cur = g_themes[0];
                set_name(cur, name);
                cur->user = 1;
            } else cur = NULL;
            continue;
        }
        if (!cur) continue;
        role = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p) continue;
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        val = p;
        apply_role_line(cur, role, val);
    }
    fclose(fp);
}

static void remember(editor_t *editor) {
    const char *name;
    if (!editor) return;
    if (editor->theme_i < 0 || editor->theme_i >= g_nthemes) editor->theme_i = 0;
    name = g_themes[editor->theme_i].name;
    snprintf(g_active, sizeof(g_active), "%s", name);
}

void theme_restyle(editor_t *editor) {
    bview_t *bv;
    cursor_t *cursor;
    uint16_t fg, bg;
    if (!editor) return;
    theme_selection(editor, &fg, &bg);
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        if (!bv->cursors) continue;
        DL_FOREACH(bv->cursors, cursor) {
            if (!cursor->sel_rule) continue;
            cursor->sel_rule->style.fg = fg;
            cursor->sel_rule->style.bg = bg;
        }
    }
}

void theme_init(editor_t *editor) {
    int idx;
    if (!g_ready) {
        load_builtins();
        load_file();
        g_ready = 1;
    }
    if (!editor) return;
    editor->theme_i = 0;
    if (g_active[0]) {
        idx = find_name(g_active);
        if (idx >= 0) editor->theme_i = idx;
    }
}

mle_theme_t *theme_current(editor_t *editor) {
    if (!g_ready) {
        load_builtins();
        load_file();
        g_ready = 1;
    }
    if (!g_nthemes) return &g_themes[0];
    if (!editor || editor->theme_i < 0 || editor->theme_i >= g_nthemes) return &g_themes[0];
    return &g_themes[editor->theme_i];
}

const char *theme_name(editor_t *editor) {
    return theme_current(editor)->name;
}

void theme_selection(editor_t *editor, uint16_t *fg, uint16_t *bg) {
    const mle_theme_t *t = theme_current(editor);
    if (fg) *fg = t->sel_fg;
    if (bg) *bg = t->sel_bg;
}

uint16_t theme_synth(editor_t *editor, uint16_t attr, int is_fg) {
    const mle_theme_t *t = theme_current(editor);
    uint16_t flags = (uint16_t)(attr & (uint16_t)~0xff);
    uint16_t base = (uint16_t)(attr & 0xff);
    uint16_t mapped;
    if (!t) return attr;
    if (base == TB_DEFAULT) {
        mapped = is_fg ? t->text_fg : t->text_bg;
        return (uint16_t)(mapped | flags);
    }
    switch (base) {
        case TB_CYAN: mapped = t->comment; break;
        case TB_YELLOW: mapped = t->string; break;
        case TB_BLUE: mapped = t->number; break;
        case TB_GREEN: mapped = t->keyword; break;
        case TB_MAGENTA: mapped = t->preproc; break;
        case TB_RED: mapped = t->punct; break;
        case TB_WHITE: mapped = t->text_fg ? t->text_fg : attr; break;
        case TB_BLACK: mapped = t->muted_fg; break;
        default: return attr;
    }
    /* Keep underline and reverse from the rule. Bold comes from the theme. */
    flags = (uint16_t)(flags & (TB_UNDERLINE | TB_REVERSE | TB_ITALIC | TB_BLINK));
    return (uint16_t)((mapped & 0xff) | (mapped & (uint16_t)~0xff) | flags);
}

void theme_cycle(editor_t *editor) {
    if (!editor || g_nthemes < 1) return;
    editor->theme_i += 1;
    if (editor->theme_i >= g_nthemes) editor->theme_i = 0;
    remember(editor);
    theme_restyle(editor);
    save_file();
}

int theme_create(editor_t *editor, const char *name) {
    mle_theme_t *dst;
    int idx;
    char clean[24];
    size_t i, j;
    if (!editor || !name || !name[0] || g_nthemes >= THEME_CAP) return -1;
    j = 0;
    for (i = 0; name[i] && j + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == ' ' || c == '\t') c = '-';
        if (!isalnum(c) && c != '-' && c != '_') continue;
        clean[j++] = (char)c;
    }
    clean[j] = '\0';
    if (!clean[0]) return -1;
    idx = find_name(clean);
    if (idx >= 0) {
        editor->theme_i = idx;
        remember(editor);
        return 0;
    }
    dst = &g_themes[g_nthemes];
    *dst = *theme_current(editor);
    set_name(dst, clean);
    dst->user = 1;
    editor->theme_i = g_nthemes;
    g_nthemes += 1;
    remember(editor);
    theme_restyle(editor);
    return save_file();
}

int theme_paint(editor_t *editor, const char *spec) {
    char role[32];
    char val[96];
    const char *p;
    size_t i;
    mle_theme_t *t;
    if (!editor || !spec) return -1;
    while (*spec == ' ') spec++;
    i = 0;
    while (spec[i] && spec[i] != ' ' && spec[i] != '\t' && i + 1 < sizeof(role)) {
        role[i] = spec[i];
        i++;
    }
    role[i] = '\0';
    p = spec + i;
    while (*p == ' ' || *p == '\t') p++;
    if (!role[0] || !p[0]) return -1;
    snprintf(val, sizeof(val), "%s", p);
    t = theme_current(editor);
    if (!t->user) {
        char born[24];
        snprintf(born, sizeof(born), "my-%.16s", t->name);
        if (theme_create(editor, born) != 0) return -1;
        t = theme_current(editor);
    }
    apply_role_line(t, role, val);
    remember(editor);
    theme_restyle(editor);
    return save_file();
}

const char *theme_paint_help(void) {
    return "role color: comment bold cyan, selection white/blue, background black";
}
