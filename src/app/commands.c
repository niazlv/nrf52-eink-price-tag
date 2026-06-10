#include "commands.h"
#include "ble/ble_service.h"
#include "display_manager.h"
#include "display_screens.h"
#include "battery.h"
#include "system_time.h"
#include "lib/graphics.h"
#include <drivers/ssd1675a.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/reboot.h>

typedef void (*cmd_handler_t)(char *args);
struct shell_cmd {
    const char *name;
    cmd_handler_t handler;
    const char *help;
};
extern const struct shell_cmd commands[];

/* ── Line accumulator ───────────────────────────────────────────────────────
 * BLE NUS delivers raw chunks; we buffer until '\n' to guarantee whole commands
 * even when LUTW (148+ chars) spans multiple MTU packets.
 */
#define RX_BUF_SIZE 320
static char rx_buf[RX_BUF_SIZE];
static int  rx_len;

/* ── Frame buffer ────────────────────────────────────────────────────────── */
#define FB_SIZE 4736   /* 128 × 296 / 8 */

static uint8_t *fb_bw(void)  { return (uint8_t *)graphics_get_buffer(); }
static uint8_t *fb_red(void) { return (uint8_t *)graphics_get_red_buffer(); }

/* ── Binary vstream (animation streaming over BLE) ──────────────────────────
 *
 * Wire protocol:
 *   Start : text command  VSTREAM:start   → device replies VSTREAM:ready
 *   Frame : [0xAA][0x55][type][len_hi][len_lo][payload*len][0xBB]
 *     0x00 RAW  — 4736 bytes uncompressed 1-bpp B&W
 *     0x01 RLE  — PackBits full frame
 *     0x02 DRLE — PackBits XOR-delta (current FB = prev frame, XOR in-place)
 *   ACK   : "TELE:vs f=N ms=M\r\n" after every flush (host flow-control)
 *   Stop  : [0xCC][0xDD] binary escape, or VSTREAM:stop text before streaming
 *
 * PackBits:  ctrl bit7=1 → next byte × (ctrl&0x7F)+1 times (run)
 *            ctrl bit7=0 → next (ctrl+1) bytes literal
 */
#define VS_HDR1  0xAAu
#define VS_HDR2  0x55u
#define VS_FLUSH 0xBBu
#define VS_STOP1 0xCCu
#define VS_STOP2 0xDDu

#define VS_TYPE_RAW  0x00u
#define VS_TYPE_RLE  0x01u
#define VS_TYPE_DRLE 0x02u

typedef enum {
    VS_IDLE, VS_HDR2_WAIT, VS_TYPE_RD, VS_LEN_HI, VS_LEN_LO,
    VS_DATA, VS_FLUSH_WAIT, VS_STOP2_WAIT
} vs_state_t;

typedef struct {
    int8_t  mode;   /* 0=ctrl  1=literal  2=run_first_byte  3=run_repeat */
    uint8_t count;
    uint8_t val;
} rle_dec_t;

static vs_state_t vs_state;
static uint8_t    vs_type;
static uint16_t   vs_plen;   /* compressed payload length */
static uint16_t   vs_prx;    /* compressed bytes received */
static uint16_t   vs_dec;    /* uncompressed bytes written to FB */
static rle_dec_t  vs_rle;
static bool       vstream_active;
static int        vs_frame_count;

/* Watchdog: if no frame flush arrives within this window, abort the stream
 * and restore the screensaver so the device doesn't stay frozen. */
#define VS_WATCHDOG_MS 30000
static struct k_work_delayable vs_watchdog_work;

static void vs_exit_streaming(void) {
    /* Wait for any in-progress pipelined display refresh to finish before
     * powering down HV rails. Safe to call even when no refresh is active. */
    ssd1675a_wait_busy();
    k_work_cancel_delayable(&vs_watchdog_work);
    if (vstream_active) {
        vs_state       = VS_IDLE;
        vstream_active = false;
        vs_frame_count = 0;
        display_manager_set_keep_on(false);
    }
    display_manager_enable_screensaver(true);
}

static void vs_watchdog_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (vstream_active) {
        ble_printf("VSTREAM:timeout\r\n");
        vs_exit_streaming();
    }
}

/* Track bytes written via FW:/RW: since last clear — reported in FAPPLY. */
static int fw_written;
static int rw_written;

/* When host_mode=true the device sends machine-readable TELE: lines after
 * every display update so the host app can track actual frame timing. */
static bool host_mode = false;

/* ── Hex helpers ─────────────────────────────────────────────────────────── */

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

/* ── Vstream helpers ─────────────────────────────────────────────────────── */

static void vs_reset_frame(void) {
    vs_prx = 0;
    vs_dec = 0;
    vs_rle.mode = 0;
    vs_rle.count = 0;
    vs_rle.val = 0;
}

static inline void vs_write_byte(uint8_t b) {
    if (vs_dec >= FB_SIZE) return;
    uint8_t *fb = fb_bw();
    fb[vs_dec] = (vs_type == VS_TYPE_DRLE) ? (fb[vs_dec] ^ b) : b;
    vs_dec++;
}

static void vs_rle_feed(uint8_t b) {
    /* Drain pending run bytes first — they don't consume compressed input.
     * Without this, each run-continuation output would eat one real input
     * byte (the next instruction), corrupting the entire stream. */
    while (vs_rle.mode == 3) {
        vs_write_byte(vs_rle.val);
        if (--vs_rle.count == 0) { vs_rle.mode = 0; break; }
    }
    /* Now consume the actual compressed byte. */
    switch (vs_rle.mode) {
    case 0:
        if (b & 0x80) { vs_rle.count = (b & 0x7F) + 1; vs_rle.mode = 2; }
        else          { vs_rle.count = b + 1;            vs_rle.mode = 1; }
        break;
    case 1:
        vs_write_byte(b);
        if (--vs_rle.count == 0) vs_rle.mode = 0;
        break;
    case 2:
        vs_rle.val = b;
        vs_write_byte(b);
        if (--vs_rle.count == 0) vs_rle.mode = 0;
        else vs_rle.mode = 3;
        break;
    }
}

static void vs_flush_frame(void) {
    /* Flush any trailing run bytes that arrived at end of compressed stream. */
    if (vs_type != VS_TYPE_RAW) {
        while (vs_rle.mode == 3 && vs_dec < FB_SIZE) {
            vs_write_byte(vs_rle.val);
            if (--vs_rle.count == 0) { vs_rle.mode = 0; break; }
        }
    }

    /* XOR checksum of the decoded framebuffer for host-side integrity check. */
    const uint8_t *fb = fb_bw();
    uint8_t crc = 0;
    for (int i = 0; i < FB_SIZE; i++) crc ^= fb[i];

    /* Pipeline: wait for the PREVIOUS frame's display refresh to finish before
     * writing new SPI data. The previous trigger was sent without blocking, so
     * the display has been refreshing in the background while we received this
     * frame over BLE. ms includes this wait so host sees real display timing. */
    int64_t t0 = k_uptime_get();
    ssd1675a_wait_busy();
    display_manager_set_keep_on(true);
    display_manager_update_partial_nowait();
    int32_t ms = (int32_t)(k_uptime_get() - t0);

    vs_frame_count++;
    /* Reset watchdog — host is alive and sending frames. */
    k_work_reschedule(&vs_watchdog_work, K_MSEC(VS_WATCHDOG_MS));
    /* ACK sent immediately after SPI+trigger — display refreshes in background
     * while host encodes and sends the next frame over BLE. */
    ble_printf("TELE:vs f=%d ms=%d dec=%d crc=%02x\r\n",
               vs_frame_count, (int)ms, (int)vs_dec, (unsigned)crc);
}

static void vs_process_byte(uint8_t b) {
    switch (vs_state) {
    case VS_IDLE:
        if      (b == VS_HDR1)  vs_state = VS_HDR2_WAIT;
        else if (b == VS_STOP1) vs_state = VS_STOP2_WAIT;
        break;
    case VS_HDR2_WAIT:
        vs_state = (b == VS_HDR2) ? VS_TYPE_RD : VS_IDLE;
        break;
    case VS_TYPE_RD:
        vs_type  = b;
        vs_state = VS_LEN_HI;
        break;
    case VS_LEN_HI:
        vs_plen  = (uint16_t)b << 8;
        vs_state = VS_LEN_LO;
        break;
    case VS_LEN_LO:
        vs_plen |= b;
        vs_reset_frame();
        vs_state = (vs_plen > 0) ? VS_DATA : VS_FLUSH_WAIT;
        break;
    case VS_DATA:
        vs_prx++;
        if (vs_type == VS_TYPE_RAW) vs_write_byte(b);
        else                        vs_rle_feed(b);
        if (vs_prx >= vs_plen) vs_state = VS_FLUSH_WAIT;
        break;
    case VS_FLUSH_WAIT:
        if (b == VS_FLUSH) vs_flush_frame();  /* flush_frame handles pending run drain */
        vs_state = VS_IDLE;
        break;
    case VS_STOP2_WAIT:
        if (b == VS_STOP2) {
            vs_exit_streaming();
            ble_printf("VSTREAM:stopped\r\n");
        }
        vs_state = VS_IDLE;
        break;
    }
}

/* ── Standard commands ───────────────────────────────────────────────────── */

void cmd_help(char *args)
{
    ble_printf("cmds:\r\n");
    for (int i = 0; commands[i].name != NULL; i++) {
        ble_printf("  %s — %s\r\n", commands[i].name, commands[i].help);
    }
}

void cmd_cls(char *args)
{
    display_manager_enable_screensaver(false);
    display_manager_clear();
    ble_printf("cleared\r\n");
}

void cmd_clean(char *args)
{
    display_manager_enable_screensaver(false);
    ble_printf("cleaning (7 cycles)...\r\n");
    display_manager_clean();
    ble_printf("done\r\n");
}

void cmd_nuke(char *args)
{
    int n = (args && *args) ? atoi(args) : 20;
    if (n < 5)  n = 5;
    if (n > 50) n = 50;
    display_manager_enable_screensaver(false);
    // Each cycle: 3 full updates (B/W/R) × ~15s each = ~45s/cycle
    ble_printf("NUKE: %d cycles (~%d min) — clearing ghost...\r\n",
               n, (n * 3 * 15 + 30) / 60);
    display_manager_deep_clean(n);
    ble_printf("NUKE: done\r\n");
}

void cmd_saver(char *args)
{
    display_manager_enable_screensaver(true);
    ble_printf("saver enabled\r\n");
}

void cmd_update(char *args)
{
    if (!host_mode) ble_printf("updating...\r\n");
    int64_t t0 = k_uptime_get();
    display_manager_force_update();
    int32_t elapsed = (int32_t)(k_uptime_get() - t0);
    if (host_mode) {
        ble_printf("TELE:full time=%dms lut=%s\r\n",
                   (int)elapsed, ssd1675a_get_use_custom_lut() ? "custom" : "builtin");
    } else {
        ble_printf("done %dms\r\n", (int)elapsed);
    }
}

void cmd_fast(char *args)
{
    if (!host_mode) ble_printf("fast update...\r\n");
    int64_t t0 = k_uptime_get();
    display_manager_update_partial();
    int32_t elapsed = (int32_t)(k_uptime_get() - t0);
    if (host_mode) {
        ble_printf("TELE:fast time=%dms lut=%s\r\n",
                   (int)elapsed, ssd1675a_get_use_custom_lut() ? "custom" : "builtin");
    } else {
        ble_printf("done %dms\r\n", (int)elapsed);
    }
}

void cmd_test(char *args)
{
    ble_printf("Starting Partial Stress Test (Infinite)... Reset to stop.\r\n");
    display_manager_enable_screensaver(false);

    int32_t count = 0;
    bt_addr_le_t addr = {0};
    size_t count_id = 1;
    char addr_str[BT_ADDR_LE_STR_LEN] = "Unknown";

    bt_id_get(&addr, &count_id);
    bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));

    display_screens_render_text("INITIAL CLEAN...\nPlease Wait");
    display_manager_force_update();

    int64_t last_time = k_uptime_get();

    while (1) {
        int64_t start = k_uptime_get();
        int32_t delta = (int32_t)(start - last_time);
        last_time = start;

        display_screens_render_partial_test(count++, start, delta, addr_str);
        display_manager_update_partial();
        k_msleep(1);
    }
}

void cmd_mode(char *args)
{
    if (!args || !*args) { ble_printf("usage: MODE: 0-3\r\n"); return; }
    int m = atoi(args);
    display_manager_set_partial_mode(m);
    ble_printf("Mode Set: %d (0=Turbo,1=Balanced,2=Stable,3=Clean)\r\n", m);
}

void cmd_text(char *args)
{
    if (!args || !*args) return;
    display_manager_enable_screensaver(false);
    display_manager_show_text(args);
    ble_printf("drawn\r\n");
}

void cmd_rot(char *args)
{
    if (!args || !*args) { ble_printf("usage: ROT: 0-3\r\n"); return; }
    int r = atoi(args);
    display_manager_set_rotation(r);
    ble_printf("rotation: %d\r\n", r);
}

void cmd_batt(char *args)
{
    int mv = battery_read_mv();
    ble_printf("bat: %d mv\r\n", mv);
}

/* TIME HH:MM:SS DD.MM.YYYY */
void cmd_time(char *args)
{
    if (!args || strlen(args) < 10) {
        ble_printf("usage: TIME HH:MM:SS DD.MM.YYYY\r\n");
        return;
    }
    int h, m, s, D, M, Y;
    if (sscanf(args, "%d:%d:%d %d.%d.%d", &h, &m, &s, &D, &M, &Y) == 6) {
        set_system_time(h, m, s, D, M, Y);
        display_manager_force_update();
        ble_printf("Time Set\r\n");
    } else {
        ble_printf("parse error\r\n");
    }
}

/* TIME=HH:MM:SS — lut_tester_host compatible; preserves current date */
void cmd_time_eq(char *args)
{
    if (!args || !*args) { ble_printf("usage: TIME=HH:MM:SS\r\n"); return; }
    int h = 0, m = 0, s = 0;
    sscanf(args, "%d:%d:%d", &h, &m, &s);
    struct tm t = {0};
    get_system_time(&t);
    set_system_time(h, m, s, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    ble_printf("TIME set %02d:%02d:%02d\r\n", h, m, s);
}

void cmd_dsaver(char *args)
{
    if (!args || !*args) {
        ble_printf("usage: DSAVER 0/1\r\n");
        return;
    }
    int mode = atoi(args);
    display_manager_enable_screensaver(true);
    display_manager_set_screensaver_mode(
        mode ? SCREENSAVER_MODE_DYNAMIC : SCREENSAVER_MODE_STATIC);
    ble_printf("Dynamic Saver: %s\r\n", mode ? "ON" : "OFF");
}

void cmd_anim(char *args)
{
    ble_printf("Starting Animation (Reset to stop)...\r\n");
    display_manager_enable_screensaver(false);
    display_manager_set_keep_on(true);

    int x = 10, y = 10, vx = 4, vy = 4, size = 20, frame = 0;
    int width  = graphics_get_width();
    int height = graphics_get_height();

    graphics_clear(GFX_WHITE);
    display_manager_force_update();

    // Charge ±15V rails once — one-time ~600ms cost.
    // Subsequent frames use update_frame_stream() which skips HV cycling.
    display_manager_begin_streaming();

    int64_t last_ms = k_uptime_get();

    while (1) {
        x += vx;  y += vy;
        if (x <= 0 || x + size >= width)  vx = -vx;
        if (y <= 0 || y + size >= height) vy = -vy;
        x = (x < 0) ? 0 : (x + size > width  ? width  - size : x);
        y = (y < 0) ? 0 : (y + size > height ? height - size : y);

        int64_t now = k_uptime_get();
        int32_t delta = (int32_t)(now - last_ms);
        last_ms = now;

        display_screens_render_animation_frame(x, y, size, frame++, delta);
        display_manager_update_frame_stream();  // ~64ms: LUT wave only, no HV cycle

        if (frame % 50 == 0) ble_printf("Anim #%d  %dms/frame\r\n", frame, (int)delta);
    }
    // Note: end_streaming() unreachable in infinite loop; HV discharges on reset.
}

void cmd_debug_vcom(char *args)
{
    if (!args || !*args) return;
    uint32_t val = strtoul(args, NULL, 16);
    ssd1675a_set_vcom_register((uint8_t)val);
    ble_printf("VCOM=0x%02X\r\n", (uint8_t)val);
}

void cmd_debug_lut(char *args)
{
    if (!args || !*args) return;
    char *colon = strchr(args, ':');
    if (colon) {
        *colon = '\0';
        int idx = atoi(args);
        uint32_t val = strtoul(colon + 1, NULL, 16);
        ssd1675a_set_lut_byte(idx, (uint8_t)val);
        ble_printf("LUT[%d]=0x%02X\r\n", idx, (uint8_t)val);
    }
}

/* ── LUT tester commands (lut_tester_host compatible) ───────────────────── */

/* LUTW:HH..  — write all 70 LUT bytes at once */
void cmd_lutw(char *args)
{
    int expected = SSD1675A_LUT_SIZE * 2;
    int got = (int)strlen(args);
    if (got != expected) {
        ble_printf("LUTW: need %d hex, got %d\r\n", expected, got);
        return;
    }
    for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
        int b = hex_byte(args + i * 2);
        if (b < 0) { ble_printf("LUTW: bad hex at %d\r\n", i * 2); return; }
        ssd1675a_set_lut_byte(i, (uint8_t)b);
    }
    ssd1675a_set_use_custom_lut(true);
    ble_printf("LUT written (%d bytes) — custom LUT active\r\n", SSD1675A_LUT_SIZE);
}

/* LW:idx:HH..  — write N bytes starting at idx */
void cmd_lw(char *args)
{
    char *endptr;
    int idx = (int)strtol(args, &endptr, 10);
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
    ssd1675a_set_use_custom_lut(true);
    ble_printf("LW: wrote %d bytes from [%d] — custom LUT active\r\n", nbytes, idx);
}

/* L:n=HH | L:DUMP | L:RESET */
void cmd_l_byte(char *args)
{
    if (strcmp(args, "DUMP") == 0) {
        ble_printf("LUT[70]:\r\n");
        for (int i = 0; i < SSD1675A_LUT_SIZE; i++) {
            ble_printf("%02X%s", ssd1675a_get_lut_byte(i),
                (i % 10 == 9 || i == SSD1675A_LUT_SIZE - 1) ? "\r\n" : " ");
        }
        return;
    }
    if (strcmp(args, "RESET") == 0) {
        ssd1675a_reset_lut();
        ble_printf("LUT reset\r\n");
        return;
    }
    char tmp[32];
    strncpy(tmp, args, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *eq = strchr(tmp, '=');
    if (!eq) { ble_printf("usage: L:n=HH\r\n"); return; }
    *eq = '\0';
    int idx = atoi(tmp);
    uint8_t val = (uint8_t)strtoul(eq + 1, NULL, 16);
    ssd1675a_set_lut_byte(idx, val);
    ssd1675a_set_use_custom_lut(true);
    ble_printf("L[%d]=0x%02X — custom LUT active\r\n", idx, val);
}

/* FW:offset:HH..  — write BW frame bytes */
void cmd_fw(char *args)
{
    char *endptr;
    int offset = (int)strtol(args, &endptr, 10);
    if (*endptr != ':') { ble_printf("FW:err no colon\r\n"); return; }
    const char *hex = endptr + 1;
    int hexlen = (int)strlen(hex);
    if (hexlen & 1) { ble_printf("FW:err odd len %d\r\n", hexlen); return; }
    int nbytes = hexlen / 2;
    if (offset < 0 || offset + nbytes > FB_SIZE) {
        ble_printf("FW:OOB off=%d n=%d\r\n", offset, nbytes); return;
    }
    uint8_t *buf = fb_bw();
    for (int i = 0; i < nbytes; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) {
            ble_printf("FW:badchar off=%d pos=%d c=%02X\r\n",
                       offset, i, (uint8_t)hex[i * 2]);
            return;
        }
        buf[offset + i] = (uint8_t)b;
    }
    fw_written += nbytes;
}

/* RW:offset:HH..  — write Red frame bytes */
void cmd_rw(char *args)
{
    char *endptr;
    int offset = (int)strtol(args, &endptr, 10);
    if (*endptr != ':') { ble_printf("RW:err no colon\r\n"); return; }
    const char *hex = endptr + 1;
    int hexlen = (int)strlen(hex);
    if (hexlen & 1) { ble_printf("RW:err odd len %d\r\n", hexlen); return; }
    int nbytes = hexlen / 2;
    if (offset < 0 || offset + nbytes > FB_SIZE) {
        ble_printf("RW:OOB off=%d n=%d\r\n", offset, nbytes); return;
    }
    uint8_t *buf = fb_red();
    for (int i = 0; i < nbytes; i++) {
        int b = hex_byte(hex + i * 2);
        if (b < 0) {
            ble_printf("RW:badchar off=%d pos=%d c=%02X\r\n",
                       offset, i, (uint8_t)hex[i * 2]);
            return;
        }
        buf[offset + i] = (uint8_t)b;
    }
    rw_written += nbytes;
}

/* FAPPLY — push FW/RW-written frame buffers to display.
 * Screensaver is disabled first; a short sleep lets its thread finish
 * the current render cycle before we push our frame. */
void cmd_fapply(char *args)
{
    display_manager_enable_screensaver(false);
    k_msleep(150);  /* wait for screensaver thread to finish current cycle */
    int bw = fw_written, rw = rw_written;
    fw_written = 0;
    rw_written = 0;
    if (!host_mode) ble_printf("FAPPLY bw=%d rw=%d...\r\n", bw, rw);
    int64_t t0 = k_uptime_get();
    display_manager_force_update();
    int32_t elapsed = (int32_t)(k_uptime_get() - t0);
    if (host_mode) {
        ble_printf("TELE:fapply time=%dms bw=%d rw=%d lut=%s\r\n",
                   (int)elapsed, bw, rw,
                   ssd1675a_get_use_custom_lut() ? "custom" : "builtin");
    } else {
        ble_printf("FAPPLY done %dms\r\n", (int)elapsed);
    }
}

/* SS:0/1 — screensaver on/off (lut_tester_host compatible) */
void cmd_ss(char *args)
{
    if (!args || !*args) { ble_printf("usage: SS:0/1\r\n"); return; }
    int en = atoi(args);
    display_manager_enable_screensaver(en != 0);
    ble_printf("screensaver %s\r\n", en ? "ON" : "OFF");
}

/* VCOM=HH — set VCOM register */
void cmd_vcom(char *args)
{
    if (!args || !*args) { ble_printf("usage: VCOM=HH\r\n"); return; }
    uint8_t v = (uint8_t)strtoul(args, NULL, 16);
    ssd1675a_set_vcom_register(v);
    ble_printf("VCOM=0x%02X\r\n", v);
}

/* HOST:0/1 — switch device into machine-readable telemetry mode */
void cmd_host(char *args)
{
    if (!args || !*args) {
        ble_printf("HOST:%s\r\n", host_mode ? "1" : "0");
        return;
    }
    host_mode = (atoi(args) != 0);
    display_manager_set_tele_enabled(host_mode);
    /* Always respond in human-readable format to confirm the switch */
    ble_printf("HOST:%s\r\n", host_mode ? "1" : "0");
}

/* STAT — snapshot of current LUT test timing stats + device state */
void cmd_stat(char *args)
{
    int32_t frame, cur_ms, min_ms, max_ms;
    display_manager_get_lut_test_stats(&frame, &cur_ms, &min_ms, &max_ms);
    ble_printf("STAT:lut=%s host=%s frame=%d last=%d min=%d max=%d\r\n",
               ssd1675a_get_use_custom_lut() ? "custom" : "builtin",
               host_mode ? "1" : "0",
               (int)frame, (int)cur_ms, (int)min_ms, (int)max_ms);
}

/* LUTUSE:0/1 — force-toggle whether lut_data[] is used for partial updates */
void cmd_lutuse(char *args)
{
    if (!args || !*args) {
        ble_printf("LUTUSE:%s\r\n",
                   ssd1675a_get_use_custom_lut() ? "1 (custom)" : "0 (builtin)");
        return;
    }
    int en = atoi(args);
    ssd1675a_set_use_custom_lut(en != 0);
    ble_printf("LUTUSE:%s\r\n", en ? "1 (custom LUT active)" : "0 (builtin LUTs)");
}

/* LGET — dump current lut_data[] back to host as 7 lines of 10 bytes each.
 * Format: "LUT:N:XXXXXXXXXXXXXXXXXXXX\r\n"  (28 chars per line).
 * 28 chars fits within even the smallest BLE ATT MTU (23 bytes would fail,
 * but MTU is negotiated higher in practice; and with CONFIG_BT_L2CAP_TX_MTU=247
 * it is guaranteed). We bypass ble_printf() to avoid its 128-byte snprintf cap. */
void cmd_lget(char *args)
{
    static const char hx[] = "0123456789ABCDEF";
    char ln[32];

    for (int chunk = 0; chunk < 7; chunk++) {
        ln[0] = 'L'; ln[1] = 'U'; ln[2] = 'T'; ln[3] = ':';
        ln[4] = '0' + chunk;
        ln[5] = ':';
        for (int j = 0; j < 10; j++) {
            uint8_t b = ssd1675a_get_lut_byte(chunk * 10 + j);
            ln[6 + j * 2]     = hx[b >> 4];
            ln[6 + j * 2 + 1] = hx[b & 0xF];
        }
        ln[26] = '\r'; ln[27] = '\n';
        ble_service_send(ln, 28);
    }
}

/* LTEST / LTEST 0 — start/stop LUT test animation mode */
void cmd_ltest(char *args)
{
    if (args && *args == '0') {
        display_manager_set_screensaver_mode(SCREENSAVER_MODE_STATIC);
        display_manager_enable_screensaver(true);
        ble_printf("LUT test stopped\r\n");
    } else {
        display_manager_enable_screensaver(true);
        display_manager_set_screensaver_mode(SCREENSAVER_MODE_LUT_TEST);
        ble_printf("LUT test started (LTEST 0 to stop)\r\n");
    }
}

/* LUTSET:<name|n> — select built-in preset by name/index and disable custom LUT.
 * Accepted: TURBO/0, BALANCED/1, STABLE/2, CLEAN/3 (case-insensitive). */
void cmd_lutset(char *args)
{
    if (!args || !*args) {
        ble_printf("LUTSET: usage: LUTSET:TURBO|BALANCED|STABLE|CLEAN (or 0-3)\r\n");
        return;
    }
    int mode = -1;
    if      (args[0] == '0' || strncasecmp(args, "TURBO",    5) == 0) mode = 0;
    else if (args[0] == '1' || strncasecmp(args, "BALANCED", 8) == 0) mode = 1;
    else if (args[0] == '2' || strncasecmp(args, "STABLE",   6) == 0) mode = 2;
    else if (args[0] == '3' || strncasecmp(args, "CLEAN",    5) == 0) mode = 3;
    else { ble_printf("LUTSET: unknown preset '%s'\r\n", args); return; }

    ssd1675a_set_use_custom_lut(false);
    display_manager_set_partial_mode(mode);
    static const char *names[] = {"TURBO", "BALANCED", "STABLE", "CLEAN"};
    ble_printf("LUTSET:%s (mode=%d, custom=off)\r\n", names[mode], mode);
}

/* VSTREAM:start|stop — enter/exit binary animation streaming mode */
void cmd_vstream(char *args)
{
    if (!args || !*args) {
        ble_printf("VSTREAM:usage start|stop\r\n");
        return;
    }
    if (args[0] == '0' || strncmp(args, "stop", 4) == 0) {
        vs_exit_streaming();
        ble_printf("VSTREAM:stopped\r\n");
        return;
    }
    if (args[0] == '1' || strncmp(args, "start", 5) == 0) {
        display_manager_enable_screensaver(false);
        k_msleep(50);  /* let screensaver thread finish any in-progress update */
        graphics_clear(GFX_WHITE);  /* ensure FB starts white before first RLE frame */
        display_manager_set_keep_on(true);
        display_manager_set_partial_mode(0);  /* TURBO for max fps */
        vs_state       = VS_IDLE;
        vs_frame_count = 0;
        vs_reset_frame();
        vstream_active = true;
        k_work_reschedule(&vs_watchdog_work, K_MSEC(VS_WATCHDOG_MS));
        ble_printf("VSTREAM:ready type=RAW/RLE/DRLE fmt=AA55 tt LL LL [payload] BB stop=CCDD\r\n");
        return;
    }
    ble_printf("VSTREAM:unknown\r\n");
}

static void cmd_reboot(char *args)
{
    (void)args;
    ble_printf("REBOOT\r\n");
    k_sleep(K_MSEC(100));   /* flush BLE TX before reset */
    sys_reboot(SYS_REBOOT_COLD);
}

/* ── Command table ───────────────────────────────────────────────────────── */

const struct shell_cmd commands[] = {
    /* General */
    {"HELP",        cmd_help,       "List commands"},
    {"BATT",        cmd_batt,       "Read battery mV"},
    {"TIME",        cmd_time,       "Set time: HH:MM:SS DD.MM.YYYY"},
    {"TIME=",       cmd_time_eq,    "Set time: TIME=HH:MM:SS (host compat)"},
    /* Display */
    {"SAVER",       cmd_saver,      "Enable screensaver"},
    {"SS:",         cmd_ss,         "Screensaver: SS:0/1"},
    {"DSAVER",      cmd_dsaver,     "Dynamic saver: DSAVER 0/1"},
    {"CLEAR",       cmd_cls,        "Clear display buffer"},
    {"CLEAN",       cmd_clean,      "Run clean cycle (7×B/W/R)"},
    {"NUKE:",       cmd_nuke,       "Deep ghost clear: NUKE:20 (default 20 cycles, ~15min)"},
    {"UPDATE",      cmd_update,     "Full display refresh"},
    {"APPLY",       cmd_update,     "Full refresh (host compat alias for UPDATE)"},
    {"FAST",        cmd_fast,       "Fast/partial update"},
    {"FAPPLY",      cmd_fapply,     "Push FW/RW frame buffers to display"},
    {"MODE:",       cmd_mode,       "Partial mode: 0=Turbo 1=Balanced 2=Stable 3=Clean"},
    {"TEXT:",       cmd_text,       "Draw text on display"},
    {"ROT:",        cmd_rot,        "Set rotation 0-3"},
    {"ANIM",        cmd_anim,       "Run bouncing-ball animation"},
    {"TEST",        cmd_test,       "Infinite partial stress test"},
    {"VSTREAM:",    cmd_vstream,    "Binary anim stream: VSTREAM:start|stop  (then binary frames)"},
    /* LUT editor */
    {"LUTW:",       cmd_lutw,       "Write full LUT: LUTW:HH..HH (140 hex)"},
    {"LW:",         cmd_lw,         "Write N LUT bytes: LW:idx:HH.."},
    {"L:",          cmd_l_byte,     "LUT byte: L:n=HH / L:DUMP / L:RESET"},
    {"LUTUSE:",     cmd_lutuse,     "Custom LUT toggle: LUTUSE:0/1"},
    {"LUTSET:",     cmd_lutset,     "Select built-in preset: LUTSET:TURBO|BALANCED|STABLE|CLEAN"},
    {"LGET",        cmd_lget,       "Dump current LUT: replies LUT:0: + LUT:1: lines"},
    {"LTEST",       cmd_ltest,      "LUT test animation: LTEST / LTEST 0"},
    {"HOST:",       cmd_host,       "Machine mode: HOST:1 (TELE: replies) HOST:0"},
    {"STAT",        cmd_stat,       "Telemetry snapshot: frame/last/min/max ms"},
    /* Frame buffers */
    {"FW:",         cmd_fw,         "Write BW frame: FW:offset:HH.."},
    {"RW:",         cmd_rw,         "Write Red frame: RW:offset:HH.."},
    /* System */
    {"REBOOT",      cmd_reboot,     "Cold reboot the device"},
    /* Debug */
    {"VCOM=",       cmd_vcom,       "Set VCOM: VCOM=HH"},
    {"DEBUG:VCOM=", cmd_debug_vcom, "Set VCOM (legacy)"},
    {"DEBUG:LUT=",  cmd_debug_lut,  "Set LUT byte: DEBUG:LUT=idx:HH"},
    {NULL, NULL, NULL}
};

/* ── Dispatch a single complete line ─────────────────────────────────────── */

static void dispatch_line(const char *line)
{
    for (int i = 0; commands[i].name != NULL; i++) {
        const char *cmd = commands[i].name;
        int cmd_len = (int)strlen(cmd);

        if (strncmp(line, cmd, cmd_len) != 0) continue;

        char *args = (char *)(line + cmd_len);
        char last  = cmd[cmd_len - 1];

        /* Commands ending with ':' or '=' consume everything after the separator.
         * Others require args to be absent or space-separated. */
        if (last != ':' && last != '=' && *args != '\0' && *args != ' ') continue;

        while (*args == ' ') args++;
        commands[i].handler(args);
        return;
    }
    ble_printf("unknown cmd\r\n");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void commands_init(void) {
    k_work_init_delayable(&vs_watchdog_work, vs_watchdog_handler);
}

void commands_on_disconnect(void) {
    vs_exit_streaming();
}

void commands_process(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        if (vstream_active) {
            vs_process_byte(p[i]);
            continue;
        }
        char c = (char)p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            rx_buf[rx_len] = '\0';
            rx_len = 0;
            if (rx_buf[0] != '\0') dispatch_line(rx_buf);
        } else if (rx_len < RX_BUF_SIZE - 1) {
            rx_buf[rx_len++] = c;
        }
    }
}
