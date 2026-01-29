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
// static int rotation = 1; // Unused
static bool screensaver_enabled = true;
static bool keep_display_on = false; // New flag for animations
static int update_counter = 59; // Start at 59 so first increment hits 60 -> Full Update

// Screensaver Modes
#define SCREENSAVER_MODE_STATIC  0
#define SCREENSAVER_MODE_DYNAMIC 1
static int screensaver_mode = SCREENSAVER_MODE_STATIC;

// Animation State
static int anim_x = 10;
static int anim_y = 10;
static int anim_vx = 4;
static int anim_vy = 4;
static int anim_size = 20;

// Semaphore to control screensaver loop (1 = run/wake, 0 = wait/timeout)
static K_SEM_DEFINE(sem_screensaver_wake, 0, 1);

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
    if (!screensaver_enabled && !keep_display_on) {
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
    ssd1675a_update_partial(); // Uses the custom LUT
    
    if (!screensaver_enabled && !keep_display_on) {
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

void display_manager_set_screensaver_mode(int mode) {
    screensaver_mode = mode;
    // If Dynamic, we MUST keep display on to avoid flicker
    if (mode == SCREENSAVER_MODE_DYNAMIC) {
        display_manager_set_keep_on(true);
        // Force Turbo Mode for speed
        display_manager_set_partial_mode(1);
        // Force wake of thread
        k_sem_give(&sem_screensaver_wake);
    } else {
        // Restore defaults
        display_manager_set_keep_on(false);
        display_manager_set_partial_mode(1); // Balanced
    }
}

void display_manager_update_status(void) {
    if (!gpio_dev_dm) return;
    
    int64_t start_render = k_uptime_get();
    
    // 1. Time
    struct tm t;
    get_system_time(&t);

    if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC) {
        // --- DYNAMIC MODE ---
        graphics_clear(GFX_WHITE);

        int width = graphics_get_width();
        int height = graphics_get_height();

        // Update Animation
        anim_x += anim_vx;
        anim_y += anim_vy;

        // Bounce
        if (anim_x <= 0 || anim_x + anim_size >= width) anim_vx = -anim_vx;
        if (anim_y <= 0 || anim_y + anim_size >= height) anim_vy = -anim_vy;

        // Clamp
        if (anim_x < 0) anim_x = 0;
        if (anim_x + anim_size > width) anim_x = width - anim_size;
        if (anim_y < 0) anim_y = 0;
        if (anim_y + anim_size > height) anim_y = height - anim_size;

        // Draw Box
        for (int i = 0; i < anim_size; i++) {
            for (int k_idx = 0; k_idx < anim_size; k_idx++) { // renamed iterator to avoid conflict
                graphics_draw_pixel(anim_x + i, anim_y + k_idx, GFX_BLACK);
            }
        }

        // Draw Time with Seconds (Large)
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        
        // Center text (approx) - Scale 3
        // Char width = 6*3 = 18. Len = 8. Total width = 144. Too big for 128 width if portrait.
        // If width < 144, use smaller scale or wrap? 
        // 128 / 8 = 16 pixels per char. Scale 2 = 12px. 8*12 = 96. Fits.
        int scale = (width >= 150) ? 3 : 2; 
        int str_w = 8 * (6 * scale);
        int tx = (width - str_w) / 2;
        int ty = height / 2 - (4 * scale); // Centered Y

        graphics_draw_string_scaled(tx, ty, time_str, scale);

        // Draw Date below
        snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d", t.tm_mday, t.tm_mon+1, t.tm_year+1900);
        int date_scale = 1;
        int date_w = 10 * (6 * date_scale);
        graphics_draw_string_scaled((width - date_w)/2, ty + (10*scale), date_str, date_scale);

        // Day of Week
        const char *wday_str[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        if (t.tm_wday >= 0 && t.tm_wday <= 6) {
           int wday_scale = 1;
           int wday_w = 3 * (6 * wday_scale);
           graphics_draw_string_scaled((width - wday_w)/2, ty + (10*scale) + 15, wday_str[t.tm_wday], wday_scale);
        }

        // Battery (Top Right)
        int mv = battery_read_mv();
        int pct = (mv > 3000) ? 100 : (mv < 2000 ? 0 : (mv-2000)/10);
        // Assuming width is dynamic. If 128 (portrait), 260 is off screen.
        // Static mode: 260, 5. This implies Static mode was assuming Landscape (296 width)?
        // Wait, if rotation=1 (default), Width=296, Height=128.
        // So 260 fits.
        graphics_draw_battery(width - 40, 5, pct);
        char bat_str[16];
        snprintf(bat_str, sizeof(bat_str), "%dmV", mv);
        graphics_draw_string(width - 40, 18, bat_str);

        // Stats (Top Left)
        static int32_t last_dur = 0;
        int64_t uptime_s = k_uptime_get() / 1000;
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
        graphics_draw_string(5, 5, stat_str);

        // Always Partial Update in Dynamic Mode
        display_manager_update_partial();
        
        last_dur = (int32_t)(k_uptime_get() - start_render);

    } else {
        // --- STATIC MODE (Original) ---
        graphics_clear(GFX_WHITE);
        
        char time_str[8];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", t.tm_hour, t.tm_min);
        graphics_draw_string_scaled(70, 30, time_str, 5); 
        
        // 2. Date
        snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d", t.tm_mday, t.tm_mon+1, t.tm_year+1900);
        graphics_draw_string_scaled(58, 80, date_str, 3);
        
        // 2.1 Day of Week (Red)
        const char *wday_str[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        if (t.tm_wday >= 0 && t.tm_wday <= 6) {
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
        update_counter++;

        if (update_counter >= 60) {
            update_counter = 0;
            perform_display_update(); // Full Update
        } else {
            // Note: display_manager_update_partial controls power off based on flags
            display_manager_update_partial(); 
        }
        
        last_dur = (int32_t)(k_uptime_get() - start_render);
    }
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



// Semaphore to control screensaver loop (moved to top)
// static K_SEM_DEFINE(sem_screensaver_wake, 0, 1);
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

        if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC) {
            // High refresh rate for animation
            // Wait e.g. 50ms (20fps target, though bus limits it)
            k_sem_take(&sem_screensaver_wake, K_MSEC(50));
        } else {
            // Static Mode - wait for next minute
            struct tm t;
            get_system_time(&t);
            int seconds_to_wait = 60 - t.tm_sec;
            if (seconds_to_wait < 1) seconds_to_wait = 1;
            k_sem_take(&sem_screensaver_wake, K_SECONDS(seconds_to_wait));
        }
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

void display_manager_set_keep_on(bool enable) {
    keep_display_on = enable;
    if (!enable && !screensaver_enabled) {
         // If disabling keep-on and screensaver is also off, power down now
         ssd1675a_sleep();
         ssd1675a_power_off();
    }
}
