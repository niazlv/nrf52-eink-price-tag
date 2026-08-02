#include "dither.h"
#include "graphics.h"

static const uint8_t bayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

void dither_fill_rect(int x, int y, int width, int height,
                      int color, int level, int phase)
{
    if (level <= 0) {
        graphics_fill_rect(x, y, width, height, GFX_WHITE);
        return;
    }
    if (level >= 16) {
        graphics_fill_rect(x, y, width, height, color);
        return;
    }

    for (int yy = 0; yy < height; yy++) {
        for (int xx = 0; xx < width; xx++) {
            int bx = (xx + phase) & 3;
            int by = (yy + phase) & 3;
            int on = level > bayer4x4[by * 4 + bx];
            graphics_draw_pixel(x + xx, y + yy, on ? color : GFX_WHITE);
        }
    }
}

void dither_fill_rect_mix(int x, int y, int width, int height,
                          int red_level, int black_level, int phase)
{
    int red_cut = red_level;
    int black_cut = red_level + black_level;

    if (red_cut < 0) red_cut = 0;
    if (red_cut > 16) red_cut = 16;
    if (black_cut < red_cut) black_cut = red_cut;
    if (black_cut > 16) black_cut = 16;

    for (int yy = 0; yy < height; yy++) {
        for (int xx = 0; xx < width; xx++) {
            int bx = (xx + phase) & 3;
            int by = (yy + phase) & 3;
            int threshold = bayer4x4[by * 4 + bx];
            int color = GFX_WHITE;

            if (threshold < red_cut) {
                color = GFX_RED;
            } else if (threshold < black_cut) {
                color = GFX_BLACK;
            }

            graphics_draw_pixel(x + xx, y + yy, color);
        }
    }
}
