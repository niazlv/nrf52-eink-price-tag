#ifndef APP_DISPLAY_SCREENS_H
#define APP_DISPLAY_SCREENS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    struct tm time;
    int battery_mv;
    int battery_percent;
    int32_t last_render_ms;
    int64_t uptime_sec;
    uint32_t energy_mah_x1000;
    int estimated_current_ua;
    int saver_mode;
    int partial_mode;
    int maintenance_countdown;
    bool custom_lut;
    bool keep_display_on;
    bool streaming_active;
    bool power_after_update;
} display_status_model_t;

void display_screens_reset_dynamic(void);
void display_screens_render_status_static(const display_status_model_t *model);
void display_screens_render_status_dynamic(const display_status_model_t *model);
void display_screens_render_text(const char *text);
void display_screens_render_partial_test(int32_t frame,
                                         int64_t uptime_ms,
                                         int32_t delta_ms,
                                         const char *addr);
void display_screens_render_animation_frame(int x, int y, int size, int frame,
                                             int32_t delta_ms);
void display_screens_render_palette_test(void);
void display_screens_render_tone_test_pass(int pass, int max_passes);

/**
 * @brief Render LUT test scene: moving bands + frame counter + timing stats.
 *
 * @param frame          Frame index (0-based)
 * @param delta_ms       Time since last frame (0 on first frame)
 * @param min_ms         Minimum observed frame time
 * @param max_ms         Maximum observed frame time
 * @param custom_lut     true if lut_data[] is a user-loaded LUT
 */
void display_screens_render_lut_test(int32_t frame, int32_t delta_ms,
                                     int32_t min_ms, int32_t max_ms,
                                     bool custom_lut);

#endif // APP_DISPLAY_SCREENS_H
