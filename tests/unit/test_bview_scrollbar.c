#include <string.h>
#include "test.h"

char *str = "";

void test(buffer_t *buf, mark_t *cur) {
    editor_t editor;
    bview_t view;
    int start;
    int size;
    (void)buf;
    (void)cur;

    ASSERT("scroll_start", MLE_OK, bview_debug_scrollbar(10, 100, 20, 0, &start, &size));
    ASSERT("scroll_start_pos", 0, start);
    ASSERT("scroll_thumb_size", 2, size);

    ASSERT("scroll_middle", MLE_OK, bview_debug_scrollbar(10, 100, 20, 40, &start, &size));
    ASSERT("scroll_middle_pos", 4, start);
    ASSERT("scroll_end", MLE_OK, bview_debug_scrollbar(10, 100, 20, 80, &start, &size));
    ASSERT("scroll_end_pos", 8, start);

    ASSERT("scroll_fits", MLE_OK, bview_debug_scrollbar(10, 8, 20, 0, &start, &size));
    ASSERT("scroll_fits_pos", 0, start);
    ASSERT("scroll_fits_size", 10, size);
    ASSERT("scroll_tiny", MLE_OK, bview_debug_scrollbar(1, 100, 1, 99, &start, &size));
    ASSERT("scroll_tiny_size", 1, size);

    memset(&editor, 0, sizeof(editor));
    memset(&view, 0, sizeof(view));
    view.type = MLE_BVIEW_TYPE_EDIT;
    view.all_next = &view;
    view.all_prev = &view;
    view.viewport_x_vcol = 73;
    editor.all_bviews = &view;
    ASSERT("mouse_activate", MLE_OK, editor_set_active_preserve_viewport(&editor, &view));
    ASSERT("mouse_activate_view", 1, editor.active == &view);
    ASSERT("mouse_keep_viewport", 73, view.viewport_x_vcol);
}
