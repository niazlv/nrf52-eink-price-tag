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

/* The write path is ~30 pin toggles per byte, so its cost is the cost of one
 * pin toggle. The Zephyr GPIO API is ~0.6 µs per call (85 ms per 4736-byte
 * frame). The nrfx HAL (nrf_gpio_pin_set/clear) looked like a direct store but
 * is not under -Os + LTO: the port-decode switch keeps it out of line, and a
 * call per toggle measured 18.9 µs per byte — 283 ms for a 400x300 plane,
 * 91 ms for 128x296 (SPITEST / TELE:vs s=). Raw OUTSET/OUTCLR stores with
 * compile-time masks are a single instruction each: ~2 µs per byte. NOPs keep
 * each clock half-period ≥ 60 ns; the controllers allow up to 20 MHz. */

#define PIN_MASK(p) (1UL << (p))

/* nRF52832 has one GPIO port; a board that moves the panel to P1 needs the
 * port pointer derived from SSD1675A_GPIO_NODE instead. */
#define SPI_PORT NRF_P0

static inline void clk_pulse(void)
{
    __NOP(); __NOP();
    SPI_PORT->OUTSET = PIN_MASK(SSD1675A_PIN_CLK);
    __NOP(); __NOP(); __NOP();
    SPI_PORT->OUTCLR = PIN_MASK(SSD1675A_PIN_CLK);
}

/* One bit is two stores to the OUT register: {data, CLK low} then {data,
 * CLK high}. Against the SSD1619A/SSD1675 write timings (datasheet table
 * 12-2): SI setup 10 ns and both clock half-periods 25 ns are covered by the
 * ~31-47 ns between consecutive APB stores at 64 MHz; SI hold 40 ns after the
 * rising edge needs the NOP before the next bit's store; CS high ≥ 100 ns
 * between bytes is the two NOPs after CS plus the call overhead. The other
 * pins of the port are carried along from a read of OUT at the start of the
 * byte — nothing but the display thread drives P0 outputs on these boards.
 * Measured: 1.4 µs per 9-bit frame, down from 2.0 with OUTSET/OUTCLR. */
#define SPI_BIT(base, cond)                                               \
    do {                                                                  \
        uint32_t v_ = (base) | ((cond) ? PIN_MASK(SSD1675A_PIN_MOSI) : 0u); \
        SPI_PORT->OUT = v_;                                               \
        SPI_PORT->OUT = v_ | PIN_MASK(SSD1675A_PIN_CLK);                  \
        __NOP();                                                          \
    } while (0)

void ssd1675a_port_write9(uint8_t byte, bool is_data)
{
    if (!configured) {
        return;
    }

    /* CS low, CLK low, MOSI low; every other pin as it is now. */
    const uint32_t base = SPI_PORT->OUT & ~(PIN_MASK(SSD1675A_PIN_CS) |
                                            PIN_MASK(SSD1675A_PIN_CLK) |
                                            PIN_MASK(SSD1675A_PIN_MOSI));
    SPI_PORT->OUT = base;

    SPI_BIT(base, is_data);        /* 1. Command/data bit (0 = command, 1 = data) */
    SPI_BIT(base, byte & 0x80);    /* 2. Eight data bits, MSB first */
    SPI_BIT(base, byte & 0x40);
    SPI_BIT(base, byte & 0x20);
    SPI_BIT(base, byte & 0x10);
    SPI_BIT(base, byte & 0x08);
    SPI_BIT(base, byte & 0x04);
    SPI_BIT(base, byte & 0x02);
    SPI_BIT(base, byte & 0x01);

    SPI_PORT->OUT = base;                                   /* CLK low */
    SPI_PORT->OUT = base | PIN_MASK(SSD1675A_PIN_CS);       /* CS high */
    __NOP(); __NOP();
}

/* Read path for the probe/identification commands. Deliberately slower than
 * the write path: the controller needs time after each falling edge to drive
 * the line (tACC), and a one-off 12 KB probe does not care about a few µs per
 * byte. Roughly 0.5 µs per half-period at 64 MHz. */
static inline void read_pause(void)
{
    for (int i = 0; i < 16; i++) {
        __NOP();
    }
}

void ssd1675a_port_read(uint8_t cmd, uint8_t *buf, int n)
{
    if (!configured) {
        for (int i = 0; i < n; i++) {
            buf[i] = 0xFF;
        }
        return;
    }

    /* CS stays low for the whole transaction (datasheet fig. 6-4); the write
     * path's per-frame CS toggling is not what the read procedure shows. */
    nrf_gpio_pin_clear(SSD1675A_PIN_CS);

    /* Command frame: D/C = 0, then 8 bits, host drives the line. */
    nrf_gpio_pin_clear(SSD1675A_PIN_MOSI);
    clk_pulse();
    for (int i = 0; i < 8; i++) {
        if (cmd & 0x80) {
            nrf_gpio_pin_set(SSD1675A_PIN_MOSI);
        } else {
            nrf_gpio_pin_clear(SSD1675A_PIN_MOSI);
        }
        cmd <<= 1;
        clk_pulse();
    }

    for (int b = 0; b < n; b++) {
        /* D/C = 1 from the host, then hand the line to the controller. The
         * pull-up makes an undriven line read back as 0xFF, which the driver
         * treats as "no read path" rather than as data. */
        nrf_gpio_cfg_output(SSD1675A_PIN_MOSI);
        nrf_gpio_pin_set(SSD1675A_PIN_MOSI);
        clk_pulse();
        nrf_gpio_cfg_input(SSD1675A_PIN_MOSI, NRF_GPIO_PIN_PULLUP);

        /* The controller puts a bit on the line after every falling edge —
         * the D/C pulse above delivered D7 — so sample, then clock. */
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            read_pause();
            v = (uint8_t)((v << 1) | (nrf_gpio_pin_read(SSD1675A_PIN_MOSI) ? 1u : 0u));
            nrf_gpio_pin_set(SSD1675A_PIN_CLK);
            read_pause();
            nrf_gpio_pin_clear(SSD1675A_PIN_CLK);
        }
        buf[b] = v;
    }

    nrf_gpio_cfg_output(SSD1675A_PIN_MOSI);
    nrf_gpio_pin_clear(SSD1675A_PIN_MOSI);
    nrf_gpio_pin_set(SSD1675A_PIN_CS);
}

void ssd1675a_port_reset(bool asserted)
{
    if (!configured) {
        return;
    }
    gpio_pin_set(gpio_dev, SSD1675A_PIN_RST, asserted ? 0 : 1);
}

/*
 * Switching the rail is only half of powering the panel down.
 *
 * The interface pins keep whatever the last transaction left them at: write9()
 * ends with CS high and reset_controller() ends with RST high. Drive either of
 * them into a panel whose VDD is gone and current flows out of the pin, through
 * the panel's input protection diode and into its dead supply net — the panel
 * ends up phantom-powered through the back door, in an undefined state, for as
 * long as the picture stays up. BUSY has the mirror problem: left as an input
 * with no pull, it floats around mid-rail once the panel stops driving it, and
 * the nRF input buffer burns shoot-through current sitting there.
 *
 * On a tag that shows a static image for weeks this runs 24/7, so park every
 * line at or below the panel's own rail before cutting it, and put them back
 * afterwards. Order matters in both directions.
 */
void ssd1675a_port_power(bool on)
{
    if (!configured) {
        return;
    }

    if (on) {
        /* Rail first, and let it actually come up before any line is driven —
         * otherwise CS goes high into a panel whose VDD is still ramping, which
         * is the same diode path this function exists to avoid, just narrower.
         * ssd1675a_power_on() adds its own settling delay after this returns. */
        gpio_pin_set(gpio_dev, SSD1675A_PIN_VCC, 0);   /* active low: rail on */
        k_msleep(2);

        gpio_pin_configure(gpio_dev, SSD1675A_PIN_BUSY, GPIO_INPUT);
        gpio_pin_configure(gpio_dev, SSD1675A_PIN_CLK, GPIO_OUTPUT_INACTIVE);
        gpio_pin_configure(gpio_dev, SSD1675A_PIN_MOSI, GPIO_OUTPUT_INACTIVE);
        gpio_pin_configure(gpio_dev, SSD1675A_PIN_CS, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(gpio_dev, SSD1675A_PIN_CS, 1);    /* idle high */
        gpio_pin_set(gpio_dev, SSD1675A_PIN_CLK, 0);
        /* RST is left to the driver: reset_controller() drives it as part of
         * the wake-up sequence, and ssd1675a_init_partial() always takes that
         * path after a power-off (power_off marks the controller asleep). */
        return;
    }

    /* Lines down first, rail second — the reverse would leave a window where
     * driven pins face a collapsing supply. */
    gpio_pin_set(gpio_dev, SSD1675A_PIN_CS, 0);
    gpio_pin_set(gpio_dev, SSD1675A_PIN_CLK, 0);
    gpio_pin_set(gpio_dev, SSD1675A_PIN_MOSI, 0);
    gpio_pin_set(gpio_dev, SSD1675A_PIN_RST, 0);   /* active low = held in reset */
    gpio_pin_configure(gpio_dev, SSD1675A_PIN_BUSY, GPIO_DISCONNECTED);

    gpio_pin_set(gpio_dev, SSD1675A_PIN_VCC, 1);   /* rail off */
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
