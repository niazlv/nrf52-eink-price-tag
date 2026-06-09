/*
 * LUT Tester — real-time SSD1675A waveform table editor + BWR frame sender over BLE NUS.
 *
 * Architecture: EXACT root project structure (display_manager + K_THREAD_DEFINE inside it).
 * display_manager_update_status() is called from main() at priority 0 AFTER BLE init,
 * so ssd1675a_init() runs without BLE preemption stretching the post-RST timing window.
 *
 * Screensaver (from display_manager) shows time/battery as in root project.
 * LUT/frame commands disable the screensaver temporarily and push custom content.
 *
 * BLE commands:
 *   LUTW:HHHH...HH   Write entire 70-byte LUT (140 hex chars)
 *   LW:idx:HH...HH   Write N bytes from idx
 *   L:n=HH           Set single LUT byte
 *   L:DUMP / L:RESET
 *   FW:offset:HH..   Write bytes into BW frame buffer
 *   RW:offset:HH..   Write bytes into Red frame buffer
 *   APPLY            Draw test image + full update
 *   FAPPLY           Push current BW+Red frame buffers to display
 *   CLEAR            Clear display to white
 *   VCOM=HH          Set VCOM register (hex)
 *   SS:0/1           Disable/enable screensaver
 *   TIME=HH:MM:SS    Set time
 *   HELP             List commands
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dk_buttons_and_leds.h>

#include "app/display_manager.h"
#include "app/battery.h"
#include "app/system_time.h"
#include "drivers/ssd1675a.h"
#include "lib/graphics.h"
#include "ble/ble_service.h"

LOG_MODULE_REGISTER(lut_tester, LOG_LEVEL_INF);

static int apply_count;

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
/* Frame buffer access                                                  */
/* ------------------------------------------------------------------ */

#define FB_SIZE 4736   /* 128 * 296 / 8 */

static uint8_t *fb_bw(void)  { return (uint8_t *)graphics_get_buffer(); }
static uint8_t *fb_red(void) { return (uint8_t *)graphics_get_red_buffer(); }

/* ------------------------------------------------------------------ */
/* Test image (APPLY command)                                           */
/* ------------------------------------------------------------------ */

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
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int cx = (x - x0) / cell, cy = (y - y0) / cell;
            graphics_draw_pixel(x, y, ((cx + cy) & 1) ? GFX_BLACK : GFX_WHITE);
        }
}

static void draw_vstripes(int x0, int y0, int x1, int y1, int w)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            graphics_draw_pixel(x, y, (((x - x0) / w) & 1) ? GFX_BLACK : GFX_WHITE);
}

static void draw_test_image(void)
{
    char buf[64];
    graphics_clear(GFX_WHITE);
    graphics_draw_rect(0, 0, 296, 128, GFX_BLACK);
    graphics_draw_string(3, 4, "LUT TESTER");
    snprintf(buf, sizeof(buf), "APPLY:%d", apply_count);
    graphics_draw_string(234, 4, buf);

    for (int x = 0; x < 296; x++)      graphics_draw_pixel(x, 16, GFX_BLACK);
    for (int x = 0; x <= INFO_X0; x++) graphics_draw_pixel(x, 107, GFX_BLACK);
    for (int y = 16; y < 128; y++)     graphics_draw_pixel(INFO_X0, y, GFX_BLACK);

    graphics_draw_string(PANEL1_X0 + 2, PANEL_Y0 + 1, "BLK");
    graphics_draw_string(PANEL2_X0 + 2, PANEL_Y0 + 1, "WHT");
    graphics_draw_string(PANEL3_X0 + 2, PANEL_Y0 + 1, "CHK");
    graphics_draw_string(PANEL4_X0 + 2, PANEL_Y0 + 1, "STR");

    graphics_fill_rect(PANEL1_X0, PANEL_Y0 + 10, PANEL1_X1 - PANEL1_X0, PANEL_H - 10, GFX_BLACK);
    graphics_draw_rect(PANEL2_X0, PANEL_Y0 + 10, PANEL2_X1 - PANEL2_X0, PANEL_H - 10, GFX_BLACK);
    draw_checkerboard(PANEL3_X0, PANEL_Y0 + 10, PANEL3_X1, PANEL_Y1, 2);
    draw_vstripes(PANEL4_X0, PANEL_Y0 + 10, PANEL4_X1, PANEL_Y1, 4);

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

    snprintf(buf, sizeof(buf), "[57]=%02X", ssd1675a_get_lut_byte(57));
    graphics_draw_string(INFO_X0 + 2, 20, buf);
    snprintf(buf, sizeof(buf), "[58]=%02X", ssd1675a_get_lut_byte(58));
    graphics_draw_string(INFO_X0 + 2, 32, buf);
    snprintf(buf, sizeof(buf), "[59]=%02X", ssd1675a_get_lut_byte(59));
    graphics_draw_string(INFO_X0 + 2, 44, buf);

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
/* Display actions — deferred to worker thread at priority 7            */
/* ------------------------------------------------------------------ */

typedef enum { DCMD_NONE, DCMD_APPLY, DCMD_FAPPLY, DCMD_CLEAR } deferred_cmd_t;
static volatile deferred_cmd_t pending_dcmd = DCMD_NONE;
static K_SEM_DEFINE(display_sem, 0, 1);

static void do_clear(void)
{
    ble_printf("CLEAR...\r\n");
    display_manager_clean();
    ble_printf("ok\r\n");
}

static void do_apply(void)
{
    apply_count++;
    ble_printf("APPLY #%d\r\n", apply_count);
    draw_test_image();
    display_manager_force_update();
    ble_printf("done\r\n");
}

static void do_fapply(void)
{
    ble_printf("FAPPLY...\r\n");
    display_manager_force_update();
    ble_printf("done\r\n");
}

/* Worker thread for display commands — same priority as screensaver_thread */
static void display_thread_fn(void *p1, void *p2, void *p3);
K_THREAD_DEFINE(display_tid, 2048, display_thread_fn, NULL, NULL, NULL, 7, 0, 0);

static void display_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Wait a bit longer than screensaver_thread's 2s so that thread runs first */
    k_sleep(K_SECONDS(3));

    while (1) {
        k_sem_take(&display_sem, K_FOREVER);

        deferred_cmd_t cmd = pending_dcmd;
        pending_dcmd = DCMD_NONE;

        switch (cmd) {
        case DCMD_APPLY:  do_apply();  break;
        case DCMD_FAPPLY: do_fapply(); break;
        case DCMD_CLEAR:  do_clear();  break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* BLE receive — line accumulator                                       */
/* ------------------------------------------------------------------ */

#define RX_BUF_SIZE 320
static char rx_buf[RX_BUF_SIZE];
static int  rx_len;

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
    int expected = SSD1675A_LUT_SIZE * 2;
    int got = (int)strlen(hex);
    if (got != expected) {
        ble_printf("LUTW: need %d hex, got %d\r\n", expected, got);
        return;
    }
    for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) { ble_printf("LUTW: bad hex at %d\r\n", i * 2); return; }
        ssd1675a_set_lut_byte(i, (uint8_t)b);
    }
    ble_printf("LUT written (%d bytes)\r\n", SSD1675A_LUT_SIZE);
}

static void cmd_lw(const char *arg)
{
    char *endptr;
    int idx = (int)strtol(arg, &endptr, 10);
    if (*endptr != ':') { ble_printf("usage: LW:idx:HH..\r\n"); return; }
    const char *hex = endptr + 1;
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

static void cmd_fw(const char *arg)
{
    char *endptr;
    int offset = (int)strtol(arg, &endptr, 10);
    if (*endptr != ':') { ble_printf("usage: FW:offset:HH..\r\n"); return; }
    const char *hex = endptr + 1;
    int nbytes = (int)strlen(hex) / 2;
    if (offset < 0 || offset + nbytes > FB_SIZE) {
        ble_printf("FW: OOB %d+%d\r\n", offset, nbytes); return;
    }
    uint8_t *buf = fb_bw();
    for (int i = 0; i < nbytes; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) { ble_printf("FW: bad hex\r\n"); return; }
        buf[offset + i] = (uint8_t)b;
    }
}

static void cmd_rw(const char *arg)
{
    char *endptr;
    int offset = (int)strtol(arg, &endptr, 10);
    if (*endptr != ':') { ble_printf("usage: RW:offset:HH..\r\n"); return; }
    const char *hex = endptr + 1;
    int nbytes = (int)strlen(hex) / 2;
    if (offset < 0 || offset + nbytes > FB_SIZE) {
        ble_printf("RW: OOB %d+%d\r\n", offset, nbytes); return;
    }
    uint8_t *buf = fb_red();
    for (int i = 0; i < nbytes; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) { ble_printf("RW: bad hex\r\n"); return; }
        buf[offset + i] = (uint8_t)b;
    }
}

static void process_line(const char *line)
{
    LOG_INF("cmd: %s", line);

    if (strncmp(line, "LUTW:", 5) == 0) { cmd_lutw(line + 5); return; }
    if (strncmp(line, "LW:",   3) == 0) { cmd_lw(line + 3);   return; }
    if (strncmp(line, "L:",    2) == 0) { cmd_l(line + 2);    return; }
    if (strncmp(line, "FW:",   3) == 0) { cmd_fw(line + 3);   return; }
    if (strncmp(line, "RW:",   3) == 0) { cmd_rw(line + 3);   return; }

    if (strcmp(line, "APPLY") == 0) {
        display_manager_enable_screensaver(false);
        pending_dcmd = DCMD_APPLY;
        k_sem_give(&display_sem);
        ble_printf("queued APPLY\r\n");
        return;
    }
    if (strcmp(line, "FAPPLY") == 0) {
        display_manager_enable_screensaver(false);
        pending_dcmd = DCMD_FAPPLY;
        k_sem_give(&display_sem);
        ble_printf("queued FAPPLY\r\n");
        return;
    }
    if (strcmp(line, "CLEAR") == 0) {
        pending_dcmd = DCMD_CLEAR;
        k_sem_give(&display_sem);
        ble_printf("queued CLEAR\r\n");
        return;
    }

    if (strncmp(line, "SS:", 3) == 0) {
        int en = atoi(line + 3);
        display_manager_enable_screensaver(en != 0);
        ble_printf("screensaver %s\r\n", en ? "ON" : "OFF");
        return;
    }

    if (strncmp(line, "VCOM=", 5) == 0) {
        uint8_t v = (uint8_t)strtoul(line + 5, NULL, 16);
        ssd1675a_set_vcom_register(v);
        ble_printf("VCOM=0x%02X\r\n", v);
        return;
    }

    if (strncmp(line, "TIME=", 5) == 0) {
        int h = 0, m = 0, s = 0;
        sscanf(line + 5, "%d:%d:%d", &h, &m, &s);
        struct tm t = {0};
        get_system_time(&t);
        set_system_time(h, m, s, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        ble_printf("TIME set %02d:%02d:%02d\r\n", h, m, s);
        return;
    }

    if (strcmp(line, "HELP") == 0) {
        ble_printf(
            "LUTW:HH..   write full LUT (140 hex)\r\n"
            "LW:i:HH..   write N LUT bytes from i\r\n"
            "L:n=HH      set one LUT byte\r\n"
            "L:DUMP/RESET\r\n"
            "FW:i:HH..   write N BW frame bytes at i\r\n"
            "RW:i:HH..   write N Red frame bytes at i\r\n"
            "APPLY       test image + LUT refresh\r\n"
            "FAPPLY      push BW+Red frame buffers\r\n"
            "CLEAR       3-cycle clean + white\r\n"
            "SS:0/1      disable/enable screensaver\r\n"
            "VCOM=HH     set VCOM\r\n"
            "TIME=HH:MM:SS set time\r\n"
        );
        return;
    }

    ble_printf("unknown cmd\r\n");
}

/* ------------------------------------------------------------------ */
/* BLE callback                                                         */
/* ------------------------------------------------------------------ */

static void ble_rx(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            rx_buf[rx_len] = '\0';
            rx_len = 0;
            if (rx_buf[0] != '\0') process_line(rx_buf);
        } else if (rx_len < RX_BUF_SIZE - 1) {
            rx_buf[rx_len++] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point — mirrors root project main() exactly                    */
/* ------------------------------------------------------------------ */

int main(void)
{
    LOG_INF("LUT Tester starting");

    dk_leds_init();
    battery_init();
    system_time_init();

    graphics_init();
    graphics_set_rotation(1);
    display_manager_init();

    int err = ble_service_init(ble_rx);
    if (err) {
        LOG_ERR("BLE init failed: %d", err);
        return -1;
    }

    /* ROOT PROJECT PATTERN: first ssd1675a_init() from main() at priority 0,
     * after BLE init but before display_manager's screensaver_thread (priority 7)
     * wakes from its 2s sleep. This guarantees no BLE preemption during post-RST. */
    display_manager_update_status();

    LOG_INF("Ready — advertising as 'LUT-Tester'");

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
