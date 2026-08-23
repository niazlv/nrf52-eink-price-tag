#include "display_screens.h"
#include "display_manager.h"
#include "system_time.h"
#include <gfx/graphics.h>
#include <gfx/life.h>
#include <gfx/dither.h>
#include <eink/ssd1675a.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static char date_str[40];
static char stat_str[48];
static char power_str[32];
static life_world_t life_world;

static const char *const wday_names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

/* Zero-pad helpers: CONFIG_CBPRINTF_NANO=y strips width specifiers (%02d). */
static void fmt_d2(char *out, int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    out[0] = '0' + v / 10; out[1] = '0' + v % 10; out[2] = '\0';
}
static void fmt_u3(char *out, unsigned int v)
{
    if (v > 999) v = 999;
    out[0] = '0' + v / 100; out[1] = '0' + (v / 10) % 10;
    out[2] = '0' + v % 10; out[3] = '\0';
}

static void format_uptime(int64_t uptime_sec, char *buf, size_t buf_size)
{
    int d = uptime_sec / 86400;
    uptime_sec %= 86400;
    int h = uptime_sec / 3600;
    uptime_sec %= 3600;
    int m = uptime_sec / 60;
    int s = uptime_sec % 60;

    if (d > 0) {
        snprintf(buf, buf_size, "%dd%dh", d, h);
    } else if (h > 0) {
        snprintf(buf, buf_size, "%dh%dm", h, m);
    } else if (m > 0) {
        snprintf(buf, buf_size, "%dm%ds", m, s);
    } else {
        snprintf(buf, buf_size, "%ds", s);
    }
}

static const char *saver_mode_tag(int mode)
{
    switch (mode) {
    case SCREENSAVER_MODE_DYNAMIC:  return "DY";
    case SCREENSAVER_MODE_LUT_TEST: return "LT";
    case SCREENSAVER_MODE_STATIC:
    default:                        return "ST";
    }
}

static const char *partial_mode_tag(const display_status_model_t *model)
{
    if (model->custom_lut) {
        return "CU";
    }

    switch (model->partial_mode) {
    case 0:  return "T";
    case 1:  return "B";
    case 2:  return "S";
    case 3:  return "C";
    case 4:  return "TD";
    case 5:  return "TL";
    case 6:  return "TF";
    case 7:  return "TB";
    case 8:  return "SD";
    case 9:  return "SL";
    default: return "?";
    }
}

static const char *power_tag(const display_status_model_t *model)
{
    if (model->power_after_update) {
        return "Z";
    }
    if (model->streaming_active) {
        return "S";
    }
    if (model->keep_display_on) {
        return "K";
    }
    return "ON";
}

/* Text at an integer scale. Scale 1 keeps the white-background variant (so
 * small text stays readable over a busy scene); larger scales draw only the
 * glyph pixels, which is what the big panel's clean status screen wants. */
static void draw_text(int x, int y, const char *s, int scale)
{
    if (scale <= 1) {
        graphics_draw_string(x, y, s);
    } else {
        graphics_draw_string_scaled(x, y, s, scale);
    }
}

static int text_width(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale;
}

static void render_stats(const display_status_model_t *model, int x, int y, int scale)
{
    char time_part[20] = {0};
    uint32_t mah = model->energy_mah_x1000 / 1000U;
    uint32_t mah_frac = model->energy_mah_x1000 % 1000U;
    int current_ma_x10 = (model->estimated_current_ua + 50) / 100;

    format_uptime(model->uptime_sec, time_part, sizeof(time_part));
    snprintf(stat_str, sizeof(stat_str), "U:%s R:%d %s:%s A:%d %s",
             time_part,
             model->last_render_ms,
             saver_mode_tag(model->saver_mode),
             partial_mode_tag(model),
             model->maintenance_countdown,
             power_tag(model));
    draw_text(x, y, stat_str, scale);

    char mf_s[4]; fmt_u3(mf_s, (unsigned int)mah_frac);
    snprintf(power_str, sizeof(power_str), "mAh:%u.%s I:%d.%dmA",
             (unsigned int)mah, mf_s,
             current_ma_x10 / 10, current_ma_x10 % 10);
    draw_text(x, y + 10 * scale, power_str, scale);
}

static void render_battery(const display_status_model_t *model, int x, int y, int scale)
{
    char bat_str[16];
    graphics_draw_battery(x, y, model->battery_percent);
    snprintf(bat_str, sizeof(bat_str), "%dmV", model->battery_mv);
    draw_text(x, y + 13, bat_str, scale);
}

void display_screens_reset_dynamic(void)
{
    life_world.initialized = false;
}

/* Status-screen layout, picked from the canvas size. The 296x128 numbers
 * reproduce the original fixed layout pixel for pixel; the large branch is
 * the 400x300 panel. Everything is centred horizontally, so any width works. */
typedef struct {
    int time_scale, time_y;
    int date_scale, date_y;
    int wday_scale, wday_y;
    int text_scale;              /* stats + battery text */
    int margin;
    bool battery_bottom;         /* bottom-right instead of top-right */
} status_layout_t;

static status_layout_t status_layout(int h)
{
    status_layout_t l;
    if (h >= 200) {
        l.time_scale = 9;  l.time_y = 62;
        l.date_scale = 4;  l.date_y = 150;
        l.wday_scale = 3;  l.wday_y = 195;
        l.text_scale = 2;  l.margin = 6;
        l.battery_bottom = true;
    } else {
        l.time_scale = 5;  l.time_y = 30;
        l.date_scale = 3;  l.date_y = 80;
        l.wday_scale = 2;  l.wday_y = 110;
        l.text_scale = 1;  l.margin = 5;
        l.battery_bottom = false;
    }
    return l;
}

void display_screens_render_status_static(const display_status_model_t *model)
{
    int w = graphics_get_width();
    int h = graphics_get_height();
    status_layout_t l = status_layout(h);
    char time_str[8];

    graphics_clear(GFX_WHITE);

    { char hh[3], mm[3]; fmt_d2(hh, model->time.tm_hour); fmt_d2(mm, model->time.tm_min);
      snprintf(time_str, sizeof(time_str), "%s:%s", hh, mm); }
    graphics_draw_string_scaled((w - text_width(time_str, l.time_scale)) / 2,
                                l.time_y, time_str, l.time_scale);

    { char d[3], mo[3]; fmt_d2(d, model->time.tm_mday); fmt_d2(mo, model->time.tm_mon + 1);
      snprintf(date_str, sizeof(date_str), "%s.%s.%d", d, mo, model->time.tm_year + 1900); }
    graphics_draw_string_scaled((w - text_width(date_str, l.date_scale)) / 2,
                                l.date_y, date_str, l.date_scale);

    if (model->time.tm_wday >= 0 && model->time.tm_wday <= 6) {
        const char *wd = wday_names[model->time.tm_wday];
        graphics_draw_string_scaled((w - text_width(wd, l.wday_scale)) / 2,
                                    l.wday_y, wd, l.wday_scale);
    }

    if (l.battery_bottom) {
        /* Bottom-right corner: the reading ("3279mV", 6 glyphs) right-aligned
         * to the margin, the 23-px icon right-aligned above it. */
        int right = w - l.margin;
        int text_w = text_width("0000mV", l.text_scale);
        int y = h - l.margin - 8 * l.text_scale - 13;
        char bat_str[16];
        graphics_draw_battery(right - 23, y, model->battery_percent);
        snprintf(bat_str, sizeof(bat_str), "%dmV", model->battery_mv);
        draw_text(right - text_w, y + 13, bat_str, l.text_scale);
    } else {
        render_battery(model, w - 36, 5, 1);
    }
    render_stats(model, l.margin, l.margin - (l.text_scale == 1 ? 5 : 0), l.text_scale);
}

void display_screens_render_status_dynamic(const display_status_model_t *model)
{
    int width = graphics_get_width();
    int height = graphics_get_height();
    /* The world is sized for 296 px at 8-px cells; on the 400-px panel use
     * 11-px cells so the same 38x38 world still covers the whole canvas. */
    int cell = (width > LIFE_MAX_DIM * LIFE_CELL_SIZE) ? 11 : LIFE_CELL_SIZE;
    int life_w = width / cell;
    int life_h = height / cell;
    char time_str[16];

    if (!life_world.initialized) {
        life_init(&life_world, (uint32_t)model->uptime_sec);
    }

    life_update(&life_world, life_w, life_h);
    graphics_clear(GFX_WHITE);

    if (life_w > LIFE_MAX_DIM) {
        life_w = LIFE_MAX_DIM;
    }
    if (life_h > LIFE_MAX_DIM) {
        life_h = LIFE_MAX_DIM;
    }

    for (int y = 0; y < life_h; y++) {
        for (int x = 0; x < life_w; x++) {
            if (life_get(&life_world, x, y)) {
                graphics_fill_rect(x * cell, y * cell, cell - 1, cell - 1, GFX_BLACK);
            }
        }
    }

    { char h[3], m[3], s[3];
      fmt_d2(h, model->time.tm_hour); fmt_d2(m, model->time.tm_min); fmt_d2(s, model->time.tm_sec);
      snprintf(time_str, sizeof(time_str), "%s:%s:%s", h, m, s); }

    int scale = (width >= 350) ? 5 : (width >= 150) ? 3 : 2;
    int small = (width >= 350) ? 2 : 1;
    int str_w = 8 * (6 * scale);
    int tx = (width - str_w) / 2;
    int ty = height / 2 - (4 * scale);

    graphics_draw_string_scaled(tx, ty, time_str, scale);

    { char d[3], mo[3]; fmt_d2(d, model->time.tm_mday); fmt_d2(mo, model->time.tm_mon + 1);
      snprintf(date_str, sizeof(date_str), "%s.%s.%d", d, mo, model->time.tm_year + 1900); }
    graphics_draw_string_scaled((width - 10 * 6 * small) / 2, ty + (10 * scale), date_str, small);

    if (model->time.tm_wday >= 0 && model->time.tm_wday <= 6) {
        graphics_draw_string_scaled((width - 3 * 6 * small) / 2,
                                    ty + (10 * scale) + 15 * small,
                                    wday_names[model->time.tm_wday],
                                    small);
    }

    render_battery(model, width - 40, 5, 1);
    render_stats(model, 5, 5, 1);
}

void display_screens_render_text(const char *text)
{
    int x = 5;
    int y = 5;

    graphics_clear(GFX_WHITE);
    graphics_draw_rect(0, 0, graphics_get_width(), graphics_get_height(), GFX_BLACK);

    while (text && *text) {
        const char *line = text;
        int len = 0;

        while (line[len] && line[len] != '\n') {
            len++;
        }

        char buf[64];
        int copy_len = len < (int)(sizeof(buf) - 1) ? len : (int)(sizeof(buf) - 1);
        for (int i = 0; i < copy_len; i++) {
            buf[i] = line[i];
        }
        buf[copy_len] = '\0';
        graphics_draw_string(x, y, buf);

        y += 10;
        text += len;
        if (*text == '\n') {
            text++;
        }
    }
}

void display_screens_render_ruler(const char *panel, int rotation)
{
    int w = graphics_get_width();
    int h = graphics_get_height();
    char buf[32];

    graphics_clear(GFX_WHITE);
    /* Two borders: the outer one sits on the very edge pixels, so a panel
     * that does not drive its first/last line shows a single frame only. */
    graphics_draw_rect(0, 0, w, h, GFX_BLACK);
    graphics_draw_rect(3, 3, w - 6, h - 6, GFX_BLACK);

    for (int x = 10; x < w; x += 10) {
        int len = (x % 50 == 0) ? 14 : 7;
        graphics_fill_rect(x, 0, 1, len, GFX_BLACK);
        if (x % 50 == 0) {
            snprintf(buf, sizeof(buf), "%d", x);
            graphics_draw_string_color_bg(x + 2, 15, buf, GFX_BLACK, GFX_TRANSPARENT);
        }
    }
    for (int y = 10; y < h; y += 10) {
        int len = (y % 50 == 0) ? 14 : 7;
        graphics_fill_rect(0, y, len, 1, GFX_BLACK);
        if (y % 50 == 0) {
            snprintf(buf, sizeof(buf), "%d", y);
            graphics_draw_string_color_bg(16, y + 2, buf, GFX_BLACK, GFX_TRANSPARENT);
        }
    }

    /* Diagonal: straight only when both axes are driven 1:1. */
    for (int x = 0; x < w; x++) {
        int y = (int)((int64_t)x * (h - 1) / (w > 1 ? w - 1 : 1));
        graphics_draw_pixel(x, y, GFX_BLACK);
    }

    graphics_draw_string_color_bg(w - 18, 6, "TR", GFX_BLACK, GFX_TRANSPARENT);
    graphics_draw_string_color_bg(6, h - 14, "BL", GFX_BLACK, GFX_TRANSPARENT);
    graphics_draw_string_color_bg(w - 18, h - 14, "BR", GFX_BLACK, GFX_TRANSPARENT);

    /* Red channel check: a red frame just inside the black ones, a solid red
     * block and a dithered pink one under the caption. */
    graphics_draw_rect(6, 6, w - 12, h - 12, GFX_RED);

    int scale = (w >= 300) ? 3 : 2;
    snprintf(buf, sizeof(buf), "%dx%d rot%d", w, h, rotation);
    int tw = (int)strlen(buf) * 6 * scale;
    graphics_draw_string_color_scaled((w - tw) / 2, h / 2 - 8 * scale, buf, GFX_BLACK, scale);

    snprintf(buf, sizeof(buf), "panel %s", panel ? panel : "?");
    tw = (int)strlen(buf) * 6;
    graphics_draw_string_color_bg((w - tw) / 2, h / 2 + 4, buf, GFX_RED, GFX_TRANSPARENT);

    int bw_ = 60, bh = 18;
    graphics_fill_rect(w / 2 - bw_ - 4, h / 2 + 16, bw_, bh, GFX_RED);
    graphics_fill_rect(w / 2 + 4, h / 2 + 16, bw_, bh, GFX_PINK);
    graphics_draw_string_color_bg(w / 2 - bw_ - 4, h / 2 + 16 + bh + 2, "RED", GFX_BLACK, GFX_TRANSPARENT);
    graphics_draw_string_color_bg(w / 2 + 4, h / 2 + 16 + bh + 2, "PINK", GFX_BLACK, GFX_TRANSPARENT);
}

static void draw_palette_row(const char *label, int y, int color,
                             const int levels[8], int sw_x, int sw_w,
                             int sw_h, int gap)
{
    graphics_draw_string(2, y + 4, label);
    for (int i = 0; i < 8; i++) {
        int x = sw_x + i * (sw_w + gap);
        dither_fill_rect(x + 1, y + 1, sw_w - 2, sw_h - 2,
                         color, levels[i], i);
        graphics_draw_rect(x, y, sw_w, sw_h, GFX_BLACK);
    }
}

void display_screens_render_palette_test(void)
{
    static const int levels[8] = {0, 2, 4, 6, 8, 10, 12, 16};
    static const int mix_red[8] = {0, 3, 6, 9, 12, 12, 8, 0};
    static const int mix_blk[8] = {0, 0, 0, 0, 0, 4, 8, 16};

    int w = graphics_get_width();
    int h = graphics_get_height();
    int sw_x = 44;
    int gap = 2;
    int sw_w = (w - sw_x - 4 - gap * 7) / 8;
    int sw_h = 16;
    int y = 13;

    if (sw_w < 10) {
        sw_w = 10;
    }

    graphics_clear(GFX_WHITE);
    graphics_fill_rect(0, 0, w, 10, GFX_BLACK);
    graphics_draw_string_color_bg(2, 1, "PALETTE TEST  B/W/R + DITHER",
                                  GFX_WHITE, GFX_TRANSPARENT);

    draw_palette_row("GRAY", y, GFX_BLACK, levels, sw_x, sw_w, sw_h, gap);
    y += 23;
    draw_palette_row("RED", y, GFX_RED, levels, sw_x, sw_w, sw_h, gap);
    y += 23;

    graphics_draw_string(2, y + 4, "MIX");
    for (int i = 0; i < 8; i++) {
        int x = sw_x + i * (sw_w + gap);
        dither_fill_rect_mix(x + 1, y + 1, sw_w - 2, sw_h - 2,
                      mix_red[i], mix_blk[i], i);
        graphics_draw_rect(x, y, sw_w, sw_h, GFX_BLACK);
    }
    y += 23;

    graphics_draw_string(2, y + 4, "TONE");
    for (int i = 0; i < 8; i++) {
        int x = sw_x + i * (sw_w + gap);
        int pulse = i == 7 ? 8 : i;
        char label[4];

        dither_fill_rect(x + 1, y + 1, sw_w - 2, sw_h - 2,
                         GFX_BLACK, levels[i], i + 1);
        graphics_draw_rect(x, y, sw_w, sw_h, GFX_BLACK);
        snprintf(label, sizeof(label), "%d", pulse);
        graphics_draw_string(x + 3, y + sw_h + 2, label);
    }

    graphics_draw_string(2, h - 10, "PALTEST=spatial  TONETEST=physical pulses");
}

void display_screens_render_tone_test_pass(int pass, int max_passes)
{
    static const int pulses[8] = {0, 1, 2, 3, 4, 5, 6, 8};
    char buf[48];
    int w = graphics_get_width();
    int h = graphics_get_height();
    int sw_x = 28;
    int gap = 3;
    int sw_w = (w - sw_x - 4 - gap * 7) / 8;
    int sw_h = 42;
    int y = 36;

    (void)max_passes;

    if (sw_w < 10) {
        sw_w = 10;
    }
    if (sw_h > h - y - 24) {
        sw_h = h - y - 24;
    }

    graphics_clear(GFX_WHITE);

    graphics_draw_string(2, 2, "TONAL GRAY  0..8 PULSES");
    graphics_draw_string(2, 13, "black-only LUT: repeated masked pulses");
    graphics_draw_string(2, 25, "0 1 2 3 4 5 6 8 pulses");

    for (int i = 0; i < 8; i++) {
        int x = sw_x + i * (sw_w + gap);

        graphics_draw_rect(x, y, sw_w, sw_h, GFX_BLACK);
        if (pulses[i] > pass) {
            graphics_fill_rect(x + 2, y + 2, sw_w - 4, sw_h - 4, GFX_BLACK);
        }

        snprintf(buf, sizeof(buf), "%d", pulses[i]);
        graphics_draw_string(x + 4, y + sw_h + 4, buf);
    }

    graphics_draw_string(2, h - 10, "After this: CLEAN/NUKE if ghosting stays");
}

void display_screens_render_partial_test(int32_t frame,
                                         int64_t uptime_ms,
                                         int32_t delta_ms,
                                         const char *addr)
{
    char buf[64];

    graphics_clear(GFX_WHITE);
    graphics_draw_string(10, 10, "PARTIAL TEST");

    snprintf(buf, sizeof(buf), "Frame: %d", frame);
    graphics_draw_string(10, 35, buf);

    snprintf(buf, sizeof(buf), "Up: %lld ms", uptime_ms);
    graphics_draw_string(10, 55, buf);

    graphics_draw_string(10, 75, "MAC:");
    graphics_draw_string(10, 90, addr ? addr : "Unknown");

    snprintf(buf, sizeof(buf), "Delta: %d ms", delta_ms);
    graphics_draw_string(10, 110, buf);
}

void display_screens_render_animation_frame(int x, int y, int size, int frame,
                                             int32_t delta_ms)
{
    static int32_t min_ms = 0;
    static int32_t max_ms = 0;

    if (frame == 0) {
        min_ms = 0;
        max_ms = 0;
    } else if (delta_ms > 0) {
        if (min_ms == 0 || delta_ms < min_ms) min_ms = delta_ms;
        if (delta_ms > max_ms)                max_ms = delta_ms;
    }

    char buf[48];
    int w      = graphics_get_width();
    int height = graphics_get_height();

    graphics_clear(GFX_WHITE);

    /* Status bar at top */
    graphics_fill_rect(0, 0, w, 10, GFX_BLACK);
    snprintf(buf, sizeof(buf), "ANIM #%d  %dms  mn=%d mx=%d",
             frame, (int)delta_ms, (int)min_ms, (int)max_ms);
    graphics_draw_string_color_bg(2, 1, buf, GFX_WHITE, GFX_TRANSPARENT);

    /* Bouncing square */
    graphics_fill_rect(x, y, size, size, GFX_BLACK);

    /* Bottom hint */
    snprintf(buf, sizeof(buf), "partial update  %dms/frame", (int)delta_ms);
    graphics_draw_string(2, height - 10, buf);
}

/*
 * Ghosting / artifact test — 3 horizontal tracks:
 *   Track 0: black ball on white background  (tests WB / BW transitions)
 *   Track 1: white ball on black background  (tests BB / WW artifacts)
 *   Track 2: red   ball on white background  (tests red channel ghosting)
 *
 * Each track has a thin center reference line so ghosting is easy to spot:
 * any faint impression left by the ball is visible against the reference line.
 */
void display_screens_render_lut_test(int32_t frame, int32_t delta_ms,
                                     int32_t min_ms, int32_t max_ms,
                                     bool custom_lut)
{
#define BALL_W      22   /* pixels wide */
#define BALL_SPEED   3   /* pixels per frame */
#define TRACK_H     28   /* height of each colour track */
#define BALL_MARGIN  2   /* top/bottom gap inside track */

    char buf[48];
    int w = graphics_get_width();    /* 296 */
    int h = graphics_get_height();   /* 128 */

    /* Layout — total: 10+10+1+28+28+28+1+12+10 = 128 */
    const int Y_TITLE  = 0;
    const int Y_TIMING = 10;
    const int Y_SEP1   = 20;
    const int Y_T0     = 21;                     /* black-on-white track */
    const int Y_T1     = Y_T0 + TRACK_H;         /* white-on-black track */
    const int Y_T2     = Y_T1 + TRACK_H;         /* red-on-white track   */
    const int Y_SEP2   = Y_T2 + TRACK_H;         /* = 105 */
    const int Y_INFO   = Y_SEP2 + 1;
    const int Y_STATUS = Y_INFO  + 12;

    /* Ball X: wraps from -BALL_W to w, so it fully exits on both sides */
    int period = w + BALL_W;
    int bx = (int)((frame * (int64_t)BALL_SPEED) % period) - BALL_W;
    int bx0 = bx < 0 ? 0 : bx;
    int bx1 = bx + BALL_W > w ? w : bx + BALL_W;
    int bw  = bx1 > bx0 ? bx1 - bx0 : 0;

    /* ── Title bar ───────────────────────────────────────────────────── */
    graphics_fill_rect(0, Y_TITLE, w, 10, GFX_BLACK);
    snprintf(buf, sizeof(buf), "GHOST TEST  #%d  %dms", (int)frame, (int)delta_ms);
    graphics_draw_string_color_bg(2, Y_TITLE + 1, buf, GFX_WHITE, GFX_TRANSPARENT);

    /* ── Timing row ──────────────────────────────────────────────────── */
    graphics_fill_rect(0, Y_TIMING, w, 10, GFX_WHITE);
    if (delta_ms > 0) {
        snprintf(buf, sizeof(buf), "last:%dms  min=%d  max=%d",
                 (int)delta_ms, (int)min_ms, (int)max_ms);
    } else {
        snprintf(buf, sizeof(buf), "warming up...");
    }
    graphics_draw_string(2, Y_TIMING + 1, buf);

    graphics_fill_rect(0, Y_SEP1, w, 1, GFX_BLACK);

    /* ── Track 0: black ball on white ────────────────────────────────── */
    graphics_fill_rect(0, Y_T0, w, TRACK_H, GFX_WHITE);
    /* Center reference line */
    graphics_fill_rect(0, Y_T0 + TRACK_H / 2, w, 1, GFX_BLACK);
    /* Track label */
    graphics_draw_string(2, Y_T0 + 1, "B");
    /* Ball */
    if (bw > 0) {
        graphics_fill_rect(bx0, Y_T0 + BALL_MARGIN,
                           bw,  TRACK_H - BALL_MARGIN * 2, GFX_BLACK);
    }

    /* ── Track 1: white ball on black ────────────────────────────────── */
    graphics_fill_rect(0, Y_T1, w, TRACK_H, GFX_BLACK);
    graphics_fill_rect(0, Y_T1 + TRACK_H / 2, w, 1, GFX_WHITE);
    graphics_draw_string_color_bg(2, Y_T1 + 1, "W", GFX_WHITE, GFX_TRANSPARENT);
    if (bw > 0) {
        graphics_fill_rect(bx0, Y_T1 + BALL_MARGIN,
                           bw,  TRACK_H - BALL_MARGIN * 2, GFX_WHITE);
    }

    /* ── Track 2: red ball on white ──────────────────────────────────── */
    graphics_fill_rect(0, Y_T2, w, TRACK_H, GFX_WHITE);
    graphics_fill_rect(0, Y_T2 + TRACK_H / 2, w, 1, GFX_RED);
    graphics_draw_string(2, Y_T2 + 1, "R");
    if (bw > 0) {
        graphics_fill_rect(bx0, Y_T2 + BALL_MARGIN,
                           bw,  TRACK_H - BALL_MARGIN * 2, GFX_RED);
    }

    /* ── Bottom info ──────────────────────────────────────────────────── */
    graphics_fill_rect(0, Y_SEP2, w, 1, GFX_BLACK);

    graphics_fill_rect(0, Y_INFO, w, 12, GFX_WHITE);
    snprintf(buf, sizeof(buf), "[0]=%02X [35]=%02X [57]=%02X [59]=%02X",
             (int)ssd1675a_get_lut_byte(0),
             (int)ssd1675a_get_lut_byte(35),
             (int)ssd1675a_get_lut_byte(57),
             (int)ssd1675a_get_lut_byte(59));
    graphics_draw_string(2, Y_INFO, buf);

    graphics_fill_rect(0, Y_STATUS, w, h - Y_STATUS, GFX_WHITE);
    snprintf(buf, sizeof(buf), "spd=%dpx/f  %s",
             BALL_SPEED, custom_lut ? "CUSTOM_LUT" : "BUILTIN_LUT");
    graphics_draw_string(2, Y_STATUS, buf);

#undef BALL_W
#undef BALL_SPEED
#undef TRACK_H
#undef BALL_MARGIN
}

/* ── Low-battery warning screen ──────────────────────────────────────────── */
void display_screens_render_low_battery(int battery_mv, int64_t uptime_sec)
{
    int w = graphics_get_width();
    int h = graphics_get_height();
    char buf[64];
    char time_part[20];

    format_uptime(uptime_sec, time_part, sizeof(time_part));

    /* The 296x128 layout, scaled up 2x on the big panel. */
    int s = (h >= 200) ? 2 : 1;

    /* White background */
    graphics_clear(GFX_WHITE);

    /* Red banner at top */
    graphics_fill_rect(0, 0, w, 22 * s, GFX_RED);
    graphics_draw_string_color_scaled(w / 2 - 33 * s, 7 * s, "LOW BATTERY", GFX_BLACK, s);

    /* Battery voltage - large */
    { char cv[3]; fmt_d2(cv, (battery_mv % 1000) / 10);
      snprintf(buf, sizeof(buf), "%d.%sV", battery_mv / 1000, cv); }
    graphics_draw_string_color_scaled(w / 2 - 15 * 2 * s, 30 * s, buf, GFX_BLACK, 2 * s);

    /* Info text */
    draw_text(10 * s, 55 * s, "Display updates stopped.", s);
    draw_text(10 * s, 67 * s, "BLE active for commands.", s);

    snprintf(buf, sizeof(buf), "Uptime: %s", time_part);
    draw_text(10 * s, 85 * s, buf, s);

    /* Red warning bar at bottom */
    graphics_fill_rect(0, h - 14 * s, w, 14 * s, GFX_RED);
    graphics_draw_string_color_scaled(10 * s, h - 12 * s, "Charge or replace battery", GFX_BLACK, s);
}

/* ── Farewell / shutdown screen ──────────────────────────────────────────── */
void display_screens_render_shutdown(int battery_mv, int64_t uptime_sec)
{
    int w = graphics_get_width();
    int h = graphics_get_height();
    char buf[64];
    char time_part[20];
    struct tm t;

    format_uptime(uptime_sec, time_part, sizeof(time_part));
    get_system_time(&t);

    /* The 296x128 layout, scaled up 2x on the big panel. */
    int s = (h >= 200) ? 2 : 1;

    /* White background */
    graphics_clear(GFX_WHITE);

    /* Red border */
    graphics_fill_rect(0, 0, w, 3 * s, GFX_RED);
    graphics_fill_rect(0, h - 3 * s, w, 3 * s, GFX_RED);
    graphics_fill_rect(0, 0, 3 * s, h, GFX_RED);
    graphics_fill_rect(w - 3 * s, 0, 3 * s, h, GFX_RED);

    /* SHUTDOWN text in red */
    graphics_draw_string_color_scaled(w / 2 - 48 * s, 12 * s, "SHUTDOWN", GFX_RED, 2 * s);

    /* Time of shutdown */
    { char hh[3], mm[3], ss[3]; fmt_d2(hh, t.tm_hour); fmt_d2(mm, t.tm_min); fmt_d2(ss, t.tm_sec);
      snprintf(buf, sizeof(buf), "%s:%s:%s", hh, mm, ss); }
    graphics_draw_string_scaled(w / 2 - 48 * s, 35 * s, buf, 2 * s);

    /* Battery */
    snprintf(buf, sizeof(buf), "Battery: %d mV", battery_mv);
    draw_text(10 * s, 60 * s, buf, s);

    /* Uptime */
    snprintf(buf, sizeof(buf), "Uptime: %s", time_part);
    draw_text(10 * s, 74 * s, buf, s);

    /* Message */
    graphics_draw_string_color_scaled(10 * s, 94 * s, "Deep sleep to protect", GFX_RED, s);
    graphics_draw_string_color_scaled(10 * s, 106 * s, "battery. Replace/charge", GFX_RED, s);
    graphics_draw_string_color_scaled(10 * s, 118 * s, "to restart.", GFX_RED, s);
}
