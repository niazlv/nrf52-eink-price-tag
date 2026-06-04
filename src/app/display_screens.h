#ifndef APP_DISPLAY_SCREENS_H
#define APP_DISPLAY_SCREENS_H

#include <stdint.h>
#include <time.h>

typedef struct {
    struct tm time;
    int battery_mv;
    int battery_percent;
    int32_t last_render_ms;
    int64_t uptime_sec;
} display_status_model_t;

void display_screens_reset_dynamic(void);
void display_screens_render_status_static(const display_status_model_t *model);
void display_screens_render_status_dynamic(const display_status_model_t *model);
void display_screens_render_text(const char *text);
void display_screens_render_partial_test(int32_t frame,
                                         int64_t uptime_ms,
                                         int32_t delta_ms,
                                         const char *addr);
void display_screens_render_animation_frame(int x, int y, int size, int frame);

#endif // APP_DISPLAY_SCREENS_H
