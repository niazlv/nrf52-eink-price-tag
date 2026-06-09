#ifndef APP_BATTERY_H
#define APP_BATTERY_H

#include <stdint.h>

/**
 * @brief Initialize the battery measurement module (ADC)
 * 
 * @return 0 on success, negative error code otherwise
 */
int battery_init(void);

/**
 * @brief Read battery voltage in millivolts
 * 
 * @return Voltage in mV, or 0/negative on error
 */
int battery_read_mv(void);

#endif // APP_BATTERY_H
