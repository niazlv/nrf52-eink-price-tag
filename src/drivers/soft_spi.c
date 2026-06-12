#include "soft_spi.h"
#include <hal/nrf_gpio.h>

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

/* Direct register writes via nrf_gpio HAL, NOT gpio_pin_set(): the Zephyr GPIO
 * API costs ~0.6µs per call through the driver abstraction, which at ~28 calls
 * per byte made a 4736-byte frame take ~85ms (measured via vstream telemetry:
 * disp 205ms = 120ms LUT wave + ~85ms SPI). Raw OUTSET/OUTCLR stores at 64MHz
 * bring it to ~1.5µs/byte — confirmed: disp dropped to wave + ~7ms. NOPs keep
 * each clock half-period ≥60ns; SSD1675A allows up to 20MHz, margin is ample. */

static inline void clk_pulse(void) {
    __NOP(); __NOP();
    nrf_gpio_pin_set(PIN_CLK);
    __NOP(); __NOP(); __NOP();
    nrf_gpio_pin_clear(PIN_CLK);
}

// 9-bit transmission: 1 bit (C/D) + 8 bits data
void soft_spi_write_9bit(uint8_t data, uint8_t is_data) {
    if (!spi_gpio_dev) return;

    nrf_gpio_pin_clear(PIN_CS);

    // 1. Command/Data Bit (0=Cmd, 1=Data)
    if (is_data) nrf_gpio_pin_set(PIN_MOSI);
    else         nrf_gpio_pin_clear(PIN_MOSI);
    clk_pulse();

    // 2. 8 Data Bits (MSB first)
    for (int i = 0; i < 8; i++) {
        if (data & 0x80) nrf_gpio_pin_set(PIN_MOSI);
        else             nrf_gpio_pin_clear(PIN_MOSI);
        data <<= 1;
        clk_pulse();
    }

    nrf_gpio_pin_set(PIN_CS);
}
