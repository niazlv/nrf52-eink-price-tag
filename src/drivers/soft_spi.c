#include "soft_spi.h"

static const struct device *spi_gpio_dev;

void soft_spi_init(const struct device *gpio_dev) {
    spi_gpio_dev = gpio_dev;
    if (!device_is_ready(spi_gpio_dev)) {
        return;
    }

    gpio_pin_configure(spi_gpio_dev, PIN_CLK, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(spi_gpio_dev, PIN_MOSI, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(spi_gpio_dev, PIN_CS, GPIO_OUTPUT_ACTIVE);

    gpio_pin_set(spi_gpio_dev, PIN_CS, 1);
    gpio_pin_set(spi_gpio_dev, PIN_CLK, 0);
}

// 9-bit transmission: 1 bit (C/D) + 8 bits data
void soft_spi_write_9bit(uint8_t data, uint8_t is_data) {
    if (!spi_gpio_dev) return;

    gpio_pin_set(spi_gpio_dev, PIN_CS, 0);

    // 1. Command/Data Bit (0=Cmd, 1=Data)
    gpio_pin_set(spi_gpio_dev, PIN_MOSI, is_data ? 1 : 0);
    gpio_pin_set(spi_gpio_dev, PIN_CLK, 1);
    gpio_pin_set(spi_gpio_dev, PIN_CLK, 0);

    // 2. 8 Data Bits (MSB first)
    // No k_busy_wait: nRF52 GPIO at 64MHz gives ~150ns per toggle → ~6MHz effective
    // clock, well within SSD1675A's 20MHz SPI max. Saves ~42ms per full frame.
    for (int i = 0; i < 8; i++) {
        gpio_pin_set(spi_gpio_dev, PIN_MOSI, (data & 0x80) ? 1 : 0);
        gpio_pin_set(spi_gpio_dev, PIN_CLK, 1);
        gpio_pin_set(spi_gpio_dev, PIN_CLK, 0);
        data <<= 1;
    }

    gpio_pin_set(spi_gpio_dev, PIN_CS, 1);
}
