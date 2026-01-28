#include "display_manager.h"
#include "battery.h"
#include "system_time.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <drivers/ssd1675a.h> 
#include <lib/graphics.h>
#include <stdio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_manager, LOG_LEVEL_INF);

static const struct device *gpio_dev_dm = NULL;

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 296

void display_manager_init(void) {
    gpio_dev_dm = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev_dm)) {
        LOG_ERR("GPIO device not ready");
        return;
    }
    // ssd1675a and graphics init might be needed here or lazy
    // In main.c it was called explicitly
    // graphics_init(); from main
}

static void perform_display_update(void) {
    if (!gpio_dev_dm) return;

    ssd1675a_init(gpio_dev_dm);
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    ssd1675a_update_display();
    ssd1675a_sleep();
    ssd1675a_power_off();
}

void display_manager_update_status(void) {
    if (!gpio_dev_dm) return;
    
    int64_t start_render = k_uptime_get();
    
    graphics_clear(GFX_WHITE);
    
    // 1. Time
    struct tm t;
    get_system_time(&t);
    
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t.tm_hour, t.tm_min);
    graphics_draw_string_scaled(70, 30, time_str, 5); 
    
    // 2. Date
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d", t.tm_mday, t.tm_mon+1, t.tm_year+1900);
    graphics_draw_string_scaled(58, 80, date_str, 3);
    
    // 3. Battery
    int mv = battery_read_mv();
    int pct = (mv > 3000) ? 100 : (mv < 2000 ? 0 : (mv-2000)/10);
    graphics_draw_battery(260, 5, pct);
    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%dmV", mv);
    graphics_draw_string(260, 18, bat_str);
    
    // 4. Stats (Bottom)
    static int32_t last_dur = 0;
    int64_t uptime_ms = k_uptime_get();
    
    char stat_str[32];
    snprintf(stat_str, sizeof(stat_str), "Up: %llds | R: %dms", uptime_ms/1000, last_dur);
    graphics_draw_string(5, graphics_get_height() - 20, stat_str);
    
    perform_display_update();
    
    last_dur = (int32_t)(k_uptime_get() - start_render);
}

void display_manager_show_text(const char *text) {
    if (!text) return;
    graphics_clear(GFX_WHITE); 
    // Frame
    for(int x=0; x<128; x++) { graphics_draw_pixel(x, 0, GFX_BLACK); graphics_draw_pixel(x, 295, GFX_BLACK); }
    for(int y=0; y<296; y++) { graphics_draw_pixel(0, y, GFX_BLACK); graphics_draw_pixel(127, y, GFX_BLACK); }
    graphics_draw_string(5, 5, text);
    
    perform_display_update();
}

void display_manager_clean(void) {
     if (!gpio_dev_dm) return;
    
    // Cycle 3 times
    for (int i=0; i<3; i++) {
        graphics_clear(GFX_BLACK);
        perform_display_update();
        k_msleep(500);
        
        graphics_clear(GFX_WHITE);
        perform_display_update();
        k_msleep(500);
        
        graphics_clear(GFX_RED);
        perform_display_update();
        k_msleep(500);
    }
    graphics_clear(GFX_WHITE);
    perform_display_update();
}

void display_manager_clear(void) {
    graphics_clear(GFX_WHITE);
}

// Semaphore to control screensaver loop (1 = run/wake, 0 = wait/timeout)
static K_SEM_DEFINE(sem_screensaver_wake, 0, 1);
static bool screensaver_enabled = true;

#define SCREENSAVER_INTERVAL_SEC 60

void display_manager_enable_screensaver(bool enable) {
    bool prev = screensaver_enabled;
    screensaver_enabled = enable;
    if (enable && !prev) {
        // Just switched on, force update
        k_sem_give(&sem_screensaver_wake);
    }
}

bool display_manager_is_screensaver_active(void) {
    return screensaver_enabled;
}

// Thread Function
static void screensaver_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    // Initial wait to let system boot
    k_sleep(K_SECONDS(2));

    while (1) {
        if (screensaver_enabled) {
            display_manager_update_status();
        }

        struct tm t;
        get_system_time(&t);
        
        // Calculate seconds to next minute (60 - current_seconds)
        // Add a small buffer (100ms) to ensure we land inside the new minute? 
        // Or just target strict boundary. k_sleep is minimum delay.
        int seconds_to_wait = 60 - t.tm_sec;
        if (seconds_to_wait < 1) seconds_to_wait = 1;
        
        // Wait for next interval OR manual wake event
        // returns 0 if semaphore taken (manual wake), -EAGAIN on timeout (interval)
        k_sem_take(&sem_screensaver_wake, K_SECONDS(seconds_to_wait));
    }
}

K_THREAD_DEFINE(screensaver_tid, 1024, screensaver_thread, NULL, NULL, NULL, 7, 0, 0);

void display_manager_force_update(void) {
    if (screensaver_enabled) {
        k_sem_give(&sem_screensaver_wake);
    } else {
        perform_display_update();
    }
}

void display_manager_set_rotation(int rot) {
    graphics_set_rotation(rot);
    if (screensaver_enabled) {
         k_sem_give(&sem_screensaver_wake);
    }
}
