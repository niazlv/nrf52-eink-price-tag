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
void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer);
void ssd1675a_update_display(void);
void ssd1675a_sleep(void);
void ssd1675a_set_vcom_register(uint8_t val);
void ssd1675a_set_lut_byte(int index, uint8_t val);
void ssd1675a_wait_busy(void);

// Partial Update
typedef enum {
    SSD1675A_PARTIAL_MODE_TURBO,
    SSD1675A_PARTIAL_MODE_BALANCED,
    SSD1675A_PARTIAL_MODE_STABLE
} ssd1675a_partial_mode_t;

void ssd1675a_set_partial_mode(ssd1675a_partial_mode_t mode);
void ssd1675a_update_partial(void);
void ssd1675a_load_default_lut(void);

#endif // SSD1675A_H
