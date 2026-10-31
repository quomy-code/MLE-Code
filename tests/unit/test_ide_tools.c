#include <string.h>
#include "test.h"

char *str =
    "int main() {\n"
    "  if (ready) {\n"
    "    work();\n"
    "  }\n"
    "}\n"
    "def hello():\n"
    "    print('hello')\n"
    "    print('world')\n"
    "done()\n"
    "plain\n"
    "    deeper\n";

void test(buffer_t *buf, mark_t *cur) {
    static const char diff[] =
        "diff --git a/a.c b/a.c\n"
        "--- a/a.c\n"
        "+++ b/a.c\n"
        "@@ -2,2 +2,3 @@\n"
        "@@ -10,2 +11,0 @@\n"
        "@@ -20,0 +20,2 @@\n";
    char marks[32];
    bint_t end = -1;
    (void)cur;

    ASSERT("brace_fold", MLE_OK, codeintel_debug_fold_end(buf, 0, &end));
    ASSERT("brace_fold_end", 4, end);
    ASSERT("indent_fold", MLE_OK, codeintel_debug_fold_end(buf, 5, &end));
    ASSERT("indent_fold_end", 7, end);
    ASSERT("inner_not_fold", MLE_ERR, codeintel_debug_fold_end(buf, 2, &end));
    ASSERT("indent_alone", MLE_ERR, codeintel_debug_fold_end(buf, 9, &end));

    ASSERT("git_hunks", 3, git_extra_debug_marks(diff, marks, sizeof(marks)));
    ASSERT("git_changed_2", '~', marks[1]);
    ASSERT("git_changed_4", '~', marks[3]);
    ASSERT("git_deletion", '-', marks[10]);
    ASSERT("git_added_20", '+', marks[19]);
    ASSERT("git_added_21", '+', marks[20]);

    {
        static const char body[] =
            "diff --git a/a.c b/a.c\n"
            "--- a/a.c\n"
            "+++ b/a.c\n"
            "@@ -2,3 +2,4 @@ main\n"
            " int x;\n"
            "-return 0;\n"
            "+return 1;\n"
            "+log();\n"
            " }\n";
        char *text = NULL;
        int first = 0;
        int n;
        const char *old_at;
        const char *new_at;
        memset(marks, ' ', sizeof(marks));
        ASSERT("body_hunks", 1, git_extra_debug_marks(body, marks, sizeof(marks)));
        ASSERT("body_context", ' ', marks[1]);
        ASSERT("body_changed", '~', marks[2]);
        ASSERT("body_added", '+', marks[3]);
        ASSERT("body_tail", ' ', marks[4]);
        n = git_extra_format_compare(body, "work tree", &text, &first);
        ASSERT("compare_count", 3, n);
        ASSERT("compare_line", 3, first);
        ASSERT("compare_text", 1, text != NULL);
        ASSERT("compare_counts", 1, text && strstr(text, "2 added, 1 removed") != NULL);
        old_at = text ? strstr(text, "return 0") : NULL;
        new_at = text ? strstr(text, "return 1") : NULL;
        ASSERT("compare_pair", 1, old_at && new_at && new_at > old_at && (new_at - old_at) < 80);
        ASSERT("compare_log", 1, text && strstr(text, "log()") != NULL);
        free(text);
    }
}
