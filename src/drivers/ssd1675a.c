#include "ssd1675a.h"
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ssd1675a, LOG_LEVEL_INF);

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

// Set to 1 to use the ported Python LUT. Set to 0 to use Display Internal OTP LUT (often better).
#define USE_CUSTOM_LUT 1

/* RAM plane size in bytes: 128 × 296 / 8 = 4736. */
#define SSD1675A_RAM_BYTES ((SSD1675A_WIDTH * SSD1675A_HEIGHT) / 8)

static const struct device *eink_gpio_dev;
static uint8_t vcom_register_value = 0x68;
static bool controller_sleeping = true;

/* Snapshot of the tuned default — used by ssd1675a_reset_lut() and as the
 * factory baseline. The mutable working table below is seeded from this at init.
 *
 * v5-balanced: bright BWR + DC-compensated Ph2 for all groups.
 * Measured: ~8707ms full update. DC balance: BLK=0, WHT=0, RED=-3.3
 * Ph2 added as ratchet extension + charge compensation (LUT2/3: 0xA8=VSL×3).
 * Ph5+Ph6: 0xFF fixation for red reveal. Ph4: RP=7 main red drive. */
static const uint8_t lut_data_default[] = {
    /* VS section — bytes 0-34 (LUT0..4 × 7 phases) */
    /* LUT0 BLACK */ 0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    /* LUT1 WHITE */ 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    /* LUT2 RED   */ 0x6A, 0x9B, 0xA8, 0x9B, 0x9B, 0xFF, 0xFF,
    /* LUT3 RED   */ 0x6A, 0x9B, 0xA8, 0x9B, 0x9B, 0xFF, 0xFF,
    /* LUT4 VCOM  */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Timing — bytes 35-69 (7 phases × TA TB TC TD RP) */
    /* Ph0 */ 0x00, 0x14, 0x00, 0x12, 0x01,
    /* Ph1 */ 0x06, 0x06, 0x06, 0x06, 0x02,
    /* Ph2 */ 0x14, 0x14, 0x14, 0x14, 0x01,
    /* Ph3 */ 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Ph4 */ 0x00, 0x00, 0x04, 0x3B, 0x07,
    /* Ph5 */ 0x14, 0x14, 0x14, 0x3B, 0x00,
    /* Ph6 */ 0x14, 0x14, 0x14, 0x3B, 0x00,
};

/* Runtime-editable working table (the "custom" LUT). Seeded from
 * lut_data_default at init; patched via ssd1675a_set_lut_byte() (LUTW/LW). */
static uint8_t lut_data[SSD1675A_LUT_SIZE];

/* Seed the working table from the default and hand both base tables to the
 * portable LUT module. Runs once at POST_KERNEL — before main(), the
 * screensaver thread, and any BLE command — so every LUT entry point can
 * assume the working table is ready without a per-call guard. */
static int eink_lut_seed(void) {
    memcpy(lut_data, lut_data_default, sizeof(lut_data));
    eink_lut_bind_base_tables(lut_data, lut_data_default);
    return 0;
}
SYS_INIT(eink_lut_seed, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

void ssd1675a_set_lut_byte(int index, uint8_t val) {
    if (index >= 0 && index < (int)sizeof(lut_data)) {
        lut_data[index] = val;
    }
}

uint8_t ssd1675a_get_lut_byte(int index) {
    if (index >= 0 && index < (int)sizeof(lut_data)) {
        return lut_data[index];
    }
    return 0;
}

/* LUT policy lives in lib/eink_lut.c. The trivial pass-through accessors
 * (use_custom, partial_mode, vlut_*) are zero-cost static inline shims in
 * ssd1675a.h. The byte editor above stays native since it owns the working
 * table. */

void ssd1675a_reset_lut(void) {
    memcpy(lut_data, lut_data_default, sizeof(lut_data));
    eink_lut_set_use_custom(false);
}

static void send_cmd(uint8_t cmd) {
    soft_spi_write_9bit(cmd, SPI_CMD);
}

static void send_data(uint8_t data) {
    soft_spi_write_9bit(data, SPI_DATA);
}

/* Upload a full EINK_LUT_SIZE-byte waveform table via command 0x32. */
static void upload_lut(const uint8_t *lut) {
    send_cmd(0x32);
    for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
        send_data(lut[i]);
    }
}

void ssd1675a_wait_busy(void) {
    if (!eink_gpio_dev) return;
    int timeout = 4000;
    // Wait while BUSY is High
    while (gpio_pin_get(eink_gpio_dev, PIN_BUSY) == 1 && timeout > 0) {
        k_msleep(2); // Polling faster (was 10ms) to catch completion earlier
        timeout--;
    }
}

static void write_lut(void) {
    /* The working table is seeded + bound at POST_KERNEL (eink_lut_seed). */
    upload_lut(lut_data);
}

static void set_ram_pointer(int x, int y) {
    send_cmd(0x4E);
    send_data(x & 0xFF);
    send_cmd(0x4F);
    send_data(y & 0xFF);
    send_data((y >> 8) & 0xFF);
}

static void configure_registers(void) {
    send_cmd(0x74); // Set analog block control
    send_data(0x54);

    send_cmd(0x7E); // Set digital block control
    send_data(0x3B);

    send_cmd(0x01); // Driver output: 296 gate lines (0x0127 + 1)
    send_data(0x27);
    send_data(0x01);
    send_data(0x00);

    send_cmd(0x3A); // Dummy line period, part of panel scan timing
    send_data(0x35);

    send_cmd(0x3B); // Gate line width, part of panel scan timing
    send_data(0x04);

    send_cmd(0x3C); // Border waveform control
    send_data(0x33);

    send_cmd(0x11); // Data entry mode
    send_data(0x03); // Auto-increment X and Y after RAM writes

    send_cmd(0x44); // RAM X range in bytes: 0..15 = 128 pixels
    send_data(0x00); // X start
    send_data(0x0F); // X end: 128 / 8 - 1

    send_cmd(0x45); // RAM Y range: 0..295
    send_data(0x00); // Y start low byte
    send_data(0x00); // Y start high byte
    send_data(0x27); // Y end low byte: 0x0127 = 295
    send_data(0x01); // Y end high byte

    send_cmd(0x04); // Source driving voltage settings
    send_data(0x41);
    send_data(0xA8);
    send_data(0x32);

    send_cmd(0x2C); // VCOM voltage, affects contrast and ghosting
    send_data(vcom_register_value);

#if USE_CUSTOM_LUT
    write_lut(); // Waveform table for pixel transitions
#endif
}

static void reset_controller(void)
{
    gpio_pin_set(eink_gpio_dev, PIN_RST, 0);
    k_msleep(10);
    gpio_pin_set(eink_gpio_dev, PIN_RST, 1);
    ssd1675a_wait_busy();

    send_cmd(0x12);
    ssd1675a_wait_busy();
    controller_sleeping = false;
}

void ssd1675a_init(const struct device *gpio_dev) {
    eink_gpio_dev = gpio_dev;
    if (!device_is_ready(eink_gpio_dev)) return;

    // Configure Control Pins
    gpio_pin_configure(eink_gpio_dev, PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_BUSY, GPIO_INPUT);

    // SPI Init (Pins)
    soft_spi_init(gpio_dev);

    // Initial Power On Sequence
    ssd1675a_power_on();

    reset_controller();

    // Initialization Sequence
    configure_registers();
}

void ssd1675a_init_partial(const struct device *gpio_dev) {
    eink_gpio_dev = gpio_dev;
    if (!device_is_ready(eink_gpio_dev)) return;

    // Configure Control Pins
    gpio_pin_configure(eink_gpio_dev, PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_BUSY, GPIO_INPUT);

    // SPI Init (Pins)
    soft_spi_init(gpio_dev);

    // Power On Sequence (Safe to call if already on)
    ssd1675a_power_on();

    // Recovery: if BUSY is already HIGH the controller is stuck (e.g. mid-operation
    // reflash, or HV rails never discharged).  Hard-reset to unblock it.
    // RAM will be lost, but a corrupted FB is better than an infinite busy-wait.
    if (controller_sleeping || gpio_pin_get(eink_gpio_dev, PIN_BUSY) == 1) {
        reset_controller();
    }

    // Re-configure registers (may be lost after Sleep or reset above)
    configure_registers();
    controller_sleeping = false;
}

void ssd1675a_set_vcom_register(uint8_t val) {
    vcom_register_value = val;
}

void ssd1675a_power_on(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 0); // ON (Active Low P-MOS assumed)
    k_msleep(10); // Reduced from 50ms for speed
}

void ssd1675a_power_off(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 1); // OFF
    controller_sleeping = true;
}

void ssd1675a_sleep(void) {
    send_cmd(0x10); // Deep Sleep
    send_data(0x01);
    controller_sleeping = true;
}

void ssd1675a_update_display(void) {
    ssd1675a_load_default_lut(); // Ensure standard LUT is active
    send_cmd(0x22);
    send_data(0xC7); // Update Control (from Python)
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_update_partial(void) {
    upload_lut(eink_lut_select_partial());
    // 0xC7: Enable CLK + Analog → Display Mode 1 → Disable Analog + CLK
    // The analog enable/disable charges ±15V rails — ~700ms overhead per call.
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

// ── Streaming mode: enable HV once, update many frames, disable HV once ───
// Usage: begin_streaming() → [update_frame_stream() × N] → end_streaming()
// Reduces per-frame time from ~700ms (HV recharge in every 0xC7) to the LUT
// wave time alone: subframes × ~15ms, e.g. ~210ms for the 14-subframe TURBO.

void ssd1675a_begin_streaming(void) {
    upload_lut(eink_lut_select_partial());
    // 0xC0: Enable CLK + Enable Analog only (no display yet).
    // Charges ±15V rails — one-time cost ~600ms.
    send_cmd(0x22);
    send_data(0xC0);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_update_frame_stream(void) {
    // 0x04: Display Mode 1 only — HV rails already on, no power cycling.
    // BUSY duration = LUT wave time only (subframes × ~15ms; TURBO ≈ 210ms).
    send_cmd(0x22);
    send_data(0x04);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_trigger_frame_stream_nowait(void) {
    /* Same as update_frame_stream but does NOT wait for BUSY.
     * Caller must call ssd1675a_wait_busy() before the next SPI write. */
    send_cmd(0x22);
    send_data(0x04);
    send_cmd(0x20);
}

void ssd1675a_end_streaming(void) {
    // 0x03: Disable Analog + CLK — powers down ±15V rails.
    send_cmd(0x22);
    send_data(0x03);
    send_cmd(0x20);
}

// Helper to reload standard LUT (if needed by main app)
void ssd1675a_load_default_lut(void) {
    upload_lut(lut_data);
}

void ssd1675a_update_display_flush_red(void) {
    upload_lut(eink_lut_flush_red());
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
    ssd1675a_load_default_lut();
}

void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer) {
    // 1. Write BW Buffer (0x24)
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(bw_buffer[i]);
    }

    // 2. Write Red Buffer (0x26)
    if (red_buffer) {
        set_ram_pointer(0, 0);
        send_cmd(0x26);
        for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
            send_data(red_buffer[i]);
        }
    } else {
        // Clear Red if null (Safety) - User can pass all-zero array if needed
        set_ram_pointer(0, 0);
        send_cmd(0x26);
        for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
            send_data(0x00);
        }
    }
}

void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer) {
     // Only Write BW Buffer (0x24)
     // Skipping Red buffer write saves ~50% of data transfer time.
     // Partial LUT disables Red phase, so Red RAM content should be ignored.
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(bw_buffer[i]);
    }
}

void ssd1675a_display_buffers_fast(const uint8_t *bw_buffer, const uint8_t *red_buffer) {
    // Write both RAM planes without a full display update. Tone-servo video
    // repurposes the red plane as a second control bit:
    //   00 = darken, 01 = lighten, 10/11 = hold/no-op via LUT2/LUT3.
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(bw_buffer[i]);
    }

    set_ram_pointer(0, 0);
    send_cmd(0x26);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(red_buffer ? red_buffer[i] : 0x00);
    }
}

void ssd1675a_clear_red_ram(void) {
    set_ram_pointer(0, 0);
    send_cmd(0x26);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(0x00);
    }
}
