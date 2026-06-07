/*
 * LUT Tester — real-time SSD1675A waveform table editor over BLE NUS.
 *
 * BLE commands (plain text, newline-terminated):
 *
 *   LUTW:HHHH...HH   Write entire 70-byte LUT as 140 hex chars (no spaces)
 *                    Example: LUTW:2211100010000011...
 *   LW:idx:HH...HH   Write N bytes starting at idx (e.g. LW:35:040402040A)
 *   L:n=HH           Set single LUT byte (e.g. L:57=10)
 *   L:DUMP           Print all 70 LUT bytes
 *   L:RESET          Restore factory LUT
 *   APPLY            CLEAR (factory LUT) → draw test image → update with custom LUT
 *   CLEAR            White refresh using factory LUT
 *   VCOM=HH          Set VCOM register (hex)
 *   HELP             List commands
 *
 * Large MTU (DLE) is enabled in prj.conf so LUTW fits in one BLE packet.
 * If the phone sends it fragmented, the line accumulator reassembles it.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dk_buttons_and_leds.h>

#include "drivers/ssd1675a.h"
#include "lib/graphics.h"
#include "ble/ble_service.h"

LOG_MODULE_REGISTER(lut_tester, LOG_LEVEL_INF);

static const struct device *gpio_dev;
static int apply_count;

/* ------------------------------------------------------------------ */
/* BLE receive — line accumulator + deferred display work               */
/* ------------------------------------------------------------------ */

/*
 * Bug fix: the old accumulate() returned on the first '\n' and discarded
 * any bytes after it in the same notification.  When the host sends
 * "LUTW:...\nAPPLY\n" in one packet (DLE, MTU=247) the APPLY line was
 * silently dropped.
 *
 * Fix 1: process every byte in the notification; execute process_line()
 *         for each complete line found — there may be more than one.
 *
 * Fix 2: do_apply() / do_clear() block the BLE callback thread for
 *         seconds (full e-ink refresh).  Defer them via k_work so the
 *         BLE stack keeps running while the display updates.
 */
#define RX_BUF_SIZE 320

static char rx_buf[RX_BUF_SIZE];
static int  rx_len;

typedef enum { DCMD_NONE, DCMD_APPLY, DCMD_CLEAR } deferred_cmd_t;
static volatile deferred_cmd_t pending_dcmd;
static void display_work_handler(struct k_work *work);
static K_WORK_DEFINE(display_work, display_work_handler);

/* ------------------------------------------------------------------ */
/* Hex helpers                                                          */
/* ------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_byte(const char *s)
{
    int hi = hex_nibble(s[0]);
    int lo = hex_nibble(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/* ------------------------------------------------------------------ */
/* Test image                                                           */
/* ------------------------------------------------------------------ */

/*
 * Layout (rotation=1 → logical 296 wide × 128 tall):
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │  LUT TESTER                               APPLY: 3            │  y 3
 *   ├──────────┬──────────┬──────────┬──────────╫────────────────── │  y 17
 *   │  SOLID   │  SOLID   │  2px     │  4px     ║  [57]=08          │
 *   │  BLACK   │  WHITE   │  CHECK   │  STRIPES ║  [58]=3C          │
 *   │          │ (border) │          │          ║  [59]=07          │
 *   ├──────────┴──────────┴──────────┴──────────╫──────────────────-│  y107
 *   │  P0: 04 18 04 16 01   P1: 0A 0A 0A 0A 02  ║  first 14 bytes  │
 *   └────────────────────────────────────────────╨──────────────────┘  y127
 */

#define PANEL_Y0   18
#define PANEL_Y1  107
#define PANEL_H   (PANEL_Y1 - PANEL_Y0)

#define PANEL1_X0   2
#define PANEL1_X1  53
#define PANEL2_X0  57
#define PANEL2_X1 108
#define PANEL3_X0 112
#define PANEL3_X1 163
#define PANEL4_X0 167
#define PANEL4_X1 211

#define INFO_X0   214
#define INFO_X1   295

static void draw_checkerboard(int x0, int y0, int x1, int y1, int cell)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int cx = (x - x0) / cell;
            int cy = (y - y0) / cell;
            graphics_draw_pixel(x, y, ((cx + cy) & 1) ? GFX_BLACK : GFX_WHITE);
        }
    }
}

static void draw_vstripes(int x0, int y0, int x1, int y1, int w)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            graphics_draw_pixel(x, y, (((x - x0) / w) & 1) ? GFX_BLACK : GFX_WHITE);
        }
    }
}

static void draw_test_image(void)
{
    char buf[64];

    graphics_clear(GFX_WHITE);

    graphics_draw_rect(0, 0, 296, 128, GFX_BLACK);

    /* Title */
    graphics_draw_string(3, 4, "LUT TESTER");
    snprintf(buf, sizeof(buf), "APPLY:%d", apply_count);
    graphics_draw_string(234, 4, buf);

    /* Separators */
    for (int x = 0; x < 296; x++)       graphics_draw_pixel(x, 16, GFX_BLACK);
    for (int x = 0; x <= INFO_X0; x++)  graphics_draw_pixel(x, 107, GFX_BLACK);
    for (int y = 16; y < 128; y++)      graphics_draw_pixel(INFO_X0, y, GFX_BLACK);

    /* Panel labels */
    graphics_draw_string(PANEL1_X0 + 2, PANEL_Y0 + 1, "BLK");
    graphics_draw_string(PANEL2_X0 + 2, PANEL_Y0 + 1, "WHT");
    graphics_draw_string(PANEL3_X0 + 2, PANEL_Y0 + 1, "CHK");
    graphics_draw_string(PANEL4_X0 + 2, PANEL_Y0 + 1, "STR");

    /* Panel 1 — solid black */
    graphics_fill_rect(PANEL1_X0, PANEL_Y0 + 10,
                       PANEL1_X1 - PANEL1_X0, PANEL_H - 10, GFX_BLACK);

    /* Panel 2 — solid white with border */
    graphics_draw_rect(PANEL2_X0, PANEL_Y0 + 10,
                       PANEL2_X1 - PANEL2_X0, PANEL_H - 10, GFX_BLACK);

    /* Panel 3 — 2-pixel checkerboard */
    draw_checkerboard(PANEL3_X0, PANEL_Y0 + 10, PANEL3_X1, PANEL_Y1, 2);

    /* Panel 4 — 4-pixel vertical stripes */
    draw_vstripes(PANEL4_X0, PANEL_Y0 + 10, PANEL4_X1, PANEL_Y1, 4);

    /* Footer: phase timings */
    snprintf(buf, sizeof(buf), "P0:%02X %02X %02X %02X %02X",
             ssd1675a_get_lut_byte(35), ssd1675a_get_lut_byte(36),
             ssd1675a_get_lut_byte(37), ssd1675a_get_lut_byte(38),
             ssd1675a_get_lut_byte(39));
    graphics_draw_string(3, 110, buf);

    snprintf(buf, sizeof(buf), "P1:%02X %02X %02X %02X %02X",
             ssd1675a_get_lut_byte(40), ssd1675a_get_lut_byte(41),
             ssd1675a_get_lut_byte(42), ssd1675a_get_lut_byte(43),
             ssd1675a_get_lut_byte(44));
    graphics_draw_string(3, 119, buf);

    /* Info column: key bytes */
    snprintf(buf, sizeof(buf), "[57]=%02X", ssd1675a_get_lut_byte(57));
    graphics_draw_string(INFO_X0 + 2, 20, buf);
    snprintf(buf, sizeof(buf), "[58]=%02X", ssd1675a_get_lut_byte(58));
    graphics_draw_string(INFO_X0 + 2, 32, buf);
    snprintf(buf, sizeof(buf), "[59]=%02X", ssd1675a_get_lut_byte(59));
    graphics_draw_string(INFO_X0 + 2, 44, buf);

    /* Mini hex dump: first 14 bytes (voltage LUT) */
    graphics_draw_string(INFO_X0 + 2, 58, "volt:");
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof(buf), "%02X", ssd1675a_get_lut_byte(i));
        graphics_draw_string(INFO_X0 + 2 + i * 12, 68, buf);
    }
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof(buf), "%02X", ssd1675a_get_lut_byte(7 + i));
        graphics_draw_string(INFO_X0 + 2 + i * 12, 78, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Display actions                                                      */
/* ------------------------------------------------------------------ */

static void do_clear(void)
{
    ble_printf("CLEAR...\r\n");
    ssd1675a_clear_with_factory_lut(gpio_dev);
    ble_printf("ok\r\n");
}

static void do_apply(void)
{
    apply_count++;
    ble_printf("APPLY #%d\r\n", apply_count);

    ssd1675a_clear_with_factory_lut(gpio_dev);

    draw_test_image();

    ssd1675a_init(gpio_dev);
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    ssd1675a_update_display();

    ble_printf("done\r\n");
}

/* ------------------------------------------------------------------ */
/* Command processing                                                   */
/* ------------------------------------------------------------------ */

static void cmd_lut_dump(void)
{
    ble_printf("LUT[70]:\r\n");
    for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
        ble_printf("%02X%s", ssd1675a_get_lut_byte(i),
                   (i % 10 == 9 || i == SSD1675A_LUT_SIZE - 1) ? "\r\n" : " ");
    }
}

static void cmd_lutw(const char *hex)
{
    /* Expect exactly SSD1675A_LUT_SIZE*2 hex chars */
    int expected = SSD1675A_LUT_SIZE * 2;
    int got = (int)strlen(hex);

    if (got != expected) {
        ble_printf("LUTW: need %d hex chars, got %d\r\n", expected, got);
        return;
    }

    for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) {
            ble_printf("LUTW: bad hex at pos %d\r\n", i * 2);
            return;
        }
        ssd1675a_set_lut_byte(i, (uint8_t)b);
    }

    ble_printf("LUT written (%d bytes)\r\n", SSD1675A_LUT_SIZE);
}

static void cmd_lw(const char *arg)
{
    /* LW:idx:HH...HH */
    char *colon = strchr(arg, ':');
    if (!colon) { ble_printf("usage: LW:idx:HH...\r\n"); return; }

    *colon = '\0';
    int idx = atoi(arg);
    const char *hex = colon + 1;
    int nbytes = (int)strlen(hex) / 2;

    if (idx < 0 || idx + nbytes > SSD1675A_LUT_SIZE) {
        ble_printf("LW: out of range\r\n"); return;
    }

    for (int i = 0; i < nbytes; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) { ble_printf("LW: bad hex at %d\r\n", i); return; }
        ssd1675a_set_lut_byte(idx + i, (uint8_t)b);
    }
    ble_printf("LW: wrote %d bytes from [%d]\r\n", nbytes, idx);
}

static void cmd_l(const char *arg)
{
    if (strcmp(arg, "DUMP") == 0)  { cmd_lut_dump(); return; }
    if (strcmp(arg, "RESET") == 0) { ssd1675a_reset_lut(); ble_printf("LUT reset\r\n"); return; }

    /* L:n=HH */
    char tmp[32];
    strncpy(tmp, arg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *eq = strchr(tmp, '=');
    if (!eq) { ble_printf("usage: L:n=HH\r\n"); return; }
    *eq = '\0';
    int idx = atoi(tmp);
    uint8_t val = (uint8_t)strtoul(eq + 1, NULL, 16);
    ssd1675a_set_lut_byte(idx, val);
    ble_printf("L[%d]=0x%02X\r\n", idx, val);
}

/* Runs in system workqueue — safe to block here for display operations. */
static void display_work_handler(struct k_work *work)
{
    deferred_cmd_t cmd = pending_dcmd;
    pending_dcmd = DCMD_NONE;

    if (cmd == DCMD_APPLY) do_apply();
    else if (cmd == DCMD_CLEAR) do_clear();
}

static void process_line(const char *line)
{
    LOG_INF("cmd: %s", line);

    if (strncmp(line, "LUTW:", 5) == 0) { cmd_lutw(line + 5); return; }
    if (strncmp(line, "LW:",   3) == 0) { cmd_lw(line + 3);   return; }
    if (strncmp(line, "L:",    2) == 0) { cmd_l(line + 2);    return; }

    /* Display commands are deferred — never block the BLE callback thread. */
    if (strcmp(line, "APPLY") == 0) {
        pending_dcmd = DCMD_APPLY;
        k_work_submit(&display_work);
        ble_printf("queued APPLY\r\n");
        return;
    }
    if (strcmp(line, "CLEAR") == 0) {
        pending_dcmd = DCMD_CLEAR;
        k_work_submit(&display_work);
        ble_printf("queued CLEAR\r\n");
        return;
    }

    if (strncmp(line, "VCOM=", 5) == 0) {
        uint8_t v = (uint8_t)strtoul(line + 5, NULL, 16);
        ssd1675a_set_vcom_register(v);
        ble_printf("VCOM=0x%02X\r\n", v);
        return;
    }

    if (strcmp(line, "HELP") == 0) {
        ble_printf(
            "LUTW:HH..   write full LUT (140 hex)\r\n"
            "LW:i:HH..   write N bytes from idx i\r\n"
            "L:n=HH      set one byte\r\n"
            "L:DUMP      print LUT\r\n"
            "L:RESET     restore factory\r\n"
            "APPLY       clear+draw+refresh\r\n"
            "CLEAR       white refresh\r\n"
            "VCOM=HH     set VCOM\r\n"
        );
        return;
    }

    ble_printf("unknown cmd\r\n");
}

/* ------------------------------------------------------------------ */
/* BLE callback                                                         */
/* ------------------------------------------------------------------ */

/*
 * Process every byte in the notification.  Call process_line() for each
 * complete '\n'-terminated line — a single notification may contain
 * multiple lines (e.g. "LUTW:...\nAPPLY\n" arrives as one 152-byte write
 * when DLE is active).
 */
static void ble_rx(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            rx_buf[rx_len] = '\0';
            rx_len = 0;
            if (rx_buf[0] != '\0') {
                process_line(rx_buf);
            }
        } else if (rx_len < RX_BUF_SIZE - 1) {
            rx_buf[rx_len++] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(void)
{
    LOG_INF("LUT Tester starting");

    dk_leds_init();

    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO not ready");
        return -1;
    }

    graphics_init();
    graphics_set_rotation(1);

    int err = ble_service_init(ble_rx);
    if (err) {
        LOG_ERR("BLE init failed: %d", err);
        return -1;
    }

    LOG_INF("Ready — advertising as 'LUT-Tester'");

    ssd1675a_clear_with_factory_lut(gpio_dev);

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
