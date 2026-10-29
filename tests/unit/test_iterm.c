#include <string.h>
#include <stdio.h>
#include "test.h"

char *str = "";

static void expect_dump(const char *name, const char *bytes, int cols, int rows, const char *want) {
    char got[1024];
    int rc;
    memset(got, 0, sizeof(got));
    rc = iterm_debug_feed(bytes, (int)strlen(bytes), cols, rows, got, (int)sizeof(got));
    printf("  dump %s\n    got  [%s]\n    want [%s]\n", name, got, want);
    ASSERT(name, 0, rc);
    ASSERT(name, 0, strcmp(got, want));
}

void test(buffer_t *buf, mark_t *cur) {
    char got[1024];
    uint32_t ch;
    int fg, bg, attr, rc;
    (void)buf;
    (void)cur;

    expect_dump("crlf", "hi\r\n", 10, 4, "hi\n\n\n");
    expect_dump("wrap", "123456", 5, 3, "12345\n6\n");
    expect_dump("cup", "\033[2;1HZ", 4, 3, "\nZ\n");
    expect_dump("scroll", "a\r\nb\r\nc", 4, 2, "b\nc");
    expect_dump("alt", "A\033[?1049hB\033[?1049l", 4, 1, "A");
    expect_dump("sgr", "\033[31mred\033[0m", 8, 1, "red");
    expect_dump("bracket", "\033[?2004h$", 8, 1, "$");
    expect_dump("osc", "\033]0;title\007PROMPT", 12, 1, "PROMPT");
    expect_dump("clear", "\033[H\033[2J\033[3Jhi", 8, 2, "hi\n");
    expect_dump("dch", "hello\033[5D\033[2P", 8, 1, "llo");
    expect_dump("ich", "ab\033[2D\033[1@Z", 8, 1, "Zab");
    expect_dump("el", "xyz\033[1D\033[K", 8, 1, "xy");
    expect_dump("cuf", "ab\033[2D\033[CZ", 8, 1, "aZ");
    expect_dump("prompt", "\033[1;32muser\033[0m@h", 12, 1, "user@h");
    expect_dump("readline", "clx\033[D\033[P./", 8, 1, "cl./");

    memset(got, 0, sizeof(got));
    rc = iterm_debug_resize("123456", 6, 5, 3, 10, 3, got, (int)sizeof(got));
    printf("  resize\n    got  [%s]\n    want [%s]\n", got, "123456\n\n");
    ASSERT("resize", 0, rc);
    ASSERT("resize_text", 0, strcmp(got, "123456\n\n"));

    ch = 0;
    fg = bg = attr = -1;
    rc = iterm_debug_cell("\033[31mZ", 6, 4, 2, 0, 0, &ch, &fg, &bg, &attr);
    printf("  cell ch=%u fg=%d bg=%d attr=%d\n", ch, fg, bg, attr);
    ASSERT("cell", 0, rc);
    ASSERT("cell_ch", (intptr_t)'Z', (intptr_t)ch);
    ASSERT("cell_fg", 2, fg);

    memset(got, 0, sizeof(got));
    rc = iterm_debug_pty(got, (int)sizeof(got));
    printf("  pty rc=%d\n    got [%s]\n", rc, got);
    ASSERT("pty", 0, rc);
    ASSERT("pty_text", 1, strstr(got, "xyzzy-iterm") != NULL);
}
