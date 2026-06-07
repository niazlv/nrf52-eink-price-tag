#include "ssd1675a.h"
#include <string.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ssd1675a, LOG_LEVEL_INF);

#include <zephyr/drivers/gpio.h>

#define USE_CUSTOM_LUT 1

static const struct device *eink_gpio_dev;
static uint8_t vcom_register_value = 0x68;

static uint8_t lut_data[SSD1675A_LUT_SIZE] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18, 0x04, 0x16, 0x01, 0x0A, 0x0A,
    0x0A, 0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x08, 0x3C, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Never mutated — used for RESET and CLEAR */
static const uint8_t lut_factory[SSD1675A_LUT_SIZE] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18, 0x04, 0x16, 0x01, 0x0A, 0x0A,
    0x0A, 0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x08, 0x3C, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void ssd1675a_set_lut_byte(int index, uint8_t val) {
    if (index >= 0 && index < SSD1675A_LUT_SIZE) {
        lut_data[index] = val;
    }
}

uint8_t ssd1675a_get_lut_byte(int index) {
    if (index >= 0 && index < SSD1675A_LUT_SIZE) {
        return lut_data[index];
    }
    return 0;
}

void ssd1675a_reset_lut(void) {
    memcpy(lut_data, lut_factory, SSD1675A_LUT_SIZE);
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
    while (gpio_pin_get(eink_gpio_dev, PIN_BUSY) == 1 && timeout > 0) {
        k_msleep(2);
        timeout--;
    }
}

static void write_lut_array(const uint8_t *lut, size_t len) {
    send_cmd(0x32);
    for (size_t i = 0; i < len; i++) {
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
    send_cmd(0x74);
    send_data(0x54);

    send_cmd(0x7E);
    send_data(0x3B);

    send_cmd(0x01);
    send_data(0x27);
    send_data(0x01);
    send_data(0x00);

    send_cmd(0x3A);
    send_data(0x35);

    send_cmd(0x3B);
    send_data(0x04);

    send_cmd(0x3C);
    send_data(0x33);

    send_cmd(0x11);
    send_data(0x03);

    send_cmd(0x44);
    send_data(0x00);
    send_data(0x0F);

    send_cmd(0x45);
    send_data(0x00);
    send_data(0x00);
    send_data(0x27);
    send_data(0x01);

    send_cmd(0x04);
    send_data(0x41);
    send_data(0xA8);
    send_data(0x32);

    send_cmd(0x2C);
    send_data(vcom_register_value);

#if USE_CUSTOM_LUT
    write_lut_array(lut_data, SSD1675A_LUT_SIZE);
#endif
}

void ssd1675a_init(const struct device *gpio_dev) {
    eink_gpio_dev = gpio_dev;
    if (!device_is_ready(eink_gpio_dev)) return;

    gpio_pin_configure(eink_gpio_dev, PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_BUSY, GPIO_INPUT);

    soft_spi_init(gpio_dev);

    ssd1675a_power_on();

    gpio_pin_set(eink_gpio_dev, PIN_RST, 0);
    k_msleep(10);
    gpio_pin_set(eink_gpio_dev, PIN_RST, 1);
    k_msleep(10);

    configure_registers();
}

void ssd1675a_init_partial(const struct device *gpio_dev) {
    eink_gpio_dev = gpio_dev;
    if (!device_is_ready(eink_gpio_dev)) return;

    gpio_pin_configure(eink_gpio_dev, PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_BUSY, GPIO_INPUT);

    soft_spi_init(gpio_dev);

    ssd1675a_power_on();
    configure_registers();
}

void ssd1675a_set_vcom_register(uint8_t val) {
    vcom_register_value = val;
}

void ssd1675a_power_on(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 0);
    k_msleep(10);
}

void ssd1675a_power_off(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 1);
}

void ssd1675a_sleep(void) {
    send_cmd(0x10);
    send_data(0x01);
}

void ssd1675a_load_default_lut(void) {
    write_lut_array(lut_data, SSD1675A_LUT_SIZE);
}

void ssd1675a_update_display(void) {
    ssd1675a_load_default_lut();
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

static uint8_t lut_balanced[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x04, 0x02, 0x04, 0x01,
    0x02, 0x04, 0x02, 0x04, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t lut_turbo[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t lut_stable[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x06, 0x02, 0x06, 0x01,
    0x02, 0x06, 0x02, 0x06, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
};

static ssd1675a_partial_mode_t current_partial_mode = SSD1675A_PARTIAL_MODE_BALANCED;

void ssd1675a_set_partial_mode(ssd1675a_partial_mode_t mode) {
    current_partial_mode = mode;
}

void ssd1675a_update_partial(void) {
    uint8_t *lut_ptr = lut_balanced;

    switch (current_partial_mode) {
        case SSD1675A_PARTIAL_MODE_TURBO:   lut_ptr = lut_turbo;   break;
        case SSD1675A_PARTIAL_MODE_STABLE:  lut_ptr = lut_stable;  break;
        default:                            lut_ptr = lut_balanced; break;
    }

    write_lut_array(lut_ptr, SSD1675A_LUT_SIZE);
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer) {
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < 4736; i++) {
        send_data(bw_buffer[i]);
    }

    set_ram_pointer(0, 0);
    send_cmd(0x26);
    for (int i = 0; i < 4736; i++) {
        send_data(red_buffer ? red_buffer[i] : 0x00);
    }
}

void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer) {
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < 4736; i++) {
        send_data(bw_buffer[i]);
    }
}

/* Write all-white with factory LUT — clean baseline before testing custom LUT */
void ssd1675a_clear_with_factory_lut(const struct device *gpio_dev) {
    eink_gpio_dev = gpio_dev;
    if (!device_is_ready(eink_gpio_dev)) return;

    gpio_pin_configure(eink_gpio_dev, PIN_VCC, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_RST, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(eink_gpio_dev, PIN_BUSY, GPIO_INPUT);
    soft_spi_init(gpio_dev);
    ssd1675a_power_on();

    gpio_pin_set(eink_gpio_dev, PIN_RST, 0);
    k_msleep(10);
    gpio_pin_set(eink_gpio_dev, PIN_RST, 1);
    k_msleep(10);

    /* Configure with factory LUT */
    send_cmd(0x74); send_data(0x54);
    send_cmd(0x7E); send_data(0x3B);
    send_cmd(0x01); send_data(0x27); send_data(0x01); send_data(0x00);
    send_cmd(0x3A); send_data(0x35);
    send_cmd(0x3B); send_data(0x04);
    send_cmd(0x3C); send_data(0x33);
    send_cmd(0x11); send_data(0x03);
    send_cmd(0x44); send_data(0x00); send_data(0x0F);
    send_cmd(0x45); send_data(0x00); send_data(0x00); send_data(0x27); send_data(0x01);
    send_cmd(0x04); send_data(0x41); send_data(0xA8); send_data(0x32);
    send_cmd(0x2C); send_data(vcom_register_value);
    write_lut_array(lut_factory, SSD1675A_LUT_SIZE);

    /* All-white BW RAM */
    set_ram_pointer(0, 0);
    send_cmd(0x24);
    for (int i = 0; i < 4736; i++) send_data(0xFF);

    /* All-zero Red RAM */
    set_ram_pointer(0, 0);
    send_cmd(0x26);
    for (int i = 0; i < 4736; i++) send_data(0x00);

    /* Trigger full update */
    write_lut_array(lut_factory, SSD1675A_LUT_SIZE);
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}
