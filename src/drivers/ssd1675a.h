#ifndef SSD1675A_H
#define SSD1675A_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "soft_spi.h"

// Pin Definitions
#define PIN_BUSY 6
#define PIN_RST  7
#define PIN_VCC  19

// Display Dimensions
#define SSD1675A_WIDTH  128
#define SSD1675A_HEIGHT 296

// Functions
void ssd1675a_init(const struct device *gpio_dev);
void ssd1675a_power_on(void);
void ssd1675a_power_off(void);
void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer);
void ssd1675a_update_display(void);
void ssd1675a_sleep(void);
void ssd1675a_wait_busy(void);

#endif // SSD1675A_H
