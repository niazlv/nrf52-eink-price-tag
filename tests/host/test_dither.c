#include "test.h"
#include "graphics.h"
#include "dither.h"

/* 0 = black, 1 = white, 2 = red (rotation-0 physical coordinates). */
static int px(int x, int y)
{
    const uint8_t *bw = graphics_get_buffer();
    const uint8_t *red = graphics_get_red_buffer();
    int idx = y * (DISPLAY_WIDTH / 8) + x / 8;
    int bit = 7 - (x % 8);

    if ((red[idx] >> bit) & 1) {
        return 2;
    }
    return (bw[idx] >> bit) & 1;
}

static void count_rect(int w, int h, int *black, int *white, int *red)
{
    *black = *white = *red = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            switch (px(x, y)) {
            case 0: (*black)++; break;
            case 1: (*white)++; break;
            case 2: (*red)++; break;
            }
        }
    }
}

static void test_levels(void)
{
    int black, white, red;

    graphics_init();
    graphics_set_rotation(0);

    /* level <= 0: everything white regardless of colour */
    dither_fill_rect(0, 0, 8, 8, GFX_BLACK, 0, 0);
    count_rect(8, 8, &black, &white, &red);
    T_ASSERT_EQ(black, 0);
    T_ASSERT_EQ(white, 64);

    /* level >= 16: solid colour */
    dither_fill_rect(0, 0, 8, 8, GFX_BLACK, 16, 0);
    count_rect(8, 8, &black, &white, &red);
    T_ASSERT_EQ(black, 64);

    dither_fill_rect(0, 0, 8, 8, GFX_BLACK, 99, 0);
    count_rect(8, 8, &black, &white, &red);
    T_ASSERT_EQ(black, 64);

    /* level 8 on the 16-step Bayer scale = exactly half coverage */
    dither_fill_rect(0, 0, 8, 8, GFX_BLACK, 8, 0);
    count_rect(8, 8, &black, &white, &red);
    T_ASSERT_EQ(black, 32);
    T_ASSERT_EQ(white, 32);

    /* level N colours exactly N pixels per 4x4 tile */
    for (int level = 1; level < 16; level++) {
        dither_fill_rect(0, 0, 4, 4, GFX_RED, level, 0);
        count_rect(4, 4, &black, &white, &red);
        T_ASSERT_MSG(red == level, "level %d -> %d red pixels", level, red);
    }
}

static void test_phase_shift(void)
{
    graphics_init();
    graphics_set_rotation(0);

    /* Same level, phases 0 and 1 must produce shifted (different) patterns
     * with identical coverage. */
    dither_fill_rect(0, 0, 4, 4, GFX_BLACK, 6, 0);
    uint8_t a[BUFFER_SIZE];
    memcpy(a, graphics_get_buffer(), BUFFER_SIZE);

    dither_fill_rect(0, 0, 4, 4, GFX_BLACK, 6, 1);
    T_ASSERT(memcmp(a, graphics_get_buffer(), BUFFER_SIZE) != 0);

    int black, white, red;
    count_rect(4, 4, &black, &white, &red);
    T_ASSERT_EQ(black, 6);
}

static void test_mix(void)
{
    int black, white, red;

    graphics_init();
    graphics_set_rotation(0);

    /* red_level 4 + black_level 4: per 16-pixel tile the 4 lowest thresholds
     * go red, the next 4 black, the rest white. */
    dither_fill_rect_mix(0, 0, 4, 4, 4, 4, 0);
    count_rect(4, 4, &black, &white, &red);
    T_ASSERT_EQ(red, 4);
    T_ASSERT_EQ(black, 4);
    T_ASSERT_EQ(white, 8);

    /* clamping: oversized red swallows everything */
    dither_fill_rect_mix(0, 0, 4, 4, 40, 40, 0);
    count_rect(4, 4, &black, &white, &red);
    T_ASSERT_EQ(red, 16);
    T_ASSERT_EQ(black, 0);

    /* negative levels clamp to empty */
    dither_fill_rect_mix(0, 0, 4, 4, -5, -5, 0);
    count_rect(4, 4, &black, &white, &red);
    T_ASSERT_EQ(white, 16);
}

int main(void)
{
    T_RUN(test_levels);
    T_RUN(test_phase_shift);
    T_RUN(test_mix);
    return t_report("dither");
}
