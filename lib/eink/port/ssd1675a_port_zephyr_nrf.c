/*
 * SSD1675A port — Zephyr on nRF5x.
 *
 * Bit-bangs the 9-bit frame on plain GPIOs (the panel needs a D/C bit inside
 * the frame, which the nRF SPIM peripheral cannot produce) and drives RST,
 * BUSY and the panel supply through the Zephyr GPIO API.
 *
 * Pin assignment lives here, not in the driver. Override any of the
 * SSD1675A_PIN_* macros from the build system to match your wiring.
 */

#include "../ssd1675a.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <hal/nrf_gpio.h>

/* ── Wiring ─────────────────────────────────────────────────────────────── */

#ifndef SSD1675A_GPIO_NODE
#define SSD1675A_GPIO_NODE DT_NODELABEL(gpio0)
#endif

#ifndef SSD1675A_PIN_BUSY
#define SSD1675A_PIN_BUSY 6
#endif
#ifndef SSD1675A_PIN_RST
#define SSD1675A_PIN_RST 7
#endif
/** Panel supply switch. Active low: the rail hangs off a P-MOS high-side switch. */
#ifndef SSD1675A_PIN_VCC
#define SSD1675A_PIN_VCC 19
#endif
#ifndef SSD1675A_PIN_CS
#define SSD1675A_PIN_CS 8
#endif
#ifndef SSD1675A_PIN_CLK
#define SSD1675A_PIN_CLK 11
#endif
#ifndef SSD1675A_PIN_MOSI
#define SSD1675A_PIN_MOSI 12
#endif

static const struct device *gpio_dev;
static bool configured;

bool ssd1675a_port_init(void)
{
    if (configured) {
        return true;
    }

    gpio_dev = DEVICE_DT_GET(SSD1675A_GPIO_NODE);
    if (!device_is_ready(gpio_dev)) {
        return false;
    }

    gpio_pin_configure(gpio_dev, SSD1675A_PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(gpio_dev, SSD1675A_PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(gpio_dev, SSD1675A_PIN_BUSY, GPIO_INPUT);

    gpio_pin_configure(gpio_dev, SSD1675A_PIN_CLK, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio_dev, SSD1675A_PIN_MOSI, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio_dev, SSD1675A_PIN_CS, GPIO_OUTPUT_ACTIVE);

    gpio_pin_set(gpio_dev, SSD1675A_PIN_CS, 1);
    gpio_pin_set(gpio_dev, SSD1675A_PIN_CLK, 0);

    configured = true;
    return true;
}

/* Direct register writes via nrf_gpio HAL, NOT gpio_pin_set(): the Zephyr GPIO
 * API costs ~0.6µs per call through the driver abstraction, which at ~28 calls
 * per byte made a 4736-byte frame take ~85ms (measured via vstream telemetry:
 * disp 205ms = 120ms LUT wave + ~85ms SPI). Raw OUTSET/OUTCLR stores at 64MHz
 * bring it to ~1.5µs/byte — confirmed: disp dropped to wave + ~7ms. NOPs keep
 * each clock half-period ≥60ns; SSD1675A allows up to 20MHz, margin is ample. */

static inline void clk_pulse(void)
{
    __NOP(); __NOP();
    nrf_gpio_pin_set(SSD1675A_PIN_CLK);
    __NOP(); __NOP(); __NOP();
    nrf_gpio_pin_clear(SSD1675A_PIN_CLK);
}

void ssd1675a_port_write9(uint8_t byte, bool is_data)
{
    if (!configured) {
        return;
    }

    nrf_gpio_pin_clear(SSD1675A_PIN_CS);

    /* 1. Command/data bit (0 = command, 1 = data) */
    if (is_data) {
        nrf_gpio_pin_set(SSD1675A_PIN_MOSI);
    } else {
        nrf_gpio_pin_clear(SSD1675A_PIN_MOSI);
    }
    clk_pulse();

    /* 2. Eight data bits, MSB first */
    for (int i = 0; i < 8; i++) {
        if (byte & 0x80) {
            nrf_gpio_pin_set(SSD1675A_PIN_MOSI);
        } else {
            nrf_gpio_pin_clear(SSD1675A_PIN_MOSI);
        }
        byte <<= 1;
        clk_pulse();
    }

    nrf_gpio_pin_set(SSD1675A_PIN_CS);
}

void ssd1675a_port_reset(bool asserted)
{
    if (!configured) {
        return;
    }
    gpio_pin_set(gpio_dev, SSD1675A_PIN_RST, asserted ? 0 : 1);
}

void ssd1675a_port_power(bool on)
{
    if (!configured) {
        return;
    }
    /* Active low: the rail hangs off a high-side P-MOS. */
    gpio_pin_set(gpio_dev, SSD1675A_PIN_VCC, on ? 0 : 1);
}

bool ssd1675a_port_busy(void)
{
    if (!configured) {
        return false;
    }
    return gpio_pin_get(gpio_dev, SSD1675A_PIN_BUSY) == 1;
}

void ssd1675a_port_delay_ms(uint32_t ms)
{
    k_msleep(ms);
}

/* Seed the waveform table before main(), the display thread and the first BLE
 * command, so LUT-editing commands that never touch the panel still see the
 * real default table rather than eink_lut's unbound fallback. The driver seeds
 * lazily too; this only pulls it earlier on platforms that have init hooks. */
static int ssd1675a_lut_preinit(void)
{
    ssd1675a_lut_init();
    return 0;
}
SYS_INIT(ssd1675a_lut_preinit, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
