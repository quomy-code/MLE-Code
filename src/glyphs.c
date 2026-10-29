#include <stdint.h>
#include <string.h>
#include "mle.h"

/* Two 8x8 bitmap fonts. Bit 7 of each row is the leftmost pixel.
   Each braille cell samples the bitmap in 4x2 blocks (2 dots by 4 dots),
   so the shape on screen is these bits, not whatever the host font drew. */

#define Z 0x00
#define L 0xF0
#define R 0x0F
#define F 0xFF

static const uint8_t font_slab[GLY_COUNT][8] = {
    /* branch */ { Z, Z, R, R, F, F, R, R },
    /* open */   { F, F, R, R, R, R, Z, Z },
    /* shut */   { L, L, F, F, L, L, Z, Z },
    /* right */  { R, R, F, F, F, F, R, R },
    /* left */   { L, L, F, F, F, F, L, L },
    /* up */     { R, R, F, F, R, R, R, R },
    /* down */   { L, L, L, L, F, F, L, L },
    /* plus */   { R, R, F, F, R, R, Z, Z },
    /* close */  { F, F, Z, Z, Z, Z, F, F },
    /* hunk */   { L, L, F, F, R, R, L, L },
    /* mod */    { F, F, F, F, F, F, F, F },
    /* add */    { R, R, R, R, F, F, R, R },
    /* del */    { Z, Z, F, F, F, F, Z, Z },
    /* untrack */{ F, F, R, R, Z, Z, R, R },
    /* rename */ { F, F, L, L, R, R, F, F },
    /* staged */ { Z, Z, R, R, R, R, Z, Z }
};

static const uint8_t font_line[GLY_COUNT][8] = {
    /* branch */ { Z, Z, R, R, Z, Z, Z, Z },
    /* open */   { F, F, Z, Z, R, R, Z, Z },
    /* shut */   { L, L, Z, Z, L, L, R, R },
    /* right */  { Z, Z, R, R, F, F, R, R },
    /* left */   { Z, Z, L, L, F, F, L, L },
    /* up */     { R, R, Z, Z, R, R, Z, Z },
    /* down */   { Z, Z, L, L, Z, Z, L, L },
    /* plus */   { Z, Z, F, F, R, R, Z, Z },
    /* close */  { F, F, Z, Z, F, F, Z, Z },
    /* hunk */   { L, L, Z, Z, R, R, Z, Z },
    /* mod */    { F, F, Z, Z, F, F, Z, Z },
    /* add */    { R, R, Z, Z, F, F, Z, Z },
    /* del */    { Z, Z, F, F, Z, Z, Z, Z },
    /* untrack */{ F, F, Z, Z, Z, Z, R, R },
    /* rename */ { F, F, Z, Z, Z, Z, F, F },
    /* staged */ { Z, Z, R, R, Z, Z, Z, Z }
};

#undef Z
#undef L
#undef R
#undef F

static const uint32_t plain_cp[GLY_COUNT] = {
    '*', '-', '+', '>', '<', '^', 'v', '+', 'x', '@',
    'M', 'A', 'D', '?', 'R', '*'
};

static const uint32_t marks_cp[GLY_COUNT] = {
    0x2666, /* diamond, branch */
    0x25BE, /* down triangle, open folder */
    0x25B8, /* right triangle, closed folder */
    0x2192, 0x2190, 0x2191, 0x2193,
    '+',
    0x00D7, /* multiplication sign, close */
    0x00A7, /* section, hunk */
    'M', 'A', 'D', '?', 'R',
    0x25CF  /* black circle, staged */
};

static int g_pack;

static uint32_t braille_from(const uint8_t rows[8]) {
    static const int dotbit[4][2] = {
        { 0, 3 },
        { 1, 4 },
        { 2, 5 },
        { 6, 7 }
    };
    unsigned bits = 0;
    int dy, dx, r, c;
    for (dy = 0; dy < 4; dy++) {
        for (dx = 0; dx < 2; dx++) {
            int on = 0;
            for (r = dy * 2; r < dy * 2 + 2; r++) {
                for (c = dx * 4; c < dx * 4 + 4; c++) {
                    if (rows[r] & (uint8_t)(1u << (7 - c))) on = 1;
                }
            }
            if (on) bits |= (unsigned)(1u << dotbit[dy][dx]);
        }
    }
    return 0x2800u + bits;
}

int glyphs_decor(void) {
    return g_pack != 0;
}

uint32_t glyphs_cp(int id) {
    if (id < 0 || id >= GLY_COUNT) return (uint32_t)' ';
    if (g_pack <= 0) return plain_cp[id];
    if (g_pack == 1) return marks_cp[id];
    if (g_pack == 2) return braille_from(font_slab[id]);
    return braille_from(font_line[id]);
}

int glyphs_utf8(int id, char *dst, int cap) {
    uint32_t cp;
    if (!dst || cap < 2) return 0;
    cp = glyphs_cp(id);
    if (cp < 0x80) {
        dst[0] = (char)cp;
        dst[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 3) return 0;
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        dst[2] = '\0';
        return 2;
    }
    if (cap < 4) return 0;
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    dst[3] = '\0';
    return 3;
}

const char *glyphs_name(void) {
    if (g_pack == 1) return "marks";
    if (g_pack == 2) return "slab";
    if (g_pack >= 3) return "line";
    return "plain";
}

void glyphs_cycle(void) {
    g_pack += 1;
    if (g_pack > 3) g_pack = 0;
}
