/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <uart_async_adapter.h>
#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h> // Вернул GPIO
#include <zephyr/usb/usb_device.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <soc.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <bluetooth/services/nus.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <zephyr/logging/log.h>

// New Drivers/Libs
#include "drivers/ssd1675a.h"
#include "lib/graphics.h"

#define LOG_MODULE_NAME peripheral_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define STACKSIZE CONFIG_BT_NUS_THREAD_STACK_SIZE
#define PRIORITY 7
#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)
#define RUN_STATUS_LED DK_LED1
#define RUN_LED_BLINK_INTERVAL 1000
#define CON_STATUS_LED DK_LED2
#define KEY_PASSKEY_ACCEPT DK_BTN1_MSK
#define KEY_PASSKEY_REJECT DK_BTN2_MSK
#define UART_BUF_SIZE CONFIG_BT_NUS_UART_BUFFER_SIZE
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_WAIT_FOR_RX CONFIG_BT_NUS_UART_RX_WAIT_TIME

// Доступ к GPIO0
static const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
// =====================================

static K_SEM_DEFINE(ble_init_ok, 0, 1);
static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;
static struct k_work adv_work;
static const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(nordic_nus_uart));
static struct k_work_delayable uart_work;

struct uart_data_t {
	void *fifo_reserved;
	uint8_t data[UART_BUF_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

void ble_log(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0 && current_conn) {
        bt_nus_send(current_conn, buf, len);
    }
    LOG_INF("%s", buf);
}

// Red fill debug value
static uint8_t red_fill_debug = 0x00;

void perform_display_update(void) {
    if (!device_is_ready(gpio_dev)) return;

    // Power ON & Init
    ssd1675a_init(gpio_dev);

    // Send Buffers
    // Use the Graphics Library RED buffer (Mandelbrot support)
    // IMPORTANT: If user wants manual debug overrides, we could OR them or switch.
    // For now, let's prefer the Graphics Library output if it has content,
    // but the library defaults RED buffer to 0 (Transparent) which is what we want.
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    
    // Update
    ssd1675a_update_display();
    ssd1675a_sleep(); // Also power off logic can be here
    ssd1675a_power_off();
}

// Универсальная функция отображения текста
void show_text_on_display(const char *text) {
    // 1. Рисуем
    graphics_clear(GFX_WHITE); 
    // Рисуем рамку
    for(int x=0; x<128; x++) { graphics_draw_pixel(x, 0, GFX_BLACK); graphics_draw_pixel(x, 295, GFX_BLACK); }
    for(int y=0; y<296; y++) { graphics_draw_pixel(0, y, GFX_BLACK); graphics_draw_pixel(127, y, GFX_BLACK); }
    graphics_draw_string(5, 5, text);
    
    // 2. Обновляем
    perform_display_update();
}

void run_cleaning_cycle(void) {
    if (!device_is_ready(gpio_dev)) return;
    
    // Cycle 3 times
    for (int i=0; i<3; i++) {
        // Black
        graphics_clear(GFX_BLACK);
        perform_display_update();
        k_msleep(500);
        
        // White
        graphics_clear(GFX_WHITE);
        perform_display_update();
        k_msleep(500);
        
        // Red
        graphics_clear(GFX_RED);
        perform_display_update();
        k_msleep(500);
    }
    // Finish with White
    graphics_clear(GFX_WHITE);
    perform_display_update();
}

void run_animation_demo(void) {
    if (!device_is_ready(gpio_dev)) return;

    ble_log("Starting Animation (100 frames)...\r\n");
    
    // Ball State
    int bx = 10, by = 10;
    int bvx = 4, bvy = 2; // Velocity (Pixels per frame)
    int br = 8; // Radius
    
    // FPS Calc
    int64_t start_time = k_uptime_get();
    
    for (int f = 0; f < 100; f++) {
        int64_t frame_start = k_uptime_get();
        
        // Clear (Logic) - Don't use full clear as it might be slow? 
        // Graphics lib memset is fast in RAM.
        graphics_clear(GFX_WHITE);
        
        // Update Physics
        bx += bvx;
        by += bvy;
        
        // Bounce X
        if (bx - br < 0) { bx = br; bvx = -bvx; }
        if (bx + br >= DISPLAY_WIDTH) { bx = DISPLAY_WIDTH - 1 - br; bvx = -bvx; }
        
        // Bounce Y
        if (by - br < 0) { by = br; bvy = -bvy; }
        if (by + br >= DISPLAY_HEIGHT) { by = DISPLAY_HEIGHT - 1 - br; bvy = -bvy; }
        
        // Draw Ball (Circle approx)
        for (int y = -br; y <= br; y++) {
             for (int x = -br; x <= br; x++) {
                 if (x*x + y*y <= br*br) {
                     graphics_draw_pixel(bx + x, by + y, GFX_BLACK);
                 }
             }
        }
        
        // Draw Frame Counter / FPS
        char fps_str[16];
        snprintf(fps_str, sizeof(fps_str), "F:%d", f);
        graphics_draw_string(5, 5, fps_str);
        
        // Partial Update
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
        ssd1675a_update_partial();
        
        // Wait? Partial update handles busy wait. 
        // If it's too fast, we can sleep.
    }
    
    int64_t total_time = k_uptime_get() - start_time;
    ble_log("Animation Done. 100 Frames in %lld ms (~%lld ms/frame)\r\n", total_time, total_time/100);
    
    // Final clean manually if needed
}

void run_display_test(void) {
   show_text_on_display("TEST SUCCESS\nE-INK DRIVER READY\n\nSend 'TEXT:msg' to print.");
}

// Demo: Mandelbrot Set
void draw_mandelbrot(void) {
    if (!device_is_ready(gpio_dev)) return;

    graphics_clear(GFX_WHITE);

    // Mandelbrot Parameters
    // We want to fit the set into 128x296
    // Standard set is roughly within X=[-2.5, 1], Y=[-1, 1]
    // Since 128x296 is tall, let's rotate 90 degrees or just center it.
    // Let's keep it simple: Map X(0..127) to [-2.0, 1.0] (Range 3.0)
    // Scale = 3.0 / 128 = 0.0234
    
    // Actually, let's create a nice view.
    // X axis (128) -> Real part -2.2 to 0.8
    // Y axis (296) -> Imag part -1.2 to 1.2
    
    // float Re_min = -2.0;
    // float Re_max = 1.0;
    // float Im_min = -1.2;
    // float Im_max = 1.2;
    
    // But 296/128 ~= 2.3. 
    // If width (128) maps to 3.0 units.
    // Height (296) would map to 3.0 * 2.3 = 6.9 units. Too spread out for standard [-1, 1].
    // Let's swap? Map X (128) to Imaginary [-1.2, 1.2] (Range 2.4).
    // Y (296) to Real [-2.0, 1.0] (Range 3.0).
    
    // Viewport
    float start_Re = -2.2;
    float end_Re   = 0.8;
    float start_Im = -1.2;
    float end_Im   = 1.2;
    
    for (int y = 0; y < DISPLAY_HEIGHT; y++) { // 296
        for (int x = 0; x < DISPLAY_WIDTH; x++) { // 128
            // Map pixel to complex plane (Rotated 90 deg visual to fit portrait?)
            // Let's try "Normal": X is Real, Y is Imag.
            // X (128) = [-2.0, 1.0]
            // Y (296) = needs to scale to preserve aspect ratio.
            
            // Let's map Y (long axis) to Re [-2.0, 1.0] (Size 3.0)
            // Let's map X (short axis) to Im [-1.2, 1.2] (Size 2.4)
            // This rotates the standard "Bug" shape to be vertical.
            
            float c_Re = start_Re + (y / (float)DISPLAY_HEIGHT) * (end_Re - start_Re);
            float c_Im = start_Im + (x / (float)DISPLAY_WIDTH) * (end_Im - start_Im);
            
            float z_Re = c_Re;
            float z_Im = c_Im;
            
            int is_inside = 1;
            int max_iter = 20; // Low iter for speed on MCU
            int i;
            
            // Should start Z at 0 for standard set
            z_Re = 0;
            z_Im = 0;
            
            for(i=0; i<max_iter; i++) {
                float z_Re2 = z_Re * z_Re;
                float z_Im2 = z_Im * z_Im;
                
                if (z_Re2 + z_Im2 > 4) {
                    is_inside = 0;
                    break;
                }
                
                z_Im = 2 * z_Re * z_Im + c_Im;
                z_Re = z_Re2 - z_Im2 + c_Re;
            }
            
            if (is_inside) {
                graphics_draw_pixel(x, y, GFX_BLACK);
            } else {
                 // Colorize based on iterations ("Escape Time")
                 if (i < 4) {
                    // Fast escape -> White (Far outside)
                    graphics_draw_pixel(x, y, GFX_WHITE);
                 } else if (i < 8) {
                    // Medium escape -> Gray (Dithered)
                    graphics_draw_pixel(x, y, GFX_GRAY); 
                 } else if (i < 12) {
                    // Slow escape -> Pink (Dithered Red)
                    graphics_draw_pixel(x, y, GFX_PINK);
                 } else {
                    // Very slow escape (Border) -> Solid Red
                    graphics_draw_pixel(x, y, GFX_RED);
                 }
            }
        }
    }
    
    perform_display_update();
}

// COMMAND HANDLER
static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
    char input[256]; 
    uint16_t in_len = MIN(len, sizeof(input) - 1);
    memcpy(input, data, in_len);
    input[in_len] = '\0';
    
    // Trim
    char *end = input + in_len - 1;
    while(end >= input && (*end == '\n' || *end == '\r')) *end-- = '\0';

    LOG_INF("RX: %s", input);

    if (strncmp(input, "CLEAR", 5) == 0) {
        graphics_clear(GFX_WHITE);
        ble_log("Buffer Cleared (White)\r\n");
    }
    else if (strncmp(input, "CLEAN", 5) == 0) {
        ble_log("Running Cleaner (Wait ~20s)...\r\n");
        run_cleaning_cycle();
        ble_log("Clean cycle done.\r\n");
    }
    else if (strncmp(input, "ANIMATION", 9) == 0) {
        run_animation_demo();
    }
    else if (strncmp(input, "UPDATE", 6) == 0) {
         ble_log("Updating Display...\r\n");
         perform_display_update();
         ble_log("Done.\r\n");
    }
    else if (strncmp(input, "TEXT-U:", 7) == 0) {
        const char *msg = input + 7;
        show_text_on_display(msg);
        ble_log("Text-U: %s\r\n", msg);
    }
    else if (strncmp(input, "MANDELBROT", 10) == 0) {
        ble_log("Drawing Mandelbrot...\r\n");
        draw_mandelbrot();
        ble_log("Done.\r\n");
    }
    else if (strncmp(input, "DEBUG:RED=00", 12) == 0) {
        red_fill_debug = 0x00;
        ble_log("Red Fill set to 0x00 (Try Update)\r\n");
    }
    else if (strncmp(input, "DEBUG:RED=FF", 12) == 0) {
        red_fill_debug = 0xFF;
        ble_log("Red Fill set to 0xFF (Try Update)\r\n");
    }
    else if (strncmp(input, "DEBUG:VCOM=", 11) == 0) {
        // Parse Hex
        const char *val_str = input + 11;
        uint32_t val = strtoul(val_str, NULL, 16);
        ssd1675a_set_vcom_register((uint8_t)val);
        ble_log("VCOM set to 0x%02X (Try Update)\r\n", (uint8_t)val);
    }
    else if (strncmp(input, "DEBUG:LUT=", 10) == 0) {
        // Change the Voltage Byte controlling the last phase
        // Index 57 is 0x08 in "0x04, 0x08, 0x3C"
        // Valid values to try: 00, 04, 08(default), 0C, etc.
        const char *val_str = input + 10;
        uint32_t val = strtoul(val_str, NULL, 16);
        ssd1675a_set_lut_byte(57, (uint8_t)val);
        ble_log("LUT[57] set to 0x%02X (Try Update)\r\n", (uint8_t)val);
    }
    else if (strncmp(input, "DEBUG:LUT_LEN=", 14) == 0) {
        // Change the Duration Byte
        // Index 58 is 0x3C (60 frames)
        const char *val_str = input + 14;
        uint32_t val = strtoul(val_str, NULL, 16);
        ssd1675a_set_lut_byte(58, (uint8_t)val);
        ble_log("LUT[58] (Len) set to 0x%02X (Try Update)\r\n", (uint8_t)val);
    }
    else if (strncmp(input, "DEBUG:LUT_SET=", 14) == 0) {
        // Generic Set: DEBUG:LUT_SET=59:00
        const char *params = input + 14;
        // Search for ':'
        char *colon = strchr(params, ':');
        if (colon) {
            *colon = '\0'; // Split string temporarily
            const char *val_str = colon + 1;
            
            int idx = atoi(params); // Index is decimal
            uint32_t val = strtoul(val_str, NULL, 16); // Value is Hex
            
            ssd1675a_set_lut_byte(idx, (uint8_t)val);
            ble_log("LUT[%d] set to 0x%02X (Try Update)\r\n", idx, (uint8_t)val);
        } else {
             ble_log("Err: Format is idx:hexval\r\n");
        }
    }
    else if (strncmp(input, "TEXT:", 5) == 0) {
        const char *msg = input + 5;
        graphics_draw_string(5, 5, msg); 
        ble_log("Text drawn to buffer.\r\n");
    } 
    else if (strncmp(input, "HELP", 4) == 0) {
        ble_log("CMDS:\r\n"
                "TEXT:msg - Draw text\r\n"
                "TEXT-U:msg - Draw & Update\r\n"
                "CLEAR - Clear Buffer\r\n"
                "UPDATE - Refresh Display\r\n"
                "CLEAN - Run Cleaner\r\n"
                "MANDELBROT - Demo\r\n"
                "ROT:0/1/2/3 - Rotation (0,90..)\r\n"
                "DEBUG:VCOM=xx\r\n"
                "DEBUG:LUT=xx\r\n"
                "DEBUG:LUT_LEN=xx\r\n");
    }
    else if (strncmp(input, "ROT:", 4) == 0) {
        int r = atoi(input + 4);
        graphics_set_rotation(r);
        ble_log("Rotation set to %d\r\n", r);
    } 
    else if (strncmp(input, "FILL:BLACK", 10) == 0) {
        graphics_clear(GFX_BLACK);
        ble_log("Buffer filled with Black.\r\n");
    }
    else if (strncmp(input, "FILL:WHITE", 10) == 0) {
        graphics_clear(GFX_WHITE); 
        ble_log("Buffer filled with White.\r\n");
    }
    else {
        // Echo
        char response[128];
        snprintf(response, sizeof(response), "Echo: %s\r\n", input);
        bt_nus_send(conn, response, strlen(response));
    }
}



// --- ФУНКЦИИ UART ---
#ifdef CONFIG_UART_ASYNC_ADAPTER
UART_ASYNC_ADAPTER_INST_DEFINE(async_adapter);
#else
#define async_adapter NULL
#endif

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(evt);
    ARG_UNUSED(user_data);
}

static bool uart_test_async_api(const struct device *dev)
{
	const struct uart_driver_api *api = (const struct uart_driver_api *)dev->api;
	return (api->callback_set != NULL);
}

static int uart_init(void)
{
	int err;
	if (!device_is_ready(uart)) return -ENODEV;

	if (IS_ENABLED(CONFIG_UART_ASYNC_ADAPTER) && !uart_test_async_api(uart)) {
		uart_async_adapter_init(async_adapter, uart);
		uart = async_adapter;
	}

	err = uart_callback_set(uart, uart_cb, NULL);
	if (err) return err;
	return 0;
}

static void uart_work_handler(struct k_work *item) { }

// --- ФУНКЦИИ BLE CONNECTION ---
static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) LOG_ERR("Advertising failed (err %d)", err);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	if (err) {
		LOG_ERR("Connection failed (err 0x%02x)", err);
		return;
	}
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);
	current_conn = bt_conn_ref(conn);
	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);
	if (auth_conn) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
		dk_set_led_off(CON_STATUS_LED);
	}
}

static void recycled_cb(void)
{
	advertising_start();
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (!err) LOG_INF("Security changed: %s level %u", addr, level);
	else LOG_WRN("Security failed: %s err %d", addr, err);
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};

#if defined(CONFIG_BT_NUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];
	auth_conn = bt_conn_ref(conn);
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing failed: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif

// --- КОНЕЦ ВОССТАНОВЛЕННЫХ ФУНКЦИЙ ---



static struct bt_nus_cb nus_cb = {
	.received = bt_receive_cb,
};

static void configure_gpio(void)
{
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
    // Кнопки оставляем, даже если их нет, чтобы компилировалось
    // err = dk_buttons_init(button_changed);
#endif
	dk_leds_init();
}

int main(void)
{
	int blink_status = 0;
	int err = 0;

	configure_gpio();
	graphics_init();
	uart_init(); // Вернул инициализацию

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
    // ВЕРНУЛ РЕГИСТРАЦИЮ CALLBACK-ов БЕЗОПАСНОСТИ
	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) LOG_ERR("Auth cb err: %d", err);

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) LOG_ERR("Auth info cb err: %d", err);
#endif

	err = bt_enable(NULL);
	if (err) return 0;

	LOG_INF("Bluetooth initialized");

	k_sem_give(&ble_init_ok);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load(); // ВЕРНУЛ ЗАГРУЗКУ НАСТРОЕК
	}

	err = bt_nus_init(&nus_cb);
	if (err) LOG_ERR("NUS init err: %d", err);

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	for (;;) {
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
	}
}