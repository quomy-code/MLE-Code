#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mle.h"

#define SIDE_FILES 0
#define SIDE_GIT 1
#define SIDE_GUIDE 2
#define SIDE_SET 3
#define SIDE_DEPTH 6
#define SIDE_DIR_CAP 2000
#define CAPTURE_MAX ((size_t)512 * 1024)
#define GH_MAX 8
#define FIELD_NONE 0
#define FIELD_COMMIT 1
#define FIELD_USER 2
#define FIELD_PASS 3
#define GIT_FILTER_ALL 0
#define GIT_FILTER_STAGED 1
#define GIT_FILTER_MODIFIED 2
#define GIT_FILTER_UNTRACKED 3
#define GIT_FILTER_CONFLICT 4
#define GIT_FILTER_COUNT 5

typedef struct gh_acct_s {
    char login[64];
    char secret[200];
    char source[12];
} gh_acct_t;

typedef struct side_row_s {
    char *name;
    char *path;
    int is_dir;
    int depth;
    int expanded;
    char gitx;
    char gity;
    int conflict;
    int added;
    int removed;
} side_row_t;

struct side_panel_s {
    int visible;
    int focused;
    int w;
    int mode;
    int drag;
    char cwd[PATH_MAX];
    char groot[PATH_MAX];
    char branch[80];
    char oid[16];
    char upstream[96];
    int staged;
    int unstaged;
    int untracked;
    int conflicts;
    int ahead;
    int behind;
    int stashes;
    int filter;
    char git_query[96];
    long git_ms;
    int git_ok;
    side_row_t *rows;
    int nrows;
    int cap;
    int cursor;
    int scroll;
    int x, y, h;
    int tab_files;
    int tab_git;
    int tab_help;
    char **open_dirs;
    int nopen;
    int capopen;
    int clicks;
    long last_ms;
    int last_row;
    char commit_msg[192];
    int commit_len;
    int commit_caret;
    int field;
    char user[64];
    int user_len;
    int user_caret;
    char pass[200];
    int pass_len;
    int pass_caret;
    gh_acct_t accts[GH_MAX];
    int nacct;
    int acct_i;
    int caretx;
    int carety;
    int careton;
    char where[96];
};

static bview_t *g_diff;
static char g_askpass[PATH_MAX];
static int auth_on;
static int saved_term_set;
static int saved_ask_set;
static char *saved_term_prompt;
static char *saved_askpass;

static int utf8_next(const char **sp, uint32_t *cp);
static const char *git_row_label(const side_row_t *row);
static void paint(int x, int y, int w, uint16_t fg, uint16_t bg, const char *s);
static void git_enter(editor_t *editor);
static void git_stage_default(editor_t *editor);
static int list_origin(const struct side_panel_s *p);

static const char *guide_text =
    "mle\n"
    "\n"
    "Everyday keys\n"
    "  Arrows, Home, End, PgUp, PgDn, and Shift-arrows.\n"
    "  Ctrl-S saves. Ctrl-O opens. Ctrl-X quits.\n"
    "  Ctrl-Z undoes. Ctrl-Y redoes. Ctrl-F finds. Ctrl-G finds next.\n"
    "  Ctrl-K cuts. Ctrl-U pastes. Shift-Insert pastes the clipboard.\n"
    "  F1 is this guide. F2 lists keys. F11 opens the command palette.\n"
    "\n"
    "Mouse\n"
    "  Drag across letters. The letter under the pointer is included.\n"
    "  Double-click selects the word. Triple-click selects the line.\n"
    "  Drag the line numbers to select whole lines.\n"
    "  The wheel scrolls. Bottom and right scrollbars can be clicked or dragged.\n"
    "  Releasing a text drag copies the selection.\n"
    "  Middle-click and Shift-Insert paste. Right-click copies the word.\n"
    "  A click in a menu runs that row and leaves the menu open.\n"
    "\n"
    "Files   F9\n"
    "  The list on the left is the folder you are in.\n"
    "  Click a file to select it. Double-click, Right, or Enter opens it.\n"
    "  Click a folder to expand it. Double-click or Right steps into it.\n"
    "  Backspace or Left on .. steps up. Press / and type a folder.\n"
    "  n creates a file, N a directory, r or right-click renames, and x or\n"
    "  Delete removes the selected file after yes.\n"
    "  A path starting with ~ is your home directory. Drag the edge to resize.\n"
    "  Esc returns to the editor. F9 hides the list.\n"
    "\n"
    "Git   F10\n"
    "  The status bar shows the branch and how many files changed.\n"
    "  Click that branch to open the git list.\n"
    "  The account line is under the branch. If gh or git already has a\n"
    "  GitHub login, mle uses it. g, [, ], Left, and Right switch accounts.\n"
    "  n starts a new login. Type the username, then the password.\n"
    "  GitHub wants a personal access token in the password field.\n"
    "  Enter saves that login and selects it. N forgets one mle stored.\n"
    "  Type the commit on the next line. Enter commits it.\n"
    "  Commit, Push, Pull, and Fetch are the buttons under that line.\n"
    "  Opening git stages every change, so Commit and Push are ready.\n"
    "  Click a file to stage it, or to unstage it if it is already staged.\n"
    "  Right-click compares it. Double-click or Enter opens it.\n"
    "  ~ changed, + added, - removed. The list shows +N -N.\n"
    "  Status labels say modified, staged, untracked, renamed, or conflict.\n"
    "  f, or a click on the branch row, filters those labels. / searches files.\n"
    "  a/u stage or unstage one file. A/U do that for every file.\n"
    "  d shows one diff. D shows the full stat. l shows the log, L its graph.\n"
    "  b lists branches. B switches one; C creates and switches to one.\n"
    "  t lists tags, T creates one, R shows remotes, and w shows worktrees.\n"
    "  h shows one file's history; v shows blame. i lists saved stashes.\n"
    "  s stashes, S pops, and F fetches. r refreshes. o opens the file.\n"
    "  m merges a branch. M aborts a merge after yes.\n"
    "  e amends the last commit after yes. q shows a short status.\n"
    "  G continues a rebase. Z aborts a rebase after yes.\n"
    "  Push asks you to type yes. Pull runs and shows the result.\n"
    "  x discards one file after you type yes.\n"
    "  Esc leaves the field, then leaves the list. Arrows, Home, End,\n"
    "  and Backspace edit the field. Shift-Insert or Ctrl-V pastes.\n"
    "\n"
    "Symbols\n"
    "  F8, symbols, cycles five sets compiled into mle.\n"
    "  plain is ASCII. marks is arrows and triangles.\n"
    "  mini, slab, and line are bitmap fonts, drawn as braille,\n"
    "  so the shape does not depend on the terminal font.\n"
    "\n"
    "Settings   F8\n"
    "  The panel draws each option as a row with a value chip.\n"
    "  Enter or a click applies that row and stays here.\n"
    "  Esc, or the close row, returns to the editor.\n"
    "  Themes: mle, paper, night, forest, ember, ocean.\n"
    "  newtheme copies the current theme. painttheme sets one color.\n"
    "  Saved themes live in ~/.mle/themes and come back next launch.\n"
    "\n"
    "Terminal   F7, Alt-grave, or Ctrl-Shift-~\n"
    "  Drag the bar between the editor and the shell to resize it.\n"
    "  + on the tab row opens another shell. x on a tab closes that one.\n"
    "  F7 or Ctrl-Shift-~ hides the panel. Ctrl-C inside it goes to the shell.\n"
    "  Alt-t still reaches the full command list.\n"
    "\n"
    "Status bar\n"
    "  Click the file name for the buffer list.\n"
    "  Click the line number to jump. Click the branch for git.\n";

const char *side_guide_text(void) {
    return guide_text;
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int is_open(struct side_panel_s *p, const char *path) {
    int i;
    for (i = 0; i < p->nopen; i++) {
        if (strcmp(p->open_dirs[i], path) == 0) return 1;
    }
    return 0;
}

static void set_open(struct side_panel_s *p, const char *path, int on) {
    int i;
    char *copy;
    for (i = 0; i < p->nopen; i++) {
        if (strcmp(p->open_dirs[i], path) == 0) {
            if (!on) {
                free(p->open_dirs[i]);
                p->open_dirs[i] = p->open_dirs[p->nopen - 1];
                p->nopen -= 1;
            }
            return;
        }
    }
    if (!on) return;
    copy = strdup(path);
    if (!copy) return;
    if (p->nopen + 1 > p->capopen) {
        char **next;
        int next_cap;
        next_cap = p->capopen ? p->capopen * 2 : 16;
        next = realloc(p->open_dirs, (size_t)next_cap * sizeof(char *));
        if (!next) {
            free(copy);
            return;
        }
        p->open_dirs = next;
        p->capopen = next_cap;
    }
    p->open_dirs[p->nopen++] = copy;
}

static void free_rows(struct side_panel_s *p) {
    int i;
    for (i = 0; i < p->nrows; i++) {
        free(p->rows[i].name);
        free(p->rows[i].path);
    }
    p->nrows = 0;
}

static int add_row(struct side_panel_s *p, const char *name, const char *path, int is_dir, int depth, int expanded) {
    side_row_t *r;
    char *name_copy;
    char *path_copy;
    name_copy = strdup(name ? name : "");
    path_copy = strdup(path ? path : "");
    if (!name_copy || !path_copy) {
        free(name_copy);
        free(path_copy);
        return -1;
    }
    if (p->nrows + 1 > p->cap) {
        side_row_t *next;
        int next_cap;
        next_cap = p->cap ? p->cap * 2 : 64;
        next = realloc(p->rows, (size_t)next_cap * sizeof(side_row_t));
        if (!next) {
            free(name_copy);
            free(path_copy);
            return -1;
        }
        p->rows = next;
        p->cap = next_cap;
    }
    r = &p->rows[p->nrows++];
    memset(r, 0, sizeof(*r));
    r->name = name_copy;
    r->path = path_copy;
    r->is_dir = is_dir;
    r->depth = depth;
    r->expanded = expanded;
    return 0;
}

typedef struct scan_item_s {
    char *name;
    char *path;
    int is_dir;
} scan_item_t;

static int cmp_scan(const void *a, const void *b) {
    const scan_item_t *ea = a;
    const scan_item_t *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static int read_dir(const char *dir, scan_item_t **out, int *nout) {
    DIR *dp;
    struct dirent *de;
    scan_item_t *items;
    int n, cap;
    items = NULL;
    n = 0;
    cap = 0;
    dp = opendir(dir);
    if (!dp) {
        *out = NULL;
        *nout = 0;
        return -1;
    }
    while ((de = readdir(dp)) != NULL) {
        struct stat st;
        scan_item_t *it;
        char path[PATH_MAX];
        int dirbit;
        char *name_copy;
        char *path_copy;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n >= SIDE_DIR_CAP) break;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        dirbit = 0;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) dirbit = 1;
        if (n + 1 > cap) {
            scan_item_t *next;
            int next_cap;
            next_cap = cap ? cap * 2 : 32;
            next = realloc(items, (size_t)next_cap * sizeof(scan_item_t));
            if (!next) break;
            items = next;
            cap = next_cap;
        }
        name_copy = strdup(de->d_name);
        path_copy = strdup(path);
        if (!name_copy || !path_copy) {
            free(name_copy);
            free(path_copy);
            break;
        }
        it = &items[n++];
        it->name = name_copy;
        it->path = path_copy;
        it->is_dir = dirbit;
    }
    closedir(dp);
    if (n > 1) qsort(items, (size_t)n, sizeof(scan_item_t), cmp_scan);
    *out = items;
    *nout = n;
    return 0;
}

static void free_scan(scan_item_t *items, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free(items[i].name);
        free(items[i].path);
    }
    free(items);
}

static void fill_dir(struct side_panel_s *p, const char *dir, int depth) {
    scan_item_t *items;
    int n, i;
    if (depth > SIDE_DEPTH) return;
    if (read_dir(dir, &items, &n) != 0) return;
    for (i = 0; i < n; i++) {
        int open = items[i].is_dir && is_open(p, items[i].path);
        add_row(p, items[i].name, items[i].path, items[i].is_dir, depth, open);
        if (open) fill_dir(p, items[i].path, depth + 1);
    }
    free_scan(items, n);
}

static void rescan_files(struct side_panel_s *p) {
    int keep;
    keep = p->cursor;
    free_rows(p);
    if (strcmp(p->cwd, "/") != 0) add_row(p, "..", p->cwd, 1, 0, 0);
    fill_dir(p, p->cwd, 0);
    if (p->nrows < 1) p->cursor = 0;
    else if (keep >= p->nrows) p->cursor = p->nrows - 1;
    else if (keep >= 0) p->cursor = keep;
}

static char *shell_arg(const char *s) {
    return util_escape_shell_arg((char *)s, (int)strlen(s));
}

static int capture_ex(const char *cmd, char **out, size_t *outlen, int *exit_code) {
    FILE *fp;
    char buf[1024];
    char *data;
    size_t len, cap, n;
    int status;
    int failed;
    if (!cmd) return -1;
    data = NULL;
    len = 0;
    cap = 0;
    failed = 0;
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        char *next;
        size_t keep;
        size_t next_cap;
        if (failed || len >= CAPTURE_MAX) continue;
        keep = n;
        if (keep > CAPTURE_MAX - len) keep = CAPTURE_MAX - len;
        if (len + keep + 1 > cap) {
            next_cap = (len + keep + 1) * 2;
            if (next_cap < 1024) next_cap = 1024;
            next = realloc(data, next_cap);
            if (!next) {
                failed = 1;
                continue;
            }
            data = next;
            cap = next_cap;
        }
        memcpy(data + len, buf, keep);
        len += keep;
    }
    status = pclose(fp);
    if (failed) {
        free(data);
        return -1;
    }
    if (data) data[len] = '\0';
    else {
        data = calloc(1, 1);
        len = 0;
    }
    *out = data;
    *outlen = len;
    if (exit_code) {
        if (status >= 0 && WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
        else *exit_code = -1;
    }
    return 0;
}

static int capture(const char *cmd, char **out, size_t *outlen) {
    return capture_ex(cmd, out, outlen, NULL);
}

static void info_line(editor_t *editor, const char *text) {
    char b[160];
    int i;
    if (!text || !text[0]) return;
    i = 0;
    while (text[i] && text[i] != '\n' && i + 1 < (int)sizeof(b)) {
        b[i] = text[i];
        i += 1;
    }
    b[i] = '\0';
    if (i) MLE_SET_INFO(editor, "%s", b);
}

static void find_git_root_from(struct side_panel_s *p, const char *start) {
    char cur[PATH_MAX];
    struct stat st;
    char probe[PATH_MAX];
    p->groot[0] = '\0';
    if (!start || !start[0]) return;
    snprintf(cur, sizeof(cur), "%s", start);
    while (cur[0]) {
        {
            size_t clen = strlen(cur);
            if (clen + 6 >= sizeof(probe)) break;
            memcpy(probe, cur, clen);
            memcpy(probe + clen, "/.git", 6);
        }
        if (stat(probe, &st) == 0) {
            snprintf(p->groot, sizeof(p->groot), "%s", cur);
            return;
        }
        if (strcmp(cur, "/") == 0) break;
        {
            char *sl = strrchr(cur, '/');
            if (!sl) break;
            if (sl == cur) cur[1] = '\0';
            else *sl = '\0';
        }
    }
}

static int git_is_conflict(char x, char y) {
    return (x == 'D' && y == 'D') || (x == 'A' && y == 'U')
        || (x == 'U' && y == 'D') || (x == 'U' && y == 'A')
        || (x == 'D' && y == 'U') || (x == 'A' && y == 'A')
        || (x == 'U' && y == 'U');
}

static const char *git_filter_name(int filter) {
    if (filter == GIT_FILTER_STAGED) return "staged";
    if (filter == GIT_FILTER_MODIFIED) return "modified";
    if (filter == GIT_FILTER_UNTRACKED) return "untracked";
    if (filter == GIT_FILTER_CONFLICT) return "conflicts";
    return "all";
}

static int git_filter_match(int filter, char x, char y, int conflict) {
    if (filter == GIT_FILTER_STAGED) return x != '.' && x != '?';
    if (filter == GIT_FILTER_MODIFIED) return y != '.' && y != '?';
    if (filter == GIT_FILTER_UNTRACKED) return x == '?' && y == '?';
    if (filter == GIT_FILTER_CONFLICT) return conflict;
    return 1;
}

static const char *field_after(const char *s, int len, int spaces) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] != ' ') continue;
        spaces -= 1;
        if (spaces == 0) return s + i + 1;
    }
    return NULL;
}

static void add_git_row(struct side_panel_s *p, const char *path, int path_len, char x, char y, int conflict) {
    char *name;
    char *full;
    if (!p || p->mode != SIDE_GIT || !path || path_len < 1 || !git_filter_match(p->filter, x, y, conflict)) return;
    name = strndup(path, (size_t)path_len);
    if (!name) return;
    if (p->git_query[0] && !strcasestr(name, p->git_query)) {
        free(name);
        return;
    }
    full = NULL;
    if (asprintf(&full, "%s/%s", p->groot, name) < 0) full = NULL;
    if (full) {
        if (add_row(p, name, full, 0, 0, 0) == 0) {
            p->rows[p->nrows - 1].gitx = x;
            p->rows[p->nrows - 1].gity = y;
            p->rows[p->nrows - 1].conflict = conflict;
        }
    }
    free(full);
    free(name);
}

static void clear_git_status(struct side_panel_s *p) {
    p->branch[0] = '\0';
    p->oid[0] = '\0';
    p->upstream[0] = '\0';
    p->staged = 0;
    p->unstaged = 0;
    p->untracked = 0;
    p->conflicts = 0;
    p->ahead = 0;
    p->behind = 0;
    p->stashes = 0;
    p->git_ok = 0;
}

static void parse_porcelain(struct side_panel_s *p, const char *text, size_t text_len) {
    const char *s;
    const char *end;
    clear_git_status(p);
    if (!text) return;
    s = text;
    end = text + text_len;
    while (s < end) {
        const char *nul;
        const char *path;
        int len;
        char x;
        char y;
        int conflict;
        nul = memchr(s, '\0', (size_t)(end - s));
        if (!nul) nul = end;
        len = (int)(nul - s);
        if (len > 13 && strncmp(s, "# branch.oid ", 13) == 0) {
            snprintf(p->oid, sizeof(p->oid), "%.12s", s + 13);
            p->git_ok = 1;
        } else if (len > 14 && strncmp(s, "# branch.head ", 14) == 0) {
            snprintf(p->branch, sizeof(p->branch), "%.*s", len - 14, s + 14);
            p->git_ok = 1;
        } else if (len > 18 && strncmp(s, "# branch.upstream ", 18) == 0) {
            snprintf(p->upstream, sizeof(p->upstream), "%.*s", len - 18, s + 18);
        } else if (len > 12 && strncmp(s, "# branch.ab ", 12) == 0) {
            sscanf(s + 12, "+%d -%d", &p->ahead, &p->behind);
        } else if (len > 8 && strncmp(s, "# stash ", 8) == 0) {
            p->stashes = atoi(s + 8);
        } else if (len > 0 && s[0] == '#') {
            p->git_ok = 1;
        } else if (len >= 4 && (s[0] == '1' || s[0] == '2' || s[0] == 'u')) {
            x = s[2];
            y = s[3];
            conflict = s[0] == 'u' || git_is_conflict(x, y);
            if (x != '.' && x != '?') p->staged += 1;
            if (y != '.' && y != '?') p->unstaged += 1;
            if (conflict) p->conflicts += 1;
            path = field_after(s, len, s[0] == '1' ? 8 : (s[0] == '2' ? 9 : 10));
            if (path) add_git_row(p, path, len - (int)(path - s), x, y, conflict);
            p->git_ok = 1;
            if (s[0] == '2' && nul < end) {
                s = nul + 1;
                nul = memchr(s, '\0', (size_t)(end - s));
                if (!nul) nul = end;
            }
        } else if (len >= 3 && s[0] == '?') {
            x = '?';
            y = '?';
            if (x == '?' && y == '?') p->untracked += 1;
            add_git_row(p, s + 2, len - 2, x, y, 0);
            p->git_ok = 1;
        }
        if (nul >= end) break;
        s = nul + 1;
    }
    if (strcmp(p->branch, "(detached)") == 0) {
        if (p->oid[0]) snprintf(p->branch, sizeof(p->branch), "detached@%.12s", p->oid);
        else snprintf(p->branch, sizeof(p->branch), "%s", "detached");
    }
}

int side_debug_parse_git(const char *text, size_t len, char *out, int out_cap) {
    struct side_panel_s p;
    int i;
    int n;
    int used;
    if (!out || out_cap < 1) return -1;
    memset(&p, 0, sizeof(p));
    p.mode = SIDE_GIT;
    snprintf(p.groot, sizeof(p.groot), "%s", "/repo");
    parse_porcelain(&p, text, len);
    n = snprintf(out, (size_t)out_cap,
        "branch=%s upstream=%s ahead=%d behind=%d stash=%d staged=%d modified=%d untracked=%d conflicts=%d rows=%d",
        p.branch, p.upstream, p.ahead, p.behind, p.stashes, p.staged,
        p.unstaged, p.untracked, p.conflicts, p.nrows);
    used = n > 0 ? n : 0;
    for (i = 0; i < p.nrows && used + 1 < out_cap; i++) {
        n = snprintf(out + used, (size_t)(out_cap - used), "|%s:%s", git_row_label(&p.rows[i]), p.rows[i].name);
        if (n < 0) break;
        if (n >= out_cap - used) {
            used = out_cap - 1;
            break;
        }
        used += n;
    }
    out[out_cap - 1] = '\0';
    free_rows(&p);
    free(p.rows);
    return used;
}

static void numstat_new_path(const char *path, int plen, char *out, int outn) {
    const char *arrow = NULL;
    const char *brace = NULL;
    const char *close = NULL;
    const char *q;
    int i;
    if (!out || outn < 2) return;
    out[0] = '\0';
    if (!path || plen < 1) return;
    for (i = 0; i + 4 <= plen; i++) {
        if (memcmp(path + i, " => ", 4) == 0) {
            arrow = path + i;
            break;
        }
    }
    if (!arrow) {
        snprintf(out, (size_t)outn, "%.*s", plen, path);
        return;
    }
    for (q = path; q < arrow; q++) if (*q == '{') brace = q;
    for (q = arrow + 4; q < path + plen; q++) {
        if (*q == '}') {
            close = q;
            break;
        }
    }
    if (brace && close) {
        snprintf(out, (size_t)outn, "%.*s%.*s%.*s",
            (int)(brace - path), path,
            (int)(close - (arrow + 4)), arrow + 4,
            (int)((path + plen) - (close + 1)), close + 1);
    } else {
        snprintf(out, (size_t)outn, "%.*s", (int)((path + plen) - (arrow + 4)), arrow + 4);
    }
}

static void add_numstat(struct side_panel_s *p, const char *path, int plen, int add, int rem) {
    int i;
    if (!p || !path || plen < 1) return;
    for (i = 0; i < p->nrows; i++) {
        if ((int)strlen(p->rows[i].name) == plen && memcmp(p->rows[i].name, path, (size_t)plen) == 0) {
            if (add < 0 || rem < 0) {
                p->rows[i].added = -1;
                p->rows[i].removed = -1;
            } else if (p->rows[i].added >= 0 && p->rows[i].removed >= 0) {
                p->rows[i].added += add;
                p->rows[i].removed += rem;
            }
            return;
        }
    }
}

static void apply_numstat(struct side_panel_s *p, const char *text, size_t len) {
    const char *s;
    const char *end;
    if (!p || !text) return;
    s = text;
    end = text + len;
    while (s < end && *s) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *tab1;
        const char *tab2;
        char path[PATH_MAX];
        int add;
        int rem;
        int plen;
        if (!nl) nl = end;
        tab1 = memchr(s, '\t', (size_t)(nl - s));
        tab2 = tab1 ? memchr(tab1 + 1, '\t', (size_t)(nl - (tab1 + 1))) : NULL;
        if (tab1 && tab2) {
            add = s[0] == '-' ? -1 : atoi(s);
            rem = tab1[1] == '-' ? -1 : atoi(tab1 + 1);
            plen = (int)(nl - (tab2 + 1));
            if (plen > 0 && tab2[plen] == '\r') plen -= 1;
            numstat_new_path(tab2 + 1, plen, path, (int)sizeof(path));
            add_numstat(p, path, (int)strlen(path), add, rem);
        }
        if (nl >= end) break;
        s = nl + 1;
    }
}

static void load_numstat(struct side_panel_s *p) {
    char *root;
    char *cmd;
    char *out;
    size_t outlen;
    if (!p || !p->groot[0] || p->mode != SIDE_GIT) return;
    root = shell_arg(p->groot);
    if (!root) return;
    cmd = NULL;
    out = NULL;
    outlen = 0;
    if (asprintf(&cmd, "git -C %s diff --numstat 2>/dev/null", root) < 0) cmd = NULL;
    if (cmd && capture(cmd, &out, &outlen) == 0) apply_numstat(p, out, outlen);
    free(cmd);
    free(out);
    cmd = NULL;
    out = NULL;
    outlen = 0;
    if (asprintf(&cmd, "git -C %s diff --cached --numstat 2>/dev/null", root) < 0) cmd = NULL;
    if (cmd && capture(cmd, &out, &outlen) == 0) apply_numstat(p, out, outlen);
    free(cmd);
    free(out);
    free(root);
}

static void refresh_git(editor_t *editor, int force) {
    struct side_panel_s *p;
    char *rootarg;
    char *cmd;
    char *out;
    size_t outlen;
    long now;
    if (!editor || !editor->side || editor->headless_mode) return;
    p = editor->side;
    now = mono_ms();
    if (!force && p->git_ms && now - p->git_ms < 2000) return;
    p->git_ms = now;
    find_git_root_from(p, p->cwd);
    if (!p->groot[0] && editor->active_edit && editor->active_edit->buffer && editor->active_edit->buffer->path) {
        char dir[PATH_MAX];
        char *sl;
        snprintf(dir, sizeof(dir), "%s", editor->active_edit->buffer->path);
        sl = strrchr(dir, '/');
        if (sl && sl != dir) {
            *sl = '\0';
            find_git_root_from(p, dir);
        }
    }
    if (!p->groot[0]) {
        clear_git_status(p);
        if (p->mode == SIDE_GIT) free_rows(p);
        return;
    }
    rootarg = shell_arg(p->groot);
    if (!rootarg) return;
    cmd = NULL;
    if (asprintf(&cmd, "git -C %s status --porcelain=v2 --branch --show-stash -z 2>/dev/null", rootarg) < 0) cmd = NULL;
    free(rootarg);
    if (!cmd) return;
    out = NULL;
    outlen = 0;
    if (p->mode == SIDE_GIT) free_rows(p);
    if (capture(cmd, &out, &outlen) == 0) parse_porcelain(p, out, outlen);
    else clear_git_status(p);
    free(cmd);
    free(out);
    if (p->mode == SIDE_GIT && p->git_ok) load_numstat(p);
    if (p->mode == SIDE_GIT && p->cursor >= p->nrows) p->cursor = p->nrows ? p->nrows - 1 : 0;
}

static void enter_dir(struct side_panel_s *p, const char *path) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) snprintf(resolved, sizeof(resolved), "%s", path);
    snprintf(p->cwd, sizeof(p->cwd), "%s", resolved);
    p->cursor = 0;
    p->scroll = 0;
    rescan_files(p);
}

int side_open_dir(editor_t *editor, const char *path) {
    struct side_panel_s *p;
    if (!editor || !path || !util_is_dir((char *)path)) return MLE_ERR;
    if (!editor->side) side_init(editor);
    p = editor->side;
    if (!p) return MLE_ERR;
    enter_dir(p, path);
    p->visible = 1;
    p->focused = 1;
    p->mode = SIDE_FILES;
    if (editor->iterm) iterm_action(editor, "unfocus");
    editor_relayout(editor);
    return MLE_OK;
}

static void go_parent(struct side_panel_s *p) {
    char parent[PATH_MAX];
    char *sl;
    if (strcmp(p->cwd, "/") == 0) return;
    snprintf(parent, sizeof(parent), "%s", p->cwd);
    sl = strrchr(parent, '/');
    if (!sl) return;
    if (sl == parent) parent[1] = '\0';
    else *sl = '\0';
    enter_dir(p, parent);
}

static void open_file_line(editor_t *editor, const char *path, bint_t line) {
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, (char *)path, (int)strlen(path), 1, line, 0, NULL, NULL);
    editor_relayout(editor);
}

static void open_file(editor_t *editor, const char *path) {
    open_file_line(editor, path, 0);
}

static void file_create(editor_t *editor, struct side_panel_s *p, int directory) {
    char *name = NULL;
    char path[PATH_MAX];
    int fd;
    editor_prompt(editor, directory ? "new directory name?" : "new file name?", NULL, &name);
    if (!name || !name[0] || strchr(name, '/')) { free(name); return; }
    if (snprintf(path, sizeof(path), "%s/%s", p->cwd, name) >= (int)sizeof(path)) {
        MLE_SET_ERR(editor, "%s", "path is too long");
        free(name);
        return;
    }
    if (directory) {
        if (mkdir(path, 0755) != 0) MLE_SET_ERR(editor, "mkdir failed: %s", strerror(errno));
    } else {
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) MLE_SET_ERR(editor, "create failed: %s", strerror(errno));
        else { close(fd); open_file(editor, path); }
    }
    rescan_files(p);
    free(name);
}

static void file_rename(editor_t *editor, struct side_panel_s *p, side_row_t *row) {
    char *name = NULL;
    char path[PATH_MAX];
    if (!row || strcmp(row->name, "..") == 0) return;
    editor_prompt(editor, "rename to?", NULL, &name);
    if (!name || !name[0] || strchr(name, '/')) { free(name); return; }
    if (snprintf(path, sizeof(path), "%s/%s", p->cwd, name) >= (int)sizeof(path)) {
        MLE_SET_ERR(editor, "%s", "path is too long");
    } else if (rename(row->path, path) != 0) {
        MLE_SET_ERR(editor, "rename failed: %s", strerror(errno));
    }
    rescan_files(p);
    free(name);
}

static void file_remove(editor_t *editor, struct side_panel_s *p, side_row_t *row) {
    char *answer = NULL;
    int rc;
    if (!row || strcmp(row->name, "..") == 0) return;
    editor_prompt_fmt(editor, NULL, &answer, "remove %s? type yes", row->name);
    if (!answer || strcmp(answer, "yes") != 0) { free(answer); return; }
    rc = row->is_dir ? rmdir(row->path) : unlink(row->path);
    if (rc != 0) MLE_SET_ERR(editor, "remove failed: %s", strerror(errno));
    else MLE_SET_INFO(editor, "%s", "removed; open buffers were not discarded");
    free(answer);
    rescan_files(p);
}

static void show_text(editor_t *editor, const char *title, const char *text) {
    bview_t *bv;
    (void)title;
    editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, 0, NULL, &bv);
    if (!bv || !text) return;
    buffer_insert(bv->buffer, 0, (char *)text, (bint_t)strlen(text), NULL);
    bv->buffer->is_unsaved = 0;
    mark_move_beginning(bv->active_cursor->mark);
    bview_zero_viewport_y(bv);
}

#define SET_MOUSE 0
#define SET_DRAG 1
#define SET_TABSPACE 2
#define SET_TABWIDTH 3
#define SET_SYNTAX 4
#define SET_WRAP 5
#define SET_UNDO 6
#define SET_LINES 7
#define SET_INDENT 8
#define SET_BRACKETS 9
#define SET_TRIM 10
#define SET_COLOR 11
#define SET_HLLINE 12
#define SET_THEME 13
#define SET_SYMBOLS 14
#define SET_TERM 15
#define SET_DOCK 16
#define SET_NEWTHEME 17
#define SET_PAINT 18
#define SET_REBIND 19
#define SET_FILES 20
#define SET_GIT 21
#define SET_CLOSE 22
#define SET_N 23

static const char *set_onoff(int v) {
    return v ? "on" : "off";
}

static const char *set_lines_name(int t) {
    if (t == MLE_LINENUM_TYPE_REL) return "relative";
    if (t == MLE_LINENUM_TYPE_BOTH) return "both";
    if (t == MLE_LINENUM_TYPE_NONE) return "off";
    return "absolute";
}

static bview_t *set_target(editor_t *editor) {
    if (editor->active_edit && !editor->active_edit->is_menu) return editor->active_edit;
    return editor->active_edit_last;
}

static void set_sync_lines(editor_t *editor) {
    bview_t *bv;
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        if (MLE_BVIEW_IS_EDIT(bv) && !bv->split_parent) bview_sync_linenums(bv);
    }
}

static const char *set_label(int id) {
    switch (id) {
        case SET_MOUSE: return "mouse";
        case SET_DRAG: return "drag copy";
        case SET_TABSPACE: return "tab spaces";
        case SET_TABWIDTH: return "tab width";
        case SET_SYNTAX: return "syntax";
        case SET_WRAP: return "soft wrap";
        case SET_UNDO: return "coarse undo";
        case SET_LINES: return "line numbers";
        case SET_INDENT: return "auto indent";
        case SET_BRACKETS: return "brackets";
        case SET_TRIM: return "trim paste";
        case SET_COLOR: return "ruler";
        case SET_HLLINE: return "cursor line";
        case SET_THEME: return "theme";
        case SET_SYMBOLS: return "symbols";
        case SET_TERM: return "terminal";
        case SET_DOCK: return "term dock";
        case SET_NEWTHEME: return "new theme";
        case SET_PAINT: return "paint theme";
        case SET_REBIND: return "rebind";
        case SET_FILES: return "files";
        case SET_GIT: return "git";
        case SET_CLOSE: return "close";
        default: return "";
    }
}

static void set_value(editor_t *editor, int id, char *buf, size_t n) {
    bview_t *bv;
    bv = set_target(editor);
    buf[0] = '\0';
    switch (id) {
        case SET_MOUSE: snprintf(buf, n, "%s", set_onoff(editor->mouse_support)); break;
        case SET_DRAG: snprintf(buf, n, "%s", set_onoff(editor->mouse_copy)); break;
        case SET_TABSPACE: snprintf(buf, n, "%s", set_onoff(bv ? bv->tab_to_space : editor->tab_to_space)); break;
        case SET_TABWIDTH: snprintf(buf, n, "%d", bv ? bv->tab_width : editor->tab_width); break;
        case SET_SYNTAX: snprintf(buf, n, "%s", (bv && bv->buffer && bv->buffer->is_style_disabled) ? "off" : "on"); break;
        case SET_WRAP: snprintf(buf, n, "%s", set_onoff(bv ? bv->soft_wrap : editor->soft_wrap)); break;
        case SET_UNDO: snprintf(buf, n, "%s", set_onoff(editor->coarse_undo)); break;
        case SET_LINES: snprintf(buf, n, "%s", set_lines_name(editor->linenum_type)); break;
        case SET_INDENT: snprintf(buf, n, "%s", set_onoff(editor->auto_indent)); break;
        case SET_BRACKETS: snprintf(buf, n, "%s", set_onoff(editor->highlight_bracket_pairs)); break;
        case SET_TRIM: snprintf(buf, n, "%s", set_onoff(editor->trim_paste)); break;
        case SET_COLOR: snprintf(buf, n, "%d", editor->color_col); break;
        case SET_HLLINE: snprintf(buf, n, "%s", set_onoff(editor->hl_line)); break;
        case SET_THEME: snprintf(buf, n, "%s", theme_name(editor)); break;
        case SET_SYMBOLS: snprintf(buf, n, "%s", glyphs_name()); break;
        case SET_TERM: snprintf(buf, n, "%s", iterm_visible(editor) ? "shown" : "hidden"); break;
        case SET_DOCK: snprintf(buf, n, "%s", iterm_dock_name(editor)); break;
        case SET_NEWTHEME: snprintf(buf, n, "%s", "copy"); break;
        case SET_PAINT: snprintf(buf, n, "%s", "color"); break;
        case SET_REBIND: snprintf(buf, n, "%s", "key"); break;
        case SET_FILES: snprintf(buf, n, "%s", "open"); break;
        case SET_GIT: snprintf(buf, n, "%s", "open"); break;
        case SET_CLOSE: snprintf(buf, n, "%s", "esc"); break;
        default: break;
    }
}

static void set_apply(editor_t *editor, struct side_panel_s *p) {
    bview_t *bv;
    int id;
    if (!p || p->cursor < 0 || p->cursor >= SET_N) return;
    id = p->cursor;
    bv = set_target(editor);
    if (id == SET_MOUSE) {
        editor->mouse_support = editor->mouse_support ? 0 : 1;
        editor_set_input_mode(editor);
        MLE_SET_INFO(editor, "mouse %s", set_onoff(editor->mouse_support));
    } else if (id == SET_DRAG) {
        editor->mouse_copy = editor->mouse_copy ? 0 : 1;
        MLE_SET_INFO(editor, "copy on release %s", set_onoff(editor->mouse_copy));
    } else if (id == SET_TABSPACE && bv) {
        int on = bv->tab_to_space ? 0 : 1;
        bv->tab_to_space = on;
        editor->tab_to_space = on;
        MLE_SET_INFO(editor, "tab inserts spaces %s", set_onoff(on));
    } else if (id == SET_TABWIDTH && bv) {
        char *val = NULL;
        int w;
        editor_prompt(editor, "tab width?", NULL, &val);
        if (!val) return;
        w = atoi(val);
        free(val);
        if (w < 1) w = 1;
        if (w > 32) w = 32;
        editor->tab_width = w;
        bv->tab_width = w;
        if (bv->buffer) buffer_set_tab_width(bv->buffer, w);
        MLE_SET_INFO(editor, "tab width %d", w);
    } else if (id == SET_SYNTAX && bv && bv->buffer) {
        if (bv->buffer->is_style_disabled) {
            buffer_set_styles_enabled(bv->buffer, 1);
            bview_set_syntax(bv, NULL);
            MLE_SET_INFO(editor, "%s", "syntax on");
        } else {
            buffer_set_styles_enabled(bv->buffer, 0);
            MLE_SET_INFO(editor, "%s", "syntax off");
        }
    } else if (id == SET_WRAP && bv) {
        int on = bv->soft_wrap ? 0 : 1;
        bv->soft_wrap = on;
        editor->soft_wrap = on;
        MLE_SET_INFO(editor, "soft wrap %s", set_onoff(on));
    } else if (id == SET_UNDO) {
        editor->coarse_undo = editor->coarse_undo ? 0 : 1;
        MLE_SET_INFO(editor, "coarse undo %s", set_onoff(editor->coarse_undo));
    } else if (id == SET_LINES) {
        int t = editor->linenum_type;
        if (t == MLE_LINENUM_TYPE_ABS) t = MLE_LINENUM_TYPE_REL;
        else if (t == MLE_LINENUM_TYPE_REL) t = MLE_LINENUM_TYPE_BOTH;
        else if (t == MLE_LINENUM_TYPE_BOTH) t = MLE_LINENUM_TYPE_NONE;
        else t = MLE_LINENUM_TYPE_ABS;
        editor->linenum_type = t;
        set_sync_lines(editor);
        MLE_SET_INFO(editor, "line numbers %s", set_lines_name(t));
    } else if (id == SET_INDENT) {
        editor->auto_indent = editor->auto_indent ? 0 : 1;
        MLE_SET_INFO(editor, "auto indent %s", set_onoff(editor->auto_indent));
    } else if (id == SET_BRACKETS) {
        editor->highlight_bracket_pairs = editor->highlight_bracket_pairs ? 0 : 1;
        MLE_SET_INFO(editor, "bracket highlight %s", set_onoff(editor->highlight_bracket_pairs));
    } else if (id == SET_TRIM) {
        editor->trim_paste = editor->trim_paste ? 0 : 1;
        MLE_SET_INFO(editor, "trim paste %s", set_onoff(editor->trim_paste));
    } else if (id == SET_COLOR) {
        char *val = NULL;
        int col;
        editor_prompt(editor, "ruler column? 0 hides it", NULL, &val);
        if (!val) return;
        col = atoi(val);
        free(val);
        editor->color_col = col < 1 ? -1 : col;
        MLE_SET_INFO(editor, "ruler column %d", editor->color_col);
    } else if (id == SET_HLLINE) {
        editor->hl_line = editor->hl_line ? 0 : 1;
        MLE_SET_INFO(editor, "cursor line %s", set_onoff(editor->hl_line));
    } else if (id == SET_THEME) {
        theme_cycle(editor);
        MLE_SET_INFO(editor, "theme %s", theme_name(editor));
    } else if (id == SET_SYMBOLS) {
        glyphs_cycle();
        MLE_SET_INFO(editor, "symbols %s", glyphs_name());
    } else if (id == SET_TERM) {
        iterm_action(editor, "toggle");
        MLE_SET_INFO(editor, "terminal %s", iterm_visible(editor) ? "shown" : "hidden");
    } else if (id == SET_DOCK) {
        iterm_action(editor, "dockcycle");
        MLE_SET_INFO(editor, "terminal dock %s", iterm_dock_name(editor));
    } else if (id == SET_NEWTHEME) {
        char *name = NULL;
        editor_prompt(editor, "new theme name?", NULL, &name);
        if (!name) return;
        if (!name[0] || theme_create(editor, name) != 0) MLE_SET_ERR(editor, "%s", "theme: could not create");
        else MLE_SET_INFO(editor, "theme %s", theme_name(editor));
        free(name);
    } else if (id == SET_PAINT) {
        char *spec = NULL;
        editor_prompt(editor, "paint: comment bold cyan", NULL, &spec);
        if (!spec) return;
        if (theme_paint(editor, spec) != 0) MLE_SET_ERR(editor, "%s", "theme: could not paint");
        else MLE_SET_INFO(editor, "theme %s", theme_name(editor));
        free(spec);
    } else if (id == SET_REBIND) {
        char *cmdn = NULL;
        char *key = NULL;
        editor_prompt(editor, "command? (cmd_save)", NULL, &cmdn);
        if (!cmdn || !cmdn[0]) { free(cmdn); return; }
        editor_prompt(editor, "key? (C-s, f5)", NULL, &key);
        if (!key || !key[0]) { free(cmdn); free(key); return; }
        if (editor_bind(editor, "mle_normal", cmdn, key, NULL) != MLE_OK) MLE_SET_ERR(editor, "rebind: no command %s", cmdn);
        else MLE_SET_INFO(editor, "rebind: %s runs %s", key, cmdn);
        free(cmdn);
        free(key);
    } else if (id == SET_FILES) {
        p->mode = SIDE_FILES;
        rescan_files(p);
    } else if (id == SET_GIT) {
        p->mode = SIDE_GIT;
        p->git_ms = 0;
        refresh_git(editor, 1);
        git_stage_default(editor);
        git_enter(editor);
    } else if (id == SET_CLOSE) {
        p->focused = 0;
    }
}

static void draw_settings(editor_t *editor, struct side_panel_s *p, int body, int body_fg, int body_bg, int on_fg, int on_bg, int path_fg, int path_bg) {
    int i;
    mle_theme_t *th;
    th = theme_current(editor);
    for (i = 0; i < body; i++) {
        int id, y, fg, bg, chip_fg, chip_bg, name_w, chip_w;
        char val[48];
        char line[160];
        const char *label;
        id = p->scroll + i;
        y = p->y + list_origin(p) + i;
        if (id < 0 || id >= SET_N) {
            paint(p->x, y, p->w - 1, body_fg, body_bg, "");
            continue;
        }
        label = set_label(id);
        set_value(editor, id, val, sizeof(val));
        chip_w = (int)strlen(val) + 2;
        if (chip_w < 4) chip_w = 4;
        if (chip_w > p->w - 6) chip_w = p->w - 6;
        if (chip_w < 1) chip_w = 1;
        name_w = p->w - 1 - chip_w;
        if (name_w < 1) name_w = 1;
        snprintf(line, sizeof(line), " %s", label);
        if (id == p->cursor && p->focused) {
            fg = on_fg;
            bg = on_bg;
        } else if (id == p->cursor) {
            fg = path_fg;
            bg = path_bg;
        } else {
            fg = body_fg;
            bg = body_bg;
        }
        paint(p->x, y, name_w, fg, bg, line);
        if (!strcmp(val, "on") || !strcmp(val, "shown")) {
            chip_fg = th->git_fg;
            chip_bg = th->git_bg;
        } else if (!strcmp(val, "off") || !strcmp(val, "hidden")) {
            chip_fg = th->muted_fg;
            chip_bg = th->muted_bg;
        } else {
            chip_fg = th->accent_fg;
            chip_bg = th->accent_bg;
        }
        if (id == p->cursor && p->focused) {
            chip_fg = on_fg;
            chip_bg = th->bar_bg ? th->bar_bg : on_bg;
        }
        snprintf(line, sizeof(line), " %s", val);
        paint(p->x + name_w, y, chip_w, chip_fg, chip_bg, line);
    }
}

static int list_origin(const struct side_panel_s *p) {
    if (p && p->mode == SIDE_GIT) return 5;
    return 2;
}

static int row_span(const struct side_panel_s *p) {
    if (p && p->mode == SIDE_SET) return SET_N;
    return p ? p->nrows : 0;
}

static void wipe_mem(void *ptr, size_t n) {
    volatile unsigned char *b;
    b = (volatile unsigned char *)ptr;
    while (n--) *b++ = 0;
}

static void pass_clear(struct side_panel_s *p) {
    wipe_mem(p->pass, sizeof(p->pass));
    p->pass_len = 0;
    p->pass_caret = 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static int pct_decode(const char *s, int n, char *out, int cap) {
    int i, o;
    if (!out || cap < 2 || n < 0) return -1;
    i = 0;
    o = 0;
    while (i < n) {
        unsigned char c;
        if (s[i] == '%' && i + 2 < n
            && isxdigit((unsigned char)s[i + 1])
            && isxdigit((unsigned char)s[i + 2])) {
            c = (unsigned char)((hexval(s[i + 1]) << 4) | hexval(s[i + 2]));
            i += 3;
        } else {
            c = (unsigned char)s[i++];
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

static int acct_ok_login(const char *s) {
    const unsigned char *p;
    int n;
    if (!s || !s[0]) return 0;
    n = 0;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p <= 32 || *p == ':' || *p == '/' || *p == '@' || *p == '\\') return 0;
        n += 1;
        if (n > 63) return 0;
    }
    return 1;
}

static int acct_ok_secret(const char *s) {
    const unsigned char *p;
    int n;
    if (!s || !s[0]) return 0;
    n = 0;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p == 127) return 0;
        n += 1;
        if (n > 199) return 0;
    }
    return 1;
}

static int acct_put(struct side_panel_s *p, const char *login, const char *secret, const char *source, int overwrite) {
    int i;
    if (!acct_ok_login(login)) return -1;
    if (secret && secret[0] && !acct_ok_secret(secret)) return -1;
    for (i = 0; i < p->nacct; i++) {
        if (strcmp(p->accts[i].login, login) != 0) continue;
        if (overwrite) {
            if (secret && secret[0]) snprintf(p->accts[i].secret, sizeof(p->accts[i].secret), "%s", secret);
            if (source && source[0]) snprintf(p->accts[i].source, sizeof(p->accts[i].source), "%s", source);
            return i;
        }
        if (!p->accts[i].secret[0] && secret && secret[0]) {
            snprintf(p->accts[i].secret, sizeof(p->accts[i].secret), "%s", secret);
            if ((!p->accts[i].source[0]) && source && source[0]) {
                snprintf(p->accts[i].source, sizeof(p->accts[i].source), "%s", source);
            }
        }
        return i;
    }
    if (!secret || !secret[0] || p->nacct >= GH_MAX) return -1;
    i = p->nacct++;
    memset(&p->accts[i], 0, sizeof(p->accts[i]));
    snprintf(p->accts[i].login, sizeof(p->accts[i].login), "%s", login);
    snprintf(p->accts[i].secret, sizeof(p->accts[i].secret), "%s", secret);
    if (source) snprintf(p->accts[i].source, sizeof(p->accts[i].source), "%s", source);
    return i;
}

static int acct_any_secret(const struct side_panel_s *p) {
    int i;
    for (i = 0; i < p->nacct; i++) {
        if (p->accts[i].secret[0]) return 1;
    }
    return 0;
}

static void copy_fit(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || cap < 1) return;
    dst[0] = '\0';
    if (!src) return;
    n = strlen(src);
    if (n >= cap) return;
    memcpy(dst, src, n + 1);
}

static int mle_path(char *buf, size_t n, const char *leaf) {
    const char *home;
    int k;
    home = getenv("HOME");
    if (!home || !home[0] || !buf || n < 8) return 0;
    if (leaf && leaf[0]) k = snprintf(buf, n, "%s/.mle/%s", home, leaf);
    else k = snprintf(buf, n, "%s/.mle", home);
    if (k < 0 || (size_t)k >= n) return 0;
    return 1;
}

static void acct_askpass_install(void) {
    char dir[PATH_MAX];
    char path[PATH_MAX];
    int fd;
    const char *body;
    size_t len;
    ssize_t wr;
    /* The token stays in the environment. This file only prints it. */
    body = "#!/bin/sh\nprintf '%s\\n' \"$MLE_GH_SECRET\"\n";
    g_askpass[0] = '\0';
    if (!mle_path(dir, sizeof(dir), NULL)) return;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return;
    if (!mle_path(path, sizeof(path), "askpass")) return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (fd < 0) return;
    len = strlen(body);
    wr = write(fd, body, len);
    if (wr == (ssize_t)len) fchmod(fd, 0700);
    close(fd);
    if (wr == (ssize_t)len) snprintf(g_askpass, sizeof(g_askpass), "%s", path);
}

static void acct_write(struct side_panel_s *p) {
    char dir[PATH_MAX];
    char path[PATH_MAX];
    int fd, i;
    FILE *fp;
    if (!mle_path(dir, sizeof(dir), NULL)) return;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return;
    if (!mle_path(path, sizeof(path), "github")) return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return;
    }
    if (p->nacct > 0 && p->acct_i >= 0 && p->acct_i < p->nacct && acct_ok_login(p->accts[p->acct_i].login)) {
        fprintf(fp, "active %s\n", p->accts[p->acct_i].login);
    }
    for (i = 0; i < p->nacct; i++) {
        if (strcmp(p->accts[i].source, "manual") != 0) continue;
        if (!acct_ok_login(p->accts[i].login) || !acct_ok_secret(p->accts[i].secret)) continue;
        fprintf(fp, "%s\t%s\n", p->accts[i].login, p->accts[i].secret);
    }
    fclose(fp);
}

static void acct_read_store(struct side_panel_s *p, char *active, size_t active_cap) {
    char path[PATH_MAX];
    FILE *fp;
    char line[512];
    if (!active || active_cap < 2) return;
    if (!mle_path(path, sizeof(path), "github")) return;
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *s, *tab;
        size_t n;
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        if (strncmp(s, "active ", 7) == 0) {
            s += 7;
            while (*s == ' ') s++;
            snprintf(active, active_cap, "%s", s);
            continue;
        }
        tab = strchr(s, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (!acct_ok_login(s) || !acct_ok_secret(tab + 1)) continue;
        acct_put(p, s, tab + 1, "manual", 1);
        wipe_mem(tab + 1, strlen(tab + 1));
    }
    fclose(fp);
}

static void line_key(const char *s, int len, int *indent, char *key, int kcap, char *val, int vcap) {
    int i, k, ind, b, e;
    i = 0;
    ind = 0;
    if (kcap > 0) key[0] = '\0';
    if (vcap > 0) val[0] = '\0';
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        ind += (s[i] == '\t') ? 4 : 1;
        i += 1;
    }
    *indent = ind;
    if (i >= len || s[i] == '#' || kcap < 2 || vcap < 2) return;
    k = 0;
    while (i < len && s[i] != ':' && s[i] != '#' && k + 1 < kcap) key[k++] = s[i++];
    while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t')) k -= 1;
    key[k] = '\0';
    if (i >= len || s[i] != ':') {
        key[0] = '\0';
        return;
    }
    i += 1;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i += 1;
    b = i;
    e = len;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e -= 1;
    if (b < e && (s[b] == '"' || s[b] == '\'')) {
        int q = s[b];
        b += 1;
        for (i = b; i < e; i++) {
            if (s[i] == q) {
                e = i;
                break;
            }
        }
    } else {
        for (i = b; i < e; i++) {
            if (s[i] == '#' && (i == b || s[i - 1] == ' ')) {
                e = i;
                break;
            }
        }
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e -= 1;
    }
    if (e < b) e = b;
    if (e - b >= vcap) e = b + vcap - 1;
    if (e > b) memcpy(val, s + b, (size_t)(e - b));
    val[e > b ? e - b : 0] = '\0';
}

static void acct_read_hosts(struct side_panel_s *p, char *hint, size_t hint_cap) {
    char path[PATH_MAX];
    FILE *fp;
    char *text, *chunk;
    size_t len, cap, nread;
    const char *home, *ghdir, *s;
    int host_indent, users_indent, user_indent;
    char host_user[64], host_token[200], pend_user[64], pend_token[200];
    home = getenv("HOME");
    ghdir = getenv("GH_CONFIG_DIR");
    if (ghdir && ghdir[0]) {
        if (snprintf(path, sizeof(path), "%s/hosts.yml", ghdir) >= (int)sizeof(path)) return;
    } else if (home && home[0]) {
        if (snprintf(path, sizeof(path), "%s/.config/gh/hosts.yml", home) >= (int)sizeof(path)) return;
    } else {
        return;
    }
    fp = fopen(path, "r");
    if (!fp) return;
    text = NULL;
    len = 0;
    cap = 0;
    chunk = malloc(4096);
    if (!chunk) {
        fclose(fp);
        return;
    }
    while ((nread = fread(chunk, 1, 4096, fp)) > 0) {
        if (len + nread + 1 > cap) {
            size_t nc = cap ? cap * 2 : 8192;
            char *nd;
            while (nc < len + nread + 1) nc *= 2;
            if (nc > 256 * 1024) {
                free(chunk);
                free(text);
                fclose(fp);
                return;
            }
            nd = realloc(text, nc);
            if (!nd) {
                free(chunk);
                free(text);
                fclose(fp);
                return;
            }
            text = nd;
            cap = nc;
        }
        memcpy(text + len, chunk, nread);
        len += nread;
    }
    free(chunk);
    fclose(fp);
    if (!text) return;
    text[len] = '\0';
    host_indent = -1;
    users_indent = -1;
    user_indent = -1;
    host_user[0] = host_token[0] = pend_user[0] = pend_token[0] = '\0';
    s = text;
    while (*s) {
        const char *nl;
        int ln, indent;
        char key[80], val[200];
        nl = strchr(s, '\n');
        ln = nl ? (int)(nl - s) : (int)strlen(s);
        line_key(s, ln, &indent, key, (int)sizeof(key), val, (int)sizeof(val));
        if (key[0] && (indent == 0 || (host_indent >= 0 && indent <= host_indent))) {
            if (host_user[0] && host_token[0]) acct_put(p, host_user, host_token, "gh", 0);
            if (pend_user[0] && pend_token[0]) acct_put(p, pend_user, pend_token, "gh", 0);
            host_user[0] = host_token[0] = pend_user[0] = pend_token[0] = '\0';
            users_indent = -1;
            user_indent = -1;
            if (strcmp(key, "github.com") == 0) host_indent = indent;
            else host_indent = -1;
        } else if (host_indent >= 0 && key[0]) {
            if (users_indent >= 0 && indent <= users_indent) {
                if (pend_user[0] && pend_token[0]) acct_put(p, pend_user, pend_token, "gh", 0);
                pend_user[0] = pend_token[0] = '\0';
                users_indent = -1;
                user_indent = -1;
            }
            if (users_indent >= 0 && indent > users_indent) {
                if (strcmp(key, "oauth_token") != 0 && strcmp(key, "git_protocol") != 0
                    && (user_indent < 0 || indent <= user_indent)) {
                    if (pend_user[0] && pend_token[0]) acct_put(p, pend_user, pend_token, "gh", 0);
                    copy_fit(pend_user, sizeof(pend_user), key);
                    pend_token[0] = '\0';
                    user_indent = indent;
                } else if (strcmp(key, "oauth_token") == 0 && pend_user[0] && val[0]) {
                    copy_fit(pend_token, sizeof(pend_token), val);
                    acct_put(p, pend_user, pend_token, "gh", 0);
                }
            } else if (strcmp(key, "users") == 0) {
                users_indent = indent;
            } else if (strcmp(key, "user") == 0 && val[0]) {
                copy_fit(host_user, sizeof(host_user), val);
                if (hint && hint_cap > 1) copy_fit(hint, hint_cap, val);
            } else if (strcmp(key, "oauth_token") == 0 && val[0]) {
                copy_fit(host_token, sizeof(host_token), val);
            }
        }
        if (!nl) break;
        s = nl + 1;
    }
    if (pend_user[0] && pend_token[0]) acct_put(p, pend_user, pend_token, "gh", 0);
    if (host_user[0] && host_token[0]) acct_put(p, host_user, host_token, "gh", 0);
    wipe_mem(host_token, sizeof(host_token));
    wipe_mem(pend_token, sizeof(pend_token));
    wipe_mem(text, len);
    free(text);
}

static void acct_read_gitcred(struct side_panel_s *p) {
    char path[PATH_MAX];
    FILE *fp;
    char line[1024];
    const char *home;
    home = getenv("HOME");
    if (!home || !home[0]) return;
    if (snprintf(path, sizeof(path), "%s/.git-credentials", home) >= (int)sizeof(path)) return;
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *at, *colon, *host, *s;
        char login[64], secret[200];
        int ulen, slen;
        if (strncmp(line, "https://", 8) != 0) continue;
        s = line + 8;
        at = strchr(s, '@');
        if (!at) continue;
        host = at + 1;
        if (strncmp(host, "github.com", 10) != 0) continue;
        if (host[10] && host[10] != '/' && host[10] != ':' && host[10] != '\n'
            && host[10] != '\r' && host[10] != ' ' && host[10] != '\t') continue;
        colon = memchr(s, ':', (size_t)(at - s));
        if (!colon || colon == s) continue;
        ulen = pct_decode(s, (int)(colon - s), login, (int)sizeof(login));
        slen = pct_decode(colon + 1, (int)(at - colon - 1), secret, (int)sizeof(secret));
        if (ulen < 1 || slen < 1 || !acct_ok_login(login) || !acct_ok_secret(secret)) {
            wipe_mem(secret, sizeof(secret));
            continue;
        }
        acct_put(p, login, secret, "git", 0);
        wipe_mem(secret, sizeof(secret));
    }
    fclose(fp);
}

static void acct_try_gh(struct side_panel_s *p, const char *hint) {
    char *out;
    size_t n;
    char tok[200];
    if (acct_any_secret(p)) return;
    out = NULL;
    n = 0;
    if (capture("gh auth token --hostname github.com 2>/dev/null", &out, &n) != 0 || !out) {
        free(out);
        return;
    }
    {
        int i, b, e;
        b = 0;
        e = (int)strlen(out);
        while (b < e && (out[b] == ' ' || out[b] == '\t' || out[b] == '\n' || out[b] == '\r')) b += 1;
        while (e > b && (out[e - 1] == ' ' || out[e - 1] == '\t' || out[e - 1] == '\n' || out[e - 1] == '\r')) e -= 1;
        if (e - b <= 0 || e - b >= (int)sizeof(tok)) {
            wipe_mem(out, n ? n : strlen(out));
            free(out);
            return;
        }
        for (i = b; i < e; i++) {
            unsigned char c = (unsigned char)out[i];
            if (c < 33 || c > 126) {
                wipe_mem(out, n ? n : strlen(out));
                free(out);
                return;
            }
        }
        memcpy(tok, out + b, (size_t)(e - b));
        tok[e - b] = '\0';
    }
    wipe_mem(out, n ? n : strlen(out));
    free(out);
    if (hint && hint[0]) acct_put(p, hint, tok, "gh", 0);
    else if (p->nacct == 1 && !p->accts[0].secret[0]) {
        snprintf(p->accts[0].secret, sizeof(p->accts[0].secret), "%s", tok);
        if (!p->accts[0].source[0]) snprintf(p->accts[0].source, sizeof(p->accts[0].source), "%s", "gh");
    }
    wipe_mem(tok, sizeof(tok));
}

static void acct_load(struct side_panel_s *p) {
    gh_acct_t keep[GH_MAX];
    int nkeep, i;
    char active[64];
    char hint[64];
    const char *home;
    nkeep = 0;
    active[0] = '\0';
    hint[0] = '\0';
    home = getenv("HOME");
    if (!home || !home[0]) {
        for (i = 0; i < p->nacct && nkeep < GH_MAX; i++) {
            if (strcmp(p->accts[i].source, "manual") == 0 && p->accts[i].login[0]) keep[nkeep++] = p->accts[i];
        }
        if (p->nacct > 0 && p->acct_i >= 0 && p->acct_i < p->nacct) {
            snprintf(active, sizeof(active), "%s", p->accts[p->acct_i].login);
        }
    }
    wipe_mem(p->accts, sizeof(p->accts));
    p->nacct = 0;
    p->acct_i = 0;
    for (i = 0; i < nkeep; i++) p->accts[p->nacct++] = keep[i];
    if (home && home[0]) acct_read_store(p, active, sizeof(active));
    acct_read_hosts(p, hint, sizeof(hint));
    acct_read_gitcred(p);
    if (!acct_any_secret(p)) acct_try_gh(p, hint);
    if (!active[0] && hint[0]) snprintf(active, sizeof(active), "%s", hint);
    p->acct_i = 0;
    if (active[0]) {
        for (i = 0; i < p->nacct; i++) {
            if (strcmp(p->accts[i].login, active) == 0) {
                p->acct_i = i;
                break;
            }
        }
    }
    acct_askpass_install();
    wipe_mem(keep, sizeof(keep));
}

static void acct_begin_login(struct side_panel_s *p) {
    wipe_mem(p->user, sizeof(p->user));
    p->user_len = 0;
    p->user_caret = 0;
    pass_clear(p);
    p->field = FIELD_USER;
}

static void acct_finish_login(editor_t *editor) {
    struct side_panel_s *p;
    int idx;
    p = editor->side;
    if (!p) return;
    if (p->user_len < 0) p->user_len = 0;
    if (p->user_len >= (int)sizeof(p->user)) p->user_len = (int)sizeof(p->user) - 1;
    p->user[p->user_len] = '\0';
    if (p->pass_len < 0) p->pass_len = 0;
    if (p->pass_len >= (int)sizeof(p->pass)) p->pass_len = (int)sizeof(p->pass) - 1;
    p->pass[p->pass_len] = '\0';
    if (!acct_ok_login(p->user)) {
        p->field = FIELD_USER;
        MLE_SET_ERR(editor, "%s", "git: type a username, with no spaces");
        return;
    }
    if (!acct_ok_secret(p->pass)) {
        p->field = FIELD_PASS;
        MLE_SET_ERR(editor, "%s", "git: type a password or a token");
        return;
    }
    idx = acct_put(p, p->user, p->pass, "manual", 1);
    pass_clear(p);
    if (idx < 0) {
        MLE_SET_ERR(editor, "%s", "git: account list is full");
        p->field = FIELD_USER;
        return;
    }
    p->acct_i = idx;
    acct_write(p);
    p->field = FIELD_COMMIT;
    if (p->commit_caret < 0) p->commit_caret = 0;
    if (p->commit_caret > p->commit_len) p->commit_caret = p->commit_len;
    MLE_SET_INFO(editor, "git: using %s", p->accts[idx].login);
}

static void acct_cycle(editor_t *editor, int dir) {
    struct side_panel_s *p;
    p = editor->side;
    if (!p || p->nacct < 1) {
        MLE_SET_INFO(editor, "%s", "git: no account yet");
        return;
    }
    p->acct_i = (p->acct_i + dir) % p->nacct;
    if (p->acct_i < 0) p->acct_i += p->nacct;
    acct_write(p);
    MLE_SET_INFO(editor, "git: %s  %s  %d/%d", p->accts[p->acct_i].login,
        p->accts[p->acct_i].source[0] ? p->accts[p->acct_i].source : "github",
        p->acct_i + 1, p->nacct);
}

static void acct_forget(editor_t *editor) {
    struct side_panel_s *p;
    int i;
    p = editor->side;
    if (!p || p->nacct < 1 || p->acct_i < 0 || p->acct_i >= p->nacct) {
        MLE_SET_INFO(editor, "%s", "git: no account");
        return;
    }
    if (strcmp(p->accts[p->acct_i].source, "manual") != 0) {
        MLE_SET_INFO(editor, "%s", "git: that login comes from gh or git");
        return;
    }
    for (i = p->acct_i; i + 1 < p->nacct; i++) p->accts[i] = p->accts[i + 1];
    p->nacct -= 1;
    wipe_mem(&p->accts[p->nacct], sizeof(p->accts[0]));
    if (p->acct_i >= p->nacct) p->acct_i = p->nacct ? p->nacct - 1 : 0;
    acct_write(p);
    if (p->nacct < 1) acct_begin_login(p);
    MLE_SET_INFO(editor, "%s", "git: forgot that login");
}

static void git_enter(editor_t *editor) {
    struct side_panel_s *p;
    p = editor->side;
    if (!p) return;
    acct_load(p);
    if (p->field == FIELD_USER || p->field == FIELD_PASS) return;
    if (p->nacct < 1) {
        if (p->user_len > 0) p->field = FIELD_USER;
        else acct_begin_login(p);
        return;
    }
    p->field = FIELD_COMMIT;
    if (p->commit_caret < 0) p->commit_caret = 0;
    if (p->commit_caret > p->commit_len) p->commit_caret = p->commit_len;
}

static void auth_end(void) {
    if (auth_on) {
        unsetenv("MLE_GH_SECRET");
        if (saved_term_set && saved_term_prompt) setenv("GIT_TERMINAL_PROMPT", saved_term_prompt, 1);
        else unsetenv("GIT_TERMINAL_PROMPT");
        if (saved_ask_set && saved_askpass) setenv("GIT_ASKPASS", saved_askpass, 1);
        else unsetenv("GIT_ASKPASS");
        auth_on = 0;
    }
    free(saved_term_prompt);
    free(saved_askpass);
    saved_term_prompt = NULL;
    saved_askpass = NULL;
    saved_term_set = 0;
    saved_ask_set = 0;
}

static int auth_begin(struct side_panel_s *p) {
    gh_acct_t *a;
    const char *old;
    if (!g_askpass[0]) return 0;
    if (!p || p->acct_i < 0 || p->acct_i >= p->nacct) return 0;
    a = &p->accts[p->acct_i];
    if (!a->login[0] || !a->secret[0]) return 0;
    if (auth_on) auth_end();
    old = getenv("GIT_TERMINAL_PROMPT");
    saved_term_set = old ? 1 : 0;
    saved_term_prompt = old ? strdup(old) : NULL;
    old = getenv("GIT_ASKPASS");
    saved_ask_set = old ? 1 : 0;
    saved_askpass = old ? strdup(old) : NULL;
    auth_on = 1;
    if (setenv("GIT_TERMINAL_PROMPT", "0", 1) != 0
        || setenv("GIT_ASKPASS", g_askpass, 1) != 0
        || setenv("MLE_GH_SECRET", a->secret, 1) != 0) {
        auth_end();
        return 0;
    }
    return 1;
}

static int git_run(editor_t *editor, const char *fmt_tail, int show_all, int use_auth) {
    struct side_panel_s *p;
    char *rootarg;
    char *userarg;
    char *cmd;
    char *out;
    size_t outlen;
    int authed;
    int have_cmd;
    int exit_code;
    p = editor->side;
    authed = 0;
    userarg = NULL;
    if (!p->groot[0]) find_git_root_from(p, p->cwd);
    if (!p->groot[0]) {
        MLE_SET_ERR(editor, "%s", "git: no repository here");
        return -1;
    }
    rootarg = shell_arg(p->groot);
    if (!rootarg) return -1;
    cmd = NULL;
    if (use_auth && p->nacct > 0 && p->acct_i >= 0 && p->acct_i < p->nacct
        && strcmp(p->accts[p->acct_i].source, "manual") == 0
        && p->accts[p->acct_i].login[0] && p->accts[p->acct_i].secret[0]) {
        userarg = shell_arg(p->accts[p->acct_i].login);
        if (!userarg || !auth_begin(p)) {
            free(rootarg);
            free(userarg);
            MLE_SET_ERR(editor, "%s", "git: could not use that account");
            return -1;
        }
        authed = 1;
        if (asprintf(&cmd, "git -c credential.helper= -c credential.username=%s -C %s %s 2>&1", userarg, rootarg, fmt_tail) < 0) {
            cmd = NULL;
        }
    } else if (asprintf(&cmd, "git -C %s %s 2>&1", rootarg, fmt_tail) < 0) {
        cmd = NULL;
    }
    out = NULL;
    outlen = 0;
    exit_code = -1;
    if (cmd) capture_ex(cmd, &out, &outlen, &exit_code);
    if (authed) auth_end();
    have_cmd = cmd ? 1 : 0;
    free(rootarg);
    free(userarg);
    free(cmd);
    if (!have_cmd || exit_code < 0) {
        free(out);
        MLE_SET_ERR(editor, "%s", "git: command failed to start");
        return -1;
    }
    if (exit_code == 0) {
        if (out && out[0]) info_line(editor, out);
        else MLE_SET_INFO(editor, "%s", "git: done");
    } else if (out && out[0]) {
        char msg[180];
        int i = 0;
        while (out[i] && out[i] != '\n' && i + 1 < 140) {
            msg[i] = out[i];
            i += 1;
        }
        msg[i] = '\0';
        MLE_SET_ERR(editor, "git: %s", msg[0] ? msg : "command failed");
    } else {
        MLE_SET_ERR(editor, "git: command failed (%d)", exit_code);
    }
    if (show_all && out && out[0]) {
        show_text(editor, "git", out);
        p->focused = 0;
    }
    free(out);
    p->git_ms = 0;
    refresh_git(editor, 1);
    return exit_code == 0 ? 0 : -1;
}

static void git_file_cmd(editor_t *editor, side_row_t *row, const char *verb) {
    char *arg;
    char *tail;
    if (!row || !row->path[0]) return;
    arg = shell_arg(row->path);
    if (!arg) return;
    tail = NULL;
    if (asprintf(&tail, "%s -- %s", verb, arg) < 0) tail = NULL;
    free(arg);
    if (!tail) return;
    git_run(editor, tail, 0, 0);
    free(tail);
}

static void git_file_show(editor_t *editor, side_row_t *row, const char *verb) {
    char *arg;
    char *tail;
    if (!row || !row->path[0]) return;
    arg = shell_arg(row->path);
    if (!arg) return;
    tail = NULL;
    if (asprintf(&tail, "%s -- %s", verb, arg) < 0) tail = NULL;
    free(arg);
    if (!tail) return;
    git_run(editor, tail, 1, 0);
    free(tail);
}

static void git_prompt_cmd(editor_t *editor, const char *prompt, const char *verb, int show_all, int use_auth) {
    char *answer;
    char *arg;
    char *tail;
    answer = NULL;
    editor_prompt(editor, (char *)prompt, NULL, &answer);
    if (!answer || !answer[0]) {
        free(answer);
        return;
    }
    arg = shell_arg(answer);
    free(answer);
    if (!arg) return;
    tail = NULL;
    if (asprintf(&tail, "%s %s", verb, arg) < 0) tail = NULL;
    free(arg);
    if (!tail) return;
    git_run(editor, tail, show_all, use_auth);
    free(tail);
}

static void git_search(editor_t *editor) {
    struct side_panel_s *p;
    char *query;
    p = editor->side;
    query = NULL;
    editor_prompt(editor, "git file search? empty clears", NULL, &query);
    if (!query) return;
    snprintf(p->git_query, sizeof(p->git_query), "%s", query);
    free(query);
    p->cursor = 0;
    p->scroll = 0;
    p->git_ms = 0;
    refresh_git(editor, 1);
    MLE_SET_INFO(editor, "git search: %s", p->git_query[0] ? p->git_query : "off");
}

static int confirm_yes(editor_t *editor, const char *prompt) {
    char *ans;
    int ok;
    ans = NULL;
    editor_prompt(editor, (char *)prompt, NULL, &ans);
    ok = ans && strcmp(ans, "yes") == 0;
    free(ans);
    return ok;
}



static int diff_live(editor_t *editor, bview_t *want) {
    bview_t *bv;
    if (!want || !editor) return 0;
    CDL_FOREACH2(editor->all_bviews, bv, all_next) {
        if (bv == want) return 1;
    }
    return 0;
}

static void present_diff(editor_t *editor, const char *text) {
    bview_t *bv;
    struct side_panel_s *p;
    if (!text) text = "";
    if (!diff_live(editor, g_diff)) {
        g_diff = NULL;
        editor_open_bview(editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, 0, NULL, &bv);
        if (!bv) return;
        g_diff = bv;
    } else {
        bv = g_diff;
        editor_set_active(editor, bv);
    }
    bview_set_syntax(bv, "syn_diff");
    buffer_set(bv->buffer, (char *)text, (bint_t)strlen(text));
    bv->buffer->is_unsaved = 0;
    mark_move_beginning(bv->active_cursor->mark);
    bview_zero_viewport_y(bv);
    p = editor->side;
    if (p) p->focused = 1;
}

static int capture_git_diff(struct side_panel_s *p, side_row_t *row, int cached, char **out) {
    char *root;
    char *file;
    char *cmd = NULL;
    size_t len = 0;
    *out = NULL;
    if (!p || !row || !row->path[0]) return -1;
    root = shell_arg(p->groot[0] ? p->groot : ".");
    file = shell_arg(row->path);
    if (root && file) {
        if (!cached && row->gitx == '?' && row->gity == '?') {
            if (asprintf(&cmd, "git -C %s diff --no-index -- /dev/null %s 2>&1", root, file) < 0) cmd = NULL;
        } else if (asprintf(&cmd, "git -C %s diff %s-- %s 2>&1", root, cached ? "--cached " : "", file) < 0) {
            cmd = NULL;
        }
    }
    if (cmd) capture(cmd, out, &len);
    free(cmd);
    free(root);
    free(file);
    return 0;
}

static void show_git_diff(editor_t *editor, side_row_t *row) {
    struct side_panel_s *p;
    char *raw_work = NULL;
    char *raw_staged = NULL;
    char *work = NULL;
    char *staged = NULL;
    char *text = NULL;
    int places = 0;
    int first = 0;
    int first_staged = 0;
    int nwork;
    int nstaged;
    size_t nw;
    size_t ns;
    const char *heading;
    if (!editor || !row || !row->path[0]) return;
    p = editor->side;
    if (!p) return;
    p->where[0] = '\0';
    heading = git_row_label(row);
    if (row->gitx == '?' && row->gity == '?') heading = "untracked";
    else if (row->gitx != '.' && row->gity == '.') heading = "staged";
    capture_git_diff(p, row, 0, &raw_work);
    if (!(row->gitx == '?' && row->gity == '?')) capture_git_diff(p, row, 1, &raw_staged);
    nwork = git_extra_format_compare(raw_work, heading, &work, &first);
    nstaged = git_extra_format_compare(raw_staged, "staged", &staged, &first_staged);
    if (!first) first = first_staged;
    if (nwork > 0) places += nwork;
    if (nstaged > 0) places += nstaged;
    nw = work ? strlen(work) : 0;
    ns = staged ? strlen(staged) : 0;
    if (nw || ns) {
        text = malloc(nw + ns + 1);
        if (text) {
            if (nw) memcpy(text, work, nw);
            if (ns) memcpy(text + nw, staged, ns);
            text[nw + ns] = '\0';
        }
    }
    if (first > 0) snprintf(p->where, sizeof(p->where), "line %d", first);
    present_diff(editor, text ? text : "no changes to show\n");
    if (places > 0 && first > 0) MLE_SET_INFO(editor, "diff: %d changed %s, line %d", places, places == 1 ? "line" : "lines", first);
    else if (places > 0) MLE_SET_INFO(editor, "diff: %d changed %s", places, places == 1 ? "line" : "lines");
    else MLE_SET_INFO(editor, "%s", "diff: no changes to show");
    free(raw_work);
    free(raw_staged);
    free(work);
    free(staged);
    free(text);
}

static void open_git_file(editor_t *editor, side_row_t *row) {
    struct side_panel_s *p;
    char *raw = NULL;
    char *formatted = NULL;
    int line = 0;
    if (!editor || !row || !row->path[0]) return;
    p = editor->side;
    if (p && row->gitx != 'D' && row->gity != 'D') {
        capture_git_diff(p, row, 0, &raw);
        git_extra_format_compare(raw, NULL, &formatted, &line);
        free(raw);
        free(formatted);
        raw = NULL;
        formatted = NULL;
        if (line < 1 && !(row->gitx == '?' && row->gity == '?')) {
            capture_git_diff(p, row, 1, &raw);
            git_extra_format_compare(raw, NULL, &formatted, &line);
            free(raw);
            free(formatted);
        }
    }
    open_file_line(editor, row->path, line > 0 ? (bint_t)line : 0);
}

static int commit_blank(const struct side_panel_s *p) {
    int i;
    if (!p || p->commit_len < 1) return 1;
    for (i = 0; i < p->commit_len; i++) {
        if (p->commit_msg[i] != ' ') return 0;
    }
    return 1;
}

static int row_fully_staged(const side_row_t *row) {
    if (!row) return 0;
    if (row->gitx == '?' || row->gity == '?') return 0;
    if (row->gitx == '.' || row->gitx == ' ' || row->gitx == '\0') return 0;
    return row->gity == '.' || row->gity == ' ' || row->gity == '\0';
}

static void git_toggle_stage(editor_t *editor, side_row_t *row) {
    if (!row || !row->path || !row->path[0]) return;
    if (row_fully_staged(row)) git_file_cmd(editor, row, "restore --staged");
    else git_file_cmd(editor, row, "add");
}

static void git_stage_default(editor_t *editor) {
    git_run(editor, "add -A", 0, 0);
}

static void git_commit_msg(editor_t *editor) {
    struct side_panel_s *p;
    char *arg;
    char *tail;
    char *rootarg;
    char *cmd;
    char *out;
    size_t outlen;
    int exit_code;
    const char *msg;
    p = editor->side;
    if (!p) return;
    if (p->commit_len < 0) p->commit_len = 0;
    if (p->commit_len >= (int)sizeof(p->commit_msg)) p->commit_len = (int)sizeof(p->commit_msg) - 1;
    p->commit_msg[p->commit_len] = '\0';
    if (!p->groot[0]) find_git_root_from(p, p->cwd);
    if (!p->groot[0]) {
        MLE_SET_ERR(editor, "%s", "git: no repository here");
        return;
    }
    if (p->staged < 1) git_run(editor, "add -A", 0, 0);
    msg = commit_blank(p) ? "update" : p->commit_msg;
    arg = shell_arg(msg);
    if (!arg) return;
    tail = NULL;
    if (asprintf(&tail, "commit -m %s", arg) < 0) tail = NULL;
    free(arg);
    if (!tail) return;
    rootarg = shell_arg(p->groot);
    cmd = NULL;
    out = NULL;
    outlen = 0;
    exit_code = -1;
    if (rootarg && asprintf(&cmd, "git -C %s %s 2>&1", rootarg, tail) < 0) cmd = NULL;
    if (cmd) capture_ex(cmd, &out, &outlen, &exit_code);
    free(rootarg);
    free(tail);
    free(cmd);
    if (exit_code == 0) {
        if (out && out[0]) info_line(editor, out);
        else MLE_SET_INFO(editor, "%s", "git: committed");
        p->commit_msg[0] = '\0';
        p->commit_len = 0;
        p->commit_caret = 0;
        p->field = FIELD_NONE;
        p->where[0] = '\0';
    } else {
        p->field = FIELD_COMMIT;
        if (out && out[0]) {
            char err[160];
            int i = 0;
            while (out[i] && out[i] != '\n' && i + 1 < (int)sizeof(err)) {
                err[i] = out[i];
                i += 1;
            }
            err[i] = '\0';
            MLE_SET_ERR(editor, "git: %s", err[0] ? err : "commit failed");
        } else MLE_SET_ERR(editor, "%s", "git: commit failed");
    }
    free(out);
    p->git_ms = 0;
    refresh_git(editor, 1);
}

static void git_push(editor_t *editor) {
    struct side_panel_s *p;
    p = editor->side;
    if (p && p->staged < 1 && (p->unstaged > 0 || p->untracked > 0)) git_stage_default(editor);
    git_run(editor, "push -u origin HEAD", 0, 1);
}

static void git_pull(editor_t *editor) {
    git_run(editor, "pull --no-edit", 0, 1);
}

static void activate_row(editor_t *editor, int dbl) {
    struct side_panel_s *p;
    side_row_t *row;
    p = editor->side;
    if (!p || p->cursor < 0 || p->cursor >= p->nrows) return;
    row = &p->rows[p->cursor];
    if (p->mode == SIDE_GIT) {
        if (dbl && row->path[0]) open_git_file(editor, row);
        else if (row->path[0]) git_toggle_stage(editor, row);
        return;
    }
    if (strcmp(row->name, "..") == 0) {
        go_parent(p);
        return;
    }
    if (row->is_dir) {
        if (dbl) enter_dir(p, row->path);
        else {
            int on = !is_open(p, row->path);
            set_open(p, row->path, on);
            rescan_files(p);
        }
        return;
    }
    if (dbl) open_file(editor, row->path);
}

static void ensure_cursor(struct side_panel_s *p) {
    int body;
    if (!p) return;
    if (row_span(p) < 1) {
        p->cursor = 0;
        p->scroll = 0;
        return;
    }
    if (p->cursor < 0) p->cursor = 0;
    if (p->cursor >= row_span(p)) p->cursor = row_span(p) - 1;
    body = p->h - (list_origin(p));
    if (body < 1) body = 1;
    if (p->cursor < p->scroll) p->scroll = p->cursor;
    if (p->cursor >= p->scroll + body) p->scroll = p->cursor - body + 1;
    if (p->scroll < 0) p->scroll = 0;
}

static void move_cursor(struct side_panel_s *p, int d) {
    if (row_span(p) < 1) return;
    p->cursor += d;
    if (p->cursor < 0) p->cursor = 0;
    if (p->cursor >= row_span(p)) p->cursor = row_span(p) - 1;
    ensure_cursor(p);
}

static void goto_typed_dir(editor_t *editor, struct side_panel_s *p) {
    char *ans;
    char path[PATH_MAX];
    struct stat st;
    const char *src;
    const char *home;
    size_t i, j;
    ans = NULL;
    editor_prompt(editor, "folder?", NULL, &ans);
    if (!ans || !ans[0]) {
        free(ans);
        return;
    }
    i = 0;
    path[0] = '\0';
    if (ans[0] == '~' && (ans[1] == '/' || ans[1] == '\0')) {
        home = getenv("HOME");
        src = ans + 1;
        if (home) {
            for (j = 0; home[j] && i + 1 < sizeof(path); j++) path[i++] = home[j];
        }
    } else if (ans[0] != '/') {
        for (j = 0; p->cwd[j] && i + 1 < sizeof(path); j++) path[i++] = p->cwd[j];
        if (i > 0 && path[i - 1] != '/' && i + 1 < sizeof(path)) path[i++] = '/';
        src = ans;
    } else {
        src = ans;
    }
    for (j = 0; src[j] && i + 1 < sizeof(path); j++) path[i++] = src[j];
    path[i] = '\0';
    free(ans);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) enter_dir(p, path);
    else MLE_SET_ERR(editor, "%s", "not a folder");
}

static void relayout(editor_t *editor) {
    editor_relayout(editor);
}

void side_init(editor_t *editor) {
    struct side_panel_s *p;
    char wd[PATH_MAX];
    p = calloc(1, sizeof(*p));
    if (!p) return;
    p->w = 30;
    p->visible = editor->headless_mode ? 0 : 1;
    p->mode = SIDE_FILES;
    if (!getcwd(wd, sizeof(wd))) snprintf(wd, sizeof(wd), "%s", ".");
    snprintf(p->cwd, sizeof(p->cwd), "%s", wd);
    if (editor->active_edit && editor->active_edit->buffer && editor->active_edit->buffer->path) {
        char *path = editor->active_edit->buffer->path;
        char *sl = strrchr(path, '/');
        if (sl && sl != path) {
            size_t n = (size_t)(sl - path);
            if (n >= sizeof(p->cwd)) n = sizeof(p->cwd) - 1;
            memcpy(p->cwd, path, n);
            p->cwd[n] = '\0';
        }
    }
    editor->side = p;
    if (p->visible) rescan_files(p);
}

void side_shutdown(editor_t *editor) {
    struct side_panel_s *p;
    int i;
    if (!editor || !editor->side) return;
    p = editor->side;
    free_rows(p);
    free(p->rows);
    for (i = 0; i < p->nopen; i++) free(p->open_dirs[i]);
    free(p->open_dirs);
    auth_end();
    wipe_mem(p, sizeof(*p));
    free(p);
    editor->side = NULL;
}

void side_apply_layout(editor_t *editor, bview_rect_t *edit) {
    struct side_panel_s *p;
    int sw;
    if (!editor || !editor->side || !edit) return;
    p = editor->side;
    p->h = 0;
    if (editor->headless_mode || !p->visible) return;
    if (edit->w < 36 || edit->h < 4) return;
    sw = p->w;
    if (sw < 18) sw = 18;
    if (sw > edit->w - 16) sw = edit->w - 16;
    if (sw < 18) return;
    p->w = sw;
    p->x = edit->x;
    p->y = edit->y;
    p->h = edit->h;
    edit->x += sw;
    edit->w -= sw;
}

static int utf8_next(const char **sp, uint32_t *cp) {
    const unsigned char *s;
    s = (const unsigned char *)*sp;
    if (!s || !*s) return 0;
    if (*s < 0x80) {
        *cp = *s;
        *sp += 1;
        return 1;
    }
    if ((*s & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(*s & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *sp += 2;
        return 1;
    }
    if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((uint32_t)(*s & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *sp += 3;
        return 1;
    }
    if ((*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *cp = ((uint32_t)(*s & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *sp += 4;
        return 1;
    }
    *cp = *s;
    *sp += 1;
    return 1;
}

static int app_bytes(char *line, int n, int cap, const char *s) {
    while (s && *s && n + 1 < cap) line[n++] = *s++;
    if (n < cap) line[n] = '\0';
    else if (cap > 0) line[cap - 1] = '\0';
    return n;
}

static int app_cp(char *line, int n, int cap, uint32_t cp) {
    char tmp[8];
    int nb;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        nb = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        nb = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        nb = 3;
    } else {
        tmp[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        nb = 4;
    }
    tmp[nb] = '\0';
    return app_bytes(line, n, cap, tmp);
}

static void paint(int x, int y, int w, uint16_t fg, uint16_t bg, const char *s) {
    int n;
    const char *p;
    if (w < 1) return;
    if (w > 240) w = 240;
    n = 0;
    p = s;
    while (p && *p && n < w) {
        uint32_t cp = ' ';
        if (!utf8_next(&p, &cp)) break;
        tb_set_cell(x + n, y, cp, fg, bg);
        n += 1;
    }
    for (; n < w; n++) tb_set_cell(x + n, y, ' ', fg, bg);
}

typedef struct git_btn_s {
    int x;
    int cols;
    char text[32];
} git_btn_t;

static void lay_buttons(struct side_panel_s *p, git_btn_t b[4]) {
    const char *labs[4];
    int icons[4];
    int cols[4];
    int i, x, icon, content, need;
    content = p->w - 1;
    if (content < 1) content = 1;
    if (content >= 30) {
        labs[0] = "[Commit]";
        labs[1] = "[Push]";
        labs[2] = "[Pull]";
        labs[3] = "[Fetch]";
    } else if (content >= 22) {
        labs[0] = "Commit";
        labs[1] = "Push";
        labs[2] = "Pull";
        labs[3] = "Fetch";
    } else {
        labs[0] = "Cmt";
        labs[1] = "Push";
        labs[2] = "Pull";
        labs[3] = "Get";
    }
    icons[0] = GLY_STAGED;
    icons[1] = GLY_UP;
    icons[2] = GLY_DOWN;
    icons[3] = GLY_FETCH;
    need = (int)strlen(labs[0]) + (int)strlen(labs[1]) + (int)strlen(labs[2]) + (int)strlen(labs[3]) + 3;
    icon = glyphs_decor() && content >= need + 8;
    for (i = 0; i < 4; i++) cols[i] = (int)strlen(labs[i]) + (icon ? 2 : 0);
    need = cols[0] + cols[1] + cols[2] + cols[3] + 3;
    x = p->x;
    if (need + 1 <= content) x = p->x + 1;
    for (i = 0; i < 4; i++) {
        int room;
        b[i].text[0] = '\0';
        if (icon) {
            char m[8];
            glyphs_utf8(icons[i], m, (int)sizeof(m));
            snprintf(b[i].text, sizeof(b[i].text), "%s %s", m, labs[i]);
        } else {
            snprintf(b[i].text, sizeof(b[i].text), "%s", labs[i]);
        }
        room = p->x + content - x;
        if (room < 1) room = 1;
        if (cols[i] > room) cols[i] = room;
        b[i].x = x;
        b[i].cols = cols[i];
        x += cols[i] + 1;
    }
}

static int git_mark(const side_row_t *row) {
    char x, y;
    x = row->gitx;
    y = row->gity;
    if (row->conflict) return GLY_CONFLICT;
    if (x == '?' || y == '?') return GLY_UNTRACK;
    if (x == 'A' || y == 'A') return GLY_ADD;
    if (x == 'D' || y == 'D') return GLY_DEL;
    if (x == 'R' || y == 'R' || x == 'C' || y == 'C') return GLY_RENAME;
    if (x != ' ' && x != '?' && x != 0) return GLY_STAGED;
    return GLY_MOD;
}

static const char *git_row_label(const side_row_t *row) {
    char x;
    char y;
    if (!row) return "changed";
    x = row->gitx;
    y = row->gity;
    if (row->conflict) return "conflict";
    if (x == '?' || y == '?') return "untracked";
    if (x == 'R' || y == 'R' || x == 'C' || y == 'C') return "renamed";
    if (x == 'D' || y == 'D') return "deleted";
    if (x == 'A' && y != '.') return "added+modified";
    if (x == 'A') return "added";
    if (x != '.' && y != '.') return "staged+modified";
    if (x != '.') return "staged";
    return "modified";
}

static int utf8_encode(uint32_t cp, char *out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int field_cols(const char *buf, int end) {
    const char *s;
    const char *lim;
    int cols;
    if (!buf || end <= 0) return 0;
    s = buf;
    lim = buf + end;
    cols = 0;
    while (s < lim) {
        uint32_t cp;
        if (!utf8_next(&s, &cp)) break;
        cols += 1;
        if (s > lim) break;
    }
    return cols;
}

static int field_byte_at_col(const char *buf, int len, int col) {
    const char *s;
    const char *lim;
    int cols;
    if (!buf || col <= 0 || len <= 0) return 0;
    s = buf;
    lim = buf + len;
    cols = 0;
    while (s < lim && cols < col) {
        uint32_t cp;
        if (!utf8_next(&s, &cp)) break;
        if (s > lim) {
            s = lim;
            break;
        }
        cols += 1;
    }
    return (int)(s - buf);
}

static int field_prev(const char *buf, int caret) {
    int i;
    if (caret <= 0) return 0;
    i = caret - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i -= 1;
    return i;
}

static int field_next(const char *buf, int len, int caret) {
    const char *s;
    uint32_t cp;
    if (caret < 0) caret = 0;
    if (caret >= len) return len;
    s = buf + caret;
    if (!utf8_next(&s, &cp)) return caret + 1 > len ? len : caret + 1;
    if (s > buf + len) return len;
    return (int)(s - buf);
}

static void field_insert(char *buf, int cap, int *len, int *caret, uint32_t cp) {
    char tmp[4];
    int nb, n, c;
    if (cp < 32 || cp == 127) return;
    nb = utf8_encode(cp, tmp);
    if (nb < 1) return;
    n = *len;
    c = *caret;
    if (n < 0) n = 0;
    if (c < 0) c = 0;
    if (c > n) c = n;
    if (n > cap - 1) n = cap - 1;
    if (n + nb > cap - 1) return;
    memmove(buf + c + nb, buf + c, (size_t)(n - c));
    memcpy(buf + c, tmp, (size_t)nb);
    n += nb;
    buf[n] = '\0';
    *len = n;
    *caret = c + nb;
}

static void field_backspace(char *buf, int *len, int *caret) {
    int prev, n, c;
    n = *len;
    c = *caret;
    if (c <= 0 || n <= 0) return;
    if (c > n) c = n;
    prev = field_prev(buf, c);
    memmove(buf + prev, buf + c, (size_t)(n - c));
    n -= c - prev;
    buf[n] = '\0';
    *len = n;
    *caret = prev;
}

static void field_delete(char *buf, int *len, int *caret) {
    int nextb, n, c;
    n = *len;
    c = *caret;
    if (c < 0) c = 0;
    if (c >= n) return;
    nextb = field_next(buf, n, c);
    memmove(buf + c, buf + nextb, (size_t)(n - nextb));
    n -= nextb - c;
    buf[n] = '\0';
    *len = n;
    *caret = c;
}

static void field_paste(char *buf, int cap, int *len, int *caret) {
    char *clip;
    size_t n;
    const char *s;
    clip = NULL;
    n = 0;
    if (util_clipboard_get(&clip, &n) != MLE_OK || !clip) {
        free(clip);
        return;
    }
    s = clip;
    while (*s && *s != '\n' && *s != '\r') {
        uint32_t cp = 0;
        if (!utf8_next(&s, &cp)) break;
        if (cp < 32 || cp == 127) continue;
        field_insert(buf, cap, len, caret, cp);
    }
    free(clip);
}

static void field_on_key(char *buf, int cap, int *len, int *caret, struct tb_event *ev) {
    uint32_t ch;
    if (*len < 0) *len = 0;
    if (*len >= cap) *len = cap - 1;
    buf[*len] = '\0';
    if (*caret < 0) *caret = 0;
    if (*caret > *len) *caret = *len;
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        field_backspace(buf, len, caret);
        return;
    }
    if (ev->key == TB_KEY_DELETE) {
        field_delete(buf, len, caret);
        return;
    }
    if (ev->key == TB_KEY_ARROW_LEFT) {
        *caret = field_prev(buf, *caret);
        return;
    }
    if (ev->key == TB_KEY_ARROW_RIGHT) {
        *caret = field_next(buf, *len, *caret);
        return;
    }
    if (ev->key == TB_KEY_HOME) {
        *caret = 0;
        return;
    }
    if (ev->key == TB_KEY_END) {
        *caret = *len;
        return;
    }
    if (ev->key == TB_KEY_CTRL_V || (ev->key == TB_KEY_INSERT && (ev->mod & TB_MOD_SHIFT))) {
        field_paste(buf, cap, len, caret);
        return;
    }
    if (ev->mod & (TB_MOD_CTRL | TB_MOD_ALT)) return;
    ch = 0;
    if (ev->key == TB_KEY_SPACE) ch = ' ';
    else if (ev->key == 0 && ev->ch >= 32 && ev->ch != 127) ch = ev->ch;
    if (ch) field_insert(buf, cap, len, caret, ch);
}

static void field_click(const char *buf, int len, int *caret, int x, int px, int w) {
    int room, rel, view, cols;
    room = (w - 1) - 3;
    if (room < 1) room = 1;
    if (*caret < 0) *caret = 0;
    if (*caret > len) *caret = len;
    cols = field_cols(buf, *caret);
    view = 0;
    if (cols >= room) view = cols - room + 1;
    rel = x - (px + 3);
    if (rel < 0) rel = 0;
    if (rel > room) rel = room;
    *caret = field_byte_at_col(buf, len, view + rel);
}

static void field_draw(struct side_panel_s *p, int y, const char *buf, int len, int caret, int secret, int active, const char *placeholder, uint16_t fg, uint16_t bg) {
    char line[256];
    int n, w, room, view, col, shown;
    uint32_t arrow;
    const char *s;
    w = p->w - 1;
    if (w < 1) return;
    if (len < 0) len = 0;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;
    arrow = glyphs_decor() ? glyphs_cp(GLY_RIGHT) : (uint32_t)'>';
    n = 0;
    line[0] = '\0';
    if (n + 1 < (int)sizeof(line)) line[n++] = ' ';
    n = app_cp(line, n, (int)sizeof(line), arrow);
    n = app_bytes(line, n, (int)sizeof(line), " ");
    room = w - 3;
    if (room < 1) room = 1;
    if (!len) {
        n = app_bytes(line, n, (int)sizeof(line), placeholder ? placeholder : "");
        paint(p->x, y, w, fg, bg, line);
        if (active) {
            p->caretx = p->x + 3;
            p->carety = y;
            p->careton = 1;
        }
        return;
    }
    view = 0;
    col = field_cols(buf, caret);
    if (col >= room) view = col - room + 1;
    s = buf;
    shown = 0;
    while (s < buf + len && shown < room && n + 1 < (int)sizeof(line)) {
        uint32_t cp = 0;
        const char *prev = s;
        if (!utf8_next(&s, &cp)) break;
        if (s > buf + len) break;
        if ((int)field_cols(buf, (int)(prev - buf)) < view) continue;
        if (secret) n = app_cp(line, n, (int)sizeof(line), '*');
        else n = app_cp(line, n, (int)sizeof(line), cp);
        shown += 1;
    }
    paint(p->x, y, w, fg, bg, line);
    if (active) {
        int cx = p->x + 3 + (col - view);
        if (cx >= p->x && cx < p->x + w) {
            p->caretx = cx;
            p->carety = y;
            p->careton = 1;
        }
    }
}

static void draw_git_chrome(editor_t *editor, struct side_panel_s *p, int classic, uint16_t on_fg, uint16_t on_bg, uint16_t path_fg, uint16_t path_bg, uint16_t body_fg, uint16_t body_bg) {
    mle_theme_t *th;
    git_btn_t b[4];
    int w;
    uint16_t fg, bg;
    uint16_t c_fg, c_bg, p_fg, p_bg, l_fg, l_bg;
    th = theme_current(editor);
    w = p->w - 1;
    if (w < 1) return;
    if (p->field == FIELD_USER) {
        field_draw(p, p->y + 2, p->user, p->user_len, p->user_caret, 0, 1, "username", on_fg, on_bg);
    } else if (p->field == FIELD_PASS) {
        field_draw(p, p->y + 2, p->pass, p->pass_len, p->pass_caret, 1, 1, "password", on_fg, on_bg);
    } else if (p->nacct < 1 || p->acct_i < 0 || p->acct_i >= p->nacct) {
        paint(p->x, p->y + 2, w, classic ? TB_YELLOW : th->string, body_bg, " login");
    } else {
        char chip[180];
        gh_acct_t *a = &p->accts[p->acct_i];
        if (p->nacct > 1) {
            snprintf(chip, sizeof(chip), " %s  %s  %d/%d", a->login, a->source[0] ? a->source : "github", p->acct_i + 1, p->nacct);
        } else {
            snprintf(chip, sizeof(chip), " %s  %s", a->login, a->source[0] ? a->source : "github");
        }
        paint(p->x, p->y + 2, w, path_fg, path_bg, chip);
    }
    if (p->field == FIELD_COMMIT) {
        fg = on_fg;
        bg = on_bg;
    } else if (p->commit_len > 0) {
        fg = path_fg;
        bg = path_bg;
    } else {
        fg = classic ? TB_YELLOW : th->string;
        bg = body_bg;
    }
    field_draw(p, p->y + 3, p->commit_msg, p->commit_len, p->commit_caret, 0, p->field == FIELD_COMMIT, "commit message", fg, bg);
    c_fg = classic ? (TB_BLACK | TB_BOLD) : th->git_fg;
    c_bg = classic ? TB_GREEN : th->git_bg;
    p_fg = classic ? (TB_BLACK | TB_BOLD) : th->accent_fg;
    p_bg = classic ? TB_CYAN : th->accent_bg;
    l_fg = classic ? (TB_BLACK | TB_BOLD) : th->pos_fg;
    l_bg = classic ? TB_YELLOW : th->pos_bg;
    paint(p->x, p->y + 4, w, body_fg, body_bg, "");
    lay_buttons(p, b);
    paint(b[0].x, p->y + 4, b[0].cols, c_fg, c_bg, b[0].text);
    paint(b[1].x, p->y + 4, b[1].cols, p_fg, p_bg, b[1].text);
    paint(b[2].x, p->y + 4, b[2].cols, l_fg, l_bg, b[2].text);
    paint(b[3].x, p->y + 4, b[3].cols, p_fg, p_bg, b[3].text);
}

void side_draw(editor_t *editor) {
    struct side_panel_s *p;
    int row, y, body, i;
    char head[160];
    const char *base;
    if (!editor || !editor->side || editor->headless_mode) return;
    p = editor->side;
    if (!p->visible || p->h < 2 || p->w < 4) return;
    p->careton = 0;
    ensure_cursor(p);
    p->tab_files = 0;
    p->tab_git = 7;
    p->tab_help = 17;
    mle_theme_t *th = theme_current(editor);
    int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
    uint16_t tab_fg = classic ? TB_WHITE : th->muted_fg;
    uint16_t tab_bg = classic ? TB_BLACK : th->muted_bg;
    uint16_t on_fg = classic ? (TB_BLACK | TB_BOLD) : th->accent_fg;
    uint16_t on_bg = classic ? TB_CYAN : th->accent_bg;
    uint16_t path_fg = classic ? (TB_BLACK | TB_BOLD) : th->bar_fg;
    uint16_t path_bg = classic ? TB_WHITE : th->bar_bg;
    uint16_t body_fg = classic ? TB_WHITE : (th->text_fg ? th->text_fg : th->muted_fg);
    uint16_t body_bg = classic ? TB_BLACK : (th->text_bg ? th->text_bg : th->muted_bg);
    uint16_t dir_fg = classic ? (TB_CYAN | TB_BOLD) : th->dir_fg;
    uint16_t dir_bg = classic ? TB_BLACK : th->dir_bg;
    uint16_t grip_fg = classic ? TB_WHITE : th->grip_fg;
    uint16_t grip_bg = classic ? (TB_BLACK | TB_BRIGHT) : th->grip_bg;
    paint(p->x, p->y, p->w, tab_fg, tab_bg, "");
    paint(p->x + p->tab_files, p->y, 6, p->mode == SIDE_FILES ? on_fg : tab_fg, p->mode == SIDE_FILES ? on_bg : tab_bg, " Files");
    paint(p->x + p->tab_git, p->y, 4, p->mode == SIDE_GIT ? on_fg : tab_fg, p->mode == SIDE_GIT ? on_bg : tab_bg, " Git");
    paint(p->x + 12, p->y, 4, p->mode == SIDE_SET ? on_fg : tab_fg, p->mode == SIDE_SET ? on_bg : tab_bg, " Set");
    paint(p->x + p->tab_help, p->y, 2, p->mode == SIDE_GUIDE ? on_fg : tab_fg, p->mode == SIDE_GUIDE ? on_bg : tab_bg, " ?");
    base = strrchr(p->cwd, '/');
    base = (base && base[1]) ? base + 1 : p->cwd;
    if (p->mode == SIDE_GIT) {
        if (p->where[0]) {
            snprintf(head, sizeof(head), " %.32s  %.48s", p->branch[0] ? p->branch : "(no git)", p->where);
        } else if (p->upstream[0]) {
            snprintf(head, sizeof(head), " %.24s -> %.24s  +%d -%d  s:%d  [%s]", p->branch[0] ? p->branch : "(no git)", p->upstream, p->ahead, p->behind, p->stashes, git_filter_name(p->filter));
        } else {
            snprintf(head, sizeof(head), " %.40s  +%d -%d  stash:%d  [%s]", p->branch[0] ? p->branch : "(no git)", p->ahead, p->behind, p->stashes, git_filter_name(p->filter));
        }
        if (p->git_query[0]) {
            size_t used;
            used = strlen(head);
            snprintf(head + used, sizeof(head) - used, " /%.48s", p->git_query);
        }
    } else if (p->mode == SIDE_GUIDE) {
        snprintf(head, sizeof(head), " %s", "guide  f1 full");
    } else if (p->mode == SIDE_SET) {
        snprintf(head, sizeof(head), " %s", "settings   enter applies");
    } else {
        snprintf(head, sizeof(head), " %.156s/", base);
    }
    paint(p->x, p->y + 1, p->w - 1, path_fg, path_bg, head);
    if (p->mode == SIDE_GIT) draw_git_chrome(editor, p, classic, on_fg, on_bg, path_fg, path_bg, body_fg, body_bg);
    body = p->h - (list_origin(p));
    if (body < 0) body = 0;
    if (p->mode == SIDE_SET) {
        draw_settings(editor, p, body, body_fg, body_bg, on_fg, on_bg, path_fg, path_bg);
    } else if (p->mode == SIDE_GUIDE) {
        const char *s = guide_text;
        y = 0;
        while (y < body && *s) {
            char line[256];
            int n = 0;
            while (*s && *s != '\n' && n + 1 < (int)sizeof(line)) line[n++] = *s++;
            line[n] = '\0';
            if (*s == '\n') s++;
            paint(p->x, p->y + (list_origin(p)) + y, p->w - 1, body_fg, body_bg, line);
            y += 1;
        }
        for (; y < body; y++) paint(p->x, p->y + (list_origin(p)) + y, p->w - 1, body_fg, body_bg, "");
    } else if (p->nrows < 1) {
        paint(p->x, p->y + (list_origin(p)), p->w - 1, classic ? TB_YELLOW : th->string, body_bg, p->mode == SIDE_GIT ? " clean" : " empty");
        for (y = 1; y < body; y++) paint(p->x, p->y + (list_origin(p)) + y, p->w - 1, body_fg, body_bg, "");
    } else {
        for (i = 0; i < body; i++) {
            char line[256];
            int fg, bg, indent, n, k;
            row = p->scroll + i;
            y = p->y + (list_origin(p)) + i;
            if (row < 0 || row >= p->nrows) {
                paint(p->x, y, p->w - 1, body_fg, body_bg, "");
                continue;
            }
            indent = p->rows[row].depth * 2;
            if (indent > 12) indent = 12;
            n = 0;
            line[n++] = ' ';
            for (k = 0; k < indent && n + 1 < (int)sizeof(line); k++) line[n++] = ' ';
            if (p->mode == SIDE_GIT && n + 8 < (int)sizeof(line)) {
                const char *label;
                if (glyphs_decor()) {
                    n = app_cp(line, n, (int)sizeof(line), glyphs_cp(git_mark(&p->rows[row])));
                    if (n + 1 < (int)sizeof(line)) line[n++] = ' ';
                }
                label = git_row_label(&p->rows[row]);
                if (n + (int)strlen(label) + 3 < (int)sizeof(line)) {
                    line[n++] = '[';
                    n = app_bytes(line, n, (int)sizeof(line), label);
                    line[n++] = ']';
                    line[n++] = ' ';
                }
            } else if (p->rows[row].is_dir && n + 6 < (int)sizeof(line)) {
                uint32_t mark;
                if (glyphs_decor()) mark = glyphs_cp(p->rows[row].expanded ? GLY_DIR_OPEN : GLY_DIR_SHUT);
                else mark = (uint32_t)(p->rows[row].expanded ? '-' : '+');
                n = app_cp(line, n, (int)sizeof(line), mark);
                if (n + 1 < (int)sizeof(line)) line[n++] = ' ';
            }
            for (k = 0; p->rows[row].name[k] && n + 1 < (int)sizeof(line); k++) line[n++] = p->rows[row].name[k];
            if (p->rows[row].is_dir && strcmp(p->rows[row].name, "..") != 0 && n + 1 < (int)sizeof(line)) line[n++] = '/';
            if (p->mode == SIDE_GIT && n + 16 < (int)sizeof(line)) {
                char stat[32];
                int sk;
                stat[0] = '\0';
                if (p->rows[row].added < 0 || p->rows[row].removed < 0) snprintf(stat, sizeof(stat), "  binary");
                else if (p->rows[row].added || p->rows[row].removed) snprintf(stat, sizeof(stat), "  +%d -%d", p->rows[row].added, p->rows[row].removed);
                for (sk = 0; stat[sk] && n + 1 < (int)sizeof(line); sk++) line[n++] = stat[sk];
            }
            line[n] = '\0';
            if (row == p->cursor && p->focused) {
                fg = on_fg;
                bg = on_bg;
            } else if (row == p->cursor) {
                fg = path_fg;
                bg = path_bg;
            } else if (p->rows[row].is_dir) {
                fg = dir_fg;
                bg = dir_bg;
            } else if (p->mode == SIDE_GIT && p->rows[row].conflict) {
                fg = classic ? (TB_RED | TB_BOLD) : th->punct;
                bg = body_bg;
            } else if (p->mode == SIDE_GIT && (p->rows[row].gitx == '?' || p->rows[row].gity == '?')) {
                fg = classic ? TB_YELLOW : th->string;
                bg = body_bg;
            } else if (p->mode == SIDE_GIT && p->rows[row].gitx != '.') {
                fg = classic ? TB_GREEN : th->git_fg;
                bg = body_bg;
            } else {
                fg = body_fg;
                bg = body_bg;
            }
            paint(p->x, y, p->w - 1, fg, bg, line);
        }
    }
    if (p->h > 0 && p->w > 0) {
        int dy;
        for (dy = 0; dy < p->h; dy++) tb_print(p->x + p->w - 1, p->y + dy, grip_fg, grip_bg, " ");
    }
}

void side_git_label(editor_t *editor, char *buf, size_t n) {
    struct side_panel_s *p;
    if (!buf || n == 0) return;
    buf[0] = '\0';
    if (!editor || !editor->side) return;
    p = editor->side;
    refresh_git(editor, 0);
    if (!p->git_ok || !p->branch[0]) return;
    if (glyphs_decor()) {
        char mark[8];
        glyphs_utf8(GLY_BRANCH, mark, (int)sizeof(mark));
        snprintf(buf, n, "%s %s +%d ~%d ?%d !%d", mark, p->branch, p->staged, p->unstaged, p->untracked, p->conflicts);
    } else {
        snprintf(buf, n, "%s +%d ~%d ?%d !%d", p->branch, p->staged, p->unstaged, p->untracked, p->conflicts);
    }
}

int side_action(editor_t *editor, const char *action) {
    struct side_panel_s *p;
    if (!editor) return MLE_ERR;
    if (!editor->side) side_init(editor);
    p = editor->side;
    if (!p) return MLE_ERR;
    if (!action || !action[0] || strcmp(action, "toggle") == 0) {
        p->visible = p->visible ? 0 : 1;
        if (p->visible) {
            p->focused = 1;
            p->mode = SIDE_FILES;
            if (editor->iterm) iterm_action(editor, "unfocus");
            rescan_files(p);
        } else {
            p->focused = 0;
        }
    } else if (strcmp(action, "blur") == 0) {
        p->focused = 0;
    } else if (strcmp(action, "hide") == 0) {
        p->visible = 0;
        p->focused = 0;
    } else if (strcmp(action, "show") == 0 || strcmp(action, "files") == 0) {
        p->visible = 1;
        p->focused = 1;
        p->mode = SIDE_FILES;
        if (editor->iterm) iterm_action(editor, "unfocus");
        rescan_files(p);
    } else if (strcmp(action, "git") == 0) {
        if (p->visible && p->mode == SIDE_GIT) {
            p->visible = 0;
            p->focused = 0;
        } else {
            p->visible = 1;
            p->focused = 1;
            p->mode = SIDE_GIT;
            if (editor->iterm) iterm_action(editor, "unfocus");
            p->git_ms = 0;
            refresh_git(editor, 1);
            git_stage_default(editor);
            git_enter(editor);
        }
    } else if (strcmp(action, "settings") == 0) {
        p->visible = 1;
        p->focused = 1;
        p->mode = SIDE_SET;
        p->cursor = 0;
        p->scroll = 0;
        if (editor->iterm) iterm_action(editor, "unfocus");
    } else if (strcmp(action, "guide") == 0) {
        p->visible = 1;
        p->focused = 1;
        p->mode = SIDE_GUIDE;
    } else if (strcmp(action, "refresh") == 0) {
        if (p->mode == SIDE_GIT) {
            p->git_ms = 0;
            refresh_git(editor, 1);
        } else {
            rescan_files(p);
        }
    }
    relayout(editor);
    return MLE_OK;
}

int side_handle_key(editor_t *editor, struct tb_event *ev) {
    struct side_panel_s *p;
    side_row_t *row;
    if (!editor || !editor->side || !ev) return 0;
    p = editor->side;
    if (!p->visible || !p->focused || p->h < 2) return 0;
    if ((ev->mod & TB_MOD_CTRL) && (ev->ch == '~' || ev->ch == '`')) return 0;
    if ((ev->mod & TB_MOD_CTRL) && (ev->mod & TB_MOD_SHIFT)
        && (ev->key == TB_KEY_CTRL_TILDE || ev->ch == '`' || ev->ch == '~')) return 0;
    if (ev->key == TB_KEY_F1 || ev->key == TB_KEY_F2 || ev->key == TB_KEY_F7 || ev->key == TB_KEY_F8) return 0;
    if (ev->key == TB_KEY_F9) {
        side_action(editor, "toggle");
        return 1;
    }
    if (ev->key == TB_KEY_F10) {
        side_action(editor, "git");
        return 1;
    }
    if (ev->key == TB_KEY_ESC) {
        if (p->mode == SIDE_GIT && p->field != FIELD_NONE) {
            if (p->field == FIELD_USER || p->field == FIELD_PASS) pass_clear(p);
            p->field = FIELD_NONE;
        } else {
            p->focused = 0;
        }
        return 1;
    }
    if (p->mode == SIDE_GIT && p->field != FIELD_NONE) {
        if (ev->key == TB_KEY_ENTER) {
            if (p->field == FIELD_USER) {
                int i, blank;
                if (p->user_len < 0) p->user_len = 0;
                if (p->user_len >= (int)sizeof(p->user)) p->user_len = (int)sizeof(p->user) - 1;
                p->user[p->user_len] = '\0';
                blank = 1;
                for (i = 0; i < p->user_len; i++) {
                    if (p->user[i] != ' ') blank = 0;
                }
                if (blank) {
                    MLE_SET_ERR(editor, "%s", "git: type a username");
                    return 1;
                }
                p->field = FIELD_PASS;
                return 1;
            }
            if (p->field == FIELD_PASS) {
                acct_finish_login(editor);
                return 1;
            }
            git_commit_msg(editor);
            return 1;
        }
        if (ev->key == TB_KEY_ARROW_UP) {
            if (p->field == FIELD_PASS) {
                p->field = FIELD_USER;
                return 1;
            }
            if (p->field == FIELD_USER) pass_clear(p);
            p->field = FIELD_NONE;
            move_cursor(p, -1);
            return 1;
        }
        if (ev->key == TB_KEY_ARROW_DOWN) {
            if (p->field == FIELD_USER) {
                p->field = FIELD_PASS;
                return 1;
            }
            if (p->field == FIELD_PASS) pass_clear(p);
            p->field = FIELD_NONE;
            move_cursor(p, 1);
            return 1;
        }
        if (p->field == FIELD_USER) field_on_key(p->user, (int)sizeof(p->user), &p->user_len, &p->user_caret, ev);
        else if (p->field == FIELD_PASS) field_on_key(p->pass, (int)sizeof(p->pass), &p->pass_len, &p->pass_caret, ev);
        else field_on_key(p->commit_msg, (int)sizeof(p->commit_msg), &p->commit_len, &p->commit_caret, ev);
        return 1;
    }
    if (ev->mod & (TB_MOD_CTRL | TB_MOD_ALT)) return 0;
    if (ev->key == TB_KEY_ARROW_UP) {
        move_cursor(p, -1);
        return 1;
    }
    if (ev->key == TB_KEY_ARROW_DOWN) {
        move_cursor(p, 1);
        return 1;
    }
    if (ev->key == TB_KEY_PGUP) {
        move_cursor(p, -((p->h > 3) ? p->h - 3 : 1));
        return 1;
    }
    if (ev->key == TB_KEY_PGDN) {
        move_cursor(p, (p->h > 3) ? p->h - 3 : 1);
        return 1;
    }
    if (ev->key == TB_KEY_HOME) {
        p->cursor = 0;
        ensure_cursor(p);
        return 1;
    }
    if (ev->key == TB_KEY_END) {
        p->cursor = row_span(p) ? row_span(p) - 1 : 0;
        ensure_cursor(p);
        return 1;
    }
    if (ev->key == TB_KEY_ENTER) {
        if (p->mode == SIDE_SET) set_apply(editor, p);
        else activate_row(editor, 1);
        return 1;
    }
    if (p->mode == SIDE_FILES && ev->key == TB_KEY_DELETE) {
        row = (p->cursor >= 0 && p->cursor < p->nrows) ? &p->rows[p->cursor] : NULL;
        file_remove(editor, p, row);
        return 1;
    }
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        if (p->mode == SIDE_FILES) go_parent(p);
        return 1;
    }
    if (ev->key == TB_KEY_ARROW_LEFT) {
        if (p->mode == SIDE_GIT) {
            acct_cycle(editor, -1);
            return 1;
        }
        if (p->mode != SIDE_FILES) return 1;
        row = (p->cursor >= 0 && p->cursor < p->nrows) ? &p->rows[p->cursor] : NULL;
        if (row && row->is_dir && row->expanded && strcmp(row->name, "..") != 0) {
            set_open(p, row->path, 0);
            rescan_files(p);
        } else {
            go_parent(p);
        }
        return 1;
    }
    if (ev->key == TB_KEY_ARROW_RIGHT) {
        if (p->mode == SIDE_GIT) {
            acct_cycle(editor, 1);
            return 1;
        }
        if (p->mode != SIDE_FILES) return 1;
        row = (p->cursor >= 0 && p->cursor < p->nrows) ? &p->rows[p->cursor] : NULL;
        if (!row) return 1;
        if (strcmp(row->name, "..") == 0) go_parent(p);
        else if (row->is_dir) enter_dir(p, row->path);
        else open_file(editor, row->path);
        return 1;
    }
    if (p->mode == SIDE_GIT && ev->ch) {
        row = (p->cursor >= 0 && p->cursor < p->nrows) ? &p->rows[p->cursor] : NULL;
        if (ev->ch == 'r') {
            p->git_ms = 0;
            p->where[0] = '\0';
            refresh_git(editor, 1);
        } else if (ev->ch == 'a' && row) {
            git_file_cmd(editor, row, "add");
        } else if (ev->ch == 'A') {
            git_run(editor, "add -A", 0, 0);
        } else if (ev->ch == 'u' && row) {
            git_file_cmd(editor, row, "restore --staged");
        } else if (ev->ch == 'U') {
            git_run(editor, "restore --staged .", 0, 0);
        } else if (ev->ch == 'd' && row) {
            show_git_diff(editor, row);
        } else if (ev->ch == 'D') {
            git_run(editor, "diff HEAD --stat", 1, 0);
        } else if (ev->ch == 'l') {
            git_run(editor, "log --decorate --oneline -n 40", 1, 0);
        } else if (ev->ch == 'L') {
            git_run(editor, "log --graph --decorate --oneline --all -n 80", 1, 0);
        } else if (ev->ch == 'b') {
            git_run(editor, "branch --all --verbose", 1, 0);
        } else if (ev->ch == 'B') {
            git_prompt_cmd(editor, "switch to branch?", "switch", 1, 0);
        } else if (ev->ch == 'C') {
            git_prompt_cmd(editor, "new branch?", "switch -c", 1, 0);
        } else if (ev->ch == 't') {
            git_run(editor, "tag --sort=-creatordate -n", 1, 0);
        } else if (ev->ch == 'T') {
            git_prompt_cmd(editor, "new tag?", "tag", 1, 0);
        } else if (ev->ch == 'R') {
            git_run(editor, "remote -v", 1, 0);
        } else if (ev->ch == 'i') {
            git_run(editor, "stash list --date=local", 1, 0);
        } else if (ev->ch == 'w') {
            git_run(editor, "worktree list", 1, 0);
        } else if (ev->ch == 'h' && row) {
            git_file_show(editor, row, "log --follow --decorate --oneline -n 60");
        } else if (ev->ch == 'v' && row) {
            git_file_show(editor, row, "blame --date=short");
        } else if (ev->ch == '/') {
            git_search(editor);
        } else if (ev->ch == 'f') {
            p->filter = (p->filter + 1) % GIT_FILTER_COUNT;
            p->cursor = 0;
            p->scroll = 0;
            p->git_ms = 0;
            refresh_git(editor, 1);
            MLE_SET_INFO(editor, "git filter: %s", git_filter_name(p->filter));
        } else if (ev->ch == 'c') {
            p->field = FIELD_COMMIT;
            if (p->commit_caret > p->commit_len) p->commit_caret = p->commit_len;
            if (p->commit_caret < 0) p->commit_caret = 0;
        } else if (ev->ch == 'n') {
            acct_begin_login(p);
        } else if (ev->ch == 'N') {
            acct_forget(editor);
        } else if (ev->ch == 'g' || ev->ch == ']') {
            acct_cycle(editor, 1);
        } else if (ev->ch == '[') {
            acct_cycle(editor, -1);
        } else if (ev->ch == 'p') {
            git_push(editor);
        } else if (ev->ch == 'P') {
            git_pull(editor);
        } else if (ev->ch == 'F') {
            git_run(editor, "fetch --prune", 1, 1);
        } else if (ev->ch == 's') {
            git_run(editor, "stash push -u", 0, 0);
        } else if (ev->ch == 'S') {
            if (confirm_yes(editor, "pop latest stash? type yes")) git_run(editor, "stash pop", 1, 0);
        } else if (ev->ch == 'x' && row) {
            if (confirm_yes(editor, "discard changes in this file? type yes")) git_file_cmd(editor, row, "restore");
        } else if (ev->ch == 'o' && row && row->path && row->path[0]) {
            open_git_file(editor, row);
        } else if (ev->ch == 'm') {
            git_prompt_cmd(editor, "merge branch?", "merge --no-edit", 1, 0);
        } else if (ev->ch == 'M') {
            if (confirm_yes(editor, "abort the merge? type yes")) git_run(editor, "merge --abort", 1, 0);
        } else if (ev->ch == 'e') {
            if (confirm_yes(editor, "amend the last commit? type yes")) git_run(editor, "commit --amend --no-edit", 1, 0);
        } else if (ev->ch == 'q') {
            git_run(editor, "status -sb", 1, 0);
        } else if (ev->ch == 'G') {
            git_run(editor, "rebase --continue", 1, 0);
        } else if (ev->ch == 'Z') {
            if (confirm_yes(editor, "abort the rebase? type yes")) git_run(editor, "rebase --abort", 1, 0);
        }
        return 1;
    }
    if (p->mode == SIDE_FILES && ev->ch) {
        row = (p->cursor >= 0 && p->cursor < p->nrows) ? &p->rows[p->cursor] : NULL;
        if (ev->ch == 'n') { file_create(editor, p, 0); return 1; }
        if (ev->ch == 'N') { file_create(editor, p, 1); return 1; }
        if (ev->ch == 'r') { file_rename(editor, p, row); return 1; }
        if (ev->ch == 'x') { file_remove(editor, p, row); return 1; }
    }
    if (p->mode == SIDE_FILES && ev->ch == '/') {
        goto_typed_dir(editor, p);
        return 1;
    }
    if (p->mode == SIDE_FILES && ev->ch >= 32 && ev->ch < 127) {
        int i, start;
        start = p->cursor + 1;
        if (start < 0) start = 0;
        for (i = 0; i < p->nrows; i++) {
            int idx = (start + i) % p->nrows;
            char c = p->rows[idx].name[0];
            if (tolower((unsigned char)c) == tolower((unsigned char)ev->ch)) {
                p->cursor = idx;
                ensure_cursor(p);
                break;
            }
        }
        return 1;
    }
    return 1;
}

int side_handle_mouse(editor_t *editor, struct tb_event *ev) {
    struct side_panel_s *p;
    int inside, x, y, body, idx;
    long now;
    if (!editor || !editor->side || !ev) return 0;
    p = editor->side;
    if (!p->visible || p->h < 2 || p->w < 4) return 0;
    x = ev->x;
    y = ev->y;
    inside = x >= p->x && y >= p->y && x < p->x + p->w && y < p->y + p->h;
    if (p->drag) {
        if (ev->key == TB_KEY_MOUSE_RELEASE) {
            p->drag = 0;
            return 1;
        }
        if (ev->key == TB_KEY_MOUSE_LEFT) {
            int nw = x - p->x + 1;
            if (nw < 18) nw = 18;
            if (nw > 72) nw = 72;
            p->w = nw;
            relayout(editor);
            return 1;
        }
    }
    if (!inside) {
        if (ev->key == TB_KEY_MOUSE_LEFT || ev->key == TB_KEY_MOUSE_RIGHT) p->focused = 0;
        return 0;
    }
    if (ev->key == TB_KEY_MOUSE_WHEEL_UP || ev->key == TB_KEY_MOUSE_WHEEL_DOWN) {
        move_cursor(p, ev->key == TB_KEY_MOUSE_WHEEL_UP ? -3 : 3);
        p->focused = 1;
        return 1;
    }
    if (ev->key == TB_KEY_MOUSE_RELEASE) return 1;
    if (ev->key != TB_KEY_MOUSE_LEFT && ev->key != TB_KEY_MOUSE_RIGHT) return 1;
    if (editor->iterm) iterm_action(editor, "unfocus");
    p->focused = 1;
    if (x == p->x + p->w - 1) {
        p->drag = 1;
        return 1;
    }
    if (y == p->y) {
        if (x >= p->tab_help && x < p->tab_help + 2) p->mode = SIDE_GUIDE;
        else if (x >= 12 && x < 16) {
            p->mode = SIDE_SET;
            p->cursor = 0;
            p->scroll = 0;
        } else if (x >= p->tab_git && x < p->tab_git + 4) {
            p->mode = SIDE_GIT;
            p->git_ms = 0;
            refresh_git(editor, 1);
            git_stage_default(editor);
            git_enter(editor);
        } else {
            p->mode = SIDE_FILES;
            rescan_files(p);
        }
        return 1;
    }
    if (y == p->y + 1) {
        if (p->mode == SIDE_GIT) {
            p->filter = (p->filter + 1) % GIT_FILTER_COUNT;
            p->cursor = 0;
            p->scroll = 0;
            p->git_ms = 0;
            refresh_git(editor, 1);
            MLE_SET_INFO(editor, "git filter: %s", git_filter_name(p->filter));
        }
        return 1;
    }
    if (p->mode == SIDE_GIT && y == p->y + 2) {
        p->clicks = 0;
        if (p->field == FIELD_USER) field_click(p->user, p->user_len, &p->user_caret, x, p->x, p->w);
        else if (p->field == FIELD_PASS) field_click(p->pass, p->pass_len, &p->pass_caret, x, p->x, p->w);
        else if (p->nacct < 1) {
            if (p->user_len > 0) p->field = FIELD_USER;
            else acct_begin_login(p);
        } else {
            acct_cycle(editor, 1);
        }
        return 1;
    }
    if (p->mode == SIDE_GIT && y == p->y + 3) {
        if (p->field == FIELD_USER || p->field == FIELD_PASS) pass_clear(p);
        p->field = FIELD_COMMIT;
        if (p->commit_caret < 0) p->commit_caret = 0;
        if (p->commit_caret > p->commit_len) p->commit_caret = p->commit_len;
        field_click(p->commit_msg, p->commit_len, &p->commit_caret, x, p->x, p->w);
        p->clicks = 0;
        return 1;
    }
    if (p->mode == SIDE_GIT && y == p->y + 4) {
        git_btn_t b[4];
        int bi;
        p->clicks = 0;
        lay_buttons(p, b);
        for (bi = 0; bi < 4; bi++) {
            if (x >= b[bi].x && x < b[bi].x + b[bi].cols) {
                if (bi == 0) git_commit_msg(editor);
                else if (bi == 1) git_push(editor);
                else if (bi == 2) git_pull(editor);
                else git_run(editor, "fetch --prune", 1, 1);
                return 1;
            }
        }
        return 1;
    }
    body = p->h - (list_origin(p));
    idx = p->scroll + (y - (p->y + (list_origin(p))));
    if (p->mode == SIDE_SET) {
        if (body < 1 || idx < 0 || idx >= SET_N || y >= p->y + (list_origin(p)) + body) return 1;
        p->cursor = idx;
        if (ev->key == TB_KEY_MOUSE_LEFT || ev->key == TB_KEY_MOUSE_RIGHT) set_apply(editor, p);
        return 1;
    }
    if (body < 1 || idx < 0 || idx >= p->nrows || y >= p->y + (list_origin(p)) + body) return 1;
    now = mono_ms();
    if (idx == p->last_row && now - p->last_ms < 400) p->clicks += 1;
    else p->clicks = 1;
    p->last_ms = now;
    p->last_row = idx;
    p->cursor = idx;
    if (p->mode == SIDE_GIT) {
        if (p->field == FIELD_USER || p->field == FIELD_PASS) pass_clear(p);
        p->field = FIELD_NONE;
    }
    if (p->mode == SIDE_FILES && ev->key == TB_KEY_MOUSE_RIGHT) {
        file_rename(editor, p, &p->rows[p->cursor]);
        return 1;
    }
    if (p->mode == SIDE_GIT && ev->key == TB_KEY_MOUSE_RIGHT) {
        show_git_diff(editor, &p->rows[p->cursor]);
        return 1;
    }
    if (p->mode == SIDE_GIT && ev->key == TB_KEY_MOUSE_LEFT && p->clicks < 2) {
        git_toggle_stage(editor, &p->rows[p->cursor]);
        return 1;
    }
    if (ev->key == TB_KEY_MOUSE_RIGHT || p->clicks >= 2) activate_row(editor, 1);
    else activate_row(editor, 0);
    return 1;
}

int side_place_cursor(editor_t *editor) {
    struct side_panel_s *p;
    int origin, vis, body;
    if (!editor || !editor->side || editor->headless_mode) return 0;
    p = editor->side;
    if (!p->visible || !p->focused || p->h < 2 || p->w < 4) return 0;
    if (p->careton && p->caretx >= 0 && p->carety >= 0) {
        tb_set_cursor(p->caretx, p->carety);
        return 1;
    }
    if (p->mode == SIDE_GUIDE || (p->mode != SIDE_SET && p->nrows < 1)) {
        tb_set_cursor(-1, -1);
        return 1;
    }
    origin = list_origin(p);
    body = p->h - origin;
    vis = p->cursor - p->scroll;
    if (body > 0 && vis >= 0 && vis < body) {
        tb_set_cursor(p->x + 1, p->y + origin + vis);
        return 1;
    }
    tb_set_cursor(-1, -1);
    return 1;
}

static bview_t *g_guide;

int cmd_side(cmd_context_t *ctx) {
    const char *action;
    action = ctx->static_param ? ctx->static_param : "toggle";
    return side_action(ctx->editor, action);
}

int cmd_guide(cmd_context_t *ctx) {
    bview_t *bv;
    int live;
    const char *text;
    live = 0;
    if (g_guide) {
        CDL_FOREACH2(ctx->editor->all_bviews, bv, all_next) {
            if (bv == g_guide) {
                live = 1;
                break;
            }
        }
    }
    if (live) {
        editor_set_active(ctx->editor, g_guide);
        return MLE_OK;
    }
    text = side_guide_text();
    editor_open_bview(ctx->editor, NULL, MLE_BVIEW_TYPE_EDIT, NULL, 0, 1, 0, 0, NULL, &bv);
    if (!bv) return MLE_ERR;
    buffer_insert(bv->buffer, 0, (char *)text, (bint_t)strlen(text), NULL);
    bv->buffer->is_unsaved = 0;
    mark_move_beginning(bv->active_cursor->mark);
    bview_zero_viewport_y(bv);
    g_guide = bv;
    return MLE_OK;
}
