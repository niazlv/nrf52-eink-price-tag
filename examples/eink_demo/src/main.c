/*
 * Example: drive an SSD1675A e-paper panel with lib/eink + lib/gfx.
 *
 * Shows three things:
 *   1. the raw path — build the two RAM planes by hand, no graphics library
 *      (display_square_without_graphics_library)
 *   2. the normal path — draw with lib/gfx, flush with a full refresh
 *   3. waveform tuning — patch the working LUT through the public byte editor
 *      to over-drive the red phase, and optionally abort the refresh part-way
 *
 * Nothing here is copied from the library; it builds against ../../lib.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include <eink/ssd1675a.h>
#include <gfx/graphics.h>
#include "mandelbrot.h"

#define EINK_WIDTH         SSD1675A_WIDTH
#define EINK_HEIGHT        SSD1675A_HEIGHT
#define EINK_BYTES_PER_ROW (EINK_WIDTH / 8)
#define EINK_BUFFER_SIZE   SSD1675A_RAM_BYTES

/* Which screen to draw: 0 = system-info cover, 1 = Mandelbrot fractal. */
#define DEMO_SCREEN_MANDELBROT 0

/* Over-drive the red phase, then cut the refresh short so the pigment freezes
 * at its most saturated instead of settling to pale red. Set to 0 for a normal,
 * fully settled refresh. */
#define COVER_USE_RED_CUTOFF 1
#define COVER_VIVID_PASSES   1

static void set_bw_pixel(uint8_t *bw_buffer, uint8_t *red_buffer,
                         int x, int y, bool black)
{
    if (x < 0 || x >= EINK_WIDTH || y < 0 || y >= EINK_HEIGHT) {
        return;
    }

    int index = y * EINK_BYTES_PER_ROW + x / 8;
    uint8_t mask = BIT(7 - (x % 8));

    if (black) {
        bw_buffer[index] &= ~mask;  // BW RAM: 0 = black
    } else {
        bw_buffer[index] |= mask;   // BW RAM: 1 = white
    }

    red_buffer[index] &= ~mask;     // Red RAM: 0 = no red
}

static void draw_filled_square(uint8_t *bw_buffer, uint8_t *red_buffer,
                               int x, int y, int size)
{
    for (int yy = y; yy < y + size; yy++) {
        for (int xx = x; xx < x + size; xx++) {
            set_bw_pixel(bw_buffer, red_buffer, xx, yy, true);
        }
    }
}

static void __maybe_unused display_square_without_graphics_library(void)
{
    static uint8_t bw_buffer[EINK_BUFFER_SIZE];
    static uint8_t red_buffer[EINK_BUFFER_SIZE];

    memset(bw_buffer, 0xFF, sizeof(bw_buffer));   // White screen
    memset(red_buffer, 0x00, sizeof(red_buffer)); // No red pixels

    draw_filled_square(bw_buffer, red_buffer, 40, 84, 48);

    ssd1675a_init();
    ssd1675a_display_buffer(bw_buffer, red_buffer);
    ssd1675a_update_display();
}

static const char cover_qr[21][22] = {
    "#######...#...#######",
    "#.....#.#..##.#.....#",
    "#.###.#..#.#..#.###.#",
    "#.###.#.##.#..#.###.#",
    "#.###.#...#...#.###.#",
    "#.....#.#####.#.....#",
    "#######.#.#.#.#######",
    "..........##.........",
    "#####.####..##.#.#.#.",
    "####.#.#.....########",
    "##..############..##.",
    "##..#..#.##..#..###..",
    "##.#.####..#..#.##..#",
    "........#.#...#####.#",
    "#######.#.#.#..#..##.",
    "#.....#......#.####..",
    "#.###.#.#...#.####...",
    "#.###.#.####....#.##.",
    "#.###.#.######.#..#..",
    "#.....#.###..#..###..",
    "#######.#...#.##.#.#."
};

static void draw_real_qr(int x, int y, int scale)
{
    const int modules = 21;
    const int quiet = 4;
    const int total = (modules + quiet * 2) * scale;

    graphics_fill_rect(x, y, total, total, GFX_WHITE);

    int ox = x + quiet * scale;
    int oy = y + quiet * scale;

    for (int yy = 0; yy < modules; yy++) {
        for (int xx = 0; xx < modules; xx++) {
            if (cover_qr[yy][xx] == '#') {
                graphics_fill_rect(ox + xx * scale, oy + yy * scale,
                                   scale, scale, GFX_BLACK);
            }
        }
    }
}

static const uint8_t cover_bayer8[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 },
    {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 },
    {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 },
    {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 },
    {42, 26, 38, 22, 41, 25, 37, 21 },
};

static void draw_red_dither_gradient(int x, int y, int width, int height,
                                     int start_density, int end_density)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int yy = 0; yy < height; yy++) {
        for (int xx = 0; xx < width; xx++) {
            int mix = (xx * 2 + yy) * 64 / (width * 2 + height);
            int density = start_density +
                          (end_density - start_density) * mix / 64;

            if (cover_bayer8[yy & 7][xx & 7] < density) {
                graphics_draw_pixel(x + xx, y + yy, GFX_RED);
            }
        }
    }
}

static void draw_badge_side_dither(int x, int y, int width, int height,
                                   int black_peak, int red_peak)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int yy = 0; yy < height; yy++) {
        for (int xx = 0; xx < width; xx++) {
            int side = width > 1 ? xx * 64 / (width - 1) : 0;
            int black_density = black_peak * (64 - side) / 64;
            int red_density = red_peak * side / 64;
            int black_threshold = cover_bayer8[(yy + 3) & 7][(xx + 5) & 7];
            int red_threshold = cover_bayer8[(yy + 1) & 7][(xx + 2) & 7];

            if (black_threshold < black_density) {
                graphics_draw_pixel(x + xx, y + yy, GFX_BLACK);
            }
            if (red_threshold < red_density) {
                graphics_draw_pixel(x + xx, y + yy, GFX_RED);
            }
        }
    }
}

static void draw_cover_screen(void)
{
    graphics_clear(GFX_WHITE);

    graphics_draw_rect(3, 3, 290, 122, GFX_BLACK);
    graphics_draw_rect(5, 5, 286, 118, GFX_RED);

    graphics_draw_string_color(13, 10, "root@price-tag", GFX_RED);
    graphics_draw_string(97, 10, ":~# ");
    graphics_draw_string_color(121, 10, "neofetch", GFX_BLACK);

    graphics_draw_string_scaled(21, 28, "HABR", 4);
    graphics_draw_string(24, 59, "e-ink price tag");
    graphics_draw_string(24, 72, "BAG OF TAGS / NO DOCS");

    graphics_draw_rect(22, 85, 112, 28, GFX_RED);
    graphics_draw_rect(24, 87, 112, 28, GFX_BLACK);
    graphics_fill_rect(24, 87, 108, 24, GFX_WHITE);
    draw_badge_side_dither(25, 88, 106, 22, 20, 24);
    graphics_draw_rect(23, 86, 110, 26, GFX_RED);
    // Scaled text paints only glyph pixels, so the dithered badge stays visible.
    graphics_draw_string_color_scaled(41, 92, "HACKED", GFX_RED, 2);

    graphics_draw_rect(151, 24, 137, 93, GFX_BLACK);
    draw_red_dither_gradient(153, 26, 133, 89, 3, 24);

    graphics_draw_string(153, 31, "price-tag");
    graphics_draw_string(207, 31, "@habr");
    graphics_draw_string(153, 43, "--------------");

    graphics_draw_string(159, 56, "OS");
    graphics_draw_string(188, 56, "Zephyr");
    graphics_draw_string(159, 68, "MCU");
    graphics_draw_string(188, 68, "nRF52832");
    graphics_draw_string(159, 80, "EPD");
    graphics_draw_string(188, 80, "SSD1675A");
    graphics_draw_string(159, 92, "SPI");
    graphics_draw_string(188, 92, "9-bit");
    graphics_draw_string(159, 104, "RAM");
    graphics_draw_string(188, 104, "64K-ish");

    draw_real_qr(254, 81, 1);
}

static void flush_full_update(void)
{
    ssd1675a_init();
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    ssd1675a_update_display();
}

/* Same waveform shape as the library default, with only the red phase pushed
 * harder: level 0x08 -> 0x0F, duration 0x3C -> 0xB0, repeat 0x07 -> 0x14.
 * Byte layout is documented in docs/eink-lut-reference.md — bytes 0..34 are the
 * five voltage LUTs, 35..69 the seven {TA TB TC TD RP} timing groups. */
static const uint8_t lut_vivid_cover[EINK_LUT_SIZE] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,   /* LUT0 black */
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,   /* LUT1 white */
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,   /* LUT2 red   */
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,   /* LUT3 red   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4 VCOM  */
    0x04, 0x18, 0x04, 0x16, 0x01,               /* Ph0 */
    0x0A, 0x0A, 0x0A, 0x0A, 0x02,               /* Ph1 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* Ph2 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* Ph3 */
    0x04, 0x04, 0x0F, 0xB0, 0x14,               /* Ph4 — the red drive */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* Ph5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* Ph6 */
};

/* How long the over-driven red phase runs before we freeze it. */
#define COVER_RED_CUTOFF_MS 6400

static void load_vivid_lut(void)
{
    for (int i = 0; i < EINK_LUT_SIZE; i++) {
        ssd1675a_set_lut_byte(i, lut_vivid_cover[i]);
    }
}

static void flush_vivid_update(void)
{
    ssd1675a_init();
    load_vivid_lut();
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());

#if COVER_USE_RED_CUTOFF
    /* Start the refresh, then kill it mid-phase: the red pigment stops where it
     * is most saturated instead of being washed back to pale red by the last
     * waves. Purely a photogenic hack — the image is not DC-balanced and will
     * ghost, so the panel needs a normal full refresh afterwards. */
    ssd1675a_trigger_update_nowait();
    k_msleep(COVER_RED_CUTOFF_MS);
    ssd1675a_port_reset(true);
    k_msleep(20);
    ssd1675a_power_off();
#else
    for (int i = 0; i < COVER_VIVID_PASSES; i++) {
        ssd1675a_update_display();
        k_msleep(300);
    }
#endif
}

static void display_clean_cycle(void)
{
    graphics_clear(GFX_WHITE);
    flush_full_update();
    k_msleep(600);
}

int main(void)
{
    if (!ssd1675a_init()) {
        return 0;   /* no panel on the bus — nothing to demo */
    }

    graphics_init();
    graphics_set_rotation(1);   /* landscape: 296x128 */

    display_clean_cycle();

#if DEMO_SCREEN_MANDELBROT
    mandelbrot_draw();
    flush_full_update();
    ssd1675a_sleep();
    ssd1675a_power_off();
#else
    draw_cover_screen();
    flush_vivid_update();

#if !COVER_USE_RED_CUTOFF
    /* The cutoff path already reset and powered the panel down. */
    ssd1675a_sleep();
    ssd1675a_power_off();
#endif
#endif /* DEMO_SCREEN_MANDELBROT */

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}
