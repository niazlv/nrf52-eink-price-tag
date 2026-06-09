#ifndef APP_DISPLAY_MANAGER_H
#define APP_DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize display manager
 */
void display_manager_init(void);

/**
 * @brief Update the status screen (Time, Date, Battery)
 */
void display_manager_update_status(void);

/**
 * @brief Show specific text on display
 * 
 * @param text Text to display
 */
void display_manager_show_text(const char *text);

/**
 * @brief Run a cleaning cycle (Black/White/Red)
 */
void display_manager_clean(void);

/**
 * @brief Clear display to white
 */
void display_manager_clear(void);

/**
 * @brief Force a display update loop (async or blocking depending on impl)
 */
void display_manager_force_update(void);

/**
 * @brief Force a partial (fast) display update (full HV cycle, ~700ms overhead)
 */
void display_manager_update_partial(void);

/**
 * @brief Streaming animation mode: charge HV rails once, then call
 *        update_frame_stream() per frame (~LUT wave time only), then end_streaming().
 */
void display_manager_begin_streaming(void);
void display_manager_update_frame_stream(void);
void display_manager_end_streaming(void);

/**
 * @brief Set the partial update mode (0=Turbo, 1=Balanced, 2=Stable)
 */
void display_manager_set_partial_mode(int mode);

/**
 * @brief Set display rotation
 * 
 * @param rot Rotation 0-3
 */
void display_manager_set_rotation(int rot);

/**
 * @brief Enable or disable automatic screensaver updates
 * 
 * @param enable true to enable, false to disable
 */
void display_manager_enable_screensaver(bool enable);
    
/**
 * @brief Keep display powered strictly ON (for animations)
 */
void display_manager_set_keep_on(bool enable);

/**
 * @brief Check if screensaver is currently active
 */
bool display_manager_is_screensaver_active(void);

#define SCREENSAVER_MODE_STATIC   0
#define SCREENSAVER_MODE_DYNAMIC  1
#define SCREENSAVER_MODE_LUT_TEST 2
/**
 * @brief Set screensaver style (Static/Minute, Dynamic/Animation, or LUT test)
 */
void display_manager_set_screensaver_mode(int mode);

/**
 * @brief Run one LUT test frame (render + partial update + timing measurement)
 */
void display_manager_update_lut_test(void);

/**
 * @brief Reset LUT test frame counter and timing stats
 */
void display_manager_reset_lut_test(void);

/**
 * @brief Query current LUT test timing stats (pass NULL for unused outputs)
 */
void display_manager_get_lut_test_stats(int32_t *frame_out, int32_t *cur_ms_out,
                                        int32_t *min_ms_out, int32_t *max_ms_out);

/**
 * @brief Enable/disable automatic TELE: BLE telemetry after every display update.
 *        Called by commands.c when HOST:1 / HOST:0 is received.
 */
void display_manager_set_tele_enabled(bool en);
bool display_manager_get_tele_enabled(void);

#endif // APP_DISPLAY_MANAGER_H
