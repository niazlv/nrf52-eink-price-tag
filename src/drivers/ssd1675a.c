#include "ssd1675a.h"

static const struct device *eink_gpio_dev;

// LUT Data from Python driver port
static const uint8_t lut_data[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00, 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00, 0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18, 0x04, 0x16, 0x01, 0x0A, 0x0A,
    0x0A, 0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x08, 0x3C, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

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
        k_msleep(10);
        timeout--;
    }
}

static void write_lut(void) {
    send_cmd(0x32);
    for (int i = 0; i < sizeof(lut_data); i++) {
        send_data(lut_data[i]);
    }
}

static void set_ram_pointer(int x, int y) {
    send_cmd(0x4E); 
    send_data(x & 0xFF);
    send_cmd(0x4F); 
    send_data(y & 0xFF);
    send_data((y >> 8) & 0xFF);
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
    k_msleep(200);
    gpio_pin_set(eink_gpio_dev, PIN_RST, 1);
    k_msleep(200);

    // Software Reset
    send_cmd(0x12);
    ssd1675a_wait_busy();

    // Initialization Sequence
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
    send_data(0x68);

    write_lut();
}

void ssd1675a_power_on(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 0); // ON (Active Low P-MOS assumed)
    k_msleep(50);
}

void ssd1675a_power_off(void) {
    gpio_pin_set(eink_gpio_dev, PIN_VCC, 1); // OFF
}

void ssd1675a_sleep(void) {
    send_cmd(0x10); // Deep Sleep
    send_data(0x01);
}

void ssd1675a_update_display(void) {
    send_cmd(0x22); 
    send_data(0xC7); // Update Control (from Python)
    send_cmd(0x20); 
    ssd1675a_wait_busy();
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
