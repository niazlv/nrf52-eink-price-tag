#include "display_screens.h"
#include "display_manager.h"
#include <lib/graphics.h>
#include <lib/life.h>
#include <drivers/ssd1675a.h>
#include <stdio.h>
#include <stdbool.h>

static char date_str[40];
static char stat_str[48];
static char power_str[32];
static life_world_t life_world;

static const char *const wday_names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

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

static void render_stats(const display_status_model_t *model, int x, int y)
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
    graphics_draw_string(x, y, stat_str);

    snprintf(power_str, sizeof(power_str), "mAh:%u.%03u I:%d.%dmA",
             (unsigned int)mah,
             (unsigned int)mah_frac,
             current_ma_x10 / 10,
             current_ma_x10 % 10);
    graphics_draw_string(x, y + 10, power_str);
}

static void render_battery(const display_status_model_t *model, int x, int y)
{
    char bat_str[16];
    graphics_draw_battery(x, y, model->battery_percent);
    snprintf(bat_str, sizeof(bat_str), "%dmV", model->battery_mv);
    graphics_draw_string(x, y + 13, bat_str);
}

void display_screens_reset_dynamic(void)
{
    life_world.initialized = false;
}

void display_screens_render_status_static(const display_status_model_t *model)
{
    char time_str[8];

    graphics_clear(GFX_WHITE);

    snprintf(time_str, sizeof(time_str), "%02d:%02d", model->time.tm_hour, model->time.tm_min);
    graphics_draw_string_scaled(70, 30, time_str, 5);

    snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d",
             model->time.tm_mday,
             model->time.tm_mon + 1,
             model->time.tm_year + 1900);
    graphics_draw_string_scaled(58, 80, date_str, 3);

    if (model->time.tm_wday >= 0 && model->time.tm_wday <= 6) {
        graphics_draw_string_scaled(130, 110, wday_names[model->time.tm_wday], 2);
    }

    render_battery(model, 260, 5);
    render_stats(model, 5, 0);
}

void display_screens_render_status_dynamic(const display_status_model_t *model)
{
    int width = graphics_get_width();
    int height = graphics_get_height();
    int life_w = width / LIFE_CELL_SIZE;
    int life_h = height / LIFE_CELL_SIZE;
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
            if (life_world.cells[y][x]) {
                graphics_fill_rect(x * LIFE_CELL_SIZE,
                                   y * LIFE_CELL_SIZE,
                                   LIFE_CELL_SIZE - 1,
                                   LIFE_CELL_SIZE - 1,
                                   GFX_BLACK);
            }
        }
    }

    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
             model->time.tm_hour,
             model->time.tm_min,
             model->time.tm_sec);

    int scale = (width >= 150) ? 3 : 2;
    int str_w = 8 * (6 * scale);
    int tx = (width - str_w) / 2;
    int ty = height / 2 - (4 * scale);

    graphics_draw_string_scaled(tx, ty, time_str, scale);

    snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d",
             model->time.tm_mday,
             model->time.tm_mon + 1,
             model->time.tm_year + 1900);
    graphics_draw_string_scaled((width - 10 * 6) / 2, ty + (10 * scale), date_str, 1);

    if (model->time.tm_wday >= 0 && model->time.tm_wday <= 6) {
        graphics_draw_string_scaled((width - 3 * 6) / 2,
                                    ty + (10 * scale) + 15,
                                    wday_names[model->time.tm_wday],
                                    1);
    }

    render_battery(model, width - 40, 5);
    render_stats(model, 5, 5);
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
    graphics_draw_string_color(2, 1, buf, GFX_WHITE);

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
    graphics_draw_string_color(2, Y_TITLE + 1, buf, GFX_WHITE);

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
    graphics_draw_string_color(2, Y_T1 + 1, "W", GFX_WHITE);
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
