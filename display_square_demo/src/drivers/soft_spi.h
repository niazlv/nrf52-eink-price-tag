#ifndef SOFT_SPI_H
#define SOFT_SPI_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

// Pin Definitions (moved from main.c)
#define PIN_CS   8
#define PIN_CLK  11
#define PIN_MOSI 12

// Constants for 9-bit SPI (Data/Command)
#define SPI_CMD  0
#define SPI_DATA 1

// Functions
void soft_spi_init(const struct device *gpio_dev);
void soft_spi_write_9bit(uint8_t data, uint8_t is_data);

#endif // SOFT_SPI_H
