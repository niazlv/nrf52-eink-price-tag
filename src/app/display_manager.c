#include "display_manager.h"
#include "battery.h"
#include "display_screens.h"
#include "system_time.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <drivers/ssd1675a.h> 
#include <lib/graphics.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_manager, LOG_LEVEL_INF);

static const struct device *gpio_dev_dm;

static bool screensaver_enabled = true;
static bool keep_display_on = false; // New flag for animations
static int update_counter = 59; // Start at 59 so first increment hits 60 -> Full Update

static int screensaver_mode = SCREENSAVER_MODE_STATIC;

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
        
        display_screens_reset_dynamic();
        
    } else {
        // Restore defaults
        display_manager_set_keep_on(false);
        display_manager_set_partial_mode(1); // Balanced
    }
}

void display_manager_update_status(void) {
    if (!gpio_dev_dm) return;
    
    int64_t start_render = k_uptime_get();
    static int32_t last_dur = 0;
    int mv = battery_read_mv();
    display_status_model_t model = {0};

    get_system_time(&model.time);
    model.battery_mv = mv;
    model.battery_percent = (mv > 3000) ? 100 : (mv < 2000 ? 0 : (mv - 2000) / 10);
    model.last_render_ms = last_dur;
    model.uptime_sec = k_uptime_get() / 1000;

    if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC) {
        display_screens_render_status_dynamic(&model);
        display_manager_update_partial();
        last_dur = (int32_t)(k_uptime_get() - start_render);
    } else {
        display_screens_render_status_static(&model);
        update_counter++;

        if (update_counter >= 60) {
            update_counter = 0;
            perform_display_update(); // Full Update
        } else {
            display_manager_update_partial(); 
        }

        last_dur = (int32_t)(k_uptime_get() - start_render);
    }
}

void display_manager_show_text(const char *text) {
    if (!text) return;
    display_screens_render_text(text);
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
