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
/**
 * @brief Force a display update loop (async or blocking depending on impl)
 */
void display_manager_force_update(void);

/**
 * @brief Force a partial (fast) display update
 */
void display_manager_update_partial(void);

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

#define SCREENSAVER_MODE_STATIC  0
#define SCREENSAVER_MODE_DYNAMIC 1
/**
 * @brief Set screensaver style (Static/Minute or Dynamic/Animation)
 */
void display_manager_set_screensaver_mode(int mode);

#endif // APP_DISPLAY_MANAGER_H
