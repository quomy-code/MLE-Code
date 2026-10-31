#include <math.h>
#include <unistd.h>
#include <wctype.h>
#include "mle.h"

static int _bview_pop_kmap(bview_t *bview, kmap_t **optret_kmap, int allow_pop_root);
static int _bview_rectify_viewport_dim(bview_t *self, bline_t *bline, bint_t vpos, int dim_scope, int dim_size, bint_t *view_vpos);
static void _bview_init(bview_t *self, buffer_t *buffer);
static void _bview_init_resized(bview_t *self);
static kmap_t *_bview_get_init_kmap(editor_t *editor);
static void _bview_buffer_callback(buffer_t *buffer, baction_t *action, void *udata);
static int _bview_set_linenum_width(bview_t *self);
static void _bview_deinit(bview_t *self);
static void _bview_set_tab_width(bview_t *self, int tab_width);
static void _bview_fix_path(bview_t *self, char *path, int path_len, char **ret_path, int *ret_path_len, bint_t *ret_line_num);
static buffer_t *_bview_open_buffer(bview_t *self, char *opt_path, int opt_path_len);
static void _bview_draw_prompt(bview_t *self);
static void _bview_draw_status(bview_t *self);
static void _bview_draw_edit(bview_t *self, int x, int y, int w, int h);
static void _bview_draw_bline(bview_t *self, bline_t *bline, int rect_y, bline_t **optret_bline, int *optret_rect_y);
static void _bview_draw_scrollbars(bview_t *self);
static void _bview_highlight_bracket_pair(bview_t *self, mark_t *mark);
static bint_t _bview_get_viewport_x(bview_t *self, bline_t *bline);
static int _bview_is_cursor_line(bview_t *self, bline_t *bline);
static int _bview_is_soft_wrapped(bview_t *self, bline_t *bline);
static int _bview_is_in_range(bline_t *bline, bint_t col, int is_block, srule_t **ret_srule);
static int _bview_is_in_isearch(bview_t *self, bint_t col, srule_t **ret_srule);
static int _bview_populate_isearch_ranges(bview_t *self, bline_t *bline);

// Create a new bview
bview_t *bview_new(editor_t *editor, int type, char *opt_path, int opt_path_len, buffer_t *opt_buffer) {
    static int id = 0;
    bview_t *self;
    buffer_t *buffer;

    // Allocate and init bview
    self = calloc(1, sizeof(bview_t));
    self->editor = editor;
    self->type = type;
    if (opt_path) self->path = strndup(opt_path, opt_path_len);
    self->rect_caption.fg = TB_WHITE;
    self->rect_caption.bg = TB_BLACK;
    self->rect_lines.fg = TB_BLACK | TB_BRIGHT;
    self->rect_lines.bg = 0;
    self->rect_margin_left.fg = TB_BLACK | TB_BRIGHT;
    self->rect_margin_right.fg = TB_BLACK | TB_BRIGHT;
    self->rect_buffer.h = 10; // TODO hack to fix _bview_set_linenum_width before bview_resize
    self->tab_width = editor->tab_width;
    self->tab_to_space = editor->tab_to_space;
    self->soft_wrap = editor->soft_wrap;
    self->viewport_scope_x = editor->viewport_scope_x;
    self->viewport_scope_y = editor->viewport_scope_y;
    self->id = (id++);

    // Open buffer
    if (opt_buffer) {
        buffer = opt_buffer;
    } else {
        buffer = _bview_open_buffer(self, opt_path, opt_path_len);
    }
    _bview_init(self, buffer);

    return self;
}

// Open a buffer in an existing bview
int bview_open(bview_t *self, char *path, int path_len) {
    buffer_t *buffer;
    buffer = _bview_open_buffer(self, path, path_len);
    if (self->path) free(self->path);
    self->path = strndup(path, path_len);
    _bview_init(self, buffer);
    bview_resize(self, self->x, self->y, self->w, self->h);
    return MLE_OK;
}

// Free a bview
int bview_destroy(bview_t *self) {
    _bview_deinit(self);
    if (self->path) free(self->path);
    // TODO ensure everything freed
    free(self);
    return MLE_OK;
}

// Move and resize a bview to the given position and dimensions
int bview_resize(bview_t *self, int x, int y, int w, int h) {
    int aw, ah;
    int body_h;
    int has_scroll;

    self->x = x;
    self->y = y;
    self->w = w;
    self->h = h;

    aw = w;
    ah = h;

    if (self->split_child) {
        if (self->split_is_vertical) {
            aw = MLE_MAX(1, (int)((float)aw * self->split_factor));
        } else {
            ah = MLE_MAX(1, (int)((float)ah * self->split_factor));
        }
    }

    if (MLE_BVIEW_IS_EDIT(self)) {
        has_scroll = ah >= 3;
        body_h = ah - 1 - (has_scroll ? 1 : 0);
        if (body_h < 0) body_h = 0;
        self->rect_caption.x = x;
        self->rect_caption.y = y;
        self->rect_caption.w = aw;
        self->rect_caption.h = 1;

        self->rect_lines.x = x;
        self->rect_lines.y = y + 1;
        self->rect_lines.w = self->linenum_width;
        self->rect_lines.h = body_h;

        self->rect_margin_left.x = x + self->linenum_width;
        self->rect_margin_left.y = y + 1;
        self->rect_margin_left.w = 1;
        self->rect_margin_left.h = body_h;

        self->rect_buffer.x = x + self->linenum_width + 1;
        self->rect_buffer.y = y + 1;
        self->rect_buffer.w = MLE_MAX(1, aw - (self->linenum_width + 1 + 1));
        self->rect_buffer.h = body_h;

        self->rect_margin_right.x = x + (aw - 1);
        self->rect_margin_right.y = y + 1;
        self->rect_margin_right.w = 1;
        self->rect_margin_right.h = body_h;

        self->rect_scroll_h.x = self->rect_buffer.x;
        self->rect_scroll_h.y = y + ah - 1;
        self->rect_scroll_h.w = MLE_MAX(1, aw - (self->linenum_width + 1));
        self->rect_scroll_h.h = has_scroll ? 1 : 0;
    } else {
        self->rect_buffer.x = x;
        self->rect_buffer.y = y;
        self->rect_buffer.w = aw;
        self->rect_buffer.h = ah;
        self->rect_scroll_h.h = 0;
    }

    if (self->split_child) {
        bview_resize(
            self->split_child,
            x + (self->split_is_vertical ? aw : 0),
            y + (self->split_is_vertical ? 0 : ah),
            w - (self->split_is_vertical ? aw : 0),
            h - (self->split_is_vertical ? 0 : ah)
        );
    }

    if (!self->is_resized) {
        _bview_init_resized(self);
        self->is_resized = 1;
    }

    bview_rectify_viewport(self);

    return MLE_OK;
}

int bview_sync_linenums(bview_t *self) {
    if (!self) return MLE_OK;
    if (_bview_set_linenum_width(self)) bview_resize(self, self->x, self->y, self->w, self->h);
    if (self->split_child) bview_sync_linenums(self->split_child);
    return MLE_OK;
}

// Return top-most split_parent of a bview
bview_t *bview_get_split_root(bview_t *self) {
    bview_t *root;
    root = self;
    while (root->split_parent) {
        root = root->split_parent;
    }
    return root;
}

// Draw bview to screen
int bview_draw(bview_t *self) {
    if (MLE_BVIEW_IS_PROMPT(self)) {
        _bview_draw_prompt(self);
    } else if (MLE_BVIEW_IS_STATUS(self)) {
        _bview_draw_status(self);
    }
    _bview_draw_edit(self, self->x, self->y, self->w, self->h);
    return MLE_OK;
}

// Set cursor to screen
int bview_draw_cursor(bview_t *self, int set_real_cursor) {
    cursor_t *cursor;
    mark_t *mark;
    int screen_x;
    int screen_y;
    struct tb_cell *cell;
    int placed;
    placed = 0;
    DL_FOREACH(self->cursors, cursor) {
        mark = cursor->mark;
        if (bview_get_screen_coords(self, mark, &screen_x, &screen_y, &cell) != MLE_OK) {
            // Out of bounds. Do not leave the previous frame's cursor here.
            continue;
        }
        if (set_real_cursor && cursor == self->active_cursor) {
            tb_set_cursor(screen_x, screen_y);
            placed = 1;
        } else {
            mle_theme_t *th = theme_current(self->editor);
            uintattr_t markc = cursor->is_asleep ? th->err_bg : th->sel_bg;
            tb_set_cell(screen_x, screen_y, cell->ch, cell->fg, cell->bg | markc);
        }
        if (self->editor->highlight_bracket_pairs) {
            _bview_highlight_bracket_pair(self, mark);
        }
    }
    if (set_real_cursor && !placed) tb_set_cursor(-1, -1);
    return MLE_OK;
}

// Push a kmap
int bview_push_kmap(bview_t *bview, kmap_t *kmap) {
    kmap_node_t *node;
    node = calloc(1, sizeof(kmap_node_t));
    node->kmap = kmap;
    node->bview = bview;
    DL_APPEND(bview->kmap_stack, node);
    bview->kmap_tail = node;
    return MLE_OK;
}

// Pop a kmap
int bview_pop_kmap(bview_t *bview, kmap_t **optret_kmap) {
    return _bview_pop_kmap(bview, optret_kmap, 0);
}

// Split a bview
int bview_split(bview_t *self, int is_vertical, float factor, bview_t **optret_bview) {
    bview_t *child;

    if (self->split_child) {
        MLE_RETURN_ERR(self->editor, "bview %p is already split", (void*)self);
    } else if (!MLE_BVIEW_IS_EDIT(self)) {
        MLE_RETURN_ERR(self->editor, "bview %p is not an edit bview", (void*)self);
    }

    // Make child
    editor_open_bview(self->editor, self, self->type, NULL, 0, 1, 0, 1, self->buffer, &child);
    child->split_parent = self;
    self->split_child = child;
    self->split_factor = factor;
    self->split_is_vertical = is_vertical;

    // Move cursor to same position
    mark_move_to(child->active_cursor->mark, self->active_cursor->mark->bline->line_index, self->active_cursor->mark->col);
    bview_center_viewport_y(child);

    // Resize self
    bview_resize(self, self->x, self->y, self->w, self->h);

    if (optret_bview) {
        *optret_bview = child;
    }
    return MLE_OK;
}

// Return number of active cursors
int bview_get_active_cursor_count(bview_t *self) {
    int count;
    cursor_t *cursor;
    count = 0;
    DL_FOREACH(self->cursors, cursor) {
        if (!cursor->is_asleep) {
            count += 1;
        }
    }
    return count;
}

// Add a cursor to a bview
int bview_add_cursor(bview_t *self, bline_t *opt_bline, bint_t opt_col, cursor_t **optret_cursor) {
    cursor_t *cursor;
    cursor = calloc(1, sizeof(cursor_t));
    cursor->bview = self;
    if (!opt_bline) opt_bline = self->buffer->first_line;
    if (opt_col < 0) opt_col = 0;
    cursor->mark = buffer_add_mark(self->buffer, opt_bline, opt_col);
    DL_APPEND(self->cursors, cursor);
    if (!self->active_cursor) {
        self->active_cursor = cursor;
    }
    cursor->is_block = self->active_cursor->is_block;
    if (optret_cursor) {
        *optret_cursor = cursor;
    }
    return MLE_OK;
}

// Add sleeping cursor
int bview_add_cursor_asleep(bview_t *self, bline_t *opt_bline, bint_t opt_col, cursor_t **optret_cursor) {
    cursor_t *cursor;
    bview_add_cursor(self, opt_bline, opt_col, &cursor);
    cursor->is_asleep = 1;
    if (optret_cursor) *optret_cursor = cursor;
    return MLE_OK;
}

// Wake all sleeping cursors
int bview_wake_sleeping_cursors(bview_t *self) {
    cursor_t *cursor;
    DL_FOREACH(self->cursors, cursor) {
        if (cursor->is_asleep) {
            cursor->is_asleep = 0;
        }
    }
    return MLE_OK;
}

// Remove all cursors except one
int bview_remove_cursors_except(bview_t *self, cursor_t *one) {
    cursor_t *cursor;
    cursor_t *cursor_tmp;
    DL_FOREACH_SAFE(self->cursors, cursor, cursor_tmp) {
        if (cursor != one) {
            bview_remove_cursor(self, cursor);
        }
    }
    return MLE_OK;
}

// Remove a cursor from a bview
int bview_remove_cursor(bview_t *self, cursor_t *cursor) {
    cursor_t *el;
    cursor_t *tmp;
    DL_FOREACH_SAFE(self->cursors, el, tmp) {
        if (el == cursor) {
            self->active_cursor = el->prev && el->prev != el ? el->prev : el->next;
            DL_DELETE(self->cursors, el);
            if (el->sel_rule) {
                buffer_remove_srule(el->bview->buffer, el->sel_rule);
                srule_destroy(el->sel_rule);
                el->sel_rule = NULL;
            }
            if (el->cut_buffer) free(el->cut_buffer);
            free(el);
            return MLE_OK;
        }
    }
    return MLE_ERR;
}

// Set viewport y safely
int bview_set_viewport_y(bview_t *self, bint_t y, int do_rectify) {
    if (y < 0) {
        y = 0;
    } else if (y >= self->buffer->line_count) {
        y = self->buffer->line_count - 1;
    }
    mark_move_to(self->viewport_mark, y, 0);
    if (do_rectify) bview_rectify_viewport(self);
    return MLE_OK;
}

// Center the viewport vertically
int bview_center_viewport_y(bview_t *self) {
    bint_t center;
    center = self->active_cursor->mark->bline->line_index - self->rect_buffer.h/2;
    if (center < 0) center = 0;
    return bview_set_viewport_y(self, center, 1);
}

// Zero the viewport vertically
int bview_zero_viewport_y(bview_t *self) {
    return bview_set_viewport_y(self, self->active_cursor->mark->bline->line_index, 1);
}

// Maximize the viewport vertically
int bview_max_viewport_y(bview_t *self) {
    bint_t max;
    max = self->active_cursor->mark->bline->line_index - self->rect_buffer.h;
    if (max < 0) max = 0;
    return bview_set_viewport_y(self, max, 1);
}

// Rectify the viewport
int bview_rectify_viewport(bview_t *self) {
    mark_t *mark;
    bint_t viewport_y;

    mark = self->active_cursor->mark;
    viewport_y = self->viewport_mark->bline->line_index;

    // Rectify each dimension of the viewport
    MLBUF_BLINE_ENSURE_CHARS(mark->bline);
    _bview_rectify_viewport_dim(self, mark->bline, MLE_MARK_COL_TO_VCOL(mark), self->viewport_scope_x, self->rect_buffer.w, &self->viewport_x_vcol);
    bline_get_col_from_vcol(mark->bline, self->viewport_x_vcol, &(self->viewport_x));

    if (_bview_rectify_viewport_dim(self, mark->bline, mark->bline->line_index, self->viewport_scope_y, self->rect_buffer.h, &viewport_y)) {
        // TODO viewport_y_vrow (soft-wrapped lines, code folding, etc)
        // Adjust viewport_mark
        mark_move_to(self->viewport_mark, viewport_y, 0);
    }

    return MLE_OK;
}

// Add a listener
int bview_add_listener(bview_t *self, bview_listener_cb_t callback, void *udata) {
    bview_listener_t *listener;
    listener = calloc(1, sizeof(bview_listener_t));
    listener->callback = callback;
    listener->udata = udata;
    DL_APPEND(self->listeners, listener);
    return MLE_OK;
}

// Remove and free a listener
int bview_destroy_listener(bview_t *self, bview_listener_t *listener) {
    DL_DELETE(self->listeners, listener);
    free(listener);
    return MLE_OK;
}

// Pop a kmap
static int _bview_pop_kmap(bview_t *bview, kmap_t **optret_kmap, int allow_pop_root) {
    kmap_node_t *node_to_pop;
    node_to_pop = bview->kmap_tail;
    if (!node_to_pop || (!allow_pop_root && node_to_pop == bview->kmap_stack)) {
        return MLE_ERR;
    }
    if (optret_kmap) {
        *optret_kmap = node_to_pop->kmap;
    }
    bview->kmap_tail = node_to_pop->prev != node_to_pop ? node_to_pop->prev : NULL;
    DL_DELETE(bview->kmap_stack, node_to_pop);
    free(node_to_pop);
    return MLE_OK;
}

// Rectify a viewport dimension. Return 1 if changed, else 0.
static int _bview_rectify_viewport_dim(bview_t *self, bline_t *bline, bint_t vpos, int dim_scope, int dim_size, bint_t *view_vpos) {
    int rc;
    bint_t vpos_start;
    bint_t vpos_stop;
    (void)self;
    (void)bline;

    // Find bounds
    if (dim_scope < 0) {
        // Keep cursor at least `dim_scope` cells away from edge
        // Remember dim_scope is negative here
        dim_scope = MLE_MAX(dim_scope, ((dim_size / 2) * -1));
        vpos_start = *view_vpos - dim_scope; // N in from left edge
        vpos_stop = (*view_vpos + dim_size) + dim_scope; // N in from right edge
    } else {
        // Keep cursor within `dim_scope/2` cells of midpoint
        dim_scope = MLE_MIN(dim_scope, dim_size);
        vpos_start = (*view_vpos + (dim_size / 2)) - (int)floorf((float)dim_scope * 0.5); // -N/2 from midpoint
        vpos_stop = (*view_vpos + (dim_size / 2)) + (int)ceilf((float)dim_scope * 0.5); // +N/2 from midpoint
    }

    // Rectify
    rc = 1;
    if (vpos < vpos_start) {
        *view_vpos -= MLE_MIN(*view_vpos, vpos_start - vpos);
    } else if (vpos >= vpos_stop) {
        *view_vpos += ((vpos - vpos_stop) + 1);
    } else {
        rc = 0;
    }

    return rc;
}

// Init a bview with a buffer
static void _bview_init(bview_t *self, buffer_t *buffer) {
    cursor_t *cursor_tmp;
    kmap_t *kmap_init;

    _bview_deinit(self);

    // Reference buffer
    self->buffer = buffer;
    self->buffer->ref_count += 1;
    _bview_set_linenum_width(self);

    // Push normal mode
    kmap_init = _bview_get_init_kmap(self->editor);
    if (kmap_init != self->editor->kmap_normal) {
        // Make kmap_normal the bottom if init kmap isn't kmap_normal
        bview_push_kmap(self, self->editor->kmap_normal);
    }
    bview_push_kmap(self, kmap_init);
    if (self->editor->kmap_basic) bview_push_kmap(self, self->editor->kmap_basic);

    // Set syntax
    bview_set_syntax(self, NULL);

    // Add a cursor
    bview_add_cursor(self, self->buffer->first_line, 0, &cursor_tmp);

    // Add viewport_mark
    self->viewport_mark = buffer_add_mark(self->buffer, NULL, 0);
    self->viewport_mark->lefty = 1; // Stay put at col 0
}

// Invoked once after a bview has been resized for the first time
static void _bview_init_resized(bview_t *self) {
    // Move cursor to startup line if present
    if (self->startup_linenum > 0) {
        mark_move_to(self->active_cursor->mark, self->startup_linenum, 0);
        bview_center_viewport_y(self);
    }
}

// Return initial kmap to use
static kmap_t *_bview_get_init_kmap(editor_t *editor) {
    if (!editor->kmap_init) {
        if (editor->kmap_init_name) {
            HASH_FIND_STR(editor->kmap_map, editor->kmap_init_name, editor->kmap_init);
        }
        if (!editor->kmap_init) {
            editor->kmap_init = editor->kmap_normal;
        }
    }
    return editor->kmap_init;
}

// Called by mlbuf after edits
static void _bview_buffer_callback(buffer_t *buffer, baction_t *action, void *udata) {
    editor_t *editor;
    bview_t *self;
    bview_t *active;
    bview_listener_t *listener;

    self = (bview_t*)udata;
    editor = self->editor;
    active = editor->active;

    // Rectify viewport if edit was on active bview
    if (active->buffer == buffer) {
        bview_rectify_viewport(active);
    }

    if (action && action->line_delta != 0) {
        bview_t *bview;
        bview_t *tmp1;
        bview_t *tmp2;
        CDL_FOREACH_SAFE2(editor->all_bviews, bview, tmp1, tmp2, all_prev, all_next) {
            if (bview->buffer == buffer) {
                // Adjust linenum_width
                if (_bview_set_linenum_width(bview)) {
                    bview_resize(bview, bview->x, bview->y, bview->w, bview->h);
                }
            }
        }
    }

    // Call bview listeners
    DL_FOREACH(self->listeners, listener) {
        listener->callback(self, action, listener->udata);
    }

    // Notify event observers
    editor_notify_observers(editor, "buffer:baction", (void*)action);
}

// Set linenum_width and return 1 if changed
static int _bview_set_linenum_width(bview_t *self) {
    int orig;
    orig = self->linenum_width;
    self->abs_linenum_width = MLE_MAX(1, (int)(floor(log10((double)self->buffer->line_count))) + 1);
    if (self->editor->linenum_type != MLE_LINENUM_TYPE_ABS) {
        self->rel_linenum_width = MLE_MAX(
            self->editor->linenum_type == MLE_LINENUM_TYPE_BOTH ? 1 : self->abs_linenum_width,
            (int)(floor(log10((double)self->rect_buffer.h))) + 1
        );
    } else {
        self->rel_linenum_width = 0;
    }
    if (self->editor->linenum_type == MLE_LINENUM_TYPE_NONE) {
        self->abs_linenum_width = 0;
        self->rel_linenum_width = 0;
        self->linenum_width = 0;
        return orig == self->linenum_width ? 0 : 1;
    }
    if (self->editor->linenum_type == MLE_LINENUM_TYPE_ABS) {
        self->linenum_width = self->abs_linenum_width;
    } else if (self->editor->linenum_type == MLE_LINENUM_TYPE_REL) {
        self->linenum_width = self->abs_linenum_width > self->rel_linenum_width ? self->abs_linenum_width : self->rel_linenum_width;
    } else if (self->editor->linenum_type == MLE_LINENUM_TYPE_BOTH) {
        self->linenum_width = self->abs_linenum_width + 1 + self->rel_linenum_width;
    }
    return orig == self->linenum_width ? 0 : 1;
}

// Deinit a bview
static void _bview_deinit(bview_t *self) {
    bview_listener_t *listener;
    bview_listener_t *listener_tmp;

    // Remove all kmaps
    while (self->kmap_tail) {
        _bview_pop_kmap(self, NULL, 1);
    }

    // Remove all syntax rules
    if (self->syntax) {
        srule_node_t *srule_node;
        buffer_set_styles_enabled(self->buffer, 0);
        DL_FOREACH(self->syntax->srules, srule_node) {
            buffer_remove_srule(self->buffer, srule_node->srule);
        }
        buffer_set_styles_enabled(self->buffer, 1);
    }

    // Remove all cursors
    while (self->active_cursor) {
        bview_remove_cursor(self, self->active_cursor);
    }

    // Destroy async proc
    if (self->aproc) {
        aproc_destroy(self->aproc, 1);
        self->aproc = NULL;
    }

    // Remove all listeners
    DL_FOREACH_SAFE(self->listeners, listener, listener_tmp) {
        bview_destroy_listener(self, listener);
    }

    // Dereference/free buffer
    if (self->buffer) {
        self->buffer->ref_count -= 1;
        if (self->buffer->ref_count < 1) {
            buffer_destroy(self->buffer);
        }
        self->buffer = NULL;
    }

    // Unset viewport_mark as it may point to a freed mark at this point if
    // we are re-using a bview, e.g., after cmd_open_replace_file.
    self->viewport_mark = NULL;

    // Free last_search
    if (self->last_search) {
        free(self->last_search);
        self->last_search = NULL;
    }

    // Free isearch_ranges
    if (self->isearch_ranges) {
        free(self->isearch_ranges);
        self->isearch_ranges = NULL;
        self->isearch_ranges_len = 0;
        self->isearch_ranges_cap = 0;
    }
}

// Set syntax on bview buffer
int bview_set_syntax(bview_t *self, char *opt_syntax) {
    syntax_t *syntax;
    syntax_t *use_syntax;
    srule_node_t *srule_node;

    // Only set syntax on edit bviews
    if (!MLE_BVIEW_IS_EDIT(self)) {
        return MLE_ERR;
    }

    use_syntax = NULL;
    if (opt_syntax) {
        // Set by opt_syntax
        HASH_FIND_STR(self->editor->syntax_map, opt_syntax, use_syntax);
    } else if (self->editor->is_in_init && self->editor->syntax_override) {
        // Set by override at init
        HASH_FIND_STR(self->editor->syntax_map, self->editor->syntax_override, use_syntax);
    } else if (self->buffer->path) {
        // Set by path. syntax_last is the newest rule, so specific languages
        // registered after syn_text win, and syn_text catches everything else.
        for (syntax = self->editor->syntax_last; syntax != NULL; syntax = syntax->hh.prev) {
            if (util_pcre_match(syntax->path_pattern, self->buffer->path, strlen(self->buffer->path), NULL, NULL)) {
                use_syntax = syntax;
                break;
            }
        }
    } else {
        HASH_FIND_STR(self->editor->syntax_map, "syn_text", use_syntax);
    }

    buffer_set_styles_enabled(self->buffer, 0);

    // Remove current syntax
    if (self->syntax) {
        DL_FOREACH(self->syntax->srules, srule_node) {
            buffer_remove_srule(self->buffer, srule_node->srule);
            self->syntax = NULL;
        }
    }

    // Set syntax if found
    if (use_syntax) {
        DL_FOREACH(use_syntax->srules, srule_node) {
            buffer_add_srule(self->buffer, srule_node->srule);
        }
        self->syntax = use_syntax;
    }

    // Set tab settings
    self->tab_to_space = (use_syntax && use_syntax->tab_to_space >= 0)
        ? use_syntax->tab_to_space
        : self->editor->tab_to_space;
    _bview_set_tab_width(self, (use_syntax && use_syntax->tab_width >= 1)
        ? use_syntax->tab_width
        : self->editor->tab_width
    );

    buffer_set_styles_enabled(self->buffer, 1);

    return use_syntax ? MLE_OK : MLE_ERR;
}

static void _bview_set_tab_width(bview_t *self, int tab_width) {
    self->tab_width = tab_width;
    if (self->buffer && self->buffer->tab_width != self->tab_width) {
        buffer_set_tab_width(self->buffer, self->tab_width);
    }
}

// Attempt to fix path by stripping away git-style diff prefixes ([ab/]) and/or
// by extracting a trailing line number after a colon (:)
static void _bview_fix_path(bview_t *self, char *path, int path_len, char **ret_path, int *ret_path_len, bint_t *ret_line_num) {
    char *tmp;
    int tmp_len;
    char *colon;
    int is_valid;
    int fix_nudge;
    int fix_len;
    bint_t line_num;
    (void)self;

    fix_nudge = 0;
    fix_len = path_len;
    line_num = 0;

    // Path already valid?
    if (util_is_file(path, NULL, NULL) || util_is_dir(path)) {
        goto _bview_fix_path_ret;
    }

    // Path valid if we strip "[ab]/" prefix?
    if (path_len >= 3
        && (strncmp(path, "a/", 2) == 0 || strncmp(path, "b/", 2) == 0)
        && (util_is_file(path+2, NULL, NULL) || util_is_dir(path+2))
    ) {
        fix_nudge = 2;
        fix_len -= 2;
        goto _bview_fix_path_ret;
    }

    // Path valid if we extract line num after colon?
    if ((colon = strrchr(path, ':')) != NULL) {
        tmp_len = colon - path;
        tmp = strndup(path, tmp_len);
        is_valid = util_is_file(tmp, NULL, NULL) ? 1 : 0;
        free(tmp);
        if (is_valid) {
            fix_len = tmp_len;
            line_num = strtoul(colon + 1, NULL, 10);
            goto _bview_fix_path_ret;
        }
    }

    // Path valid if we strip "[ab]/" prefix and extract line num?
    if (path_len >= 3
        && (strncmp(path, "a/", 2) == 0 || strncmp(path, "b/", 2) == 0)
        && (colon = strrchr(path, ':')) != NULL
    ) {
        tmp_len = (colon - path) - 2;
        tmp = strndup(path+2, tmp_len);
        is_valid = util_is_file(tmp, NULL, NULL) ? 1 : 0;
        free(tmp);
        if (is_valid) {
            fix_nudge = 2;
            fix_len = tmp_len;
            line_num = strtoul(colon + 1, NULL, 10);
            goto _bview_fix_path_ret;
        }
    }

_bview_fix_path_ret:
    *ret_path = strndup(path + fix_nudge, fix_len);
    *ret_path_len = strlen(*ret_path);
    *ret_line_num = line_num > 0 ? line_num - 1 : 0;
}

// Open a buffer with an optional path to load, otherwise empty
static buffer_t *_bview_open_buffer(bview_t *self, char *opt_path, int opt_path_len) {
    buffer_t *buffer;
    int has_path, fix_path_len, exp_path_len;
    char *fix_path, *exp_path;
    bint_t startup_line_num;
    int buffer_errno = 0;

    buffer = NULL;
    has_path = opt_path && opt_path_len > 0 ? 1 : 0;

    if (has_path) {
        util_expand_tilde(opt_path, opt_path_len, &exp_path, &exp_path_len);
        _bview_fix_path(self, exp_path, exp_path_len, &fix_path, &fix_path_len, &startup_line_num);
        buffer = buffer_new_open(fix_path, &buffer_errno);
        if (buffer) self->startup_linenum = startup_line_num;
        free(fix_path);
        free(exp_path);
    }
    if (!buffer) {
        buffer = buffer_new();
        if (has_path) buffer->path = strndup(opt_path, opt_path_len);
        if (buffer_errno != 0 && buffer_errno != ENOENT) {
            MLE_SET_ERR(self->editor, "_bview_open_buffer: %s", strerror(buffer_errno));
        }
    }
    buffer_set_callback(buffer, _bview_buffer_callback, self);
    buffer_set_action_group_ptr(buffer, &self->editor->user_input_count);
    _bview_set_tab_width(self, self->tab_width);
    return buffer;
}

static void _bview_draw_prompt(bview_t *self) {
    _bview_draw_bline(self, self->buffer->first_line, 0, NULL, NULL);
}

static int _status_cols(const char *s) {
    int cols = 0;
    if (!s) return 0;
    while (*s) {
        unsigned char ch = (unsigned char)*s;
        int adv = 1;
        if ((ch & 0x80) == 0) adv = 1;
        else if ((ch & 0xE0) == 0xC0) adv = s[1] ? 2 : 1;
        else if ((ch & 0xF0) == 0xE0) adv = (s[1] && s[2]) ? 3 : 1;
        else if ((ch & 0xF8) == 0xF0) adv = (s[1] && s[2] && s[3]) ? 4 : 1;
        s += adv;
        cols += 1;
    }
    return cols;
}

static void _bview_draw_status(bview_t *self) {
    editor_t *editor;
    bview_t *active;
    bview_t *active_edit;
    mark_t *mark;

    editor = self->editor;
    active = editor->active;
    active_edit = editor->active_edit;

    // Prompt
    if (active == editor->prompt) {
        mle_theme_t *th = theme_current(editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        tb_printf_rect(editor->rect_status, 0, 0, classic ? (TB_GREEN | TB_BOLD) : th->accent_fg, classic ? TB_BLACK : th->accent_bg, "%-*.*s", editor->rect_status.w, editor->rect_status.w, self->editor->prompt->prompt_str);
        goto _bview_draw_status_end;
    }
    if (active_edit && active_edit->is_menu && editor->active_edit_last) active_edit = editor->active_edit_last;
    if (!active_edit || !active_edit->buffer || !active_edit->active_cursor) {
        mle_theme_t *th = theme_current(editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        tb_printf_rect(editor->rect_status, 0, 0, classic ? TB_WHITE : th->muted_fg, classic ? TB_BLACK : th->muted_bg, "%*.*s", editor->rect_status.w, editor->rect_status.w, " ");
        goto _bview_draw_status_end;
    }
    mark = active_edit->active_cursor->mark;

    bint_t anchor_len;
    bint_t anchor_nlines;
    cursor_t *cursor;
    bview_t *bview_tmp;
    int bview_count;
    int bview_num;
    int x;
    int n;
    char chunk[160];
    char git[80];
    const char *fname;
    const char *base;
    mle_theme_t *th;
    int classic;

    th = theme_current(editor);
    classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
    cursor = active_edit->active_cursor;
    anchor_len = 0;
    anchor_nlines = 0;
    if (cursor && cursor->is_anchored) {
        mark_get_nchars_between(cursor->anchor, cursor->mark, &anchor_len);
        if (anchor_len < 0) anchor_len *= -1;
        anchor_nlines = cursor->anchor->bline->line_index - cursor->mark->bline->line_index;
        if (anchor_nlines < 0) anchor_nlines *= -1;
        if (anchor_len > 0) anchor_nlines += 1;
    }

    bview_count = 0;
    bview_num = 0;
    CDL_FOREACH2(editor->all_bviews, bview_tmp, all_next) {
        if (!MLE_BVIEW_IS_EDIT(bview_tmp) || bview_tmp->is_menu) continue;
        bview_count += 1;
        if (bview_tmp == active_edit) bview_num = bview_count;
    }

    MLBUF_BLINE_ENSURE_CHARS(mark->bline);
    tb_printf_rect(editor->rect_status, 0, 0, classic ? TB_WHITE : th->bar_fg, classic ? TB_BLACK : th->bar_bg, "%*.*s", editor->rect_status.w, editor->rect_status.w, " ");
    editor->status_file_x = 0;
    editor->status_file_w = 0;
    editor->status_pos_x = 0;
    editor->status_pos_w = 0;
    editor->status_git_x = 0;
    editor->status_git_w = 0;

    {
        char right[4][80];
        int rfg[4], rbg[4], rw[4], nr, rwsum, rx, i;
        nr = 0;
        rwsum = 0;
        git[0] = '\0';
        side_git_label(editor, git, sizeof(git));
        if (git[0]) {
            snprintf(right[nr], sizeof(right[nr]), " %.74s ", git);
            rfg[nr] = classic ? (TB_BLACK | TB_BOLD) : th->git_fg;
            rbg[nr] = classic ? TB_GREEN : th->git_bg;
            rw[nr] = _status_cols(right[nr]);
            rwsum += rw[nr];
            nr += 1;
        }
        snprintf(right[nr], sizeof(right[nr]), " %s ", active_edit->syntax ? active_edit->syntax->name : "text");
        rfg[nr] = classic ? TB_BLACK : th->accent_fg;
        rbg[nr] = classic ? TB_WHITE : th->accent_bg;
        rw[nr] = _status_cols(right[nr]);
        rwsum += rw[nr] + (nr ? 1 : 0);
        nr += 1;
        if (workspace_status(editor)[0]) {
            snprintf(right[nr], sizeof(right[nr]), " %s ", workspace_status(editor));
            rfg[nr] = classic ? TB_BLACK : th->muted_fg;
            rbg[nr] = classic ? TB_WHITE : th->muted_bg;
            rw[nr] = _status_cols(right[nr]);
            rwsum += rw[nr] + 1;
            nr += 1;
        }
        if (editor->is_recording_macro || editor->aprocs || active->is_menu) {
            const char *flag = editor->is_recording_macro ? "REC" : (editor->aprocs ? "RUN" : "menu");
            snprintf(right[nr], sizeof(right[nr]), " %s ", flag);
            if (editor->is_recording_macro) {
                rfg[nr] = th->err_fg;
                rbg[nr] = th->err_bg;
            } else if (editor->aprocs) {
                rfg[nr] = classic ? (TB_BLACK | TB_BOLD) : th->pos_fg;
                rbg[nr] = classic ? TB_YELLOW : th->pos_bg;
            } else {
                rfg[nr] = classic ? TB_BLACK : th->bar_fg;
                rbg[nr] = classic ? TB_WHITE : th->bar_bg;
            }
            rw[nr] = _status_cols(right[nr]);
            rwsum += rw[nr] + 1;
            nr += 1;
        }

        fname = active_edit->buffer->path;
        base = fname ? strrchr(fname, '/') : NULL;
        base = (base && base[1]) ? base + 1 : (fname ? fname : "<unsaved>");
        {
            int room = editor->rect_status.w - rwsum - 8;
            int keep;
            if (room < 8) room = 8;
            if (room > (int)sizeof(chunk) - 1) room = (int)sizeof(chunk) - 1;
            keep = room > 4 ? room - 4 : 1;
            n = snprintf(chunk, sizeof(chunk), " %.*s%s ", keep, base, active_edit->buffer->is_unsaved ? "*" : "");
        }
        if (n < 0) n = 0;
        if (n >= (int)sizeof(chunk)) n = (int)sizeof(chunk) - 1;
        if (n > editor->rect_status.w) n = editor->rect_status.w;
        x = 0;
        tb_print(x, editor->rect_status.y, th->file_fg, th->file_bg, chunk);
        editor->status_file_x = x;
        editor->status_file_w = _status_cols(chunk);
        if (editor->status_file_w > n) editor->status_file_w = n;
        x += editor->status_file_w;

        n = snprintf(chunk, sizeof(chunk), " %d/%d ", bview_num, bview_count);
        if (n > 0 && x + n + rwsum < editor->rect_status.w) {
            tb_print(x, editor->rect_status.y, classic ? TB_WHITE : th->caption_fg, classic ? TB_BLACK : th->caption_bg, chunk);
            x += n;
        }

        n = snprintf(chunk, sizeof(chunk), " %llu:%llu ",
            (unsigned long long)(mark->bline->line_index + 1),
            (unsigned long long)mark->col);
        if (n > 0 && x + n + rwsum < editor->rect_status.w) {
            tb_print(x, editor->rect_status.y, classic ? (TB_BLACK | TB_BOLD) : th->pos_fg, classic ? TB_CYAN : th->pos_bg, chunk);
            editor->status_pos_x = x;
            editor->status_pos_w = n;
            x += n;
        }

        if (anchor_len > 0) {
            n = snprintf(chunk, sizeof(chunk), " %lld ", (long long)anchor_len);
            if (n > 0 && x + n + rwsum < editor->rect_status.w) {
                tb_print(x, editor->rect_status.y, th->accent_fg, th->sel_bg ? th->sel_bg : th->accent_bg, chunk);
                x += n;
            }
        }

        rx = editor->rect_status.w - rwsum;
        if (rx < x + 1) rx = x + 1;
        for (i = nr - 1; i >= 0; i--) {
            rx -= rw[i];
            if (rx >= x) {
                tb_print(rx, editor->rect_status.y, rfg[i], rbg[i], right[i]);
                if (git[0] && i == 0) {
                    editor->status_git_x = rx;
                    editor->status_git_w = rw[i];
                }
            }
            rx -= 1;
        }
    }
    (void)anchor_nlines;

    // Overlay errstr if present
_bview_draw_status_end:
    if (editor->errstr[0] != '\0') {
        int errstrlen = strlen(editor->errstr) + 5; // Add 5 for "err! "
        {
            mle_theme_t *th = theme_current(editor);
            tb_printf_rect(editor->rect_status, editor->rect_status.w - errstrlen, 0, th->err_fg, th->err_bg, "err! %s", editor->errstr);
        }
        editor->errstr[0] = '\0'; // Clear errstr
    } else if (editor->infostr[0] != '\0') {
        int infostrlen = strlen(editor->infostr);
        {
            mle_theme_t *th = theme_current(editor);
            int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
            tb_printf_rect(editor->rect_status, editor->rect_status.w - infostrlen, 0, classic ? TB_WHITE : th->text_fg, classic ? 0 : th->bar_bg, "%s", editor->infostr);
        }
        editor->infostr[0] = '\0'; // Clear errstr
    }
}

static void _bview_draw_edit(bview_t *self, int x, int y, int w, int h) {
    int split_w;
    int split_h;
    int min_w;
    int min_h;
    int rect_y;
    int fg_attr;
    int bg_attr;
    bline_t *bline;

    // Handle split
    if (self->split_child) {
        // Calc split dimensions
        if (self->split_is_vertical) {
            split_w = w - (int)((float)w * self->split_factor);
            split_h = h;
        } else {
            split_w = w;
            split_h = h - (int)((float)h * self->split_factor);
        }

        // Draw child
        _bview_draw_edit(self->split_child, x + (w - split_w), y + (h - split_h), split_w, split_h);

        // Continue drawing self minus split dimensions
        w -= (w - split_w);
        h -= (h - split_h);
    }

    // Calc min dimensions
    min_w = self->linenum_width + 3;
    min_h = 2;

    // Ensure renderable
    if (w < min_w || h < min_h
        || x + w > self->editor->w
        || y + h > self->editor->h
    ) {
        return;
    }

    // Render caption. The stock mle theme keeps the old blue bar.
    {
        mle_theme_t *th = theme_current(self->editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        if (self->editor->active_edit == self) {
            fg_attr = classic ? TB_BOLD : th->caption_fg;
            bg_attr = classic ? TB_BLUE : th->caption_bg;
        } else if (classic) {
            fg_attr = 0;
            bg_attr = 0;
        } else {
            fg_attr = th->muted_fg;
            bg_attr = th->muted_bg;
        }
    }
    tb_printf_rect(self->rect_caption, 0, 0, fg_attr, bg_attr, "%*.*s", self->rect_caption.w, self->rect_caption.w, " ");
    if (self->buffer->path) {
        tb_printf_rect(self->rect_caption, 0, 0, fg_attr, bg_attr, "%*.s%s %c",
            self->linenum_width, " ",
            self->buffer->path, self->buffer->is_unsaved ? '*' : ' ');
    } else {
        tb_printf_rect(self->rect_caption, 0, 0, fg_attr, bg_attr, "%*.s<buffer-%p> %c",
            self->linenum_width, " ",
            self->buffer, self->buffer->is_unsaved ? '*' : ' ');
    }

    // Render lines and margins
    bline = codeintel_screen_line(self->editor, self, self->viewport_mark->bline, 0);
    for (rect_y = 0; rect_y < self->rect_buffer.h; rect_y++) {
        if (!bline) {
            // Draw pre/post blank
            mle_theme_t *th = theme_current(self->editor);
            int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
            int bfg = classic ? (TB_BLACK | TB_BRIGHT) : th->text_fg;
            int bbg = classic ? 0 : th->text_bg;
            if (self->linenum_width > 0) tb_printf_rect(self->rect_lines, 0, rect_y, bfg, bbg, "%*c", self->linenum_width, '~');
            tb_printf_rect(self->rect_margin_left, 0, rect_y, bfg, bbg, "%c", ' ');
            tb_printf_rect(self->rect_margin_right, 0, rect_y, bfg, bbg, "%c", ' ');
            tb_printf_rect(self->rect_buffer, 0, rect_y, bfg, bbg, "%-*.*s", self->rect_buffer.w, self->rect_buffer.w, " ");
        } else {
            // Draw bline at self->rect_buffer self->viewport_mark + rect_y
            _bview_draw_bline(self, bline, rect_y, &bline, &rect_y);
            bline = codeintel_next_visible_line(self->editor, self, bline);
        }
    }
    _bview_draw_scrollbars(self);
}

static int _bview_scrollbar_thumb(int track, bint_t total, bint_t visible, bint_t offset, int *thumb_start, int *thumb_size) {
    bint_t max_offset;
    int size;
    int start;
    if (!thumb_start || !thumb_size || track < 1) return MLE_ERR;
    if (visible < 1) visible = 1;
    if (total < visible) total = visible;
    max_offset = total - visible;
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    size = max_offset > 0 ? (int)(((bint_t)track * visible) / total) : track;
    if (size < 1) size = 1;
    if (size > track) size = track;
    start = max_offset > 0 && track > size
        ? (int)(((bint_t)(track - size) * offset) / max_offset)
        : 0;
    *thumb_start = start;
    *thumb_size = size;
    return MLE_OK;
}

int bview_debug_scrollbar(int track, bint_t total, bint_t visible, bint_t offset, int *thumb_start, int *thumb_size) {
    return _bview_scrollbar_thumb(track, total, visible, offset, thumb_start, thumb_size);
}

static bint_t _bview_horizontal_total(bview_t *self) {
    bline_t *bline;
    bint_t total;
    if (!self || !self->active_cursor || !self->active_cursor->mark) return 1;
    bline = self->active_cursor->mark->bline;
    MLBUF_BLINE_ENSURE_CHARS(bline);
    total = bline->char_vwidth + 1;
    if (total < self->rect_buffer.w) total = self->rect_buffer.w;
    return total;
}

static void _bview_set_viewport_x_vcol(bview_t *self, bint_t x) {
    bline_t *bline;
    bint_t max_x;
    if (!self || !self->active_cursor || !self->active_cursor->mark) return;
    bline = self->active_cursor->mark->bline;
    MLBUF_BLINE_ENSURE_CHARS(bline);
    max_x = _bview_horizontal_total(self) - self->rect_buffer.w;
    if (max_x < 0) max_x = 0;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    self->viewport_x_vcol = x;
    bline_get_col_from_vcol(bline, x, &self->viewport_x);
}

static void _bview_set_scroll_y(bview_t *self, bint_t y) {
    bint_t max_y;
    if (!self || !self->buffer) return;
    max_y = self->buffer->line_count - self->rect_buffer.h;
    if (max_y < 0) max_y = 0;
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    bview_set_viewport_y(self, y, 0);
}

static void _bview_draw_scrollbars(bview_t *self) {
    mle_theme_t *th;
    int classic;
    uint16_t track_fg;
    uint16_t track_bg;
    uint16_t thumb_fg;
    uint16_t thumb_bg;
    uint32_t htrack;
    uint32_t vtrack;
    uint32_t thumb;
    int i;
    int start;
    int size;
    int track;
    bint_t total;
    if (!self || self->rect_scroll_h.h < 1 || self->rect_buffer.h < 1) return;
    th = theme_current(self->editor);
    classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
    track_fg = classic ? TB_WHITE : th->muted_fg;
    track_bg = classic ? TB_BLACK : th->muted_bg;
    thumb_fg = classic ? TB_WHITE : th->muted_fg;
    thumb_bg = classic ? (TB_BLACK | TB_BRIGHT) : th->muted_bg;
    htrack = glyphs_decor() ? 0x2500 : (uint32_t)'-';
    vtrack = glyphs_decor() ? 0x2502 : (uint32_t)'|';
    thumb = glyphs_decor() ? 0x2588 : (uint32_t)'#';

    for (i = self->x; i < self->rect_scroll_h.x; i++) {
        tb_set_cell(i, self->rect_scroll_h.y, ' ', track_fg, track_bg);
    }
    for (i = 0; i < self->rect_scroll_h.w; i++) {
        tb_set_cell(self->rect_scroll_h.x + i, self->rect_scroll_h.y, htrack, track_fg, track_bg);
    }
    if (self->rect_scroll_h.w >= 2) {
        tb_set_cell(self->rect_scroll_h.x, self->rect_scroll_h.y,
            glyphs_decor() ? glyphs_cp(GLY_LEFT) : (uint32_t)'<', thumb_fg, track_bg);
        tb_set_cell(self->rect_scroll_h.x + self->rect_scroll_h.w - 1, self->rect_scroll_h.y,
            glyphs_decor() ? glyphs_cp(GLY_RIGHT) : (uint32_t)'>', thumb_fg, track_bg);
    }
    track = self->rect_scroll_h.w - 2;
    if (track > 0) {
        total = _bview_horizontal_total(self);
        _bview_scrollbar_thumb(track, total, self->rect_buffer.w, self->viewport_x_vcol, &start, &size);
        for (i = 0; i < size; i++) {
            tb_set_cell(self->rect_scroll_h.x + 1 + start + i, self->rect_scroll_h.y, thumb, thumb_fg, thumb_bg);
        }
    }

    for (i = 0; i < self->rect_margin_right.h; i++) {
        tb_set_cell(self->rect_margin_right.x, self->rect_margin_right.y + i, vtrack, track_fg, track_bg);
    }
    if (self->rect_margin_right.h >= 2) {
        tb_set_cell(self->rect_margin_right.x, self->rect_margin_right.y,
            glyphs_decor() ? glyphs_cp(GLY_UP) : (uint32_t)'^', thumb_fg, track_bg);
        tb_set_cell(self->rect_margin_right.x, self->rect_margin_right.y + self->rect_margin_right.h - 1,
            glyphs_decor() ? glyphs_cp(GLY_DOWN) : (uint32_t)'v', thumb_fg, track_bg);
    }
    track = self->rect_margin_right.h - 2;
    if (track > 0) {
        _bview_scrollbar_thumb(track, self->buffer->line_count, self->rect_buffer.h,
            self->viewport_mark->bline->line_index, &start, &size);
        for (i = 0; i < size; i++) {
            tb_set_cell(self->rect_margin_right.x, self->rect_margin_right.y + 1 + start + i, thumb, thumb_fg, thumb_bg);
        }
    }
}

static void _bview_drag_scrollbar(bview_t *self, int pos) {
    int track;
    int start;
    int size;
    int thumb_pos;
    bint_t max_offset;
    bint_t offset;
    bint_t total;
    bint_t visible;
    if (self->scroll_drag == 1) {
        track = self->rect_scroll_h.w - 2;
        total = _bview_horizontal_total(self);
        visible = self->rect_buffer.w;
        pos -= self->rect_scroll_h.x + 1;
    } else {
        track = self->rect_margin_right.h - 2;
        total = self->buffer->line_count;
        visible = self->rect_buffer.h;
        pos -= self->rect_margin_right.y + 1;
    }
    if (track < 1) return;
    _bview_scrollbar_thumb(track, total, visible,
        self->scroll_drag == 1 ? self->viewport_x_vcol : self->viewport_mark->bline->line_index,
        &start, &size);
    thumb_pos = pos - self->scroll_drag_offset;
    if (thumb_pos < 0) thumb_pos = 0;
    if (thumb_pos > track - size) thumb_pos = track - size;
    max_offset = total > visible ? total - visible : 0;
    offset = max_offset > 0 && track > size
        ? ((bint_t)thumb_pos * max_offset + (track - size) / 2) / (track - size)
        : 0;
    if (self->scroll_drag == 1) _bview_set_viewport_x_vcol(self, offset);
    else _bview_set_scroll_y(self, offset);
}

int bview_scrollbar_dragging(bview_t *self) {
    if (!self) return 0;
    if (self->scroll_drag) return 1;
    return self->split_child ? bview_scrollbar_dragging(self->split_child) : 0;
}

int bview_handle_scrollbar_mouse(bview_t *self, struct tb_event *ev) {
    int horizontal;
    int pos;
    int first;
    int last;
    int track;
    int start;
    int size;
    bint_t offset;
    bint_t total;
    bint_t visible;
    if (!self || !ev || !MLE_BVIEW_IS_EDIT(self)) return 0;
    if (self->scroll_drag) {
        if (ev->key == TB_KEY_MOUSE_RELEASE) {
            self->scroll_drag = 0;
            self->scroll_drag_offset = 0;
            return 1;
        }
        if (ev->key == TB_KEY_MOUSE_LEFT) {
            _bview_drag_scrollbar(self, self->scroll_drag == 1 ? ev->x : ev->y);
            return 1;
        }
        return 1;
    }
    if (self->split_child && bview_handle_scrollbar_mouse(self->split_child, ev)) return 1;
    if (ev->key != TB_KEY_MOUSE_LEFT || self->rect_scroll_h.h < 1) return 0;
    horizontal = ev->y == self->rect_scroll_h.y
        && ev->x >= self->rect_scroll_h.x
        && ev->x < self->rect_scroll_h.x + self->rect_scroll_h.w;
    if (!horizontal && !(ev->x == self->rect_margin_right.x
        && ev->y >= self->rect_margin_right.y
        && ev->y < self->rect_margin_right.y + self->rect_margin_right.h)) return 0;
    editor_set_active_preserve_viewport(self->editor, self);
    if (horizontal) {
        pos = ev->x;
        first = self->rect_scroll_h.x;
        last = first + self->rect_scroll_h.w - 1;
        total = _bview_horizontal_total(self);
        visible = self->rect_buffer.w;
        offset = self->viewport_x_vcol;
        track = self->rect_scroll_h.w - 2;
    } else {
        pos = ev->y;
        first = self->rect_margin_right.y;
        last = first + self->rect_margin_right.h - 1;
        total = self->buffer->line_count;
        visible = self->rect_buffer.h;
        offset = self->viewport_mark->bline->line_index;
        track = self->rect_margin_right.h - 2;
    }
    if (pos == first) offset -= horizontal ? 4 : 1;
    else if (pos == last) offset += horizontal ? 4 : 1;
    else if (track > 0) {
        _bview_scrollbar_thumb(track, total, visible, offset, &start, &size);
        pos -= first + 1;
        if (pos >= start && pos < start + size) {
            self->scroll_drag = horizontal ? 1 : 2;
            self->scroll_drag_offset = pos - start;
            return 1;
        }
        offset += pos < start ? -visible : visible;
    }
    if (horizontal) _bview_set_viewport_x_vcol(self, offset);
    else _bview_set_scroll_y(self, offset);
    return 1;
}

static void _bview_draw_bline(bview_t *self, bline_t *bline, int rect_y, bline_t **optret_bline, int *optret_rect_y) {
    int rect_x;
    bint_t char_col;
    int fg;
    int bg, tbg;
    uint32_t ch;
    int char_w;
    bint_t viewport_x;
    bint_t viewport_x_vcol;
    int i, j;
    int is_cursor_line;
    int is_soft_wrapped;
    int orig_rect_y;
    srule_t *srule;

    MLBUF_BLINE_ENSURE_CHARS(bline);

    is_cursor_line = _bview_is_cursor_line(self, bline);
    is_soft_wrapped = _bview_is_soft_wrapped(self, bline);
    viewport_x = _bview_get_viewport_x(self, bline);
    viewport_x_vcol = MLE_COL_TO_VCOL(bline, viewport_x);

    // Draw linenums and margins
    if (MLE_BVIEW_IS_EDIT(self) && self->linenum_width > 0) {
        mle_theme_t *th = theme_current(self->editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        int linenum_fg;
        int linenum_bg = 0;
        if (classic) {
            linenum_fg = is_cursor_line ? TB_WHITE : (TB_BLACK | TB_BRIGHT);
        } else {
            linenum_fg = th->text_fg ? th->text_fg : (uint16_t)(TB_BLACK | TB_BRIGHT);
            linenum_fg |= is_cursor_line ? TB_BOLD : TB_DIM;
            linenum_bg = th->text_bg;
            if (self->editor->hl_line && is_cursor_line && !self->is_menu) linenum_bg = th->line_bg;
        }
        if (self->editor->linenum_type == MLE_LINENUM_TYPE_ABS
            || self->editor->linenum_type == MLE_LINENUM_TYPE_BOTH
            || (self->editor->linenum_type == MLE_LINENUM_TYPE_REL && is_cursor_line)
        ) {
            tb_printf_rect(self->rect_lines, 0, rect_y, linenum_fg, linenum_bg, "%*d", self->abs_linenum_width, (int)(bline->line_index + 1) % (int)pow(10, self->linenum_width));
            if (self->editor->linenum_type == MLE_LINENUM_TYPE_BOTH) {
                tb_printf_rect(self->rect_lines, self->abs_linenum_width, rect_y, linenum_fg, linenum_bg, " %*d", self->rel_linenum_width, (int)labs(bline->line_index - self->active_cursor->mark->bline->line_index));
            }
        } else if (self->editor->linenum_type == MLE_LINENUM_TYPE_REL) {
            tb_printf_rect(self->rect_lines, 0, rect_y, linenum_fg, linenum_bg, "%*d", self->rel_linenum_width, (int)labs(bline->line_index - self->active_cursor->mark->bline->line_index));
        }
    }
    if (MLE_BVIEW_IS_EDIT(self)) {
        mle_theme_t *th = theme_current(self->editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        int mbg = classic ? 0 : th->text_bg;
        if (!classic && self->editor->hl_line && is_cursor_line && !self->is_menu) mbg = th->line_bg;
        {
            char mark = codeintel_fold_mark(self->editor, self, bline);
            char git_mark = git_extra_line_mark(self->editor, self, bline->line_index);
            int gfg = classic ? (TB_BLACK | TB_BRIGHT) : (int)(th->muted_fg | TB_DIM);
            if (git_mark != ' ') {
                mark = git_mark;
                if (classic) {
                    if (git_mark == '+') gfg = TB_GREEN;
                    else if (git_mark == '-') gfg = TB_RED;
                    else if (git_mark == '~') gfg = TB_YELLOW;
                } else if (git_mark == '+') gfg = th->git_fg;
                else if (git_mark == '-') gfg = th->err_fg;
                else if (git_mark == '~') gfg = th->string;
            }
            if (viewport_x > 0 && bline->char_count > 0) {
                mark = '^';
                gfg = classic ? (TB_BLACK | TB_BRIGHT) : (int)(th->muted_fg | TB_DIM);
            }
            if (!gfg) gfg = TB_BLACK | TB_BRIGHT;
            tb_printf_rect(self->rect_margin_left, 0, rect_y, gfg, mbg, "%c", mark);
        }
        if (!is_soft_wrapped && bline->char_vwidth - viewport_x_vcol > self->rect_buffer.w) {
            int ofg = classic ? (TB_BLACK | TB_BRIGHT) : (int)(th->muted_fg | TB_DIM);
            tb_printf_rect(self->rect_margin_right, 0, rect_y, ofg ? ofg : (TB_BLACK | TB_BRIGHT), mbg, "%c", '$');
        }
    }

    // Render 0 thru rect_buffer.w cell by cell
    orig_rect_y = rect_y;
    rect_x = 0;
    char_col = viewport_x;
    _bview_populate_isearch_ranges(self, bline);
    while (char_col < bline->char_count) {
        ch = bline->chars[char_col].ch;
        fg = bline->chars[char_col].style.fg;
        bg = bline->chars[char_col].style.bg;
        char_w = char_col == bline->char_count - 1
            ? bline->char_vwidth - bline->chars[char_col].vcol
            : bline->chars[char_col + 1].vcol - bline->chars[char_col].vcol;
        if (ch == '\t') {
            ch = ' ';
        } else if (!iswprint(ch)) {
            ch = '?';
        }
        if (MLE_BVIEW_IS_MENU(self) && is_cursor_line) {
            // Highlight menu line
            bg |= TB_REVERSE;
        }
        if (_bview_is_in_isearch(self, char_col, &srule)) {
        } else if (_bview_is_in_range(bline, char_col, self->active_cursor->is_block, &srule)) {
        } else {
            srule = NULL;
        }
        if (srule) {
            fg = srule->style.fg;
            bg = srule->style.bg;
        } else if (MLE_BVIEW_IS_EDIT(self) || MLE_BVIEW_IS_PROMPT(self)) {
            mle_theme_t *th = theme_current(self->editor);
            fg = theme_synth(self->editor, (uint16_t)fg, 1);
            bg = theme_synth(self->editor, (uint16_t)bg, 0);
            if (self->editor->hl_line && is_cursor_line && !self->is_menu) {
                if ((bg & 0xff) == (th->text_bg & 0xff)) bg = th->line_bg;
            }
        }
        // Draw char_w chars of ch
        for (i = 0; i < char_w && rect_x + i < self->rect_buffer.w; i++) {
            if (MLE_BVIEW_IS_EDIT(self)
                && rect_y == orig_rect_y // not a soft wrapped line
                && self->editor->color_col == rect_x + i + viewport_x_vcol
            ) {
                // Apply color col style
                tbg = bg | TB_RED;
            } else {
                tbg = bg;
            }
            tb_set_cell(self->rect_buffer.x + rect_x + i, self->rect_buffer.y + rect_y, ch, fg, tbg);
        }
        if (i < char_w) {
            // There was not enough width to draw
            if (is_soft_wrapped && rect_y + 1 < self->rect_buffer.h) {
                // Soft wrap to next line
                rect_x = 0;
                rect_y += 1;
                char_w -= i;
                // Draw remaining ch on next line
                for (j = 0; j < char_w && rect_x + j < self->rect_buffer.w; j++) {
                    tb_set_cell(self->rect_buffer.x + j, self->rect_buffer.y + rect_y, ch, fg, bg);
                }
                // Draw ellipsis for line num to indicate soft wrap
                for (j = 0; j < self->linenum_width; j++) {
                    tb_printf_rect(self->rect_lines, j, rect_y, 0, 0, "%c", '.');
                }
            } else {
                // We are off-screen and soft-wrap is not enabled or there is not
                // enough height to soft wrap
                break;
            }
        }
        rect_x += char_w;
        char_col += 1;
    }
    if (MLE_BVIEW_IS_EDIT(self) && rect_y < self->rect_buffer.h) {
        mle_theme_t *th = theme_current(self->editor);
        int classic = th && !th->user && th->text_fg == 0 && th->text_bg == 0;
        int fill_fg = classic ? 0 : (self->editor->hl_line && is_cursor_line && !self->is_menu ? th->line_fg : th->text_fg);
        int fill_bg = classic ? 0 : th->text_bg;
        if (!classic && self->editor->hl_line && is_cursor_line && !self->is_menu) fill_bg = th->line_bg;
        for (i = rect_x; i < self->rect_buffer.w; i++) {
            tb_set_cell(self->rect_buffer.x + i, self->rect_buffer.y + rect_y, ' ', fill_fg, fill_bg);
        }
    }
    for (i = orig_rect_y; i < rect_y && bline->next; i++) {
        bline = bline->next;
    }
    if (optret_bline) *optret_bline = bline;
    if (optret_rect_y) *optret_rect_y = rect_y;
}

// Highlight matching bracket pair under mark
static void _bview_highlight_bracket_pair(bview_t *self, mark_t *mark) {
    bline_t *line;
    bint_t brkt;
    bint_t col;
    mark_t pair;
    int screen_x;
    int screen_y;
    struct tb_cell *cell;

    MLBUF_BLINE_ENSURE_CHARS(mark->bline);

    if (mark_is_at_eol(mark) || !util_get_bracket_pair(mark->bline->chars[mark->col].ch, NULL)) {
        // Not a bracket
        return;
    }
    if (mark_find_bracket_pair(mark, MLE_BRACKET_PAIR_MAX_SEARCH, &line, &col, &brkt) != MLBUF_OK) {
        // No pair found
        return;
    }
    if (mark->bline == line && (mark->col == col - 1 || mark->col == col + 1)) {
        // One char away, do not highlight (looks confusing in UI)
        return;
    }

    pair.bline = line;
    pair.col = col;
    if (bview_get_screen_coords(self, &pair, &screen_x, &screen_y, &cell) != MLE_OK) {
        // Out of bounds
        return;
    }
    tb_set_cell(screen_x, screen_y, cell->ch, cell->fg | TB_UNDERLINE, cell->bg); // TODO configurable
}

// Find screen coordinates for a mark
int bview_get_screen_coords(bview_t *self, mark_t *mark, int *ret_x, int *ret_y, struct tb_cell **optret_cell) {
    int screen_x;
    int screen_y;
    int is_soft_wrapped;
    bint_t viewport_x;
    bline_t *viewport_line;

    MLBUF_BLINE_ENSURE_CHARS(mark->bline);

    is_soft_wrapped = _bview_is_soft_wrapped(self, mark->bline);

    viewport_line = codeintel_screen_line(self->editor, self, self->viewport_mark->bline, 0);
    if (is_soft_wrapped) {
        screen_x = self->rect_buffer.x + MLE_MARK_COL_TO_VCOL(mark) % self->rect_buffer.w;
        screen_y = self->rect_buffer.y + codeintel_visible_distance(self->editor, self, viewport_line, mark->bline) + (MLE_MARK_COL_TO_VCOL(mark) / self->rect_buffer.w);
    } else {
        viewport_x = _bview_get_viewport_x(self, mark->bline);
        screen_x = self->rect_buffer.x + MLE_MARK_COL_TO_VCOL(mark) - MLE_COL_TO_VCOL(mark->bline, viewport_x);
        screen_y = self->rect_buffer.y + codeintel_visible_distance(self->editor, self, viewport_line, mark->bline);
    }
    if (screen_x < self->rect_buffer.x || screen_x >= self->rect_buffer.x + self->rect_buffer.w
       || screen_y < self->rect_buffer.y || screen_y >= self->rect_buffer.y + self->rect_buffer.h
    ) {
        // Out of bounds
        return MLE_ERR;
    }
    *ret_x = screen_x;
    *ret_y = screen_y;
    if (optret_cell) {
        *optret_cell = tb_cell_buffer() + (ptrdiff_t)(tb_width() * screen_y + screen_x);
    }
    return MLE_OK;
}

// Find bline col given a screen coordinate
int bview_screen_to_bline_col(bview_t *self, int x, int y, bview_t **ret_bview, bline_t **ret_bline, bint_t *ret_col) {
    bint_t vcol;
    *ret_bview = NULL;
    *ret_bline = NULL;
    *ret_col = 0;
    if (   x >= self->rect_buffer.x
        && x < self->rect_buffer.x + self->rect_buffer.w
        && y >= self->rect_buffer.y
        && y < self->rect_buffer.y + self->rect_buffer.h
    ) {
        *ret_bline = codeintel_screen_line(self->editor, self, self->viewport_mark->bline, y - self->rect_buffer.y);
        if (*ret_bline) {
            vcol = _bview_get_viewport_x(self, *ret_bline) + (x - self->rect_buffer.x);
            bline_get_col_from_vcol(*ret_bline, vcol, ret_col);
            *ret_bview = self;
            return MLE_OK;
        }
    }
    if (self->split_child) {
        return bview_screen_to_bline_col(self->split_child, x, y, ret_bview, ret_bline, ret_col);
    }
    return MLE_ERR;
}


static bint_t _bview_get_viewport_x(bview_t *self, bline_t *bline) {
    bint_t col;
    if (!self || !bline || self->viewport_x_vcol < 1 || _bview_is_soft_wrapped(self, bline)) return 0;
    MLBUF_BLINE_ENSURE_CHARS(bline);
    col = 0;
    bline_get_col_from_vcol(bline, self->viewport_x_vcol, &col);
    return col;
}

static int _bview_is_cursor_line(bview_t *self, bline_t *bline) {
    return self->active_cursor->mark->bline == bline ? 1 : 0;
}

static int _bview_is_soft_wrapped(bview_t *self, bline_t *bline) {
    return self->soft_wrap
        && MLE_BVIEW_IS_EDIT(self)
        && _bview_is_cursor_line(self, bline) ? 1 : 0;
}

static int _bview_is_in_range(bline_t *bline, bint_t col, int is_block, srule_t **ret_srule) {
    mark_t mark = {0};
    srule_node_t *node;
    mark.bline = bline;
    mark.col = col;
    int rv;
    DL_FOREACH(bline->buffer->range_srules, node) {
        rv = is_block
            ? mark_block_is_between(&mark, node->srule->range_a, node->srule->range_b)
            : mark_is_between(&mark, node->srule->range_a, node->srule->range_b);
        if (rv) {
            *ret_srule = node->srule;
            return 1;
        }
    }
    return 0;
}

static int _bview_is_in_isearch(bview_t *self, bint_t col, srule_t **ret_srule) {
    size_t lo, hi, i;

    if (!self->isearch_rule) return 0;

    lo = 0;
    hi = self->isearch_ranges_len / 2;
    while (lo < hi) {
        i = (lo + hi) / 2;
        if (col < self->isearch_ranges[i * 2]) {
            hi = i;
        } else if (col >= self->isearch_ranges[i * 2 + 1]) {
            lo = i + 1;
        } else {
            *ret_srule = self->isearch_rule;
            return 1;
        }
    }

    return 0;
}

static int _bview_populate_isearch_ranges(bview_t *self, bline_t *bline) {
    int rc;
    PCRE2_SIZE substrs[3];
    pcre2_code *cre;
    bint_t look_offset, start, stop;

    if (!self->isearch_rule) return MLBUF_OK;

    cre = self->isearch_rule->cre;
    self->isearch_ranges_len = 0;
    look_offset = 0;

    while (look_offset < bline->data_len) {
        rc = pcre2_match(cre, (PCRE2_SPTR)bline->data, (PCRE2_SIZE)bline->data_len, (PCRE2_SIZE)look_offset, 0, pcre2_md, NULL);
        if (rc < 0) break;
        memcpy(substrs, pcre2_get_ovector_pointer(pcre2_md), 3 * sizeof(PCRE2_SIZE));
        if (substrs[1] == PCRE2_UNSET) break;
        self->isearch_ranges_len += 2;
        if (self->isearch_ranges_len > self->isearch_ranges_cap) {
            while (self->isearch_ranges_len > self->isearch_ranges_cap) {
                self->isearch_ranges_cap = 2 * MLE_MAX(self->isearch_ranges_cap, 2);
            }
            self->isearch_ranges = realloc(self->isearch_ranges, sizeof(bint_t) * self->isearch_ranges_cap);
        }
        bline_index_to_col(bline, substrs[0], &start);
        bline_index_to_col(bline, substrs[1], &stop);
        self->isearch_ranges[self->isearch_ranges_len - 2] = start;
        self->isearch_ranges[self->isearch_ranges_len - 1] = stop;
        look_offset = MLBUF_MAX(stop, look_offset + 1);
    }

    return MLBUF_OK;
}
