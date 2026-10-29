#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utlist.h>
#include "mle.h"

/*
 * Integrated terminal.
 *
 * A panel docks beside the edit area (bottom by default, like VS Code).
 * Each terminal is a real PTY plus a VT screen. While the panel is focused,
 * keys go to the shell. f7 / M-` toggles the panel. M-t is the command
 * prefix and is not delivered to the shell.
 *
 * The outer termbox stays in its normal 16-color mode so editor colors are
 * unchanged. 256-color and truecolor sequences are mapped onto those 16.
 */

#define ITERM_SB_MAX     2000
#define ITERM_PARAMS     16
#define ITERM_OSC_MAX    512
#define ITERM_MAX_DIM    500

enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_CSI,
    ST_OSC,
    ST_OSC_ESC,
    ST_IGNORE,
    ST_IGNORE_ESC,
    ST_UTF8,
    ST_CHARSET
};

enum {
    ATTR_BOLD    = 1,
    ATTR_DIM     = 2,
    ATTR_ITALIC  = 4,
    ATTR_UNDER   = 8,
    ATTR_BLINK   = 16,
    ATTR_REVERSE = 32,
    ATTR_INVIS   = 64
};

enum {
    CELL_EMPTY = 0,
    CELL_CHAR  = 1,
    CELL_WIDE  = 2,
    CELL_CONT  = 3
};

enum {
    DOCK_BOTTOM = 0,
    DOCK_TOP,
    DOCK_LEFT,
    DOCK_RIGHT,
    DOCK_MAX
};

typedef struct cell_s {
    uint32_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t w;
} cell_t;

typedef struct row_s {
    cell_t *cells;
    int cols;
    int wrapped;
} row_t;

typedef struct grid_s {
    row_t *rows;
    int ncols;
    int nrows;
} grid_t;

typedef struct pen_s {
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
} pen_t;

typedef struct lline_s {
    cell_t *cells;
    int len;
    int cap;
} lline_t;

typedef struct iterm_s {
    int id;
    int fd;
    pid_t pid;
    int alive;
    int exit_code;
    char title[96];
    int title_locked;
    char cwd[PATH_MAX];
    grid_t primary;
    grid_t alt;
    int alt_on;
    row_t *sb;
    int sb_start;
    int sb_count;
    int sb_cap;
    int cx, cy;
    int pending_wrap;
    int pri_cx, pri_cy, pri_pending;
    int saved_cx, saved_cy;
    pen_t pen;
    pen_t saved_pen;
    int scroll_top, scroll_bot;
    int wrap_mode;
    int origin_mode;
    int cursor_visible;
    int app_cursor;
    int bracket_paste;
    int mouse_1000, mouse_1002, mouse_1006, focus_events;
    int insert_mode;
    int g0_special, g1_special, using_g1;
    int saved_origin, saved_wrap;
    int state;
    char ubuf[8];
    int ulen;
    int uneed;
    int csi_priv;
    int csi_inter;
    int csi_n;
    int csi_p[ITERM_PARAMS];
    int csi_set[ITERM_PARAMS];
    char osc[ITERM_OSC_MAX];
    int osc_len;
    int charset_g;
    uint32_t last_ch;
    int last_width;
    int tab_x;
    int tab_w;
    int close_x;
    struct iterm_s *next;
    struct iterm_s *prev;
} iterm_t;

typedef struct iview_s {
    int split;
    int vertical;
    float factor;
    struct iview_s *parent;
    struct iview_s *a;
    struct iview_s *b;
    iterm_t *term;
    int x, y, w, h;
    int scroll;
    int copy_mode;
    int copy_x, copy_y;
    int sel_on;
    int sel_ax, sel_ay;
} iview_t;

typedef struct iterm_panel_s {
    editor_t *editor;
    int visible;
    int focused;
    int dock;
    int size_h;
    int size_w;
    int x, y, w, h;
    int tabh;
    iterm_t *terms;
    int next_id;
    int ever_hinted;
    iview_t *root;
    iview_t *focus;
    int prefix;
    const char *hint;
    int mouse_down;
    int drag; /* 1 panel size, 2 split factor */
    iview_t *drag_split;
    int last_btn;
    iview_t *sel_view;
    int grip; /* 0 none, 1 top row, 2 bottom row, 3 right col, 4 left col */
    int tab_y;
    int plus_x;
    int plus_w;
} iterm_panel_t;

static const uint8_t ansi_rgb[16][3] = {
    {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
    {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
    {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255}
};

static uint32_t dec_line(unsigned char b) {
    static const uint32_t map[] = {
        0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
        0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
        0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
        0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
    };
    if (b >= 0x60 && b <= 0x7e) return map[b - 0x60];
    return b;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t nearest_ansi(int r, int g, int b) {
    int i, best, bestd;
    best = 0;
    bestd = 1 << 30;
    for (i = 0; i < 16; i++) {
        int dr = r - ansi_rgb[i][0];
        int dg = g - ansi_rgb[i][1];
        int db = b - ansi_rgb[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return (uint8_t)(best + 1);
}

static uint8_t color_from_256(int idx) {
    static const int level[6] = {0, 95, 135, 175, 215, 255};
    int i, ri, gi, bi, v;
    if (idx < 0) return 0;
    if (idx > 255) idx = 255;
    if (idx < 16) return (uint8_t)(idx + 1);
    if (idx >= 232) {
        v = 8 + (idx - 232) * 10;
        return nearest_ansi(v, v, v);
    }
    i = idx - 16;
    ri = i / 36;
    gi = (i / 6) % 6;
    bi = i % 6;
    return nearest_ansi(level[ri], level[gi], level[bi]);
}

static uintattr_t tb_from_color(uint8_t c) {
    if (c == 0) return 0;
    if (c <= 8) return (uintattr_t)c;
    return (uintattr_t)(c - 8) | TB_BRIGHT;
}

static uintattr_t tb_fg(cell_t *c) {
    uintattr_t a;
    a = tb_from_color(c->fg);
    if (c->attr & ATTR_BOLD) a |= TB_BOLD;
    if (c->attr & ATTR_DIM) a |= TB_DIM;
    if (c->attr & ATTR_ITALIC) a |= TB_ITALIC;
    if (c->attr & ATTR_UNDER) a |= TB_UNDERLINE;
    if (c->attr & ATTR_BLINK) a |= TB_BLINK;
    if (c->attr & ATTR_REVERSE) a |= TB_REVERSE;
    return a;
}

static uintattr_t tb_bg(cell_t *c) {
    return tb_from_color(c->bg);
}

static void grid_free(grid_t *g) {
    int i;
    if (!g || !g->rows) return;
    for (i = 0; i < g->nrows; i++) free(g->rows[i].cells);
    free(g->rows);
    g->rows = NULL;
    g->nrows = 0;
    g->ncols = 0;
}

static int grid_init(grid_t *g, int cols, int rows) {
    int i;
    memset(g, 0, sizeof(*g));
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > ITERM_MAX_DIM) cols = ITERM_MAX_DIM;
    if (rows > ITERM_MAX_DIM) rows = ITERM_MAX_DIM;
    g->ncols = cols;
    g->nrows = rows;
    g->rows = calloc((size_t)rows, sizeof(row_t));
    if (!g->rows) return -1;
    for (i = 0; i < rows; i++) {
        g->rows[i].cols = cols;
        g->rows[i].cells = calloc((size_t)cols, sizeof(cell_t));
        if (!g->rows[i].cells) return -1;
    }
    return 0;
}

static void grid_resize_geom(grid_t *g, int cols, int rows) {
    grid_t n;
    int r, cr, cc;
    if (grid_init(&n, cols, rows) != 0) return;
    if (g->rows) {
        cr = rows < g->nrows ? rows : g->nrows;
        cc = cols < g->ncols ? cols : g->ncols;
        for (r = 0; r < cr; r++) {
            memcpy(n.rows[r].cells, g->rows[r].cells, (size_t)cc * sizeof(cell_t));
            n.rows[r].wrapped = g->rows[r].wrapped;
        }
        grid_free(g);
    }
    *g = n;
}

static grid_t *ag(iterm_t *t) {
    return t->alt_on ? &t->alt : &t->primary;
}

static void sb_free_all(iterm_t *t) {
    int i;
    if (!t->sb) return;
    for (i = 0; i < t->sb_cap; i++) free(t->sb[i].cells);
    free(t->sb);
    t->sb = NULL;
    t->sb_count = 0;
    t->sb_start = 0;
    t->sb_cap = 0;
}

static void sb_push(iterm_t *t, row_t *row) {
    int idx;
    if (t->alt_on) {
        free(row->cells);
        return;
    }
    if (t->sb_cap == 0) {
        t->sb_cap = ITERM_SB_MAX;
        t->sb = calloc((size_t)t->sb_cap, sizeof(row_t));
        if (!t->sb) {
            t->sb_cap = 0;
            free(row->cells);
            return;
        }
    }
    if (t->sb_count == t->sb_cap) {
        free(t->sb[t->sb_start].cells);
        t->sb[t->sb_start].cells = NULL;
        t->sb_start = (t->sb_start + 1) % t->sb_cap;
        t->sb_count--;
    }
    idx = (t->sb_start + t->sb_count) % t->sb_cap;
    t->sb[idx] = *row;
    t->sb_count++;
}

static row_t *sb_at(iterm_t *t, int nth) {
    int idx;
    if (nth < 0 || nth >= t->sb_count || !t->sb) return NULL;
    idx = (t->sb_start + nth) % t->sb_cap;
    return &t->sb[idx];
}

static void blank_cell(iterm_t *t, cell_t *c) {
    memset(c, 0, sizeof(*c));
    c->fg = t->pen.fg;
    c->bg = t->pen.bg;
}

static void clear_row_range(iterm_t *t, row_t *row, int x0, int x1) {
    int x;
    if (!row || !row->cells) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= row->cols) x1 = row->cols - 1;
    for (x = x0; x <= x1; x++) blank_cell(t, &row->cells[x]);
}

static void term_reset_modes(iterm_t *t) {
    t->pen.fg = 0;
    t->pen.bg = 0;
    t->pen.attr = 0;
    t->wrap_mode = 1;
    t->origin_mode = 0;
    t->cursor_visible = 1;
    t->app_cursor = 0;
    t->bracket_paste = 0;
    t->mouse_1000 = 0;
    t->mouse_1002 = 0;
    t->mouse_1006 = 0;
    t->focus_events = 0;
    t->insert_mode = 0;
    t->g0_special = 0;
    t->g1_special = 0;
    t->using_g1 = 0;
    t->pending_wrap = 0;
    t->last_ch = 0;
    t->last_width = 1;
}

static void term_clear_grid(iterm_t *t, grid_t *g) {
    int r, x;
    if (!g || !g->rows) return;
    for (r = 0; r < g->nrows; r++) {
        g->rows[r].wrapped = 0;
        for (x = 0; x < g->rows[r].cols; x++) blank_cell(t, &g->rows[r].cells[x]);
    }
}

static void term_full_reset(iterm_t *t) {
    term_reset_modes(t);
    sb_free_all(t);
    term_clear_grid(t, &t->primary);
    term_clear_grid(t, &t->alt);
    t->alt_on = 0;
    t->cx = 0;
    t->cy = 0;
    t->pri_cx = 0;
    t->pri_cy = 0;
    t->pri_pending = 0;
    if (t->primary.nrows > 0) {
        t->scroll_top = 0;
        t->scroll_bot = t->primary.nrows - 1;
    }
}

static void clamp_cursor(iterm_t *t) {
    grid_t *g;
    int top, bot;
    g = ag(t);
    if (!g->nrows) return;
    top = t->origin_mode ? t->scroll_top : 0;
    bot = t->origin_mode ? t->scroll_bot : g->nrows - 1;
    if (top < 0) top = 0;
    if (bot >= g->nrows) bot = g->nrows - 1;
    if (bot < top) bot = top;
    t->cy = clampi(t->cy, top, bot);
    t->cx = clampi(t->cx, 0, g->ncols - 1);
}

static void scroll_up_n(iterm_t *t, int n) {
    grid_t *g;
    int top, bot, i, full;
    g = ag(t);
    if (!g->rows || g->nrows < 1) return;
    top = t->scroll_top;
    bot = t->scroll_bot;
    if (top < 0) top = 0;
    if (bot >= g->nrows) bot = g->nrows - 1;
    if (bot < top) return;
    if (n > bot - top + 1) n = bot - top + 1;
    full = !t->alt_on && top == 0 && bot == g->nrows - 1;
    for (i = 0; i < n; i++) {
        row_t dying;
        dying = g->rows[top];
        if (bot > top) {
            memmove(&g->rows[top], &g->rows[top + 1], (size_t)(bot - top) * sizeof(row_t));
        }
        if (full) sb_push(t, &dying);
        else free(dying.cells);
        g->rows[bot].cols = g->ncols;
        g->rows[bot].wrapped = 0;
        g->rows[bot].cells = calloc((size_t)g->ncols, sizeof(cell_t));
        if (g->rows[bot].cells) {
            int x;
            for (x = 0; x < g->ncols; x++) blank_cell(t, &g->rows[bot].cells[x]);
        }
    }
}

static void scroll_down_n(iterm_t *t, int n) {
    grid_t *g;
    int top, bot, i;
    g = ag(t);
    if (!g->rows || g->nrows < 1) return;
    top = t->scroll_top;
    bot = t->scroll_bot;
    if (top < 0) top = 0;
    if (bot >= g->nrows) bot = g->nrows - 1;
    if (bot < top) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (i = 0; i < n; i++) {
        row_t dying;
        dying = g->rows[bot];
        if (bot > top) {
            memmove(&g->rows[top + 1], &g->rows[top], (size_t)(bot - top) * sizeof(row_t));
        }
        free(dying.cells);
        g->rows[top].cols = g->ncols;
        g->rows[top].wrapped = 0;
        g->rows[top].cells = calloc((size_t)g->ncols, sizeof(cell_t));
        if (g->rows[top].cells) {
            int x;
            for (x = 0; x < g->ncols; x++) blank_cell(t, &g->rows[top].cells[x]);
        }
    }
}

static void index_down(iterm_t *t) {
    grid_t *g;
    g = ag(t);
    if (t->cy == t->scroll_bot) scroll_up_n(t, 1);
    else if (t->cy < g->nrows - 1) t->cy++;
}

static void index_up(iterm_t *t) {
    if (t->cy == t->scroll_top) scroll_down_n(t, 1);
    else if (t->cy > 0) t->cy--;
}

static void do_wrap(iterm_t *t) {
    grid_t *g;
    g = ag(t);
    if (t->cy >= 0 && t->cy < g->nrows) g->rows[t->cy].wrapped = 1;
    t->pending_wrap = 0;
    t->cx = 0;
    index_down(t);
}

static row_t *cur_row(iterm_t *t) {
    grid_t *g;
    g = ag(t);
    if (!g->rows || t->cy < 0 || t->cy >= g->nrows) return NULL;
    return &g->rows[t->cy];
}

static void put_char(iterm_t *t, uint32_t ch) {
    row_t *row;
    int width, x;
    grid_t *g;
    if (ch == 0) return;
    width = tb_wcwidth(ch);
    if (width < 0) return;
    if (width == 0) return;
    if (width > 2) width = 1;
    g = ag(t);
    if (!g->rows || g->ncols < 1) return;
    if (t->pending_wrap && t->wrap_mode) do_wrap(t);
    if (width == 2 && t->cx >= g->ncols - 1 && t->wrap_mode) {
        t->pending_wrap = 1;
        do_wrap(t);
    }
    if (t->cx >= g->ncols) t->cx = g->ncols - 1;
    if (t->cx < 0) t->cx = 0;
    row = cur_row(t);
    if (!row || !row->cells) return;

    if (t->insert_mode && width == 1) {
        for (x = row->cols - 1; x > t->cx; x--) row->cells[x] = row->cells[x - 1];
    }

    if (row->cells[t->cx].w == CELL_CONT && t->cx > 0) {
        blank_cell(t, &row->cells[t->cx - 1]);
        blank_cell(t, &row->cells[t->cx]);
    }
    if (row->cells[t->cx].w == CELL_WIDE && t->cx + 1 < row->cols) {
        blank_cell(t, &row->cells[t->cx + 1]);
    }

    row->cells[t->cx].ch = ch;
    row->cells[t->cx].fg = t->pen.fg;
    row->cells[t->cx].bg = t->pen.bg;
    row->cells[t->cx].attr = t->pen.attr;
    row->cells[t->cx].w = (uint8_t)(width == 2 ? CELL_WIDE : CELL_CHAR);
    if (width == 2 && t->cx + 1 < row->cols) {
        blank_cell(t, &row->cells[t->cx + 1]);
        row->cells[t->cx + 1].w = CELL_CONT;
        row->cells[t->cx + 1].fg = t->pen.fg;
        row->cells[t->cx + 1].bg = t->pen.bg;
    }
    t->last_ch = ch;
    t->last_width = width;
    t->cx += width;
    if (t->cx >= g->ncols) {
        t->cx = g->ncols - 1;
        if (t->wrap_mode) t->pending_wrap = 1;
    }
}

static void carriage_return(iterm_t *t) {
    t->cx = 0;
    t->pending_wrap = 0;
}

static void linefeed(iterm_t *t) {
    grid_t *g;
    g = ag(t);
    t->pending_wrap = 0;
    if (t->cy >= 0 && t->cy < g->nrows) g->rows[t->cy].wrapped = 0;
    index_down(t);
}

static void backspace(iterm_t *t) {
    if (t->pending_wrap) {
        t->pending_wrap = 0;
        return;
    }
    if (t->cx > 0) t->cx--;
}

static void tab_forward(iterm_t *t) {
    grid_t *g;
    int next;
    g = ag(t);
    t->pending_wrap = 0;
    next = (t->cx + 8) & ~7;
    if (next >= g->ncols) next = g->ncols - 1;
    t->cx = next;
}

static void save_cursor(iterm_t *t) {
    t->saved_cx = t->cx;
    t->saved_cy = t->cy;
    t->saved_pen = t->pen;
    t->saved_origin = t->origin_mode;
    t->saved_wrap = t->wrap_mode;
}

static void restore_cursor(iterm_t *t) {
    t->cx = t->saved_cx;
    t->cy = t->saved_cy;
    t->pen = t->saved_pen;
    t->origin_mode = t->saved_origin;
    t->wrap_mode = t->saved_wrap;
    t->pending_wrap = 0;
    clamp_cursor(t);
}

static void enter_alt(iterm_t *t, int clear) {
    if (!t->alt_on) {
        t->pri_cx = t->cx;
        t->pri_cy = t->cy;
        t->pri_pending = t->pending_wrap;
        t->alt_on = 1;
        t->cx = 0;
        t->cy = 0;
        t->pending_wrap = 0;
    }
    t->scroll_top = 0;
    t->scroll_bot = t->alt.nrows > 0 ? t->alt.nrows - 1 : 0;
    if (clear) term_clear_grid(t, &t->alt);
}

static void leave_alt(iterm_t *t) {
    if (!t->alt_on) return;
    t->alt_on = 0;
    t->cx = t->pri_cx;
    t->cy = t->pri_cy;
    t->pending_wrap = t->pri_pending;
    t->scroll_top = 0;
    t->scroll_bot = t->primary.nrows > 0 ? t->primary.nrows - 1 : 0;
    clamp_cursor(t);
}

static int cursor_row_base(iterm_t *t) {
    return t->origin_mode ? t->scroll_top : 0;
}

static void move_to(iterm_t *t, int row1, int col1) {
    int base;
    t->pending_wrap = 0;
    base = cursor_row_base(t);
    t->cy = base + row1 - 1;
    t->cx = col1 - 1;
    clamp_cursor(t);
}

static void erase_display(iterm_t *t, int mode) {
    grid_t *g;
    int r;
    g = ag(t);
    if (!g->rows) return;
    if (mode == 3 && !t->alt_on) sb_free_all(t);
    if (mode == 2 || mode == 3) {
        term_clear_grid(t, g);
        return;
    }
    if (mode == 0) {
        clear_row_range(t, cur_row(t), t->cx, 9999);
        for (r = t->cy + 1; r < g->nrows; r++) {
            g->rows[r].wrapped = 0;
            clear_row_range(t, &g->rows[r], 0, 9999);
        }
    } else if (mode == 1) {
        for (r = 0; r < t->cy && r < g->nrows; r++) {
            g->rows[r].wrapped = 0;
            clear_row_range(t, &g->rows[r], 0, 9999);
        }
        clear_row_range(t, cur_row(t), 0, t->cx);
    }
}

static void erase_line(iterm_t *t, int mode) {
    row_t *row;
    row = cur_row(t);
    if (!row) return;
    if (mode == 2) {
        row->wrapped = 0;
        clear_row_range(t, row, 0, 9999);
    } else if (mode == 1) {
        clear_row_range(t, row, 0, t->cx);
    } else {
        clear_row_range(t, row, t->cx, 9999);
    }
}

static void insert_lines(iterm_t *t, int n) {
    int saved_top, saved_bot;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bot) return;
    saved_top = t->scroll_top;
    saved_bot = t->scroll_bot;
    t->scroll_top = t->cy;
    scroll_down_n(t, n);
    t->scroll_top = saved_top;
    t->scroll_bot = saved_bot;
    t->cx = 0;
    t->pending_wrap = 0;
}

static void delete_lines(iterm_t *t, int n) {
    int saved_top;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bot) return;
    saved_top = t->scroll_top;
    t->scroll_top = t->cy;
    scroll_up_n(t, n);
    t->scroll_top = saved_top;
    t->cx = 0;
    t->pending_wrap = 0;
}

static void insert_cells(iterm_t *t, int n) {
    row_t *row;
    int x;
    row = cur_row(t);
    if (!row || n < 1) return;
    if (n > row->cols) n = row->cols;
    for (x = row->cols - 1; x >= t->cx + n; x--) row->cells[x] = row->cells[x - n];
    clear_row_range(t, row, t->cx, t->cx + n - 1);
}

static void delete_cells(iterm_t *t, int n) {
    row_t *row;
    int x;
    row = cur_row(t);
    if (!row || n < 1) return;
    if (t->cx + n > row->cols) n = row->cols - t->cx;
    for (x = t->cx; x < row->cols - n; x++) row->cells[x] = row->cells[x + n];
    clear_row_range(t, row, row->cols - n, row->cols - 1);
}

static void csi_reset(iterm_t *t) {
    t->csi_n = 0;
    t->csi_priv = 0;
    t->csi_inter = 0;
    memset(t->csi_p, 0, sizeof(t->csi_p));
    memset(t->csi_set, 0, sizeof(t->csi_set));
}

static void csi_digit(iterm_t *t, int d) {
    int i;
    if (t->csi_n <= 0) t->csi_n = 1;
    i = t->csi_n - 1;
    if (i < 0 || i >= ITERM_PARAMS) return;
    t->csi_set[i] = 1;
    if (t->csi_p[i] < 100000) t->csi_p[i] = t->csi_p[i] * 10 + d;
}

static void csi_semi(iterm_t *t) {
    if (t->csi_n <= 0) t->csi_n = 1;
    if (t->csi_n < ITERM_PARAMS) {
        t->csi_n++;
        t->csi_p[t->csi_n - 1] = 0;
        t->csi_set[t->csi_n - 1] = 0;
    }
}

static int csi_get(iterm_t *t, int i, int def) {
    if (i < 0 || i >= t->csi_n || !t->csi_set[i]) return def;
    return t->csi_p[i];
}

static int term_write(iterm_t *t, const char *data, int len);

static void reply(iterm_t *t, const char *s) {
    if (!s) return;
    term_write(t, s, (int)strlen(s));
}

static void set_mode(iterm_t *t, int priv, int code, int on) {
    if (!priv) {
        if (code == 4) t->insert_mode = on;
        return;
    }
    switch (code) {
        case 1: t->app_cursor = on; break;
        case 6:
            t->origin_mode = on;
            t->cx = 0;
            t->cy = on ? t->scroll_top : 0;
            t->pending_wrap = 0;
            break;
        case 7: t->wrap_mode = on; break;
        case 25: t->cursor_visible = on; break;
        case 47:
            if (on) enter_alt(t, 0);
            else leave_alt(t);
            break;
        case 1047:
            if (on) enter_alt(t, 1);
            else leave_alt(t);
            break;
        case 1048:
            if (on) save_cursor(t);
            else restore_cursor(t);
            break;
        case 1049:
            if (on) {
                save_cursor(t);
                enter_alt(t, 1);
            } else {
                leave_alt(t);
                restore_cursor(t);
            }
            break;
        case 1000: t->mouse_1000 = on; break;
        case 1002: t->mouse_1002 = on; if (on) t->mouse_1000 = 1; break;
        case 1006: t->mouse_1006 = on; break;
        case 1004: t->focus_events = on; break;
        case 2004: t->bracket_paste = on; break;
        default: break;
    }
}

static void sgr(iterm_t *t) {
    int i, n, p;
    n = t->csi_n;
    if (n <= 0) {
        t->pen.fg = 0;
        t->pen.bg = 0;
        t->pen.attr = 0;
        return;
    }
    i = 0;
    while (i < n) {
        p = csi_get(t, i, 0);
        if (p == 38 || p == 48) {
            int mode = csi_get(t, i + 1, 0);
            uint8_t *dest = (p == 38) ? &t->pen.fg : &t->pen.bg;
            if (mode == 5) {
                *dest = color_from_256(csi_get(t, i + 2, 0));
                i += 3;
                continue;
            }
            if (mode == 2) {
                *dest = nearest_ansi(csi_get(t, i + 2, 0), csi_get(t, i + 3, 0), csi_get(t, i + 4, 0));
                i += 5;
                continue;
            }
            i++;
            continue;
        }
        if (p == 0) {
            t->pen.fg = 0;
            t->pen.bg = 0;
            t->pen.attr = 0;
        } else if (p == 1) t->pen.attr |= ATTR_BOLD;
        else if (p == 2) t->pen.attr |= ATTR_DIM;
        else if (p == 3) t->pen.attr |= ATTR_ITALIC;
        else if (p == 4) t->pen.attr |= ATTR_UNDER;
        else if (p == 5) t->pen.attr |= ATTR_BLINK;
        else if (p == 7) t->pen.attr |= ATTR_REVERSE;
        else if (p == 8) t->pen.attr |= ATTR_INVIS;
        else if (p == 22) t->pen.attr &= (uint8_t)~(ATTR_BOLD | ATTR_DIM);
        else if (p == 23) t->pen.attr &= (uint8_t)~ATTR_ITALIC;
        else if (p == 24) t->pen.attr &= (uint8_t)~ATTR_UNDER;
        else if (p == 25) t->pen.attr &= (uint8_t)~ATTR_BLINK;
        else if (p == 27) t->pen.attr &= (uint8_t)~ATTR_REVERSE;
        else if (p == 28) t->pen.attr &= (uint8_t)~ATTR_INVIS;
        else if (p >= 30 && p <= 37) t->pen.fg = (uint8_t)(p - 29);
        else if (p == 39) t->pen.fg = 0;
        else if (p >= 40 && p <= 47) t->pen.bg = (uint8_t)(p - 39);
        else if (p == 49) t->pen.bg = 0;
        else if (p >= 90 && p <= 97) t->pen.fg = (uint8_t)(p - 81);
        else if (p >= 100 && p <= 107) t->pen.bg = (uint8_t)(p - 91);
        i++;
    }
}

static void osc_finish(iterm_t *t) {
    char *body;
    t->osc[t->osc_len < ITERM_OSC_MAX ? t->osc_len : ITERM_OSC_MAX - 1] = '\0';
    body = t->osc;
    if ((body[0] == '0' || body[0] == '2') && body[1] == ';' && !t->title_locked) {
        snprintf(t->title, sizeof(t->title), "%s", body + 2);
    } else if (body[0] == '7' && body[1] == ';') {
        char *p = strstr(body, "file://");
        if (p) {
            char *slash;
            p += 7;
            slash = strchr(p, '/');
            if (slash && strlen(slash) < sizeof(t->cwd)) {
                char *d, *s;
                d = t->cwd;
                for (s = slash; *s && (size_t)(d - t->cwd) < sizeof(t->cwd) - 1; s++) {
                    if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                        char hex[3];
                        hex[0] = s[1];
                        hex[1] = s[2];
                        hex[2] = '\0';
                        *d++ = (char)strtol(hex, NULL, 16);
                        s += 2;
                    } else {
                        *d++ = *s;
                    }
                }
                *d = '\0';
            }
        }
    }
    t->osc_len = 0;
}

static void csi_dispatch(iterm_t *t, unsigned char final) {
    int n, i;
    char buf[64];
    grid_t *g;
    g = ag(t);
    if (t->csi_inter == '!' && final == 'p') {
        term_reset_modes(t);
        return;
    }
    switch (final) {
        case 'A':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cy -= n;
            clamp_cursor(t);
            break;
        case 'B':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cy += n;
            clamp_cursor(t);
            break;
        case 'C':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cx += n;
            clamp_cursor(t);
            break;
        case 'D':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cx -= n;
            clamp_cursor(t);
            break;
        case 'E':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cy += n;
            t->cx = 0;
            clamp_cursor(t);
            break;
        case 'F':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cy -= n;
            t->cx = 0;
            clamp_cursor(t);
            break;
        case 'G':
        case '`':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cx = n - 1;
            clamp_cursor(t);
            break;
        case 'H':
        case 'f':
            move_to(t, csi_get(t, 0, 1), csi_get(t, 1, 1));
            break;
        case 'd':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            t->pending_wrap = 0;
            t->cy = cursor_row_base(t) + n - 1;
            clamp_cursor(t);
            break;
        case 'J':
            erase_display(t, csi_get(t, 0, 0));
            break;
        case 'K':
            erase_line(t, csi_get(t, 0, 0));
            break;
        case 'L':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            insert_lines(t, n);
            break;
        case 'M':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            delete_lines(t, n);
            break;
        case 'P':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            delete_cells(t, n);
            break;
        case '@':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            insert_cells(t, n);
            break;
        case 'X':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            clear_row_range(t, cur_row(t), t->cx, t->cx + n - 1);
            break;
        case 'S':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            scroll_up_n(t, n);
            break;
        case 'T':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            scroll_down_n(t, n);
            break;
        case 'm':
            sgr(t);
            break;
        case 'r':
            {
                int top = csi_get(t, 0, 1);
                int bot = csi_get(t, 1, g->nrows);
                if (top < 1) top = 1;
                if (bot > g->nrows) bot = g->nrows;
                if (bot > top) {
                    t->scroll_top = top - 1;
                    t->scroll_bot = bot - 1;
                } else if (g->nrows > 0) {
                    t->scroll_top = 0;
                    t->scroll_bot = g->nrows - 1;
                }
                t->cx = 0;
                t->cy = t->origin_mode ? t->scroll_top : 0;
                t->pending_wrap = 0;
            }
            break;
        case 'h':
        case 'l':
            n = t->csi_n > 0 ? t->csi_n : 1;
            for (i = 0; i < n; i++) set_mode(t, t->csi_priv == '?', csi_get(t, i, 0), final == 'h');
            break;
        case 'n':
            if (csi_get(t, 0, 0) == 6) {
                snprintf(buf, sizeof(buf), "\033[%d;%dR", t->cy + 1, t->cx + 1);
                reply(t, buf);
            } else if (csi_get(t, 0, 0) == 5) {
                reply(t, "\033[0n");
            }
            break;
        case 'c':
            if (t->csi_priv == '>') reply(t, "\033[>0;0;0c");
            else reply(t, "\033[?1;2c");
            break;
        case 's':
            save_cursor(t);
            break;
        case 'u':
            restore_cursor(t);
            break;
        case 'b':
            n = csi_get(t, 0, 1);
            if (n < 1) n = 1;
            if (n > 1000) n = 1000;
            for (i = 0; i < n && t->last_ch; i++) put_char(t, t->last_ch);
            break;
        default:
            break;
    }
}

static void feed_byte(iterm_t *t, unsigned char b);

static void exec_c0(iterm_t *t, unsigned char b) {
    switch (b) {
        case 0x07:
            break;
        case 0x08:
            backspace(t);
            break;
        case 0x09:
            tab_forward(t);
            break;
        case 0x0a:
        case 0x0b:
        case 0x0c:
            linefeed(t);
            break;
        case 0x0d:
            carriage_return(t);
            break;
        case 0x0e:
            t->using_g1 = 1;
            break;
        case 0x0f:
            t->using_g1 = 0;
            break;
        default:
            break;
    }
}

static void feed_byte(iterm_t *t, unsigned char b) {
    if (b == 0x18 || b == 0x1a) {
        t->state = ST_GROUND;
        return;
    }
    if (t->state == ST_UTF8) {
        if ((b & 0xc0) != 0x80) {
            t->state = ST_GROUND;
            put_char(t, 0xfffd);
            feed_byte(t, b);
            return;
        }
        if (t->ulen < (int)sizeof(t->ubuf) - 1) t->ubuf[t->ulen++] = (char)b;
        if (t->ulen >= t->uneed) {
            uint32_t cp = 0xfffd;
            t->ubuf[t->ulen] = '\0';
            utf8_char_to_unicode(&cp, t->ubuf, t->ubuf + t->ulen);
            t->state = ST_GROUND;
            if (cp == 0) cp = 0xfffd;
            put_char(t, cp);
        }
        return;
    }

    if (t->state == ST_GROUND) {
        if (b == 0x1b) {
            t->state = ST_ESC;
            return;
        }
        if (b < 0x20) {
            exec_c0(t, b);
            return;
        }
        if (b == 0x7f) return;
        if (b >= 0x80) {
            int need = tb_utf8_char_length((char)b);
            if (need < 2) {
                put_char(t, 0xfffd);
                return;
            }
            t->ubuf[0] = (char)b;
            t->ulen = 1;
            t->uneed = need;
            t->state = ST_UTF8;
            return;
        }
        if ((t->using_g1 ? t->g1_special : t->g0_special) && b >= 0x60 && b <= 0x7e) {
            put_char(t, dec_line(b));
        } else {
            put_char(t, b);
        }
        return;
    }

    if (t->state == ST_ESC) {
        t->state = ST_GROUND;
        switch (b) {
            case '[':
                csi_reset(t);
                t->state = ST_CSI;
                break;
            case ']':
                t->osc_len = 0;
                t->state = ST_OSC;
                break;
            case 'P':
            case 'X':
            case '^':
            case '_':
                t->state = ST_IGNORE;
                break;
            case '7':
                save_cursor(t);
                break;
            case '8':
                restore_cursor(t);
                break;
            case 'D':
                index_down(t);
                break;
            case 'M':
                index_up(t);
                break;
            case 'E':
                carriage_return(t);
                index_down(t);
                break;
            case 'c':
                term_full_reset(t);
                break;
            case '(':
                t->charset_g = 0;
                t->state = ST_CHARSET;
                break;
            case ')':
                t->charset_g = 1;
                t->state = ST_CHARSET;
                break;
            case '=':
            case '>':
                break;
            default:
                break;
        }
        return;
    }

    if (t->state == ST_CHARSET) {
        int special = (b == '0');
        if (t->charset_g == 0) t->g0_special = special;
        else t->g1_special = special;
        t->state = ST_GROUND;
        return;
    }

    if (t->state == ST_CSI) {
        if (b >= '0' && b <= '9') csi_digit(t, b - '0');
        else if (b == ';' || b == ':') csi_semi(t);
        else if ((b == '?' || b == '>' || b == '!' || b == '=') && t->csi_n == 0 && t->csi_priv == 0) t->csi_priv = b;
        else if (b >= 0x20 && b <= 0x2f) t->csi_inter = b;
        else if (b >= 0x40 && b <= 0x7e) {
            csi_dispatch(t, b);
            t->state = ST_GROUND;
        } else if (b == 0x1b) {
            t->state = ST_ESC;
        } else if (b < 0x20) {
            exec_c0(t, b);
        } else {
            t->state = ST_GROUND;
        }
        return;
    }

    if (t->state == ST_OSC) {
        if (b == 0x07) {
            osc_finish(t);
            t->state = ST_GROUND;
        } else if (b == 0x1b) {
            t->state = ST_OSC_ESC;
        } else if (t->osc_len < ITERM_OSC_MAX - 1) {
            t->osc[t->osc_len++] = (char)b;
        }
        return;
    }

    if (t->state == ST_OSC_ESC) {
        if (b == '\\') {
            osc_finish(t);
            t->state = ST_GROUND;
        } else {
            if (t->osc_len < ITERM_OSC_MAX - 1) t->osc[t->osc_len++] = 0x1b;
            t->state = ST_OSC;
            feed_byte(t, b);
        }
        return;
    }

    if (t->state == ST_IGNORE) {
        if (b == 0x1b) t->state = ST_IGNORE_ESC;
        else if (b == 0x07) t->state = ST_GROUND;
        return;
    }

    if (t->state == ST_IGNORE_ESC) {
        t->state = (b == '\\') ? ST_GROUND : ST_IGNORE;
        return;
    }
}

static void term_feed(iterm_t *t, const char *data, int len) {
    int i;
    for (i = 0; i < len; i++) feed_byte(t, (unsigned char)data[i]);
}

static int lline_add(lline_t *L, cell_t c) {
    if (L->len >= L->cap) {
        int nc = L->cap ? L->cap * 2 : 32;
        cell_t *p = realloc(L->cells, (size_t)nc * sizeof(cell_t));
        if (!p) return -1;
        L->cells = p;
        L->cap = nc;
    }
    L->cells[L->len++] = c;
    return 0;
}

static int row_is_blank(row_t *row) {
    int x;
    if (!row || !row->cells) return 1;
    for (x = 0; x < row->cols; x++) {
        if (row->cells[x].w == CELL_CHAR || row->cells[x].w == CELL_WIDE) return 0;
    }
    return 1;
}

static void snapshot_row(lline_t **lines, int *nlines, int *cap, int *closed, row_t *row, int is_cur, int cx, int pending, int *cur_line, int *cur_off) {
    lline_t *L;
    int x, glyphs_before;
    if (!row) return;
    if (*closed || *nlines == 0) {
        if (*nlines >= *cap) {
            int nc = *cap ? *cap * 2 : 16;
            lline_t *p = realloc(*lines, (size_t)nc * sizeof(lline_t));
            if (!p) return;
            *lines = p;
            *cap = nc;
        }
        memset(&(*lines)[*nlines], 0, sizeof(lline_t));
        (*nlines)++;
        *closed = 0;
    }
    L = &(*lines)[*nlines - 1];
    glyphs_before = L->len;
    for (x = 0; x < row->cols; x++) {
        cell_t c = row->cells[x];
        if (is_cur && !pending && x == cx) {
            *cur_line = *nlines - 1;
            *cur_off = glyphs_before + (L->len - glyphs_before);
        }
        if (c.w != CELL_CHAR && c.w != CELL_WIDE) continue;
        if (lline_add(L, c) != 0) return;
    }
    if (is_cur && pending) {
        *cur_line = *nlines - 1;
        *cur_off = L->len;
    }
    if (!row->wrapped) *closed = 1;
}

static void primary_reflow(iterm_t *t, int cols, int rows) {
    lline_t *lines = NULL;
    int nlines = 0, cap = 0, closed = 1;
    int cur_line = 0, cur_off = 0, i, g;
    int was_alt, save_cx, save_cy;
    pen_t save_pen;

    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > ITERM_MAX_DIM) cols = ITERM_MAX_DIM;
    if (rows > ITERM_MAX_DIM) rows = ITERM_MAX_DIM;

    if (!t->primary.rows) {
        grid_init(&t->primary, cols, rows);
        grid_init(&t->alt, cols, rows);
        t->scroll_top = 0;
        t->scroll_bot = rows - 1;
        return;
    }

    was_alt = t->alt_on;
    save_cx = t->cx;
    save_cy = t->cy;
    save_pen = t->pen;

    if (!was_alt) {
        int last, r, si;
        last = t->cy;
        for (r = t->primary.nrows - 1; r >= t->cy; r--) {
            if (!row_is_blank(&t->primary.rows[r])) {
                last = r;
                break;
            }
        }
        for (si = 0; si < t->sb_count; si++) {
            snapshot_row(&lines, &nlines, &cap, &closed, sb_at(t, si), 0, 0, 0, &cur_line, &cur_off);
        }
        for (r = 0; r <= last && r < t->primary.nrows; r++) {
            snapshot_row(&lines, &nlines, &cap, &closed, &t->primary.rows[r], r == t->cy, t->cx, t->pending_wrap && r == t->cy, &cur_line, &cur_off);
        }
    }

    sb_free_all(t);
    grid_free(&t->primary);
    grid_init(&t->primary, cols, rows);
    grid_resize_geom(&t->alt, cols, rows);
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    t->cx = 0;
    t->cy = 0;
    t->pending_wrap = 0;
    t->alt_on = 0;
    t->pen = save_pen;

    if (!was_alt) {
        int marked = 0;
        for (i = 0; i < nlines; i++) {
            if (i == cur_line && cur_off == 0) {
                marked = 1;
            }
            for (g = 0; g < lines[i].len; g++) {
                cell_t c = lines[i].cells[g];
                t->pen.fg = c.fg;
                t->pen.bg = c.bg;
                t->pen.attr = c.attr;
                put_char(t, c.ch ? c.ch : ' ');
                if (i == cur_line && g + 1 == cur_off) marked = 1;
            }
            if (i + 1 < nlines) {
                carriage_return(t);
                linefeed(t);
            }
            if (i == cur_line && !marked && cur_off >= lines[i].len) marked = 1;
        }
        t->pen = save_pen;
        if (nlines == 0) {
            t->cx = 0;
            t->cy = 0;
        }
        (void)marked;
    }

    for (i = 0; i < nlines; i++) free(lines[i].cells);
    free(lines);

    t->pri_cx = t->cx;
    t->pri_cy = t->cy;
    t->pri_pending = t->pending_wrap;
    if (was_alt) {
        t->alt_on = 1;
        t->cx = clampi(save_cx, 0, t->alt.ncols - 1);
        t->cy = clampi(save_cy, 0, t->alt.nrows - 1);
        t->pending_wrap = 0;
        t->scroll_top = 0;
        t->scroll_bot = t->alt.nrows - 1;
    }
    clamp_cursor(t);
}

static int term_set_size(iterm_t *t, int cols, int rows) {
    struct winsize ws;
    if (!t) return 0;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > ITERM_MAX_DIM) cols = ITERM_MAX_DIM;
    if (rows > ITERM_MAX_DIM) rows = ITERM_MAX_DIM;
    if (t->primary.rows && t->primary.ncols == cols && t->primary.nrows == rows
        && t->alt.ncols == cols && t->alt.nrows == rows) {
        return 0;
    }
    primary_reflow(t, cols, rows);
    if (t->fd >= 0 && t->alive) {
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)t->primary.ncols;
        ws.ws_row = (unsigned short)t->primary.nrows;
        ioctl(t->fd, TIOCSWINSZ, &ws);
        if (t->pid > 0) kill(-t->pid, SIGWINCH);
    }
    return 1;
}

static void term_hangup(iterm_t *t) {
    int st;
    pid_t r;
    if (!t) return;
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    if (t->pid > 0) {
        r = waitpid(t->pid, &st, WNOHANG);
        if (r == t->pid) {
            t->alive = 0;
            t->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        } else if (r == 0) {
            kill(-t->pid, SIGHUP);
            t->alive = 0;
        } else {
            t->alive = 0;
        }
    } else {
        t->alive = 0;
    }
}

static void term_consume(iterm_t *t) {
    char buf[65536];
    if (!t || t->fd < 0) return;
    for (;;) {
        ssize_t n = read(t->fd, buf, sizeof(buf));
        if (n > 0) {
            term_feed(t, buf, (int)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        term_hangup(t);
        break;
    }
}

static int term_write(iterm_t *t, const char *data, int len) {
    int off = 0;
    if (!t || t->fd < 0 || !t->alive || !data || len <= 0) return -1;
    while (off < len) {
        ssize_t n = write(t->fd, data + off, (size_t)(len - off));
        if (n > 0) {
            off += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = t->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 20) <= 0) break;
            continue;
        }
        break;
    }
    return off;
}

static void term_free(iterm_t *t) {
    if (!t) return;
    if (t->alive && t->pid > 0) {
        kill(-t->pid, SIGHUP);
        kill(-t->pid, SIGTERM);
    }
    if (t->fd >= 0) close(t->fd);
    if (t->pid > 0) {
        int st;
        waitpid(t->pid, &st, WNOHANG);
    }
    t->fd = -1;
    t->alive = 0;
    sb_free_all(t);
    grid_free(&t->primary);
    grid_free(&t->alt);
    free(t);
}

static const char *user_shell(void) {
    const char *s = getenv("SHELL");
    if (s && s[0] && access(s, X_OK) == 0) return s;
    if (access("/bin/bash", X_OK) == 0) return "/bin/bash";
    return "/bin/sh";
}

static void editor_cwd(editor_t *editor, char *out, size_t cap) {
    out[0] = '\0';
    if (editor && editor->active_edit && editor->active_edit->buffer && editor->active_edit->buffer->path) {
        snprintf(out, cap, "%s", editor->active_edit->buffer->path);
        char *slash = strrchr(out, '/');
        if (slash) {
            if (slash == out) slash[1] = '\0';
            else *slash = '\0';
            if (util_is_dir(out)) return;
        }
        out[0] = '\0';
    }
    if (!getcwd(out, cap)) out[0] = '\0';
}

static int term_spawn(iterm_t *t, const char *cwd, const char *shell, const char *cmdline) {
    int master;
    char name[64];
    pid_t pid;
    struct winsize ws;
    const char *base;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    if (ptsname_r(master, name, sizeof(name)) != 0) {
        close(master);
        return -1;
    }
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(t->primary.ncols > 0 ? t->primary.ncols : 80);
    ws.ws_row = (unsigned short)(t->primary.nrows > 0 ? t->primary.nrows : 24);
    ioctl(master, TIOCSWINSZ, &ws);

    pid = fork();
    if (pid < 0) {
        close(master);
        return -1;
    }
    if (pid == 0) {
        int slave, maxfd, i;
        setsid();
        slave = open(name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        ioctl(slave, TIOCSWINSZ, &ws);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        maxfd = (int)sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 1024) maxfd = 1024;
        for (i = 3; i < maxfd; i++) close(i);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCONT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGWINCH, SIG_DFL);
        unsetenv("LINES");
        unsetenv("COLUMNS");
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("MLE_ITERM", "1", 1);
        if (cwd && cwd[0]) chdir(cwd);
        if (cmdline && cmdline[0]) {
            execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        } else {
            base = strrchr(shell ? shell : "/bin/sh", '/');
            base = base ? base + 1 : (shell ? shell : "sh");
            execl(shell ? shell : "/bin/sh", base, (char *)NULL);
        }
        _exit(127);
    }

    fcntl(master, F_SETFL, O_NONBLOCK);
    t->fd = master;
    t->pid = pid;
    t->alive = 1;
    t->exit_code = 0;
    return 0;
}

static iterm_t *term_new(int cols, int rows) {
    iterm_t *t;
    t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fd = -1;
    t->close_x = -1;
    t->cursor_visible = 1;
    t->wrap_mode = 1;
    t->last_width = 1;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    grid_init(&t->primary, cols, rows);
    grid_init(&t->alt, cols, rows);
    t->scroll_top = 0;
    t->scroll_bot = t->primary.nrows - 1;
    snprintf(t->title, sizeof(t->title), "shell");
    return t;
}

static void dump_row(row_t *row, char *dst, int cap, int *used) {
    char tmp[8];
    int x, end, n, k;
    if (!row || !row->cells) {
        return;
    }
    end = row->cols - 1;
    while (end >= 0) {
        cell_t c = row->cells[end];
        if (c.w == CELL_CHAR || c.w == CELL_WIDE) {
            if (c.ch != 0 && c.ch != ' ') break;
        }
        end--;
    }
    for (x = 0; x <= end; x++) {
        cell_t c = row->cells[x];
        uint32_t ch;
        if (c.w == CELL_CONT || c.w == CELL_EMPTY) continue;
        ch = c.ch ? c.ch : ' ';
        n = tb_utf8_unicode_to_char(tmp, ch);
        if (n < 1) continue;
        for (k = 0; k < n && *used < cap - 1; k++) dst[(*used)++] = tmp[k];
    }
}

static void term_dump(iterm_t *t, char *out, int cap) {
    grid_t *g;
    int r, used;
    if (!out || cap < 1) return;
    out[0] = '\0';
    g = ag(t);
    used = 0;
    for (r = 0; r < g->nrows; r++) {
        if (r > 0 && used < cap - 1) out[used++] = '\n';
        dump_row(&g->rows[r], out, cap, &used);
    }
    out[used] = '\0';
}

static int hist_count(iterm_t *t) {
    grid_t *g;
    if (!t) return 0;
    g = ag(t);
    if (t->alt_on) return g->nrows;
    return t->sb_count + g->nrows;
}

static row_t *hist_row(iterm_t *t, int idx) {
    grid_t *g;
    g = ag(t);
    if (t->alt_on) {
        if (idx < 0 || idx >= g->nrows) return NULL;
        return &g->rows[idx];
    }
    if (idx < t->sb_count) return sb_at(t, idx);
    idx -= t->sb_count;
    if (idx < 0 || idx >= g->nrows) return NULL;
    return &g->rows[idx];
}

static int iview_is_leaf(iview_t *v) {
    return v && !v->split;
}

static iview_t *first_leaf(iview_t *n) {
    while (n && n->split) n = n->a;
    return n;
}

static void collect_leaves(iview_t *n, iview_t **arr, int *count, int cap) {
    if (!n || *count >= cap) return;
    if (!n->split) {
        arr[(*count)++] = n;
        return;
    }
    collect_leaves(n->a, arr, count, cap);
    collect_leaves(n->b, arr, count, cap);
}

static iview_t *leaf_at(iview_t *n, int x, int y) {
    iview_t *hit;
    if (!n || n->w <= 0 || n->h <= 0) return NULL;
    if (n->split) {
        hit = leaf_at(n->a, x, y);
        if (hit) return hit;
        return leaf_at(n->b, x, y);
    }
    if (x >= n->x && y >= n->y && x < n->x + n->w && y < n->y + n->h) return n;
    return NULL;
}

static iview_t *split_divider_at(iview_t *n, int x, int y) {
    iview_t *hit;
    if (!n || !n->split || !n->a || !n->b) return NULL;
    hit = split_divider_at(n->a, x, y);
    if (hit) return hit;
    hit = split_divider_at(n->b, x, y);
    if (hit) return hit;
    if (n->vertical) {
        int dx = n->a->x + n->a->w;
        if (n->a->w > 0 && x == dx && y >= n->y && y < n->y + n->h) return n;
    } else {
        int dy = n->a->y + n->a->h;
        if (n->a->h > 0 && y == dy && x >= n->x && x < n->x + n->w) return n;
    }
    return NULL;
}

static iview_t *leaf_showing(iview_t *n, iterm_t *term) {
    iview_t *hit;
    if (!n || !term) return NULL;
    if (!n->split) return n->term == term ? n : NULL;
    hit = leaf_showing(n->a, term);
    if (hit) return hit;
    return leaf_showing(n->b, term);
}

static void layout_view(iview_t *n, int x, int y, int w, int h) {
    int left, top;
    if (!n) return;
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
    if (!n->split || !n->a || !n->b) return;
    if (n->vertical && w >= 5) {
        left = (int)((float)(w - 1) * n->factor);
        left = clampi(left, 2, w - 3);
        layout_view(n->a, x, y, left, h);
        layout_view(n->b, x + left + 1, y, w - left - 1, h);
    } else if (!n->vertical && h >= 3) {
        top = (int)((float)(h - 1) * n->factor);
        top = clampi(top, 1, h - 2);
        layout_view(n->a, x, y, w, top);
        layout_view(n->b, x, y + top + 1, w, h - top - 1);
    } else {
        layout_view(n->a, x, y, w, h);
        n->b->x = x;
        n->b->y = y;
        n->b->w = 0;
        n->b->h = 0;
    }
}

static void sync_leaf_sizes(iview_t *n) {
    int changed;
    if (!n) return;
    if (n->split) {
        sync_leaf_sizes(n->a);
        sync_leaf_sizes(n->b);
        return;
    }
    if (!n->term || n->w < 1 || n->h < 1) return;
    changed = term_set_size(n->term, n->w, n->h);
    if (changed) {
        n->scroll = 0;
        n->copy_mode = 0;
        n->sel_on = 0;
    }
}

static iview_t *make_leaf(iterm_t *term) {
    iview_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->term = term;
    v->factor = 0.5f;
    return v;
}

static void close_leaf(iterm_panel_t *panel, iview_t *leaf) {
    iview_t *parent, *sib, *gp;
    if (!leaf) return;
    parent = leaf->parent;
    if (!parent) {
        leaf->term = NULL;
        return;
    }
    sib = (parent->a == leaf) ? parent->b : parent->a;
    gp = parent->parent;
    if (sib) sib->parent = gp;
    if (!gp) panel->root = sib;
    else if (gp->a == parent) gp->a = sib;
    else gp->b = sib;
    if (panel->focus == leaf || panel->focus == parent) panel->focus = first_leaf(sib ? sib : panel->root);
    free(leaf);
    free(parent);
}

static void split_leaf(iterm_panel_t *panel, iview_t *leaf, int vertical, iterm_t *term) {
    iview_t *gp, *sp, *right;
    int was_a;
    if (!panel || !leaf) return;
    gp = leaf->parent;
    was_a = gp && gp->a == leaf;
    sp = calloc(1, sizeof(*sp));
    right = make_leaf(term);
    if (!sp || !right) {
        free(sp);
        free(right);
        return;
    }
    sp->split = 1;
    sp->vertical = vertical;
    sp->factor = 0.5f;
    sp->parent = gp;
    sp->a = leaf;
    sp->b = right;
    leaf->parent = sp;
    right->parent = sp;
    if (!gp) panel->root = sp;
    else if (was_a) gp->a = sp;
    else gp->b = sp;
    panel->focus = right;
}

static iterm_t *focused_term(iterm_panel_t *panel) {
    if (!panel || !panel->focus) return NULL;
    return panel->focus->term;
}

static void send_focus(iterm_t *t, int in) {
    if (!t || !t->alive || !t->focus_events || t->fd < 0) return;
    term_write(t, in ? "\033[I" : "\033[O", 3);
}

static void set_focused(iterm_panel_t *panel, int focused) {
    iterm_t *t;
    if (!panel) return;
    if (panel->focused == focused) return;
    t = focused_term(panel);
    panel->focused = focused;
    if (!focused) panel->prefix = 0;
    send_focus(t, focused);
}

static int term_count(iterm_panel_t *panel) {
    iterm_t *t;
    int n = 0;
    if (!panel) return 0;
    DL_FOREACH(panel->terms, t) n++;
    return n;
}

static int term_alive_count(iterm_panel_t *panel) {
    iterm_t *t;
    int n = 0;
    if (!panel) return 0;
    DL_FOREACH(panel->terms, t) if (t->alive) n++;
    return n;
}

static const char *dock_name(int dock) {
    switch (dock) {
        case DOCK_TOP: return "top";
        case DOCK_LEFT: return "left";
        case DOCK_RIGHT: return "right";
        case DOCK_MAX: return "max";
        default: return "bottom";
    }
}

static void draw_cell_at(int x, int y, cell_t *c, int selected, editor_t *editor) {
    uint32_t ch;
    uintattr_t fg, bg;
    if (c->attr & ATTR_INVIS) ch = ' ';
    else ch = c->ch ? c->ch : ' ';
    fg = tb_fg(c);
    bg = tb_bg(c);
    if (selected) {
        uint16_t sfg = TB_BLACK, sbg = TB_CYAN;
        if (editor) theme_selection(editor, &sfg, &sbg);
        fg = sfg;
        bg = sbg;
    }
    tb_set_cell(x, y, ch, fg, bg);
}

static int cell_selected(iview_t *v, int hist_y, int x) {
    int y1, x1, y2, x2;
    if (!v || !v->sel_on) return 0;
    y1 = v->sel_ay;
    x1 = v->sel_ax;
    y2 = v->copy_y;
    x2 = v->copy_x;
    if (y1 > y2 || (y1 == y2 && x1 > x2)) {
        int ty = y1, tx = x1;
        y1 = y2;
        x1 = x2;
        y2 = ty;
        x2 = tx;
    }
    if (hist_y < y1 || hist_y > y2) return 0;
    if (y1 == y2) return x >= x1 && x <= x2;
    if (hist_y == y1) return x >= x1;
    if (hist_y == y2) return x <= x2;
    return 1;
}

static int max_scroll(iview_t *v) {
    int total, page;
    if (!v || !v->term) return 0;
    total = hist_count(v->term);
    page = v->h;
    if (total <= page) return 0;
    return total - page;
}

static void clamp_scroll(iview_t *v) {
    if (!v) return;
    if (v->scroll < 0) v->scroll = 0;
    if (v->scroll > max_scroll(v)) v->scroll = max_scroll(v);
}

static void reveal_copy(iview_t *v) {
    int total, page, top, bot;
    if (!v || !v->term) return;
    clamp_scroll(v);
    total = hist_count(v->term);
    page = v->h > 0 ? v->h : 1;
    bot = total - v->scroll;
    top = bot - page;
    if (v->copy_y < top) v->scroll += (top - v->copy_y);
    if (v->copy_y >= bot) v->scroll -= (v->copy_y - bot + 1);
    clamp_scroll(v);
}

static void draw_view(iterm_panel_t *panel, iview_t *v) {
    iterm_t *t;
    int row, total, top, x;
    if (!v) return;
    if (v->split) {
        draw_view(panel, v->a);
        draw_view(panel, v->b);
        if (v->vertical && v->a && v->a->w > 0 && v->b && v->b->w > 0) {
            int dx = v->a->x + v->a->w;
            int y;
            for (y = v->y; y < v->y + v->h; y++) {
                tb_set_cell(dx, y, 0x2502, TB_BLACK | TB_BRIGHT, 0);
            }
        } else if (!v->vertical && v->a && v->a->h > 0 && v->b && v->b->h > 0) {
            int dy = v->a->y + v->a->h;
            int xx;
            for (xx = v->x; xx < v->x + v->w; xx++) {
                tb_set_cell(xx, dy, 0x2500, TB_BLACK | TB_BRIGHT, 0);
            }
        }
        return;
    }
    t = v->term;
    if (!t || v->w < 1 || v->h < 1) return;
    clamp_scroll(v);
    total = hist_count(t);
    top = total - v->scroll - v->h;
    if (top < 0) top = 0;
    for (row = 0; row < v->h; row++) {
        row_t *hr = hist_row(t, top + row);
        int cols = hr ? hr->cols : 0;
        for (x = 0; x < v->w; x++) {
            cell_t blank;
            cell_t *c;
            int sel;
            memset(&blank, 0, sizeof(blank));
            c = (hr && x < cols) ? &hr->cells[x] : &blank;
            if (c->w == CELL_CONT) {
                tb_set_cell(v->x + x, v->y + row, ' ', tb_fg(c), tb_bg(c));
                continue;
            }
            sel = cell_selected(v, top + row, x);
            draw_cell_at(v->x + x, v->y + row, c, sel, panel->editor);
        }
    }
    if (panel->focused && panel->focus == v) {
        int cx = -1, cy = -1;
        if (v->copy_mode) {
            int vis = v->copy_y - top;
            if (vis >= 0 && vis < v->h) {
                cx = v->x + clampi(v->copy_x, 0, v->w - 1);
                cy = v->y + vis;
            }
        } else if (t->cursor_visible && v->scroll == 0 && !t->alt_on) {
            /* Live cursor sits on the screen, which starts at hist index sb_count.
             * With scroll 0, top == sb_count when the page equals the screen. */
            int screen_top = t->alt_on ? 0 : t->sb_count;
            int vis = (screen_top + t->cy) - top;
            if (vis >= 0 && vis < v->h && t->cx >= 0 && t->cx < v->w) {
                cx = v->x + t->cx;
                cy = v->y + vis;
            }
        } else if (t->cursor_visible && v->scroll == 0 && t->alt_on) {
            if (t->cy >= 0 && t->cy < v->h && t->cx >= 0 && t->cx < v->w) {
                cx = v->x + t->cx;
                cy = v->y + t->cy;
            }
        }
        if (cx >= 0) tb_set_cursor(cx, cy);
        else tb_set_cursor(-1, -1);
    }
}

static void tab_box(iterm_panel_t *panel, int *x, int *y, int *w) {
    int inset = panel->grip == 4 ? 1 : 0;
    int trim = (panel->grip == 3 || panel->grip == 4) ? 1 : 0;
    *x = panel->x + inset;
    *y = panel->y + (panel->grip == 1 ? 1 : 0);
    *w = panel->w - trim;
    if (*w < 0) *w = 0;
}

static int hit_grip(iterm_panel_t *panel, int x, int y) {
    if (!panel || panel->grip == 0) return 0;
    if (x < panel->x || y < panel->y || x >= panel->x + panel->w || y >= panel->y + panel->h) return 0;
    if (panel->grip == 1) return y == panel->y;
    if (panel->grip == 2) return y == panel->y + panel->h - 1;
    if (panel->grip == 4) return x == panel->x;
    if (panel->grip == 3) return x == panel->x + panel->w - 1;
    return 0;
}

static void draw_grip(iterm_panel_t *panel) {
    mle_theme_t *th;
    uintattr_t fg, bg;
    int i;
    if (!panel || panel->grip == 0) return;
    th = theme_current(panel->editor);
    fg = th->grip_fg;
    bg = th->grip_bg;
    if (panel->grip == 1 || panel->grip == 2) {
        int y = panel->grip == 1 ? panel->y : panel->y + panel->h - 1;
        for (i = 0; i < panel->w; i++) tb_set_cell(panel->x + i, y, ' ', fg, bg);
        if (panel->w >= 12) {
            const char *s = " drag ";
            int cx = panel->x + (panel->w / 2) - 3;
            for (i = 0; s[i]; i++) tb_set_cell(cx + i, y, (uint32_t)s[i], fg, bg);
        }
    } else {
        int x = panel->grip == 4 ? panel->x : panel->x + panel->w - 1;
        for (i = 0; i < panel->h; i++) tb_set_cell(x, panel->y + i, ' ', fg, bg);
    }
}

static void draw_tabs(iterm_panel_t *panel) {
    iterm_t *t;
    int x0, y, tw, x, badge_w, limit, plus_reserve, px;
    char badge[32];
    uintattr_t bar_fg, bar_bg, act_fg, act_bg, plus_fg, plus_bg, xfg, xbg;
    mle_theme_t *th;
    th = theme_current(panel->editor);
    tab_box(panel, &x0, &y, &tw);
    panel->tab_y = y;
    panel->plus_x = -1;
    panel->plus_w = 0;
    DL_FOREACH(panel->terms, t) t->close_x = -1;
    if (panel->tabh < 1 || tw < 1) return;
    bar_fg = th->muted_fg;
    bar_bg = th->muted_bg;
    act_fg = th->accent_fg;
    act_bg = th->accent_bg;
    if (!th->user && th->text_fg == 0 && th->text_bg == 0) {
        bar_fg = TB_WHITE;
        bar_bg = TB_BLACK | TB_BRIGHT;
        act_fg = TB_BLACK | TB_BOLD;
        act_bg = TB_CYAN;
    }
    plus_fg = th->accent_fg;
    plus_bg = th->accent_bg;
    xfg = th->err_fg;
    xbg = th->err_bg;
    for (x = 0; x < tw; x++) tb_set_cell(x0 + x, y, ' ', bar_fg, bar_bg);
    badge[0] = '\0';
    if (tw >= 28) {
        if (panel->focus && panel->focus->copy_mode) snprintf(badge, sizeof(badge), " COPY ");
        else snprintf(badge, sizeof(badge), " %s ", dock_name(panel->dock));
    }
    badge_w = (int)strlen(badge);
    if (badge_w >= tw) badge_w = 0;
    limit = tw - badge_w;
    plus_reserve = limit >= 8 ? 3 : 0;
    limit -= plus_reserve;
    x = 0;
    DL_FOREACH(panel->terms, t) {
        char label[40];
        int labw, k;
        uintattr_t fg, bg;
        const char *title = t->title[0] ? t->title : "shell";
        snprintf(label, sizeof(label), " %d:%.12s ", t->id, title);
        labw = (int)strlen(label);
        if (x + labw + 1 > limit) break;
        fg = (panel->focus && panel->focus->term == t) ? act_fg : bar_fg;
        bg = (panel->focus && panel->focus->term == t) ? act_bg : bar_bg;
        for (k = 0; k < labw; k++) tb_set_cell(x0 + x + k, y, (uint32_t)label[k], fg, bg);
        t->tab_x = x0 + x;
        t->tab_w = labw;
        tb_set_cell(x0 + x + labw, y, glyphs_decor() ? glyphs_cp(GLY_CLOSE) : (uint32_t)'x', xfg, xbg);
        t->close_x = x0 + x + labw;
        x += labw + 1;
    }
    if (plus_reserve) {
        px = tw - badge_w - 3;
        if (px >= 0 && px + 3 <= tw - badge_w) {
            tb_set_cell(x0 + px, y, ' ', plus_fg, plus_bg);
            tb_set_cell(x0 + px + 1, y, glyphs_decor() ? glyphs_cp(GLY_PLUS) : (uint32_t)'+', plus_fg, plus_bg);
            tb_set_cell(x0 + px + 2, y, ' ', plus_fg, plus_bg);
            panel->plus_x = x0 + px;
            panel->plus_w = 3;
        }
    }
    if (badge_w > 0 && badge_w <= tw) {
        tb_printf(x0 + tw - badge_w, y, th->bar_fg, th->bar_bg, "%s", badge);
    }
}

static void clear_selection(iterm_panel_t *panel) {
    iview_t *leaves[64];
    int n = 0, i;
    collect_leaves(panel->root, leaves, &n, 64);
    for (i = 0; i < n; i++) leaves[i]->sel_on = 0;
    panel->sel_view = NULL;
}

static int view_top(iview_t *v) {
    int total, top;
    if (!v || !v->term) return 0;
    clamp_scroll(v);
    total = hist_count(v->term);
    top = total - v->scroll - v->h;
    if (top < 0) top = 0;
    return top;
}

static void yank_view(iterm_panel_t *panel, iview_t *v) {
    str_t text = {0};
    int y1, x1, y2, x2, y;
    editor_t *editor;
    if (!panel || !v || !v->term || !v->sel_on) return;
    y1 = v->sel_ay;
    x1 = v->sel_ax;
    y2 = v->copy_y;
    x2 = v->copy_x;
    if (y1 > y2 || (y1 == y2 && x1 > x2)) {
        int ty = y1, tx = x1;
        y1 = y2;
        x1 = x2;
        y2 = ty;
        x2 = tx;
    }
    for (y = y1; y <= y2; y++) {
        row_t *row = hist_row(v->term, y);
        int from = 0, to, xx;
        char tmp[8];
        if (!row) continue;
        to = row->cols - 1;
        if (y == y1) from = x1;
        if (y == y2) to = x2;
        if (from < 0) from = 0;
        if (to >= row->cols) to = row->cols - 1;
        while (to > from) {
            cell_t c = row->cells[to];
            if ((c.w == CELL_CHAR || c.w == CELL_WIDE) && c.ch && c.ch != ' ') break;
            to--;
        }
        for (xx = from; xx <= to; xx++) {
            cell_t c = row->cells[xx];
            int n, k;
            uint32_t ch;
            if (c.w == CELL_CONT || c.w == CELL_EMPTY) continue;
            ch = c.ch ? c.ch : ' ';
            n = tb_utf8_unicode_to_char(tmp, ch);
            for (k = 0; k < n; k++) str_append_char(&text, tmp[k]);
        }
        if (y != y2) str_append_char(&text, '\n');
    }
    if (!text.data) return;
    editor = panel->editor;
    if (editor->cut_buffer) free(editor->cut_buffer);
    editor->cut_buffer = text.data;
    if (tb_width() >= 0) util_clipboard_set(text.data, text.len);
    if (editor->active && editor->active->active_cursor) {
        cursor_t *c = editor->active->active_cursor;
        if (c->cut_buffer) free(c->cut_buffer);
        c->cut_buffer = strdup(text.data);
    }
    MLE_SET_INFO(editor, "terminal: copied %d bytes", (int)text.len);
}

static void enter_copy(iview_t *v) {
    iterm_t *t;
    if (!v || !v->term) return;
    t = v->term;
    v->copy_mode = 1;
    v->copy_x = t->cx;
    if (t->alt_on) v->copy_y = t->cy;
    else v->copy_y = t->sb_count + t->cy;
    if (v->copy_y < 0) v->copy_y = 0;
    if (v->copy_y >= hist_count(t)) v->copy_y = hist_count(t) - 1;
    reveal_copy(v);
}

static void move_copy(iview_t *v, int dx, int dy, int extend) {
    int total;
    if (!v || !v->term) return;
    if (extend && !v->sel_on) {
        v->sel_on = 1;
        v->sel_ax = v->copy_x;
        v->sel_ay = v->copy_y;
    }
    if (!extend) v->sel_on = 0;
    total = hist_count(v->term);
    v->copy_x = clampi(v->copy_x + dx, 0, v->w > 0 ? v->w - 1 : 0);
    v->copy_y = clampi(v->copy_y + dy, 0, total > 0 ? total - 1 : 0);
    reveal_copy(v);
}

static int copy_key(iterm_panel_t *panel, iview_t *v, struct tb_event *ev) {
    int page, extend;
    if (!v) return 1;
    page = v->h > 1 ? v->h - 1 : 1;
    extend = (ev->mod & TB_MOD_SHIFT) ? 1 : 0;
    if (ev->key == TB_KEY_ESC || ev->ch == 'q') {
        v->copy_mode = 0;
        v->sel_on = 0;
        return 1;
    }
    if (ev->key == TB_KEY_ENTER || ev->ch == 'y' || ev->ch == 'Y') {
        if (v->sel_on) yank_view(panel, v);
        if (ev->ch != 'y') v->copy_mode = 0;
        return 1;
    }
    if (ev->ch == 'v' || ev->ch == ' ') {
        if (!v->sel_on) {
            v->sel_on = 1;
            v->sel_ax = v->copy_x;
            v->sel_ay = v->copy_y;
        } else {
            v->sel_on = 0;
        }
        return 1;
    }
    if (ev->key == TB_KEY_ARROW_LEFT || ev->ch == 'h') move_copy(v, -1, 0, extend);
    else if (ev->key == TB_KEY_ARROW_RIGHT || ev->ch == 'l') move_copy(v, 1, 0, extend);
    else if (ev->key == TB_KEY_ARROW_UP || ev->ch == 'k') move_copy(v, 0, -1, extend);
    else if (ev->key == TB_KEY_ARROW_DOWN || ev->ch == 'j') move_copy(v, 0, 1, extend);
    else if (ev->key == TB_KEY_PGUP || ev->ch == 'u') move_copy(v, 0, -page, extend);
    else if (ev->key == TB_KEY_PGDN || ev->ch == 'd') move_copy(v, 0, page, extend);
    else if (ev->key == TB_KEY_HOME || ev->ch == 'g') {
        if (extend && !v->sel_on) {
            v->sel_on = 1;
            v->sel_ax = v->copy_x;
            v->sel_ay = v->copy_y;
        }
        v->copy_x = 0;
        v->copy_y = 0;
        reveal_copy(v);
    } else if (ev->key == TB_KEY_END || ev->ch == 'G') {
        if (extend && !v->sel_on) {
            v->sel_on = 1;
            v->sel_ax = v->copy_x;
            v->sel_ay = v->copy_y;
        }
        v->copy_y = hist_count(v->term) - 1;
        if (v->copy_y < 0) v->copy_y = 0;
        reveal_copy(v);
    } else {
        MLE_SET_INFO(panel->editor, "%s", "copy: arrows move, v select, y yank, q leave");
    }
    return 1;
}

static int encode_key(iterm_t *t, struct tb_event *ev, char *out, int cap) {
    int modn, n = 0;
    char tmp[64];
    modn = 1;
    if (ev->mod & TB_MOD_SHIFT) modn += 1;
    if (ev->mod & TB_MOD_ALT) modn += 2;
    if (ev->mod & TB_MOD_CTRL) modn += 4;

    if (ev->key == TB_KEY_ARROW_UP || ev->key == TB_KEY_ARROW_DOWN || ev->key == TB_KEY_ARROW_LEFT || ev->key == TB_KEY_ARROW_RIGHT) {
        char dir = 'A';
        if (ev->key == TB_KEY_ARROW_DOWN) dir = 'B';
        else if (ev->key == TB_KEY_ARROW_RIGHT) dir = 'C';
        else if (ev->key == TB_KEY_ARROW_LEFT) dir = 'D';
        if (modn == 1) {
            if (t->app_cursor) snprintf(tmp, sizeof(tmp), "\033O%c", dir);
            else snprintf(tmp, sizeof(tmp), "\033[%c", dir);
        } else {
            snprintf(tmp, sizeof(tmp), "\033[1;%d%c", modn, dir);
        }
    } else if (ev->key == TB_KEY_HOME) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), t->app_cursor ? "\033OH" : "\033[H");
        else snprintf(tmp, sizeof(tmp), "\033[1;%dH", modn);
    } else if (ev->key == TB_KEY_END) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), t->app_cursor ? "\033OF" : "\033[F");
        else snprintf(tmp, sizeof(tmp), "\033[1;%dF", modn);
    } else if (ev->key == TB_KEY_PGUP) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), "\033[5~");
        else snprintf(tmp, sizeof(tmp), "\033[5;%d~", modn);
    } else if (ev->key == TB_KEY_PGDN) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), "\033[6~");
        else snprintf(tmp, sizeof(tmp), "\033[6;%d~", modn);
    } else if (ev->key == TB_KEY_INSERT) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), "\033[2~");
        else snprintf(tmp, sizeof(tmp), "\033[2;%d~", modn);
    } else if (ev->key == TB_KEY_DELETE) {
        if (modn == 1) snprintf(tmp, sizeof(tmp), "\033[3~");
        else snprintf(tmp, sizeof(tmp), "\033[3;%d~", modn);
    } else if (ev->key >= TB_KEY_F1 && ev->key <= TB_KEY_F12) {
        int idx = TB_KEY_F1 - ev->key; /* F1 is 0xffff-0, so idx is 0 for F1? Wait */
        idx = (int)TB_KEY_F1 - (int)ev->key;
        if (idx < 0) idx = -idx;
        /* TB_KEY_F1 is (0xffff-0), F2 is 0xffff-1, so F1 - key is 0, 1, 2... if key <= F1 numerically.
         * 0xffff-0 > 0xffff-1, so F1 > F2 numerically. idx = F1 - key is 0 for F1, 1 for F2. */
        if (idx >= 0 && idx <= 3) {
            if (modn == 1) snprintf(tmp, sizeof(tmp), "\033%c", "PQRS"[idx] == 'P' ? 'O' : 'O');
            /* rewrite properly below */
        }
        if (idx >= 0 && idx <= 3) {
            if (modn == 1) snprintf(tmp, sizeof(tmp), "\033O%c", "PQRS"[idx]);
            else snprintf(tmp, sizeof(tmp), "\033[1;%d%c", modn, "PQRS"[idx]);
        } else if (idx >= 4 && idx <= 11) {
            static const int tilde[] = {15, 17, 18, 19, 20, 21, 23, 24};
            if (modn == 1) snprintf(tmp, sizeof(tmp), "\033[%d~", tilde[idx - 4]);
            else snprintf(tmp, sizeof(tmp), "\033[%d;%d~", tilde[idx - 4], modn);
        } else {
            tmp[0] = '\0';
        }
    } else if (ev->key == TB_KEY_BACK_TAB) {
        snprintf(tmp, sizeof(tmp), "\033[Z");
    } else if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2 || ev->key == TB_KEY_CTRL_H || ev->key == TB_KEY_CTRL_8) {
        if (ev->mod & TB_MOD_ALT) snprintf(tmp, sizeof(tmp), "\033\177");
        else snprintf(tmp, sizeof(tmp), "\177");
    } else if (ev->key > 0 && ev->key < 0x20) {
        if (ev->mod & TB_MOD_ALT) snprintf(tmp, sizeof(tmp), "\033%c", (char)ev->key);
        else snprintf(tmp, sizeof(tmp), "%c", (char)ev->key);
    } else if (ev->ch) {
        char utf[8];
        int un = tb_utf8_unicode_to_char(utf, ev->ch);
        if (un < 0) un = 0;
        if (un > 6) un = 6;
        utf[un] = '\0';
        if (ev->mod & TB_MOD_ALT) snprintf(tmp, sizeof(tmp), "\033%s", utf);
        else snprintf(tmp, sizeof(tmp), "%s", utf);
    } else {
        tmp[0] = '\0';
    }
    n = (int)strlen(tmp);
    if (n >= cap) n = cap - 1;
    memcpy(out, tmp, (size_t)n);
    out[n] = '\0';
    return n;
}

static iterm_panel_t *ensure_panel(editor_t *editor) {
    iterm_panel_t *p;
    if (!editor) return NULL;
    if (editor->iterm) return editor->iterm;
    p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->editor = editor;
    p->dock = DOCK_BOTTOM;
    p->root = make_leaf(NULL);
    p->focus = p->root;
    editor->iterm = p;
    return p;
}

static iterm_t *spawn_term(iterm_panel_t *panel, const char *cmdline) {
    iterm_t *t;
    iview_t *leaf;
    const char *shell;
    int cols = 80, rows = 24;
    char cwd[PATH_MAX];
    if (!panel) return NULL;
    leaf = panel->focus ? panel->focus : first_leaf(panel->root);
    if (leaf && leaf->split) leaf = first_leaf(leaf);
    if (!panel->focus) panel->focus = leaf;
    if (leaf && leaf->w > 1 && leaf->h > 1) {
        cols = leaf->w;
        rows = leaf->h;
    }
    t = term_new(cols, rows);
    if (!t) return NULL;
    t->id = ++panel->next_id;
    shell = user_shell();
    if (cmdline && cmdline[0]) {
        snprintf(t->title, sizeof(t->title), "%.60s", cmdline);
    } else {
        const char *base = strrchr(shell, '/');
        snprintf(t->title, sizeof(t->title), "%s", base ? base + 1 : shell);
    }
    editor_cwd(panel->editor, cwd, sizeof(cwd));
    snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
    if (term_spawn(t, cwd, shell, cmdline) != 0) {
        MLE_SET_ERR(panel->editor, "terminal: failed to open pty (%s)", strerror(errno));
        term_free(t);
        return NULL;
    }
    DL_APPEND(panel->terms, t);
    if (leaf && iview_is_leaf(leaf)) {
        iview_t *other = leaf_showing(panel->root, t);
        (void)other;
        leaf->term = t;
        leaf->scroll = 0;
        leaf->copy_mode = 0;
        leaf->sel_on = 0;
        panel->focus = leaf;
    }
    if (!panel->ever_hinted) {
        panel->ever_hinted = 1;
        MLE_SET_INFO(panel->editor, "%s", "drag the edge to resize   + new shell   x close   f7 hide");
    }
    return t;
}

static void relayout(iterm_panel_t *panel) {
    if (!panel || !panel->editor) return;
    editor_relayout(panel->editor);
}

static const char *action_from_event(struct tb_event *ev) {
    if (!ev || ev->type != TB_EVENT_KEY) return NULL;
    if (ev->key == TB_KEY_ESC) return "";
    if (ev->ch == 'n') return "new";
    if (ev->ch == 'N') return "newcmd";
    if (ev->ch == 'x') return "close";
    if (ev->ch == 'k') return "kill";
    if (ev->ch == ']') return "next";
    if (ev->ch == '[') return "prev";
    if (ev->ch == 'o') return "pane";
    if (ev->ch == 'v') return "splitv";
    if (ev->ch == 's') return "splith";
    if (ev->ch == 'w') return "unfocus";
    if (ev->ch == 'f') return "focus";
    if (ev->ch == 'q') return "hide";
    if (ev->ch == '+' || ev->ch == '=') return "grow";
    if (ev->ch == '-' || ev->ch == '_') return "shrink";
    if (ev->ch == ',') return "splitsmaller";
    if (ev->ch == '.') return "splitbigger";
    if (ev->ch == 'b') return "bottom";
    if (ev->ch == 't') return "top";
    if (ev->ch == 'l') return "left";
    if (ev->ch == 'r') return "right";
    if (ev->ch == 'm') return "max";
    if (ev->ch == 'y') return "copy";
    if (ev->ch == 'p') return "paste";
    if (ev->ch == 'c') return "clear";
    if (ev->ch == 'R') return "rename";
    if (ev->ch == '?') return "help";
    return NULL;
}

static void action_help(editor_t *editor) {
    MLE_SET_INFO(editor, "%s", "drag edge, + new, x close, f7 hide. M-t: n N x k [ ] o v s w + - b t l r m y p c R q");
}

int iterm_action(editor_t *editor, const char *action) {
    iterm_panel_t *panel;
    iterm_t *t;
    if (!editor || editor->headless_mode) return MLE_OK;
    if (!action || !action[0]) action = "toggle";
    panel = ensure_panel(editor);
    if (!panel) return MLE_ERR;

    if (strcmp(action, "help") == 0) {
        action_help(editor);
        return MLE_OK;
    }

    if (strcmp(action, "toggle") == 0) {
        if (!panel->visible) {
            panel->visible = 1;
            if (!panel->root) {
                panel->root = make_leaf(NULL);
                panel->focus = panel->root;
            }
            set_focused(panel, 1);
            relayout(panel);
            if (!panel->terms) spawn_term(panel, NULL);
            relayout(panel);
        } else {
            panel->visible = 0;
            set_focused(panel, 0);
            relayout(panel);
        }
        return MLE_OK;
    }

    if (strcmp(action, "hide") == 0) {
        panel->visible = 0;
        set_focused(panel, 0);
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "focus") == 0) {
        if (!panel->visible) return iterm_action(editor, "toggle");
        set_focused(panel, panel->focused ? 0 : 1);
        return MLE_OK;
    }

    if (strcmp(action, "unfocus") == 0) {
        set_focused(panel, 0);
        return MLE_OK;
    }

    if (strcmp(action, "new") == 0 || strcmp(action, "newcmd") == 0) {
        char *cmd = NULL;
        panel->visible = 1;
        if (!panel->root) {
            panel->root = make_leaf(NULL);
            panel->focus = panel->root;
        }
        set_focused(panel, 1);
        relayout(panel);
        if (strcmp(action, "newcmd") == 0) {
            editor_prompt(editor, "terminal command:", NULL, &cmd);
            if (!cmd) return MLE_OK;
            if (cmd[0] == '\0') {
                free(cmd);
                cmd = NULL;
            }
        }
        spawn_term(panel, cmd);
        free(cmd);
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "grow") == 0 || strcmp(action, "shrink") == 0) {
        int delta = strcmp(action, "grow") == 0 ? 1 : -1;
        int step;
        if (panel->dock == DOCK_LEFT || panel->dock == DOCK_RIGHT) {
            step = panel->size_w / 10;
            if (step < 1) step = 1;
            panel->size_w += delta * step;
            if (panel->size_w < 12) panel->size_w = 12;
        } else if (panel->dock != DOCK_MAX) {
            step = panel->size_h / 10;
            if (step < 1) step = 1;
            panel->size_h += delta * step;
            if (panel->size_h < 6) panel->size_h = 6;
        }
        if (!panel->visible) panel->visible = 1;
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "dockcycle") == 0) {
        if (panel->dock == DOCK_BOTTOM) panel->dock = DOCK_TOP;
        else if (panel->dock == DOCK_TOP) panel->dock = DOCK_LEFT;
        else if (panel->dock == DOCK_LEFT) panel->dock = DOCK_RIGHT;
        else panel->dock = DOCK_BOTTOM;
        if (panel->visible) relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "bottom") == 0 || strcmp(action, "top") == 0 || strcmp(action, "left") == 0
        || strcmp(action, "right") == 0 || strcmp(action, "max") == 0) {
        if (strcmp(action, "bottom") == 0) panel->dock = DOCK_BOTTOM;
        else if (strcmp(action, "top") == 0) panel->dock = DOCK_TOP;
        else if (strcmp(action, "left") == 0) panel->dock = DOCK_LEFT;
        else if (strcmp(action, "right") == 0) panel->dock = DOCK_RIGHT;
        else panel->dock = DOCK_MAX;
        if (!panel->visible) panel->visible = 1;
        relayout(panel);
        return MLE_OK;
    }

    t = focused_term(panel);

    if (strcmp(action, "next") == 0 || strcmp(action, "prev") == 0) {
        iterm_t *cur, *pick;
        if (!panel->terms) return iterm_action(editor, "new");
        cur = t;
        if (!cur) cur = panel->terms;
        if (strcmp(action, "next") == 0) pick = cur->next ? cur->next : panel->terms;
        else pick = cur->prev ? cur->prev : panel->terms;
        if (!pick) pick = panel->terms;
        while (pick->prev) pick = pick->prev;
        if (strcmp(action, "prev") == 0) {
            iterm_t *last = panel->terms;
            iterm_t *it;
            if (cur && cur->prev) pick = cur->prev;
            else {
                DL_FOREACH(panel->terms, it) last = it;
                pick = last;
            }
        } else if (cur && cur->next) {
            pick = cur->next;
        } else {
            pick = panel->terms;
        }
        if (panel->focus && iview_is_leaf(panel->focus)) {
            iview_t *owner = leaf_showing(panel->root, pick);
            if (owner) panel->focus = owner;
            else panel->focus->term = pick;
        }
        if (!panel->visible) panel->visible = 1;
        set_focused(panel, 1);
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "pane") == 0) {
        iview_t *leaves[64];
        int n = 0, i, cur = 0;
        collect_leaves(panel->root, leaves, &n, 64);
        if (n < 1) return MLE_OK;
        for (i = 0; i < n; i++) if (leaves[i] == panel->focus) cur = i;
        panel->focus = leaves[(cur + 1) % n];
        set_focused(panel, 1);
        return MLE_OK;
    }

    if (strcmp(action, "splitv") == 0 || strcmp(action, "splith") == 0) {
        iterm_t *nt;
        panel->visible = 1;
        set_focused(panel, 1);
        if (!panel->focus) panel->focus = first_leaf(panel->root);
        relayout(panel);
        nt = spawn_term(panel, NULL);
        if (!nt) return MLE_ERR;
        /* spawn_term assigned nt onto the current leaf. Split that leaf and
         * move nt onto the new side, restoring the previous term if any. */
        if (panel->focus && panel->focus->term == nt) {
            iterm_t *prev = nt->prev;
            panel->focus->term = prev && prev != nt ? prev : NULL;
            if (!panel->focus->term) {
                iterm_t *it;
                DL_FOREACH(panel->terms, it) {
                    if (it != nt) panel->focus->term = it;
                }
            }
            split_leaf(panel, panel->focus, strcmp(action, "splitv") == 0, nt);
        }
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "splitsmaller") == 0 || strcmp(action, "splitbigger") == 0) {
        iview_t *sp;
        float d = strcmp(action, "splitbigger") == 0 ? 0.08f : -0.08f;
        sp = panel->focus ? panel->focus->parent : NULL;
        if (sp && sp->split) {
            sp->factor += d;
            if (sp->factor < 0.15f) sp->factor = 0.15f;
            if (sp->factor > 0.85f) sp->factor = 0.85f;
            relayout(panel);
        }
        return MLE_OK;
    }

    if (strcmp(action, "close") == 0 || strcmp(action, "kill") == 0) {
        iview_t *owner;
        iterm_t *replacement;
        if (!t) return MLE_OK;
        if (strcmp(action, "kill") == 0 && t->alive && t->pid > 0) {
            kill(-t->pid, SIGTERM);
            MLE_SET_INFO(editor, "terminal: signaled %d", t->id);
            return MLE_OK;
        }
        owner = leaf_showing(panel->root, t);
        replacement = t->next ? t->next : t->prev;
        DL_DELETE(panel->terms, t);
        term_free(t);
        if (owner) {
            if (replacement && !leaf_showing(panel->root, replacement)) owner->term = replacement;
            else owner->term = NULL;
            if (!owner->term) close_leaf(panel, owner);
        }
        if (!panel->terms) {
            panel->visible = 0;
            set_focused(panel, 0);
        } else if (!panel->focus || !panel->focus->term) {
            panel->focus = first_leaf(panel->root);
            if (panel->focus && !panel->focus->term) panel->focus->term = panel->terms;
        }
        relayout(panel);
        return MLE_OK;
    }

    if (strcmp(action, "clear") == 0) {
        if (!t) return MLE_OK;
        if (!t->alt_on) sb_free_all(t);
        term_clear_grid(t, ag(t));
        if (panel->focus) panel->focus->scroll = 0;
        return MLE_OK;
    }

    if (strcmp(action, "paste") == 0 || strcmp(action, "pasteclip") == 0) {
        char *cut;
        char *clip;
        size_t n;
        int free_clip;
        if (!t || !t->alive) return MLE_OK;
        clip = NULL;
        n = 0;
        free_clip = 0;
        cut = NULL;
        if (strcmp(action, "pasteclip") == 0 && tb_width() >= 0
            && util_clipboard_get(&clip, &n) == MLE_OK && clip && clip[0]) {
            cut = clip;
            free_clip = 1;
        }
        if (!cut || !cut[0]) cut = editor->cut_buffer;
        if ((!cut || !cut[0]) && strcmp(action, "paste") == 0 && tb_width() >= 0
            && util_clipboard_get(&clip, &n) == MLE_OK && clip && clip[0]) {
            cut = clip;
            free_clip = 1;
        }
        if (!cut || !cut[0]) {
            MLE_SET_INFO(editor, "%s", "terminal: nothing to paste");
            if (free_clip) free(clip);
            return MLE_OK;
        }
        if (panel->focus) panel->focus->scroll = 0;
        if (t->bracket_paste) term_write(t, "\033[200~", 6);
        term_write(t, cut, (int)strlen(cut));
        if (t->bracket_paste) term_write(t, "\033[201~", 6);
        if (free_clip) free(clip);
        return MLE_OK;
    }

    if (strcmp(action, "rename") == 0) {
        char *name = NULL;
        if (!t) return MLE_OK;
        editor_prompt(editor, "terminal name:", NULL, &name);
        if (!name) return MLE_OK;
        snprintf(t->title, sizeof(t->title), "%s", name[0] ? name : "shell");
        t->title_locked = 1;
        free(name);
        return MLE_OK;
    }

    if (strcmp(action, "copy") == 0) {
        iview_t *v = panel->focus;
        if (!v) return MLE_OK;
        if (v->sel_on) {
            yank_view(panel, v);
            return MLE_OK;
        }
        if (!v->copy_mode) enter_copy(v);
        else v->copy_mode = 0;
        set_focused(panel, 1);
        if (!panel->visible) {
            panel->visible = 1;
            relayout(panel);
        }
        return MLE_OK;
    }

    return MLE_OK;
}

int cmd_iterm(cmd_context_t *ctx) {
    const char *action;
    if (!ctx || !ctx->editor) return MLE_ERR;
    action = ctx->static_param ? ctx->static_param : "toggle";
    return iterm_action(ctx->editor, action);
}

int iterm_visible(editor_t *editor) {
    return editor && editor->iterm && editor->iterm->visible;
}

int iterm_dragging(editor_t *editor) {
    return editor && editor->iterm && editor->iterm->visible && editor->iterm->drag;
}

const char *iterm_dock_name(editor_t *editor) {
    if (!editor || !editor->iterm) return "bottom";
    return dock_name(editor->iterm->dock);
}

int iterm_running(editor_t *editor) {
    iterm_t *t;
    if (!editor || !editor->iterm) return 0;
    DL_FOREACH(editor->iterm->terms, t) {
        if (t->fd >= 0) return 1;
    }
    return 0;
}

int iterm_pty_fds(editor_t *editor, int *fds, int cap) {
    iterm_t *t;
    int n = 0;
    if (!editor || !editor->iterm || !fds || cap < 1) return 0;
    DL_FOREACH(editor->iterm->terms, t) {
        if (t->fd < 0) continue;
        if (n >= cap) break;
        fds[n++] = t->fd;
    }
    return n;
}

void iterm_drain_fd(editor_t *editor, int fd) {
    iterm_t *t;
    if (!editor || !editor->iterm || fd < 0) return;
    DL_FOREACH(editor->iterm->terms, t) {
        if (t->fd == fd) {
            term_consume(t);
            return;
        }
    }
}

void iterm_drain_pending(editor_t *editor) {
    iterm_t *t;
    if (!editor || !editor->iterm) return;
    DL_FOREACH(editor->iterm->terms, t) term_consume(t);
}

void iterm_apply_layout(editor_t *editor, bview_rect_t *edit) {
    iterm_panel_t *p;
    int tabh, cx, cy, cw, ch;
    if (!editor || !editor->iterm || !edit) return;
    p = editor->iterm;
    if (!p->visible) return;
    if (edit->w < 1 || edit->h < 1) return;

    if (p->dock == DOCK_MAX) {
        p->x = edit->x;
        p->y = edit->y;
        p->w = edit->w;
        p->h = edit->h;
    } else if (p->dock == DOCK_BOTTOM || p->dock == DOCK_TOP) {
        int ph, maxh;
        if (p->size_h < 1) p->size_h = edit->h / 3;
        if (p->size_h < 6) p->size_h = edit->h > 6 ? 6 : edit->h;
        ph = p->size_h;
        maxh = edit->h - 3;
        if (maxh < 4) maxh = edit->h;
        if (ph > maxh) ph = maxh;
        if (ph < 1) return;
        p->size_h = ph;
        p->x = edit->x;
        p->w = edit->w;
        p->h = ph;
        if (p->dock == DOCK_BOTTOM) {
            p->y = edit->y + edit->h - ph;
            edit->h -= ph;
        } else {
            p->y = edit->y;
            edit->y += ph;
            edit->h -= ph;
        }
    } else {
        int pw, maxw;
        if (p->size_w < 1) p->size_w = edit->w / 3;
        if (p->size_w < 12) p->size_w = edit->w > 12 ? 12 : edit->w;
        pw = p->size_w;
        maxw = edit->w - 10;
        if (maxw < 16) maxw = edit->w;
        if (pw > maxw) pw = maxw;
        if (pw < 1) return;
        p->size_w = pw;
        p->y = edit->y;
        p->h = edit->h;
        p->w = pw;
        if (p->dock == DOCK_RIGHT) {
            p->x = edit->x + edit->w - pw;
            edit->w -= pw;
        } else {
            p->x = edit->x;
            edit->x += pw;
            edit->w -= pw;
        }
    }

    {
        int edge = 0;
        p->grip = 0;
        if (p->dock == DOCK_BOTTOM && p->h >= 5) {
            p->grip = 1;
            edge = 1;
        } else if (p->dock == DOCK_TOP && p->h >= 5) {
            p->grip = 2;
            edge = 1;
        } else if (p->dock == DOCK_LEFT && p->w >= 18) {
            p->grip = 3;
        } else if (p->dock == DOCK_RIGHT && p->w >= 18) {
            p->grip = 4;
        }
        tabh = (p->h - edge) >= 4 ? 1 : 0;
        p->tabh = tabh;
        p->tab_y = p->y + (p->grip == 1 ? 1 : 0);
        cx = p->x + (p->grip == 4 ? 1 : 0);
        cy = p->y + (p->grip == 1 ? 1 : 0) + tabh;
        cw = p->w - ((p->grip == 3 || p->grip == 4) ? 1 : 0);
        ch = p->h - edge - tabh;
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (!p->root) {
        p->root = make_leaf(NULL);
        p->focus = p->root;
    }
    layout_view(p->root, cx, cy, cw, ch > 0 ? ch : 0);
    sync_leaf_sizes(p->root);
}

void iterm_draw(editor_t *editor) {
    iterm_panel_t *p;
    int alive;
    if (!editor || !editor->iterm || editor->headless_mode) return;
    p = editor->iterm;
    alive = term_alive_count(p);
    if (!p->visible) {
        if (term_count(p) > 0 && !editor->prompt && editor->h > 0) {
            if (alive > 0) tb_printf(0, editor->h - 1, TB_BLACK | TB_BOLD, TB_GREEN, " f7 term:%d ", alive);
            else tb_printf(0, editor->h - 1, TB_BLACK | TB_BOLD, TB_YELLOW, " f7 term ");
        }
        return;
    }
    if (p->w < 1 || p->h < 1) return;
    draw_grip(p);
    draw_tabs(p);
    draw_view(p, p->root);
    if (p->focused && p->focus && p->focus->copy_mode) {
        /* cursor already placed */
    } else if (!p->focused) {
        /* leave the editor cursor */
    }
}

static int is_toggle_key(struct tb_event *ev) {
    if (ev->key == TB_KEY_F7 && !(ev->mod & TB_MOD_ALT)) return 1;
    if ((ev->mod & TB_MOD_ALT) && ev->ch == '`') return 1;
    /* Ctrl+Shift+` / ~ only. Plain Ctrl+Space is the same key with no shift. */
    if (ev->key == TB_KEY_CTRL_TILDE && (ev->mod & TB_MOD_CTRL) && (ev->mod & TB_MOD_SHIFT)) return 1;
    return 0;
}

static int is_select_arrow(struct tb_event *ev) {
    if (!(ev->mod & TB_MOD_SHIFT) || (ev->mod & TB_MOD_CTRL)) return 0;
    return ev->key == TB_KEY_ARROW_LEFT || ev->key == TB_KEY_ARROW_RIGHT
        || ev->key == TB_KEY_ARROW_UP || ev->key == TB_KEY_ARROW_DOWN
        || ev->key == TB_KEY_HOME || ev->key == TB_KEY_END;
}

int iterm_handle_key(editor_t *editor, struct tb_event *ev) {
    iterm_panel_t *panel;
    iview_t *view;
    iterm_t *t;
    const char *act;
    char seq[64];
    int n;
    if (!editor || !editor->iterm || !ev) return 0;
    panel = editor->iterm;
    if (editor->active && editor->active->is_menu) return 0;
    if (editor->prompt) return 0;
    if ((ev->key == TB_KEY_F1 || ev->key == TB_KEY_F8 || ev->key == TB_KEY_F9 || ev->key == TB_KEY_F10)
        && !(ev->mod & (TB_MOD_CTRL | TB_MOD_ALT))) {
        panel->prefix = 0;
        panel->hint = NULL;
        return 0;
    }
    if (!panel->focused || !panel->visible) return 0;
    view = panel->focus;
    t = view ? view->term : NULL;

    if (panel->prefix) {
        panel->prefix = 0;
        panel->hint = NULL;
        act = action_from_event(ev);
        if (act == NULL) {
            MLE_SET_INFO(editor, "%s", "terminal: unknown M-t command (M-t ? lists them)");
            return 1;
        }
        if (act[0]) iterm_action(editor, act);
        return 1;
    }

    if (is_toggle_key(ev)) {
        iterm_action(editor, "toggle");
        return 1;
    }
    if ((ev->mod & TB_MOD_ALT) && ev->ch == 'T') {
        iterm_action(editor, "new");
        return 1;
    }
    if ((ev->mod & TB_MOD_ALT) && ev->ch == 't') {
        panel->prefix = 1;
        panel->hint = NULL;
        MLE_SET_INFO(editor, "%s", "+ new, x close, drag the edge. M-t ? lists every command");
        return 1;
    }

    if (view && view->copy_mode) return copy_key(panel, view, ev);

    if (view && view->term && is_select_arrow(ev)) {
        if (!view->copy_mode) enter_copy(view);
        return copy_key(panel, view, ev);
    }
    if (ev->key == TB_KEY_INSERT && (ev->mod & TB_MOD_SHIFT) && !(ev->mod & TB_MOD_CTRL)) {
        iterm_action(editor, "pasteclip");
        return 1;
    }

    if (ev->key == TB_KEY_PGUP && (ev->mod & TB_MOD_SHIFT) && !(ev->mod & TB_MOD_CTRL)) {
        if (view) {
            view->scroll += view->h > 1 ? view->h / 2 : 1;
            clamp_scroll(view);
        }
        return 1;
    }
    if (ev->key == TB_KEY_PGDN && (ev->mod & TB_MOD_SHIFT) && !(ev->mod & TB_MOD_CTRL)) {
        if (view) {
            view->scroll -= view->h > 1 ? view->h / 2 : 1;
            clamp_scroll(view);
        }
        return 1;
    }
    if (ev->key == TB_KEY_PGUP && (ev->mod & TB_MOD_CTRL)) {
        iterm_action(editor, "prev");
        return 1;
    }
    if (ev->key == TB_KEY_PGDN && (ev->mod & TB_MOD_CTRL)) {
        iterm_action(editor, "next");
        return 1;
    }

    if (!t) return 1;
    if (!t->alive) {
        if (ev->key == TB_KEY_ENTER) iterm_action(editor, "close");
        else MLE_SET_INFO(editor, "%s", "terminal exited. enter or x closes it");
        return 1;
    }

    clear_selection(panel);
    if (view) view->scroll = 0;
    n = encode_key(t, ev, seq, (int)sizeof(seq));
    if (n > 0) term_write(t, seq, n);
    return 1;
}

static void send_mouse(iterm_t *t, int btn, int x, int y, int release) {
    char buf[64];
    int n;
    if (!t || t->fd < 0 || !t->alive) return;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (t->mouse_1006) {
        n = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", btn, x, y, release ? 'm' : 'M');
        term_write(t, buf, n);
    } else if (!release && x < 223 && y < 223) {
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = (char)(32 + btn);
        buf[4] = (char)(32 + x);
        buf[5] = (char)(32 + y);
        term_write(t, buf, 6);
    } else if (release && x < 223 && y < 223) {
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = (char)(32 + 3);
        buf[4] = (char)(32 + x);
        buf[5] = (char)(32 + y);
        term_write(t, buf, 6);
    }
}

static int mouse_tracking(iterm_t *t) {
    return t && (t->mouse_1000 || t->mouse_1002 || t->mouse_1006);
}

int iterm_handle_mouse(editor_t *editor, struct tb_event *ev) {
    iterm_panel_t *panel;
    int inside, x, y;
    iview_t *leaf, *div;
    if (!editor || !editor->iterm || !ev) return 0;
    panel = editor->iterm;
    if (!panel->visible || panel->w < 1 || panel->h < 1) return 0;
    x = ev->x;
    y = ev->y;
    inside = x >= panel->x && y >= panel->y && x < panel->x + panel->w && y < panel->y + panel->h;

    /* Keep resizing after the pointer leaves the panel, same as the explorer. */
    if (panel->drag) {
        if (ev->key == TB_KEY_MOUSE_RELEASE) {
            panel->drag = 0;
            panel->mouse_down = 0;
            return 1;
        }
        if (ev->key == TB_KEY_MOUSE_LEFT || (ev->mod & TB_MOD_MOTION)) {
            if (panel->drag == 1) {
                if (panel->dock == DOCK_BOTTOM) panel->size_h = (panel->y + panel->h) - y;
                else if (panel->dock == DOCK_TOP) panel->size_h = y - panel->y + 1;
                else if (panel->dock == DOCK_LEFT) panel->size_w = x - panel->x + 1;
                else if (panel->dock == DOCK_RIGHT) panel->size_w = (panel->x + panel->w) - x;
                if (panel->size_h < 6) panel->size_h = 6;
                if (panel->size_w < 12) panel->size_w = 12;
                relayout(panel);
                return 1;
            }
            if (panel->drag == 2 && panel->drag_split && panel->drag_split->split) {
                iview_t *sp = panel->drag_split;
                if (sp->vertical && sp->w > 1) {
                    float f = (float)(x - sp->x) / (float)sp->w;
                    if (f < 0.15f) f = 0.15f;
                    if (f > 0.85f) f = 0.85f;
                    sp->factor = f;
                } else if (sp->h > 1) {
                    float f = (float)(y - sp->y) / (float)sp->h;
                    if (f < 0.15f) f = 0.15f;
                    if (f > 0.85f) f = 0.85f;
                    sp->factor = f;
                }
                relayout(panel);
                return 1;
            }
        }
    }

    if (!inside) {
        if (ev->key == TB_KEY_MOUSE_RELEASE) {
            panel->mouse_down = 0;
            return 0;
        }
        if (ev->key == TB_KEY_MOUSE_LEFT || ev->key == TB_KEY_MOUSE_RIGHT) {
            if (panel->focused) set_focused(panel, 0);
            panel->prefix = 0;
            panel->mouse_down = 0;
        }
        return 0;
    }

    if (ev->key == TB_KEY_MOUSE_WHEEL_UP || ev->key == TB_KEY_MOUSE_WHEEL_DOWN) {
        leaf = leaf_at(panel->root, x, y);
        if (!leaf) return 1;
        panel->focus = leaf;
        set_focused(panel, 1);
        if (leaf->term && mouse_tracking(leaf->term) && !(ev->mod & TB_MOD_SHIFT) && !leaf->copy_mode) {
            int btn = ev->key == TB_KEY_MOUSE_WHEEL_UP ? 64 : 65;
            if (ev->mod & TB_MOD_ALT) btn += 8;
            if (ev->mod & TB_MOD_CTRL) btn += 16;
            send_mouse(leaf->term, btn, x - leaf->x + 1, y - leaf->y + 1, 0);
        } else {
            int step = ev->key == TB_KEY_MOUSE_WHEEL_UP ? 3 : -3;
            leaf->scroll += step;
            clamp_scroll(leaf);
        }
        return 1;
    }

    if (ev->key == TB_KEY_MOUSE_RELEASE) {
        iview_t *rel = panel->focus;
        panel->mouse_down = 0;
        panel->drag = 0;
        if (rel && rel->term && mouse_tracking(rel->term) && !rel->copy_mode && !rel->sel_on) {
            send_mouse(rel->term, panel->last_btn, x - rel->x + 1, y - rel->y + 1, 1);
        } else if (rel && rel->sel_on && (rel->sel_ax != rel->copy_x || rel->sel_ay != rel->copy_y)) {
            yank_view(panel, rel);
        }
        return 1;
    }

    if (ev->key != TB_KEY_MOUSE_LEFT && ev->key != TB_KEY_MOUSE_RIGHT && ev->key != TB_KEY_MOUSE_MIDDLE) {
        return 1;
    }

    if (!panel->mouse_down) {
        iterm_t *tab;
        panel->mouse_down = 1;
        panel->drag = 0;
        set_focused(panel, 1);
        panel->prefix = 0;
        if (hit_grip(panel, x, y) && ev->key == TB_KEY_MOUSE_LEFT) {
            panel->drag = 1;
            return 1;
        }
        if (panel->tabh && y == panel->tab_y) {
            if (panel->plus_w > 0 && x >= panel->plus_x && x < panel->plus_x + panel->plus_w) {
                iterm_action(editor, "new");
                return 1;
            }
            DL_FOREACH(panel->terms, tab) {
                if (tab->close_x >= 0 && x == tab->close_x) {
                    iview_t *owner = leaf_showing(panel->root, tab);
                    if (owner) panel->focus = owner;
                    else if (panel->focus && iview_is_leaf(panel->focus)) panel->focus->term = tab;
                    iterm_action(editor, "close");
                    return 1;
                }
            }
            DL_FOREACH(panel->terms, tab) {
                if (tab->tab_w > 0 && x >= tab->tab_x && x < tab->tab_x + tab->tab_w) {
                    iview_t *owner = leaf_showing(panel->root, tab);
                    if (owner) panel->focus = owner;
                    else if (panel->focus && iview_is_leaf(panel->focus)) panel->focus->term = tab;
                    break;
                }
            }
            return 1;
        }
        div = split_divider_at(panel->root, x, y);
        if (div) {
            panel->drag = 2;
            panel->drag_split = div;
            return 1;
        }
        leaf = leaf_at(panel->root, x, y);
        if (leaf && ev->key == TB_KEY_MOUSE_MIDDLE
            && !(leaf->term && mouse_tracking(leaf->term) && !(ev->mod & TB_MOD_SHIFT))) {
            panel->focus = leaf;
            iterm_action(editor, "pasteclip");
            return 1;
        }
        if (leaf) {
            panel->focus = leaf;
            if (leaf->term && mouse_tracking(leaf->term) && !(ev->mod & TB_MOD_SHIFT) && !leaf->copy_mode) {
                int btn = 0;
                if (ev->key == TB_KEY_MOUSE_MIDDLE) btn = 1;
                if (ev->key == TB_KEY_MOUSE_RIGHT) btn = 2;
                if (ev->mod & TB_MOD_SHIFT) btn += 4;
                if (ev->mod & TB_MOD_ALT) btn += 8;
                if (ev->mod & TB_MOD_CTRL) btn += 16;
                panel->last_btn = btn;
                send_mouse(leaf->term, btn, x - leaf->x + 1, y - leaf->y + 1, 0);
            } else {
                int hy = view_top(leaf) + (y - leaf->y);
                int hx = x - leaf->x;
                leaf->copy_x = hx;
                leaf->copy_y = hy;
                leaf->sel_on = 1;
                leaf->sel_ax = hx;
                leaf->sel_ay = hy;
                panel->sel_view = leaf;
            }
        }
        return 1;
    }

    leaf = panel->focus;
    if (leaf && leaf->term && mouse_tracking(leaf->term) && !(ev->mod & TB_MOD_SHIFT) && !leaf->sel_on && (leaf->term->mouse_1002 || (ev->mod & TB_MOD_MOTION))) {
        int btn = panel->last_btn;
        if (ev->mod & TB_MOD_MOTION) btn += 32;
        send_mouse(leaf->term, btn, x - leaf->x + 1, y - leaf->y + 1, 0);
        return 1;
    }
    if (leaf && leaf->sel_on) {
        leaf->copy_x = x - leaf->x;
        leaf->copy_y = view_top(leaf) + (y - leaf->y);
        if (leaf->copy_x < 0) leaf->copy_x = 0;
        return 1;
    }
    return 1;
}

static void free_view(iview_t *n) {
    if (!n) return;
    free_view(n->a);
    free_view(n->b);
    free(n);
}

void iterm_shutdown(editor_t *editor) {
    iterm_t *t, *tmp;
    if (!editor || !editor->iterm) return;
    DL_FOREACH_SAFE(editor->iterm->terms, t, tmp) {
        DL_DELETE(editor->iterm->terms, t);
        term_free(t);
    }
    free_view(editor->iterm->root);
    free(editor->iterm);
    editor->iterm = NULL;
}

static iterm_t *debug_term(int cols, int rows) {
    iterm_t *t = term_new(cols, rows);
    return t;
}

int iterm_debug_feed(const char *bytes, int len, int cols, int rows, char *out, int out_cap) {
    iterm_t *t;
    if (!out || out_cap < 1) return -1;
    t = debug_term(cols, rows);
    if (!t) return -1;
    if (bytes && len > 0) term_feed(t, bytes, len);
    term_dump(t, out, out_cap);
    term_free(t);
    return 0;
}

int iterm_debug_cell(const char *bytes, int len, int cols, int rows, int x, int y, uint32_t *ch, int *fg, int *bg, int *attr) {
    iterm_t *t;
    cell_t *c;
    t = debug_term(cols, rows);
    if (!t) return -1;
    if (bytes && len > 0) term_feed(t, bytes, len);
    if (y < 0 || x < 0 || y >= t->primary.nrows || x >= t->primary.ncols) {
        term_free(t);
        return -1;
    }
    c = &ag(t)->rows[y].cells[x];
    if (ch) *ch = c->ch;
    if (fg) *fg = c->fg;
    if (bg) *bg = c->bg;
    if (attr) *attr = c->attr;
    term_free(t);
    return 0;
}

int iterm_debug_resize(const char *bytes, int len, int cols, int rows, int cols2, int rows2, char *out, int out_cap) {
    iterm_t *t;
    if (!out || out_cap < 1) return -1;
    t = debug_term(cols, rows);
    if (!t) return -1;
    if (bytes && len > 0) term_feed(t, bytes, len);
    term_set_size(t, cols2, rows2);
    term_dump(t, out, out_cap);
    term_free(t);
    return 0;
}

int iterm_debug_pty(char *out, int out_cap) {
    iterm_t *t;
    int elapsed = 0;
    if (!out || out_cap < 2) return -1;
    out[0] = '\0';
    t = term_new(40, 8);
    if (!t) return -1;
    if (term_spawn(t, "/", "/bin/sh", "printf 'xyzzy-iterm\\n'") != 0) {
        term_free(t);
        return -1;
    }
    while (elapsed < 2000 && t->fd >= 0) {
        struct pollfd pfd;
        pfd.fd = t->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 50) > 0) term_consume(t);
        elapsed += 50;
    }
    term_dump(t, out, out_cap);
    term_free(t);
    if (!strstr(out, "xyzzy-iterm")) return -1;
    return 0;
}
