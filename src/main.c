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
// static struct k_work_delayable uart_work; // Removed

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

#include <zephyr/drivers/adc.h>
#include <zephyr/pm/pm.h>

// ADC Config (Internal VDD)
#define APP_ADC_DT_SPEC DT_PATH(zephyr_user)

static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc)); // nRF52 SAADC

// ADC Channel Config
// We want to measure VDD/3 (Internal scaling) against Internal Reference (0.6V)
// Result = (VDD/3) / 0.6 * MaxCounts
// VDD = Result * 0.6 * 3 / MaxCounts
// Actually nRF52 SAADC is flexible. Let's use standard config.
// Simple: Measure VDD directly if possible via Internal Input.
// Enable CONFIG_ADC=y.

// Just a simple Mock/Stub for Battery for this step if ADC complex setup needed.
// But let's try real ADC.
// On nRF52, Channel 0, Input VDD.
struct adc_channel_cfg channel_cfg = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .input_positive = SAADC_CH_PSELP_PSELP_VDD // Measure VDD
};

int16_t sample_buffer[1];
struct adc_sequence sequence = {
    .channels = BIT(0),
    .buffer = sample_buffer,
    .buffer_size = sizeof(sample_buffer),
    .resolution = 10,
};

int init_adc(void) {
    if (!device_is_ready(adc_dev)) {
        ble_log("ADC not ready\r\n");
        return -1;
    }
    adc_channel_setup(adc_dev, &channel_cfg);
    return 0;
}

int read_battery_mv(void) {
    if (!adc_dev) return 0;
    adc_read(adc_dev, &sequence);
    // 10 bit resolution, Ref 0.6V, Gain 1/6.
    // Input range: 0 to 3.6V.
    // Val = (Input * (1/6)) / 0.6 * 1023
    // Input = Val * 0.6 * 6 / 1023 = Val * 3.6 / 1023
    // mV = Val * 3600 / 1023
    int32_t val = sample_buffer[0];
    return (val * 3600) / 1023;
}

// Global flag
static bool ble_connected = false;

// ... (Keep existing helpers or move them)

// Old update_screensaver removed.
// See new implementation below receive_cb.

// Main logic moved to bottom
// Helpers ready.

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
// --- Time & Date Logic ---
#include <time.h>

static int64_t time_offset_sec = 0; // Offset from Uptime in Seconds
static int32_t last_render_duration_ms = 0;

// Simple structure for time (tm is available in standard lib usually)
// We use seconds since epoch for internal storage.

void set_system_time(int h, int m, int s, int D, int M, int Y) {
    struct tm t = {0};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    t.tm_isdst = -1;
    
    time_t target_ts = mktime(&t);
    int64_t uptime_sec = k_uptime_get() / 1000;
    
    time_offset_sec = (int64_t)target_ts - uptime_sec;
    ble_log("Time Set: %02d:%02d:%02d %02d.%02d.%04d\r\n", h, m, s, D, M, Y);
}

void get_system_time(struct tm *t) {
    int64_t uptime_sec = k_uptime_get() / 1000;
    time_t now = (time_t)(uptime_sec + time_offset_sec);
    // Use gmtime_r or similar if available, or simplified.
    // Zephyr minimal libc might behave differently.
    struct tm *tmp = gmtime(&now); 
    if (tmp) *t = *tmp;
    else memset(t, 0, sizeof(struct tm));
}

// --- Command System ---
typedef void (*cmd_handler_t)(char *args);

struct shell_cmd {
    const char *name;
    cmd_handler_t handler;
    const char *help;
};

// Global State
static bool screensaver_active = true;

// Define helper prototypes if needed
void run_cleaning_cycle(void);
void update_screensaver(void);

extern const struct shell_cmd commands[]; // Forward Decl

// Handlers implementation

void cmd_help(char *args) {
    ble_log("cmds:\r\n");
    for (int i=0; commands[i].name != NULL; i++) {
        ble_log("  %s\r\n", commands[i].name);
    }
}

void cmd_cls(char *args) {
    screensaver_active = false;
    graphics_clear(GFX_WHITE);
    ble_log("cleared\r\n");
}

void cmd_clean(char *args) {
    screensaver_active = false; // logic takes control
    ble_log("cleaning...\r\n");
    run_cleaning_cycle();
    ble_log("done\r\n");
}

void cmd_saver(char *args) {
    screensaver_active = true;
    ble_log("saver enabled\r\n");
    update_screensaver(); // Immediate update
}

void cmd_update(char *args) {
    ble_log("updating...\r\n");
    int64_t start = k_uptime_get();
    perform_display_update();
    last_render_duration_ms = (int32_t)(k_uptime_get() - start);
    ble_log("done in %d ms\r\n", last_render_duration_ms);
}

void cmd_text(char *args) {
    screensaver_active = false;
    if (!args || !*args) return;
    graphics_draw_string(5, 5, args);
    ble_log("drawn\r\n");
}

void cmd_text_u(char *args) {
    screensaver_active = false;
    if (!args || !*args) return;
    show_text_on_display(args);
    ble_log("shown\r\n");
}

void cmd_rot(char *args) {
    if (!args) return;
    int r = atoi(args);
    graphics_set_rotation(r);
    ble_log("rotation: %d\r\n", r);
    // If screensaver was active, it will be redrawn rotated on next tick.
}

void cmd_batt(char *args) {
    int mv = read_battery_mv();
    ble_log("bat: %d mv\r\n", mv);
}

void cmd_time(char *args) {
     // Format: HH:MM:SS DD.MM.YYYY
     // Or just split by separators
     if (!args || strlen(args) < 10) {
         ble_log("usage: TIME HH:MM:SS DD.MM.YYYY\r\n");
         return;
     }
     
     int h, m, s, D, M, Y;
     int count = sscanf(args, "%d:%d:%d %d.%d.%d", &h, &m, &s, &D, &M, &Y);
     if (count == 6) {
         set_system_time(h, m, s, D, M, Y);
     } else {
         ble_log("parse error. count=%d\r\n", count);
     }
}

void cmd_mandelbrot(char *args) {
    screensaver_active = false;
    draw_mandelbrot();
}

void cmd_debug_vcom(char *args) {
    if (!args) return;
    uint32_t val = strtoul(args, NULL, 16);
    ssd1675a_set_vcom_register((uint8_t)val);
    ble_log("VCOM=0x%02X\r\n", (uint8_t)val);
}

void cmd_debug_lut(char *args) {
    // Expect "idx:hex"
    if (!args) return;
    char *colon = strchr(args, ':');
    if (colon) {
        *colon = '\0';
        int idx = atoi(args);
        uint32_t val = strtoul(colon+1, NULL, 16);
        ssd1675a_set_lut_byte(idx, (uint8_t)val);
        ble_log("LUT[%d]=0x%02X\r\n", idx, (uint8_t)val);
    }
}

void cmd_debug_red(char *args) {
    if (!args) return;
    uint32_t val = strtoul(args, NULL, 16);
    red_fill_debug = (uint8_t)val;
    ble_log("RED_FILL=0x%02X\r\n", red_fill_debug);
}

// Registry
const struct shell_cmd commands[] = {
    {"HELP", cmd_help, "List commands"},
    {"SAVER", cmd_saver, "Activate Screensaver mode"},
    {"CLEAR", cmd_cls, "Clear buffer"},
    {"CLEAN", cmd_clean, "Run clean cycle"},
    {"UPDATE", cmd_update, "Refresh display"},
    {"TEXT:", cmd_text, "Draw text (arg: msg)"},
    {"TEXT-U:", cmd_text_u, "Draw&Update text"},
    {"ROT:", cmd_rot, "Set Rotation 0-3"},
    {"BATT", cmd_batt, "Get Battery mV"},
    {"TIME", cmd_time, "Set Time (HH:MM:SS DD.MM.YYYY)"},
    {"MANDEL", cmd_mandelbrot, "Mandelbrot Demo"}, 
    {"MANDELBROT", cmd_mandelbrot, "Mandelbrot Demo"},
    {"DEBUG:VCOM=", cmd_debug_vcom, "Set VCOM (hex)"},
    {"DEBUG:LUT=", cmd_debug_lut, "Set LUT idx:hex"},
    {"DEBUG:RED=", cmd_debug_red, "Set Red Fill hex"},
    {NULL, NULL, NULL}
};

// New Callback
static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
    char input[256]; 
    uint16_t in_len = MIN(len, sizeof(input) - 1);
    memcpy(input, data, in_len);
    input[in_len] = '\0';
    
    // Trim CRLF
    char *end = input + in_len - 1;
    while(end >= input && (*end == '\n' || *end == '\r')) *end-- = '\0';

    if (strlen(input) == 0) return;
    LOG_INF("RX: %s", input);

    // Find Command
    bool found = false;
    for (int i=0; commands[i].name != NULL; i++) {
        const char *cmd = commands[i].name;
        int cmd_len = strlen(cmd);
        
        // Check if input starts with cmd
        if (strncmp(input, cmd, cmd_len) == 0) {
            // Check boundary: cmd terminator (space or \0) usually,
            // but some cmds have ':' at end like "TEXT:" which handles args differently.
            // If cmd ends with ':', we treat rest as args.
            // If cmd is word like "HELP", we expect space or end.
            
            char *args = input + cmd_len;
            
            // If simple command (no ':'), check if next char is space or nul
            if (cmd[cmd_len-1] != ':' && *args != '\0' && *args != ' ') {
                continue; // Partial match, e.g. "TIMES" vs "TIME"
            }
            
            // Skip leading space in args
            while (*args == ' ') args++;
            
            commands[i].handler(args);
            found = true;
            break;
        }
    }
    
    if (!found) {
        ble_log("unknown cmd. try HELP\r\n");
    }
}

// --- Display Logic Updates ---

void update_screensaver(void) {
    if (!device_is_ready(gpio_dev)) return;
    
    int64_t start_render = k_uptime_get();
    
    graphics_clear(GFX_WHITE);
    
    // 1. Time
    struct tm t;
    get_system_time(&t);
    
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", t.tm_hour, t.tm_min);
    graphics_draw_string_scaled(70, 30, time_str, 5); // Raised slightly
    
    // 2. Date (Centered below time)
    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%02d.%02d.%04d", t.tm_mday, t.tm_mon+1, t.tm_year+1900);
    // Font 5x7 -> Width 6*10 = 60 centered? No, scale 2?
    // Scale 2: Width 10 chars * 6 * 2 = 120. Screen 296. Center ~88.
    // Y = 30 + (7*5=35) + 10 padding = 75.
    graphics_draw_string_scaled(58, 80, date_str, 3);
    
    // 3. Battery
    int mv = read_battery_mv();
    int pct = (mv > 3000) ? 100 : (mv < 2000 ? 0 : (mv-2000)/10);
    graphics_draw_battery(260, 5, pct);
    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%dmV", mv);
    graphics_draw_string(260, 18, bat_str);
    
    // 4. Render Stats (Bottom Left)
    char stat_str[32];
    snprintf(stat_str, sizeof(stat_str), "Render: %dms", last_render_duration_ms);
    graphics_draw_string(5, DISPLAY_HEIGHT - 10, stat_str);
    
    // Update
    perform_display_update();
    
    last_render_duration_ms = (int32_t)(k_uptime_get() - start_render);
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

// static void uart_work_handler(struct k_work *item) { } // Removed

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
	} else {
		bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
		LOG_INF("Connected %s", addr);
		current_conn = bt_conn_ref(conn);
		dk_set_led_on(CON_STATUS_LED);
        ble_connected = true;
	}
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
    screensaver_active = true;
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

    // Init Peripherals
	configure_gpio();
    init_adc();
	graphics_init();
	uart_init(); 

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
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
		settings_load(); 
	}

	err = bt_nus_init(&nus_cb);
	if (err) LOG_ERR("NUS init err: %d", err);

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

    // Screensaver State
    int64_t next_update = 0;

	for (;;) {
        // Blink LED
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
        
        // Screensaver Logic
        if (screensaver_active) {
            if (k_uptime_get() > next_update) {
                // LOG_INF("Screensaver Update"); // Optional log
                update_screensaver();
                next_update = k_uptime_get() + 60000; // 1 min
            }
            // Sleep
            k_sleep(K_MSEC(1000));
        } else {
            // Connected/Manual Mode - Sleep faster or normal blink
            k_sleep(K_MSEC(1000));
        }
	}
}