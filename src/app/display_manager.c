#include "display_manager.h"
#include "battery.h"
#include "display_screens.h"
#include "system_time.h"
#include "ble/ble_service.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <drivers/ssd1675a.h>
#include <lib/graphics.h>
#include <zephyr/logging/log.h>
#include <limits.h>

LOG_MODULE_REGISTER(display_manager, LOG_LEVEL_INF);

static const struct device *gpio_dev_dm;

static bool screensaver_enabled = true;
static bool keep_display_on = false;
static bool streaming_active = false;
static int update_counter = 59; // Start at 59 so first increment hits 60 -> Full Update

// DC-balance maintenance: after this many streaming partial frames, force one
// full 0xC7 cycle to prevent particle polarization / ghost burn-in.
// At ~270ms/frame: 500 frames ≈ 2.25min between maintenance passes (~1-2s pause each).
#define STREAM_REFRESH_INTERVAL 500
static int stream_partial_count = 0;

static int screensaver_mode = SCREENSAVER_MODE_STATIC;

/* When true, the device sends TELE: lines after every display update so the
 * host app can track actual frame timing without polling. */
static bool tele_enabled = false;

void display_manager_set_tele_enabled(bool en) { tele_enabled = en; }
bool display_manager_get_tele_enabled(void)     { return tele_enabled; }

static int32_t lut_test_frame    = 0;
static int64_t lut_test_last_ms  = 0;
static int32_t lut_test_cur_ms   = 0;
static int32_t lut_test_min_ms   = 0;
static int32_t lut_test_max_ms   = 0;

// Semaphore to control screensaver loop (1 = run/wake, 0 = wait/timeout)
static K_SEM_DEFINE(sem_screensaver_wake, 0, 1);

K_MUTEX_DEFINE(display_lock);

static void stop_streaming_if_active(void) {
    if (streaming_active) {
        ssd1675a_end_streaming();
        streaming_active = false;
    }
}

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

// Like perform_display_update() but uses the red-clearing LUT (VSL on red
// channel) to actively drive red pigment away.  Called after clean/nuke cycles
// to eliminate the reddish tint left by the "red fixation" phases of the main LUT.
static void perform_display_update_flush_red(void) {
    if (!gpio_dev_dm) return;
    ssd1675a_init(gpio_dev_dm);
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    ssd1675a_update_display_flush_red();
    if (!screensaver_enabled && !keep_display_on) {
        ssd1675a_sleep();
        ssd1675a_power_off();
    }
}

void display_manager_update_partial(void) {
    if (!gpio_dev_dm) return;

    if (keep_display_on) {
        stream_partial_count++;

        // Periodic DC-balance refresh: break out of streaming for one 0xC7 cycle
        // to prevent particle polarization ghost burn-in.
        if (streaming_active && (stream_partial_count % STREAM_REFRESH_INTERVAL == 0)) {
            stop_streaming_if_active();
            ssd1675a_init_partial(gpio_dev_dm);
            ssd1675a_display_buffer_fast(graphics_get_buffer());
            ssd1675a_update_partial();  // 0xC7: full HV cycle with current LUT
            return;                     // streaming restarts on next call
        }

        if (!streaming_active) {
            ssd1675a_init_partial(gpio_dev_dm);
            ssd1675a_display_buffer_fast(graphics_get_buffer());
            ssd1675a_begin_streaming();
            streaming_active = true;
        } else {
            ssd1675a_display_buffer_fast(graphics_get_buffer());
        }
        ssd1675a_update_frame_stream();
    } else {
        stop_streaming_if_active();
        ssd1675a_init_partial(gpio_dev_dm);
        ssd1675a_display_buffer_fast(graphics_get_buffer());
        ssd1675a_update_partial();
        if (!screensaver_enabled) {
            ssd1675a_sleep();
            ssd1675a_power_off();
        }
    }
}



void display_manager_update_partial_nowait(void) {
    if (!gpio_dev_dm) return;

    /* Non-streaming path must always block — fall back to regular update. */
    if (!keep_display_on) {
        display_manager_update_partial();
        return;
    }

    stream_partial_count++;

    /* Periodic DC-balance: every STREAM_REFRESH_INTERVAL frames do a full
     * 0xC7 HV cycle. This one always blocks (caller's wait_busy already ran). */
    if (streaming_active && (stream_partial_count % STREAM_REFRESH_INTERVAL == 0)) {
        stop_streaming_if_active();
        ssd1675a_init_partial(gpio_dev_dm);
        ssd1675a_display_buffer_fast(graphics_get_buffer());
        ssd1675a_update_partial();
        return;
    }

    if (!streaming_active) {
        ssd1675a_init_partial(gpio_dev_dm);
        ssd1675a_display_buffer_fast(graphics_get_buffer());
        ssd1675a_begin_streaming();
        streaming_active = true;
    } else {
        ssd1675a_display_buffer_fast(graphics_get_buffer());
    }
    /* Trigger display refresh but return immediately — display runs in background. */
    ssd1675a_trigger_frame_stream_nowait();
}

void display_manager_set_partial_mode(int mode) {
    stop_streaming_if_active();  // new LUT must be loaded in next begin_streaming()
    if (mode == 0) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TURBO);
    else if (mode == 1) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_BALANCED);
    else if (mode == 2) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_STABLE);
    else if (mode == 3) ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_CLEAN);
}

void display_manager_begin_streaming(void) {
    ssd1675a_begin_streaming();
}

void display_manager_update_frame_stream(void) {
    ssd1675a_update_frame_stream();
}

void display_manager_end_streaming(void) {
    ssd1675a_end_streaming();
}

void display_manager_reset_lut_test(void) {
    lut_test_frame   = 0;
    lut_test_last_ms = 0;
    lut_test_cur_ms  = 0;
    lut_test_min_ms  = 0;
    lut_test_max_ms  = 0;
    stop_streaming_if_active();  // reload LUT on next begin_streaming()
}

void display_manager_get_lut_test_stats(int32_t *frame_out, int32_t *cur_ms_out,
                                        int32_t *min_ms_out, int32_t *max_ms_out)
{
    if (frame_out)   *frame_out   = lut_test_frame;
    if (cur_ms_out)  *cur_ms_out  = lut_test_cur_ms;
    if (min_ms_out)  *min_ms_out  = lut_test_min_ms;
    if (max_ms_out)  *max_ms_out  = lut_test_max_ms;
}

void display_manager_update_lut_test(void) {
    if (!gpio_dev_dm) return;

    int64_t now = k_uptime_get();
    int32_t delta_ms = 0;

    if (lut_test_last_ms != 0) {
        delta_ms = (int32_t)(now - lut_test_last_ms);
        if (lut_test_frame > 0) {
            if (lut_test_min_ms == 0 || delta_ms < lut_test_min_ms) lut_test_min_ms = delta_ms;
            if (delta_ms > lut_test_max_ms) lut_test_max_ms = delta_ms;
        }
    }
    lut_test_last_ms = now;
    lut_test_cur_ms  = delta_ms;

    display_screens_render_lut_test(lut_test_frame, delta_ms,
                                    lut_test_min_ms, lut_test_max_ms,
                                    ssd1675a_get_use_custom_lut());
    lut_test_frame++;

    /* Write BOTH BW and Red buffers so the red track in the ghosting test
     * is visible. ssd1675a_display_buffer_fast() skips the red buffer. */
    if (!streaming_active) {
        ssd1675a_init_partial(gpio_dev_dm);
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
        ssd1675a_begin_streaming();
        streaming_active = true;
    } else {
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    }
    ssd1675a_update_frame_stream();

    /* Send telemetry every frame once we have a valid measurement (frame >= 2). */
    if (tele_enabled && delta_ms > 0) {
        ble_printf("TELE:ltest frame=%d last=%d min=%d max=%d lut=%s\r\n",
                   (int)(lut_test_frame - 1), (int)delta_ms,
                   (int)lut_test_min_ms, (int)lut_test_max_ms,
                   ssd1675a_get_use_custom_lut() ? "custom" : "builtin");
    }
}

void display_manager_set_screensaver_mode(int mode) {
    if (mode != screensaver_mode) {
        stop_streaming_if_active();
    }
    screensaver_mode = mode;
    if (mode == SCREENSAVER_MODE_DYNAMIC) {
        display_manager_set_keep_on(true);
        if (!ssd1675a_get_use_custom_lut()) {
            display_manager_set_partial_mode(0); // TURBO default for DSAVER (overrideable via MODE:)
        }
        k_sem_give(&sem_screensaver_wake);
        display_screens_reset_dynamic();
    } else if (mode == SCREENSAVER_MODE_LUT_TEST) {
        display_manager_set_keep_on(true);
        display_manager_reset_lut_test();
        k_sem_give(&sem_screensaver_wake);
    } else {
        display_manager_set_keep_on(false);
        display_manager_set_partial_mode(1);
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
        if (tele_enabled) {
            static int32_t dyn_frame_ctr = 0;
            dyn_frame_ctr++;
            if (dyn_frame_ctr % 10 == 0) {
                ble_printf("TELE:dynamic frame=%d last=%dms\r\n",
                           (int)dyn_frame_ctr, (int)last_dur);
            }
        }
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
    stop_streaming_if_active();
    stream_partial_count = 0;
    for (int i = 0; i < 7; i++) {
        graphics_clear(GFX_BLACK);
        perform_display_update();
        graphics_clear(GFX_WHITE);
        perform_display_update();
        graphics_clear(GFX_RED);
        perform_display_update();
    }
    // Two red-clearing passes: VSL on red channel actively drives pigment away,
    // eliminating the reddish tint left by the "red fixation" phases in lut_data.
    graphics_clear(GFX_WHITE);
    perform_display_update_flush_red();
    perform_display_update_flush_red();
}

void display_manager_deep_clean(int cycles) {
    if (!gpio_dev_dm) return;
    stop_streaming_if_active();
    stream_partial_count = 0;
    // Phase 1: white-only pre-soak — repeated VSL application kills VSH1
    // polarization from streaming without re-applying it (no black phase).
    for (int i = 0; i < cycles; i++) {
        graphics_clear(GFX_WHITE);
        perform_display_update();
    }
    // Phase 2: W→R cycles — VSL depolarize, then VSH2 drive red.
    // No black phase here either: black (LUT0 Ph4) re-applies 472f of VSH1,
    // which is exactly what caused the red burn-in.
    for (int i = 0; i < cycles; i++) {
        graphics_clear(GFX_WHITE);
        perform_display_update();
        graphics_clear(GFX_RED);
        perform_display_update();
    }
    graphics_clear(GFX_WHITE);
    perform_display_update_flush_red();
    perform_display_update_flush_red();
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
            if (screensaver_mode == SCREENSAVER_MODE_LUT_TEST) {
                display_manager_update_lut_test();
            } else {
                display_manager_update_status();
            }
        }

        if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC ||
            screensaver_mode == SCREENSAVER_MODE_LUT_TEST) {
            k_sem_take(&sem_screensaver_wake, K_MSEC(10));
        } else {
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
    if (!enable) {
        stop_streaming_if_active();
    }
    keep_display_on = enable;
    if (!enable && !screensaver_enabled) {
        ssd1675a_sleep();
        ssd1675a_power_off();
    }
}
