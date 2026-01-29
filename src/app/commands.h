#ifndef APP_COMMANDS_H
#define APP_COMMANDS_H

#include <stdint.h>

/**
 * @brief Initialize commands module
 */
void commands_init(void);

/**
 * @brief Process received data string as command
 * 
 * @param data Input data
 * @param len Length of data
 */
void commands_process(const void *data, uint16_t len);

/**
 * @brief Run the test loop directly (blocking)
 */
void cmd_test(char *args);

#endif // APP_COMMANDS_H
