#include "display_manager.h"
#include "battery.h"
#include "system_time.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <drivers/ssd1675a.h> 
#include <lib/graphics.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_manager, LOG_LEVEL_INF);

static const struct device *gpio_dev_dm;

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 296

static char date_str[24]; 
static char stat_str[48]; 
static int rotation = 1;
static bool screensaver_enabled = true;
static int update_counter = 59; // Start at 59 so first increment hits 60 -> Full Update

K_MUTEX_DEFINE(display_lock);

void display_manager_init(void) {
    gpio_dev_dm = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev_dm)) {
        LOG_ERR("GPIO_0 not found!");
        return;
    }
}

static void perform_display_update(void) {
    if (!gpio_dev_dm) return;

    // k_mutex_lock(&display_lock, K_FOREVER);

    ssd1675a_init(gpio_dev_dm);
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    ssd1675a_update_display();
    
    // Only power off/sleep if Screensaver is disabled. 
    // If active, we keep VCC On and avoid Deep Sleep to allow Partial Init (No Reset).
    if (!screensaver_enabled) {
        ssd1675a_sleep();
        ssd1675a_power_off();
    }

    // k_mutex_unlock(&display_lock);
}

void display_manager_update_partial(void) {
    if (!gpio_dev_dm) return;

    // k_mutex_lock(&display_lock, K_FOREVER);

    // Use Partial Init (Skips HW Reset to preserve RAM)
    ssd1675a_init_partial(gpio_dev_dm);
    
    // Note: display_buffer writes to the controller's RAM. 
    // Partial update relies on the new data being there.
    // Optimization: Skip Red Buffer write
    ssd1675a_display_buffer_fast(graphics_get_buffer());
    // Optimization: Skip Red Buffer write
    ssd1675a_display_buffer_fast(graphics_get_buffer());
    ssd1675a_update_partial(); // Uses the custom LUT
    
    if (!screensaver_enabled) {
        ssd1675a_sleep();
        ssd1675a_power_off();
    }

    // k_mutex_unlock(&display_lock);
}



void display_manager_set_partial_mode(int mode) {
    // No lock needed for simple variable set, but safe to add if complex
    if (mode == 0) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TURBO);
    else if (mode == 1) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_BALANCED);
    else if (mode == 2) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_STABLE);
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
    // char date_str[20]; // Use global static
    snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d", t.tm_mday, t.tm_mon+1, t.tm_year+1900);
    graphics_draw_string_scaled(58, 80, date_str, 3);
    
    // 2.1 Day of Week (Red)
    const char *wday_str[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    // Calculate Day of Week manually since mktime might not populate it reliably if we just set time?
    // Actually mktime DOES normalize struct tm. 
    // Let's rely on standard logic: get_system_time calls gmtime() which should set tm_wday.
    if (t.tm_wday >= 0 && t.tm_wday <= 6) {
        // Draw below Date, Centered (Scale 2)
        // Width ~ 3 chars * 6 * 2 = 36. Center = (296-36)/2 = 130
        graphics_draw_string_scaled(130, 110, wday_str[t.tm_wday], 2);
    }
    
    // 3. Battery
    int mv = battery_read_mv();
    int pct = (mv > 3000) ? 100 : (mv < 2000 ? 0 : (mv-2000)/10);
    graphics_draw_battery(260, 5, pct);
    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%dmV", mv);
    graphics_draw_string(260, 18, bat_str);
    
    // 4. Stats
    static int32_t last_dur = 0;
    int64_t uptime_s = k_uptime_get() / 1000;
    
    // char stat_str[48]; // Use global static
    char time_part[20] = {0};
    
    int d = uptime_s / 86400; uptime_s %= 86400;
    int h = uptime_s / 3600;  uptime_s %= 3600;
    int m = uptime_s / 60;    uptime_s %= 60;
    int s = uptime_s;
    
    if (d > 0) snprintf(time_part, sizeof(time_part), "%dd%dh%dm%ds", d, h, m, s);
    else if (h > 0) snprintf(time_part, sizeof(time_part), "%dh%dm%ds", h, m, s);
    else if (m > 0) snprintf(time_part, sizeof(time_part), "%dm%ds", m, s);
    else snprintf(time_part, sizeof(time_part), "%ds", s);

    snprintf(stat_str, sizeof(stat_str), "Up: %s | R: %dms", time_part, last_dur);
    graphics_draw_string(5, 0, stat_str);
    
    // Logic: Full Update once every 60 minutes (or on first run)
    // Partial Update otherwise.
    // static int update_counter = 59; // Moved to file scope
    update_counter++;

    if (update_counter >= 60) {
        update_counter = 0;
        perform_display_update(); // Full Update
    } else {
        display_manager_update_partial(); // Partial Update
    }
    
    // k_mutex_unlock(&display_lock);
    
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
// static bool screensaver_enabled = true; // Moved to top

#define SCREENSAVER_INTERVAL_SEC 60

void display_manager_enable_screensaver(bool enable) {
    bool prev = screensaver_enabled;
    screensaver_enabled = enable;
    if (enable && !prev) {
        // Just switched on, force update
        update_counter = 59; // Force Full Update next time
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
