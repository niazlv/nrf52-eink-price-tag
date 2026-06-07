#include "ssd1675a.h"
#include <string.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ssd1675a, LOG_LEVEL_INF);

#include <zephyr/drivers/gpio.h>

// Set to 1 to use the ported Python LUT. Set to 0 to use Display Internal OTP LUT (often better).
#define USE_CUSTOM_LUT 1

static const struct device *eink_gpio_dev;
static uint8_t vcom_register_value = 0x68;

// LUT Data from Python driver port
static uint8_t lut_data[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18, 0x04, 0x16, 0x01, 0x0A, 0x0A,
    0x0A, 0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x08 /* ЯРКОСТЬ */, 0x3C /* Длительность */, 0x07 /* Повтор */, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Red phase knobs for the cover photo. Defaults were 0x08, 0x3C, 0x07.
#define COVER_RED_LEVEL   0x0F
#define COVER_RED_TIME    0xB0
#define COVER_RED_REPEAT  0x14
#define COVER_RED_CUTOFF_MS 6400

// Cover LUT: keep the known-good waveform shape, tune only the red phase.
static uint8_t lut_vivid_cover[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18, 0x04, 0x16, 0x01, 0x0A, 0x0A,
    0x0A, 0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x04, COVER_RED_LEVEL, COVER_RED_TIME, COVER_RED_REPEAT, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

void ssd1675a_set_lut_byte(int index, uint8_t val) {
    if (index >= 0 && index < sizeof(lut_data)) {
        lut_data[index] = val;
    }
}

static void send_cmd(uint8_t cmd) {
    soft_spi_write_9bit(cmd, SPI_CMD);
}

static void send_data(uint8_t data) {
    soft_spi_write_9bit(data, SPI_DATA);
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
    send_cmd(0x32);
    for (int i = 0; i < sizeof(lut_data); i++) {
        send_data(lut_data[i]);
    }
}

static void write_lut_table(const uint8_t *lut, int size) {
    send_cmd(0x32);
    for (int i = 0; i < size; i++) {
        send_data(lut[i]);
    }
}

static void set_ram_pointer(int x, int y) {
    send_cmd(0x4E); 
    send_data(x & 0xFF);
    send_cmd(0x4F); 
    send_data(y & 0xFF);
    send_data((y >> 8) & 0xFF);
}

static void configure_registers(void) {
    send_cmd(0x74); // Analog Block
    send_data(0x54);

    send_cmd(0x7E); // Digital Block
    send_data(0x3B);

    send_cmd(0x01); // Driver Output
    send_data(0x27);
    send_data(0x01);
    send_data(0x00);

    send_cmd(0x3A); // 130Hz
    send_data(0x35);

    send_cmd(0x3B); // 130Hz
    send_data(0x04);

    send_cmd(0x3C); // Border
    send_data(0x33);

    send_cmd(0x11); // Data Entry
    send_data(0x03); // X+, Y+

    send_cmd(0x44); // RAM X Address
    send_data(0x00);
    send_data(0x0F); // 128/8 - 1 = 15

    send_cmd(0x45); // RAM Y Address
    send_data(0x00);
    send_data(0x00);
    send_data(0x27);
    send_data(0x01);

    send_cmd(0x04); // Voltages
    send_data(0x41);
    send_data(0xA8);
    send_data(0x32);

    send_cmd(0x2C); // VCOM
    send_data(vcom_register_value);

#if USE_CUSTOM_LUT
    write_lut();
#endif
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
    
    // Hardware Reset
    gpio_pin_set(eink_gpio_dev, PIN_RST, 0);
    k_msleep(10); // Reduced to 10ms for speed (PLL boost removed, so safe now)
    gpio_pin_set(eink_gpio_dev, PIN_RST, 1);
    k_msleep(10); // Reduced to 10ms for speed

    // Software Reset - REMOVED for speed (HW reset is sufficient)
    // send_cmd(0x12);
    // ssd1675a_wait_busy();

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
    
    // SKIP Hardware Reset to preserve RAM
    // Just re-configure registers which might be lost in Sleep
    configure_registers();
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
}

void ssd1675a_sleep(void) {
    send_cmd(0x10); // Deep Sleep
    send_data(0x01);
}

void ssd1675a_update_display(void) {
    ssd1675a_load_default_lut(); // Ensure standard LUT is active
    send_cmd(0x22); 
    send_data(0xC7); // Update Control (from Python)
    send_cmd(0x20); 
    ssd1675a_wait_busy();
}

void ssd1675a_update_display_vivid(void) {
    write_lut_table(lut_vivid_cover, sizeof(lut_vivid_cover));
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_update_display_red_cutoff(void) {
    write_lut_table(lut_vivid_cover, sizeof(lut_vivid_cover));
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);

    k_msleep(COVER_RED_CUTOFF_MS);

    // Cover-photo hack: stop before the last red waves wash burgundy into pale red.
    gpio_pin_set(eink_gpio_dev, PIN_RST, 0);
    k_msleep(20);
    ssd1675a_power_off();
}


// Preserved as "Stable/Reddish" (Works but ~1.1s)
static uint8_t lut_stable[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    // Timings
    0x02, 0x06, 0x02, 0x06, 0x01, // Phase 0
    0x02, 0x06, 0x02, 0x06, 0x01, // Phase 1
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 2
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 3
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 4 (Red) - CLEARED
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 5
    0x00, 0x00, 0x00, 0x00, 0x00  // Phase 6
};

// Balanced LUT (Target ~800ms, No Artifacts)
// Slower than Turbo (650ms) but faster than Stable (1.1s)
static uint8_t lut_balanced[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    // Timings
    0x02, 0x04, 0x02, 0x04, 0x01, // Phase 0 (Reduced from 0x06 to 0x04)
    0x02, 0x04, 0x02, 0x04, 0x01, // Phase 1
    0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

// Turbo LUT (Hyper Fast ~600ms)
static uint8_t lut_turbo[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    // Timings (Hyper Aggressive)
    0x01, 0x01, 0x01, 0x01, 0x01, // Phase 0 (All 1s - Fastest possible)
    0x01, 0x01, 0x01, 0x01, 0x01, // Phase 1
    0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t lut_fast[70]; 

void ssd1675a_init_fast_lut(void) {
    memcpy(lut_fast, lut_data, sizeof(lut_fast));
    lut_fast[57] = 0x00;
    lut_fast[58] = 0x00;
    lut_fast[59] = 0x00;
}

static ssd1675a_partial_mode_t current_partial_mode = SSD1675A_PARTIAL_MODE_BALANCED;

void ssd1675a_set_partial_mode(ssd1675a_partial_mode_t mode) {
    current_partial_mode = mode;
}

void ssd1675a_update_partial(void) {
    uint8_t *lut_ptr = lut_balanced; // Default

    switch (current_partial_mode) {
        case SSD1675A_PARTIAL_MODE_TURBO: 
            lut_ptr = lut_turbo; 
            // LOG_INF("Partial: Turbo");
            break;
        case SSD1675A_PARTIAL_MODE_BALANCED: 
            lut_ptr = lut_balanced; 
            // LOG_INF("Partial: Balanced");
            break;
        case SSD1675A_PARTIAL_MODE_STABLE: 
            lut_ptr = lut_stable; 
            // LOG_INF("Partial: Stable");
            break;
    }

    // Ensure PLL is maxed out for speed (0x3C = 50Hz/Higher)
    // REMOVED: Causing Instability/Hangs
    // send_cmd(0x30);
    // send_data(0x3C);

    send_cmd(0x32);
    for (int i = 0; i < 70; i++) { // All LUTs are 70 bytes
        send_data(lut_ptr[i]);
    }

    send_cmd(0x22); 
    send_data(0xC7); // Update Control
    send_cmd(0x20); 
    ssd1675a_wait_busy();
    
    // Restore Default LUT is REDUNDANT (update_display does it). Removed for speed.
}

// Helper to reload standard LUT (if needed by main app)
void ssd1675a_load_default_lut(void) {
    send_cmd(0x32);
    for (int i = 0; i < sizeof(lut_data); i++) {
        send_data(lut_data[i]);
    }
}

void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer) {
    // 1. Write BW Buffer (0x24)
    set_ram_pointer(0, 0);
    send_cmd(0x24); 
    for (int i = 0; i < 4736; i++) {
        send_data(bw_buffer[i]); 
    }

    // 2. Write Red Buffer (0x26)
    if (red_buffer) {
        set_ram_pointer(0, 0);
        send_cmd(0x26); 
        for (int i = 0; i < 4736; i++) {
            send_data(red_buffer[i]); 
        }
    } else {
        // Clear Red if null (Safety) - User can pass all-zero array if needed
        set_ram_pointer(0, 0);
        send_cmd(0x26);
        for (int i = 0; i < 4736; i++) {
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
    for (int i = 0; i < 4736; i++) {
        send_data(bw_buffer[i]); 
    }
}
