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
    "done()";

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

    ASSERT("git_hunks", 3, git_extra_debug_marks(diff, marks, sizeof(marks)));
    ASSERT("git_changed_2", '~', marks[1]);
    ASSERT("git_changed_4", '~', marks[3]);
    ASSERT("git_deletion", '-', marks[10]);
    ASSERT("git_added_20", '+', marks[19]);
    ASSERT("git_added_21", '+', marks[20]);
}
