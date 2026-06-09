#include "display_screens.h"
#include <lib/graphics.h>
#include <lib/life.h>
#include <stdio.h>

static char date_str[24];
static char stat_str[48];
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
        snprintf(buf, buf_size, "%dd%dh%dm%ds", d, h, m, s);
    } else if (h > 0) {
        snprintf(buf, buf_size, "%dh%dm%ds", h, m, s);
    } else if (m > 0) {
        snprintf(buf, buf_size, "%dm%ds", m, s);
    } else {
        snprintf(buf, buf_size, "%ds", s);
    }
}

static void render_stats(const display_status_model_t *model, int x, int y)
{
    char time_part[20] = {0};
    format_uptime(model->uptime_sec, time_part, sizeof(time_part));
    snprintf(stat_str, sizeof(stat_str), "Up: %s | R: %dms", time_part, model->last_render_ms);
    graphics_draw_string(x, y, stat_str);
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

void display_screens_render_animation_frame(int x, int y, int size, int frame)
{
    char buf[32];
    int height = graphics_get_height();

    graphics_clear(GFX_WHITE);
    graphics_fill_rect(x, y, size, size, GFX_BLACK);

    snprintf(buf, sizeof(buf), "TURBO FRAME %d", frame);
    graphics_draw_string(10, height - 16, buf);
}
