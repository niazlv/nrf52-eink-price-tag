#include "test.h"
#include "graphics.h"

/* Read back one physical pixel (rotation-0 coordinates).
 * Returns 0 = black, 1 = white, 2 = red. */
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

static void test_init_defaults(void)
{
    graphics_init();

    const uint8_t *bw = graphics_get_buffer();
    const uint8_t *red = graphics_get_red_buffer();

    for (int i = 0; i < BUFFER_SIZE; i++) {
        T_ASSERT_MSG(bw[i] == 0xFF, "bw[%d] = 0x%02X", i, bw[i]);
        T_ASSERT_MSG(red[i] == 0x00, "red[%d] = 0x%02X", i, red[i]);
        if (t_failures) {
            return; /* don't spam 4736 lines */
        }
    }

    /* graphics_canvas_init() defaults to rotation 1 (landscape). */
    T_ASSERT_EQ(graphics_get_width(), DISPLAY_HEIGHT);
    T_ASSERT_EQ(graphics_get_height(), DISPLAY_WIDTH);
}

static void test_rotation_transforms(void)
{
    graphics_init();

    /* rotation 0: logical == physical */
    graphics_set_rotation(0);
    T_ASSERT_EQ(graphics_get_width(), DISPLAY_WIDTH);
    T_ASSERT_EQ(graphics_get_height(), DISPLAY_HEIGHT);
    graphics_draw_pixel(0, 0, GFX_BLACK);
    T_ASSERT_EQ(px(0, 0), 0);
    graphics_draw_pixel(DISPLAY_WIDTH - 1, 0, GFX_BLACK);
    T_ASSERT_EQ(px(DISPLAY_WIDTH - 1, 0), 0);
    graphics_draw_pixel(0, DISPLAY_HEIGHT - 1, GFX_BLACK);
    T_ASSERT_EQ(px(0, DISPLAY_HEIGHT - 1), 0);

    /* rotation 1: (x,y) -> (W-1-y, x) */
    graphics_init();
    graphics_set_rotation(1);
    graphics_draw_pixel(0, 0, GFX_BLACK);
    T_ASSERT_EQ(px(DISPLAY_WIDTH - 1, 0), 0);
    graphics_draw_pixel(DISPLAY_HEIGHT - 1, DISPLAY_WIDTH - 1, GFX_BLACK);
    T_ASSERT_EQ(px(0, DISPLAY_HEIGHT - 1), 0);

    /* rotation 2: (x,y) -> (W-1-x, H-1-y) */
    graphics_init();
    graphics_set_rotation(2);
    graphics_draw_pixel(0, 0, GFX_BLACK);
    T_ASSERT_EQ(px(DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1), 0);

    /* rotation 3: (x,y) -> (y, H-1-x) */
    graphics_init();
    graphics_set_rotation(3);
    graphics_draw_pixel(0, 0, GFX_BLACK);
    T_ASSERT_EQ(px(0, DISPLAY_HEIGHT - 1), 0);

    /* negative rotation is normalised */
    graphics_set_rotation(-1);
    T_ASSERT_EQ(graphics_get_width(), DISPLAY_HEIGHT);
}

static void test_out_of_bounds_ignored(void)
{
    graphics_init();
    graphics_set_rotation(0);

    uint8_t before[BUFFER_SIZE];
    memcpy(before, graphics_get_buffer(), BUFFER_SIZE);

    graphics_draw_pixel(-1, 0, GFX_BLACK);
    graphics_draw_pixel(0, -1, GFX_BLACK);
    graphics_draw_pixel(DISPLAY_WIDTH, 0, GFX_BLACK);
    graphics_draw_pixel(0, DISPLAY_HEIGHT, GFX_BLACK);
    graphics_draw_pixel(1000000, 1000000, GFX_BLACK);
    graphics_draw_pixel(-1000000, -1000000, GFX_BLACK);

    T_ASSERT(memcmp(before, graphics_get_buffer(), BUFFER_SIZE) == 0);
}

static void test_colors(void)
{
    graphics_init();
    graphics_set_rotation(0);

    graphics_draw_pixel(0, 0, GFX_RED);
    T_ASSERT_EQ(px(0, 0), 2);

    /* red pixel overwritten by white clears the red plane bit */
    graphics_draw_pixel(0, 0, GFX_WHITE);
    T_ASSERT_EQ(px(0, 0), 1);

    /* GRAY checkerboard: even parity black, odd parity white */
    graphics_draw_pixel(2, 2, GFX_GRAY);
    graphics_draw_pixel(2, 3, GFX_GRAY);
    T_ASSERT_EQ(px(2, 2), 0);
    T_ASSERT_EQ(px(2, 3), 1);

    /* PINK checkerboard: even parity red, odd parity white */
    graphics_draw_pixel(4, 4, GFX_PINK);
    graphics_draw_pixel(4, 5, GFX_PINK);
    T_ASSERT_EQ(px(4, 4), 2);
    T_ASSERT_EQ(px(4, 5), 1);
}

static void test_clear(void)
{
    graphics_init();

    graphics_clear(GFX_BLACK);
    T_ASSERT_EQ(graphics_get_buffer()[0], 0x00);
    T_ASSERT_EQ(graphics_get_red_buffer()[0], 0x00);

    graphics_clear(GFX_RED);
    T_ASSERT_EQ(graphics_get_buffer()[0], 0xFF);
    T_ASSERT_EQ(graphics_get_red_buffer()[0], 0xFF);

    graphics_clear(GFX_WHITE);
    T_ASSERT_EQ(graphics_get_buffer()[0], 0xFF);
    T_ASSERT_EQ(graphics_get_red_buffer()[0], 0x00);
}

static int buffer_has_black(void)
{
    const uint8_t *bw = graphics_get_buffer();

    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (bw[i] != 0xFF) {
            return 1;
        }
    }
    return 0;
}

static void test_strings(void)
{
    graphics_init();
    graphics_set_rotation(0);

    graphics_draw_string(0, 0, "Hello 123 ~!");
    T_ASSERT(buffer_has_black());

    /* 2-byte UTF-8 Cyrillic decodes and renders */
    graphics_init();
    graphics_set_rotation(0);
    graphics_draw_string(0, 0, "Привет мир");
    T_ASSERT(buffer_has_black());

    /* Truncated UTF-8 lead byte at end of string must not read past it */
    graphics_init();
    graphics_draw_string(0, 0, "\xD0");
    graphics_draw_string(0, 0, "\xD0\xD0");
    graphics_draw_string(0, 0, "\xFF\x80");

    /* Long string wraps to the next row instead of running off the canvas */
    graphics_init();
    graphics_set_rotation(0);
    graphics_draw_string(0, 0,
        "0123456789012345678901234567890123456789 wrap wrap wrap");
    int below_first_row = 0;
    for (int y = 8; y < 16 && !below_first_row; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            if (px(x, y) == 0) {
                below_first_row = 1;
                break;
            }
        }
    }
    T_ASSERT(below_first_row);

    /* Scaled text renders and covers a taller area */
    graphics_init();
    graphics_set_rotation(0);
    graphics_draw_string_scaled(0, 0, "A", 3);
    int tall = 0;
    for (int y = 14; y < 21; y++) {
        for (int x = 0; x < 15; x++) {
            if (px(x, y) == 0) {
                tall = 1;
            }
        }
    }
    T_ASSERT(tall);
}

static void test_text_background(void)
{
    graphics_init();
    graphics_set_rotation(0);

    /* Default background is white: white text on a black bar is unreadable,
     * which is exactly why the transparent variant exists. */
    graphics_fill_rect(0, 0, 40, 10, GFX_BLACK);
    graphics_draw_string_color(2, 1, "AB", GFX_WHITE);
    /* Inside the 5x7 cell of 'A' every pixel is white — glyph and background
     * alike — so the letter is invisible. (The 1px gaps between cells stay
     * black, which is why the scan stops at the cell edge.) */
    int black_in_cell = 0;
    for (int y = 1; y < 8; y++) {
        for (int x = 2; x < 7; x++) {
            if (px(x, y) == 0) {
                black_in_cell = 1;
            }
        }
    }
    T_ASSERT(!black_in_cell); /* documents the opaque behaviour */

    /* Transparent background keeps the bar, so the glyphs read as white-on-black */
    graphics_init();
    graphics_set_rotation(0);
    graphics_fill_rect(0, 0, 40, 10, GFX_BLACK);
    graphics_draw_string_color_bg(2, 1, "AB", GFX_WHITE, GFX_TRANSPARENT);
    int black = 0, white = 0;
    for (int y = 1; y < 8; y++) {
        for (int x = 2; x < 14; x++) {
            if (px(x, y) == 0) black++;
            else if (px(x, y) == 1) white++;
        }
    }
    T_ASSERT(black > 0);
    T_ASSERT(white > 0);

    /* Transparent black text over a red banner stays red between the strokes */
    graphics_init();
    graphics_set_rotation(0);
    graphics_fill_rect(0, 0, 40, 10, GFX_RED);
    graphics_draw_string_color_bg(2, 1, "AB", GFX_BLACK, GFX_TRANSPARENT);
    int red = 0;
    for (int y = 1; y < 8; y++) {
        for (int x = 2; x < 14; x++) {
            if (px(x, y) == 2) red++;
        }
    }
    T_ASSERT(red > 0);
}

static void test_battery_icon(void)
{
    graphics_init();
    graphics_set_rotation(0);

    /* over-100% clamps to a full bar, no overflow past the outline */
    graphics_draw_battery(0, 0, 150);
    T_ASSERT_EQ(px(18, 5), 0);  /* last fill column */
    T_ASSERT_EQ(px(19, 5), 1);  /* gap before right wall stays white */

    /* negative percent draws an empty bar without crashing */
    graphics_init();
    graphics_set_rotation(0);
    graphics_draw_battery(0, 0, -10);
    T_ASSERT_EQ(px(2, 5), 1);
}

static void test_custom_canvas(void)
{
    static uint8_t bw[(16 * 16) / 8];
    static uint8_t red[(16 * 16) / 8];
    graphics_canvas_t canvas;
    graphics_canvas_t *saved = graphics_get_canvas();

    graphics_canvas_init(&canvas, 16, 16, bw, red, sizeof(bw));
    memset(bw, 0xFF, sizeof(bw));
    memset(red, 0x00, sizeof(red));
    graphics_set_canvas(&canvas);
    graphics_set_rotation(0);

    T_ASSERT_EQ(graphics_get_width(), 16);
    graphics_draw_pixel(0, 0, GFX_BLACK);
    T_ASSERT_EQ(bw[0], 0x7F);

    /* out of bounds on the small canvas is ignored, not clipped into memory */
    graphics_draw_pixel(16, 0, GFX_BLACK);
    graphics_draw_pixel(0, 16, GFX_BLACK);

    /* NULL canvas is rejected, active canvas unchanged */
    graphics_set_canvas(NULL);
    T_ASSERT(graphics_get_canvas() == &canvas);

    graphics_set_canvas(saved);
}

static void test_rects(void)
{
    graphics_init();
    graphics_set_rotation(0);

    graphics_fill_rect(2, 2, 4, 3, GFX_BLACK);
    T_ASSERT_EQ(px(2, 2), 0);
    T_ASSERT_EQ(px(5, 4), 0);
    T_ASSERT_EQ(px(6, 2), 1);
    T_ASSERT_EQ(px(2, 5), 1);

    graphics_init();
    graphics_set_rotation(0);
    graphics_draw_rect(1, 1, 5, 4, GFX_BLACK);
    T_ASSERT_EQ(px(1, 1), 0);   /* corner */
    T_ASSERT_EQ(px(5, 4), 0);   /* opposite corner */
    T_ASSERT_EQ(px(3, 2), 1);   /* interior stays white */

    /* degenerate sizes are no-ops */
    graphics_draw_rect(0, 0, 0, 5, GFX_BLACK);
    graphics_draw_rect(0, 0, 5, -1, GFX_BLACK);
}

int main(void)
{
    T_RUN(test_init_defaults);
    T_RUN(test_rotation_transforms);
    T_RUN(test_out_of_bounds_ignored);
    T_RUN(test_colors);
    T_RUN(test_clear);
    T_RUN(test_strings);
    T_RUN(test_text_background);
    T_RUN(test_battery_icon);
    T_RUN(test_custom_canvas);
    T_RUN(test_rects);
    return t_report("graphics");
}
