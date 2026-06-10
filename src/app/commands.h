#ifndef APP_COMMANDS_H
#define APP_COMMANDS_H

#include <stdint.h>

/** @brief Initialize commands module (call once at startup). */
void commands_init(void);

/**
 * @brief Process received BLE data (text commands or binary vstream frames).
 * @param data  Raw bytes received over NUS RX.
 * @param len   Number of bytes.
 */
void commands_process(const void *data, uint16_t len);

/**
 * @brief Notify commands module that the BLE connection was lost.
 *
 * Cancels any in-progress vstream session and returns the display to the
 * screensaver so the device stays responsive after an unexpected disconnect.
 */
void commands_on_disconnect(void);

/** @brief Run the test loop directly (blocking). */
void cmd_test(char *args);

#endif // APP_COMMANDS_H
