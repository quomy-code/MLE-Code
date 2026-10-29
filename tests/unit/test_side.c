#include <string.h>
#include <stdio.h>
#include "test.h"

char *str = "";

void test(buffer_t *buf, mark_t *cur) {
    static const char status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head main\0"
        "# branch.upstream origin/main\0"
        "# branch.ab +2 -1\0"
        "# stash 3\0"
        "1 M. N... 100644 100644 100644 aaaaaaa bbbbbbb staged file.c\0"
        "1 .M N... 100644 100644 100644 aaaaaaa bbbbbbb modified.c\0"
        "? new file.txt\0"
        "u UU N... 100644 100644 100644 100644 aaaaaaa bbbbbbb ccccccc conflict.c\0"
        "2 R. N... 100644 100644 100644 aaaaaaa bbbbbbb R100 renamed new.c\0"
        "renamed old.c\0";
    static const char detached[] =
        "# branch.oid deadbeef01234567890123456789012345678901\0"
        "# branch.head (detached)\0";
    char got[2048];
    int rc;
    (void)buf;
    (void)cur;

    memset(got, 0, sizeof(got));
    rc = side_debug_parse_git(status, sizeof(status) - 1, got, (int)sizeof(got));
    printf("  git status\n    %s\n", got);
    ASSERT("parse", 1, rc > 0);
    ASSERT("summary", 1, strstr(got, "branch=main upstream=origin/main ahead=2 behind=1 stash=3 staged=3 modified=2 untracked=1 conflicts=1 rows=5") != NULL);
    ASSERT("staged", 1, strstr(got, "|staged:staged file.c") != NULL);
    ASSERT("modified", 1, strstr(got, "|modified:modified.c") != NULL);
    ASSERT("untracked", 1, strstr(got, "|untracked:new file.txt") != NULL);
    ASSERT("conflict", 1, strstr(got, "|conflict:conflict.c") != NULL);
    ASSERT("rename", 1, strstr(got, "|renamed:renamed new.c") != NULL);
    ASSERT("rename_old_hidden", 1, strstr(got, "renamed old.c") == NULL);

    memset(got, 0, sizeof(got));
    rc = side_debug_parse_git(detached, sizeof(detached) - 1, got, (int)sizeof(got));
    ASSERT("detached_parse", 1, rc > 0);
    ASSERT("detached_name", 1, strstr(got, "branch=detached@deadbeef0123") != NULL);

    ASSERT("glyph_plain", 0, strcmp(glyphs_name(), "plain"));
    glyphs_cycle();
    ASSERT("glyph_marks", 0, strcmp(glyphs_name(), "marks"));
    glyphs_cycle();
    ASSERT("glyph_mini", 0, strcmp(glyphs_name(), "mini"));
    glyphs_cycle();
    ASSERT("glyph_slab", 0, strcmp(glyphs_name(), "slab"));
    glyphs_cycle();
    ASSERT("glyph_line", 0, strcmp(glyphs_name(), "line"));
    glyphs_cycle();
    ASSERT("glyph_wrap", 0, strcmp(glyphs_name(), "plain"));
}
