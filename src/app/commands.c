#include "commands.h"
#include "ble/ble_service.h"
#include "display_manager.h"
#include "display_screens.h"
#include "battery.h"
#include "system_time.h"
#include "persist.h"
#include "lib/graphics.h"
#include <drivers/ssd1675a.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/reboot.h>
#include <app_version.h>

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
#define RX_BUF_SIZE 256
static char rx_buf[RX_BUF_SIZE];
static int  rx_len;

/* ── Frame buffer ────────────────────────────────────────────────────────── */
#define FB_SIZE 4736   /* 128 × 296 / 8 */

static uint8_t *fb_bw(void)  { return (uint8_t *)graphics_get_buffer(); }
static uint8_t *fb_red(void) { return (uint8_t *)graphics_get_red_buffer(); }

/* ── Binary vstream (animation streaming over BLE) ──────────────────────────
 *
 * Wire protocol:
 *   Start : text command  VSTREAM:start[:preset] → device replies VSTREAM:ready
 *   Frame : [0xAA][0x55][type][len_hi][len_lo][payload*len][0xBB]
 *     0x00 RAW  — 4736 bytes uncompressed 1-bpp B&W
 *     0x01 RLE  — PackBits full frame
 *     0x02 DRLE — PackBits XOR-delta (current FB = prev frame, XOR in-place)
 *     0x03 RAW2 — BW plane then red/control plane (9472 bytes decoded)
 *     0x04 RLE2 — PackBits over both planes
 *     0x05 DRLE2— PackBits XOR-delta over both planes
 *   ACK   : "TELE:vs f=N ms=M\r\n" after every flush (host flow-control)
 *   Stop  : [0xCC][0xDD] binary escape for video/animation streams.
 *           This restores the screensaver after shutting down HV/fast BLE.
 *   Photo : [0xCC][0xDE] binary escape for still-photo tone streams.
 *           This shuts down HV/fast BLE but keeps the rendered photo on screen
 *           by leaving the screensaver disabled.
 *
 * PackBits:  ctrl bit7=1 → next byte × (ctrl&0x7F)+1 times (run)
 *            ctrl bit7=0 → next (ctrl+1) bytes literal
 */
#define VS_HDR1  0xAAu
#define VS_HDR2  0x55u
#define VS_FLUSH 0xBBu
#define VS_STOP1       0xCCu  /* Common binary stop escape prefix. */
#define VS_VIDEO_STOP2 0xDDu  /* Video stop: restore saver after stream ends. */
#define VS_PHOTO_STOP2 0xDEu  /* Photo stop: keep saver off so the image stays. */

#define VS_TYPE_RAW  0x00u
#define VS_TYPE_RLE  0x01u
#define VS_TYPE_DRLE 0x02u
#define VS_TYPE_RAW2  0x03u
#define VS_TYPE_RLE2  0x04u
#define VS_TYPE_DRLE2 0x05u

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

static void vs_exit_streaming(bool restore_screensaver) {
    /* Wait for any in-progress pipelined display refresh to finish before
     * powering down HV rails. Safe to call even when no refresh is active. */
    ssd1675a_wait_busy();
    ble_service_set_streaming_mode(false);
    display_manager_set_stream_write_red_plane(false);
    k_work_cancel_delayable(&vs_watchdog_work);
    if (vstream_active) {
        vs_state       = VS_IDLE;
        vstream_active = false;
        vs_frame_count = 0;
        display_manager_set_keep_on(false);
    }
    /* Video/animation wants the device to resume its saver after stop.
     * Still-photo rendering wants a sleeping display with the final image left
     * alone, so that path passes false here. */
    if (restore_screensaver) {
        display_manager_enable_screensaver(true);
    }
}

static bool vs_type_is_dual(uint8_t type)
{
    return type == VS_TYPE_RAW2 || type == VS_TYPE_RLE2 || type == VS_TYPE_DRLE2;
}

static bool vs_type_is_raw(uint8_t type)
{
    return type == VS_TYPE_RAW || type == VS_TYPE_RAW2;
}

static bool vs_type_is_delta(uint8_t type)
{
    return type == VS_TYPE_DRLE || type == VS_TYPE_DRLE2;
}

static void vs_watchdog_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (vstream_active) {
        ble_printf("VSTREAM:timeout\r\n");
        vs_exit_streaming(true);
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
    const bool dual = vs_type_is_dual(vs_type);
    const uint16_t max_dec = dual ? (FB_SIZE * 2) : FB_SIZE;
    if (vs_dec >= max_dec) return;

    uint8_t *fb;
    uint16_t off = vs_dec;
    if (dual && off >= FB_SIZE) {
        fb = fb_red();
        off -= FB_SIZE;
    } else {
        fb = fb_bw();
    }

    fb[off] = vs_type_is_delta(vs_type) ? (fb[off] ^ b) : b;
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
    const bool dual = vs_type_is_dual(vs_type);
    const uint16_t max_dec = dual ? (FB_SIZE * 2) : FB_SIZE;

    /* Flush any trailing run bytes that arrived at end of compressed stream. */
    if (!vs_type_is_raw(vs_type)) {
        while (vs_rle.mode == 3 && vs_dec < max_dec) {
            vs_write_byte(vs_rle.val);
            if (--vs_rle.count == 0) { vs_rle.mode = 0; break; }
        }
    }

    /* XOR checksum of the decoded framebuffer for host-side integrity check. */
    const uint8_t *fb = fb_bw();
    uint8_t crc = 0;
    for (int i = 0; i < FB_SIZE; i++) crc ^= fb[i];
    if (dual) {
        const uint8_t *fr = fb_red();
        for (int i = 0; i < FB_SIZE; i++) crc ^= fr[i];
    }

    /* Pipeline: wait for the PREVIOUS frame's display refresh to finish before
     * writing new SPI data. The previous trigger was sent without blocking, so
     * the display has been refreshing in the background while we received this
     * frame over BLE. ms includes this wait so host sees real display timing. */
    int64_t t0 = k_uptime_get();
    ssd1675a_wait_busy();
    display_manager_set_keep_on(true);
    display_manager_set_stream_write_red_plane(dual);
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
        vs_state = (b <= VS_TYPE_DRLE2) ? VS_LEN_HI : VS_IDLE;
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
        if (vs_type_is_raw(vs_type)) vs_write_byte(b);
        else                         vs_rle_feed(b);
        if (vs_prx >= vs_plen) vs_state = VS_FLUSH_WAIT;
        break;
    case VS_FLUSH_WAIT:
        if (b == VS_FLUSH) vs_flush_frame();  /* flush_frame handles pending run drain */
        vs_state = VS_IDLE;
        break;
    case VS_STOP2_WAIT:
        if (b == VS_VIDEO_STOP2) {
            /* [CC DD] = video semantics: stop streaming and restore saver. */
            vs_exit_streaming(true);
            ble_printf("VSTREAM:stopped\r\n");
        } else if (b == VS_PHOTO_STOP2) {
            /* [CC DE] = still-photo semantics: stop streaming, power down
             * display/HV, but do not restart saver over the rendered photo. */
            vs_exit_streaming(false);
            ble_printf("VSTREAM:photo_stopped\r\n");
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
    if (!args || !*args) { ble_printf("usage: MODE: 0-7\r\n"); return; }
    int m = atoi(args);
    display_manager_set_partial_mode(m);
    ble_printf("Mode Set: %d (0=T,1=B,2=S,3=C,4=ToneDark,5=ToneLight,6=ToneBidirFast,7=ToneBidir)\r\n", m);
}

void cmd_text(char *args)
{
    if (!args || !*args) return;
    display_manager_enable_screensaver(false);
    display_manager_show_text(args);
    ble_printf("drawn\r\n");
}

void cmd_paltest(char *args)
{
    ARG_UNUSED(args);
    display_manager_enable_screensaver(false);
    ble_printf("PALTEST: rendering spatial B/W/R palette...\r\n");
    display_manager_show_palette_test();
    ble_printf("PALTEST: done\r\n");
}

void cmd_tonetest(char *args)
{
    ARG_UNUSED(args);
    display_manager_enable_screensaver(false);
    ble_printf("TONETEST: white base + 8 black-only pulse passes...\r\n");
    display_manager_run_tone_test();
    ble_printf("TONETEST: done (use CLEAN/NUKE if residual ghosting stays)\r\n");
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
    display_manager_force_update();
    ble_printf("TIME set %d:%s%d:%s%d\r\n",
               h, m < 10 ? "0" : "", m, s < 10 ? "0" : "", s);
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

static int parse_partial_mode_name(const char *name)
{
    if (!name || !*name) return -1;

    if      (name[0] == '0' || strncasecmp(name, "TURBO",    5) == 0) return 0;
    else if (name[0] == '1' || strncasecmp(name, "BALANCED", 8) == 0) return 1;
    else if (name[0] == '2' || strncasecmp(name, "STABLE",   6) == 0) return 2;
    else if (name[0] == '3' || strncasecmp(name, "CLEAN",    5) == 0) return 3;
    else if (name[0] == '4' || strncasecmp(name, "TONE_DARK", 9) == 0 ||
             strncasecmp(name, "DARK", 4) == 0) return 4;
    else if (name[0] == '5' || strncasecmp(name, "TONE_LIGHT", 10) == 0 ||
             strncasecmp(name, "LIGHT", 5) == 0) return 5;
    else if (name[0] == '6' || strcasecmp(name, "TONE_BIDIR_FAST") == 0 ||
             strcasecmp(name, "BIDIR_FAST") == 0 ||
             strcasecmp(name, "BIDIR2") == 0) return 6;
    else if (name[0] == '7' || strcasecmp(name, "TONE_BIDIR") == 0 ||
             strcasecmp(name, "BIDIR") == 0 ||
             strcasecmp(name, "BIDIR4") == 0) return 7;
    else if (name[0] == '8' || strcasecmp(name, "TONE_SOFT_DARK") == 0 ||
             strcasecmp(name, "SOFT_DARK") == 0) return 8;
    else if (name[0] == '9' || strcasecmp(name, "TONE_SOFT_LIGHT") == 0 ||
             strcasecmp(name, "SOFT_LIGHT") == 0) return 9;

    return -1;
}

static const char *partial_mode_name(int mode)
{
    static const char *names[] = {
        "TURBO", "BALANCED", "STABLE", "CLEAN", "TONE_DARK", "TONE_LIGHT",
        "TONE_BIDIR_FAST", "TONE_BIDIR", "TONE_SOFT_DARK", "TONE_SOFT_LIGHT"
    };

    return (mode >= 0 && mode < (int)ARRAY_SIZE(names)) ? names[mode] : "?";
}

/* LUTSET:<name|n> — select built-in preset by name/index and disable custom LUT.
 * Accepted: TURBO/0, BALANCED/1, STABLE/2, CLEAN/3, TONE_DARK/4,
 * TONE_LIGHT/5, TONE_BIDIR_FAST/6, TONE_BIDIR/7 (case-insensitive). */
void cmd_lutset(char *args)
{
    if (!args || !*args) {
        ble_printf("LUTSET: usage: TURBO|BALANCED|STABLE|CLEAN|TONE_DARK|TONE_LIGHT|TONE_BIDIR_FAST|TONE_BIDIR (0-7)\r\n");
        return;
    }
    int mode = parse_partial_mode_name(args);
    if (mode < 0) {
        ble_printf("LUTSET: unknown preset '%s'\r\n", args);
        return;
    }

    ssd1675a_set_use_custom_lut(false);
    display_manager_set_partial_mode(mode);
    ble_printf("LUTSET:%s (mode=%d, custom=off)\r\n", partial_mode_name(mode), mode);
}

/* VLUT:slot:base:off=val,off=val,... — define/activate/list/clear virtual LUT
 *   VLUT:0:4:35=01,36=01      — define slot 0 based on mode 4 (TONE_DARK), patch TA/TB to 1
 *   VLUT:0                     — activate slot 0
 *   VLUT:OFF                   — deactivate virtual LUT, revert to partial mode
 *   VLUT:LIST                  — show defined slots
 *   VLUT:CLEAR                 — clear all virtual slots
 */
void cmd_vlut(char *args)
{
    if (!args || !*args) {
        ble_printf("VLUT: usage: slot:base:off=val,... | slot | OFF | LIST | CLEAR\r\n");
        return;
    }

    if (strcmp(args, "CLEAR") == 0 || strcmp(args, "clear") == 0) {
        ssd1675a_vlut_clear();
        ble_printf("VLUT:cleared\r\n");
        return;
    }

    if (strcmp(args, "OFF") == 0 || strcmp(args, "off") == 0) {
        ssd1675a_vlut_activate(-1);
        ble_printf("VLUT:off\r\n");
        return;
    }

    if (strcmp(args, "LIST") == 0 || strcmp(args, "list") == 0) {
        for (int i = 0; i < ssd1675a_vlut_get_count(); i++) {
            if (ssd1675a_vlut_slot_defined(i)) {
                ble_printf("VLUT[%d]: defined%s\r\n", i,
                           (ssd1675a_vlut_active() == i) ? " *active*" : "");
            }
        }
        if (ssd1675a_vlut_active() < 0) ble_printf("VLUT: none active\r\n");
        return;
    }

    /* Parse: could be just "slot" to activate, or "slot:base:patches" to define */
    char *p1 = strchr(args, ':');
    int slot = atoi(args);

    if (!p1) {
        /* Just a slot number — activate it */
        if (!ssd1675a_vlut_slot_defined(slot)) {
            ble_printf("VLUT:slot %d not defined\r\n", slot);
            return;
        }
        ssd1675a_vlut_activate(slot);
        ssd1675a_set_use_custom_lut(false);
        ble_printf("VLUT:%d active\r\n", slot);
        return;
    }

    /* Define: slot:base:off=val,off=val,... */
    char *p2 = strchr(p1 + 1, ':');
    if (!p2) {
        ble_printf("VLUT:err format slot:base:patches\r\n");
        return;
    }

    int base_mode = atoi(p1 + 1);
    char *patches_str = p2 + 1;

    uint8_t offsets[16], values[16];
    int count = 0;

    while (*patches_str && count < 16) {
        int off = (int)strtol(patches_str, &patches_str, 10);
        if (*patches_str != '=') break;
        patches_str++;
        int val = (int)strtol(patches_str, &patches_str, 16);
        if (off >= 0 && off < 70) {
            offsets[count] = (uint8_t)off;
            values[count]  = (uint8_t)val;
            count++;
        }
        if (*patches_str == ',') patches_str++;
    }

    int rc = ssd1675a_vlut_define(slot, (uint8_t)base_mode, offsets, values, count);
    if (rc < 0) {
        ble_printf("VLUT:err rc=%d\r\n", rc);
        return;
    }
    ble_printf("VLUT[%d]:defined base=%d patches=%d\r\n", slot, base_mode, count);
}

/* VSTREAM:start[:preset]|stop — enter/exit binary animation streaming mode */
void cmd_vstream(char *args)
{
    if (!args || !*args) {
        ble_printf("VSTREAM:usage start[:TURBO|...|TONE_BIDIR_FAST]|stop\r\n");
        return;
    }
    if (args[0] == '0' || strncmp(args, "stop", 4) == 0) {
        vs_exit_streaming(true);
        ble_printf("VSTREAM:stopped\r\n");
        return;
    }
    if (args[0] == '1' || strncmp(args, "start", 5) == 0) {
        const char *mode_arg = (args[0] == '1') ? args + 1 : args + 5;
        int mode = 0;  /* Backward-compatible default: TURBO. */

        while (*mode_arg == ':' || *mode_arg == '=' || *mode_arg == ' ') mode_arg++;
        if (*mode_arg) {
            mode = parse_partial_mode_name(mode_arg);
            if (mode < 0) {
                ble_printf("VSTREAM:bad lut '%s'\r\n", mode_arg);
                return;
            }
        }

        display_manager_enable_screensaver(false);
        k_msleep(50);  /* let screensaver thread finish any in-progress update */
        graphics_clear(GFX_WHITE);  /* ensure FB starts white before first RLE frame */
        display_manager_set_keep_on(true);
        display_manager_set_stream_write_red_plane(false);
        ssd1675a_set_use_custom_lut(false);
        display_manager_set_partial_mode(mode);
        /* Prime the display before replying ready: wakes the controller from
         * deep sleep (where BUSY idles high and vs_flush_frame's wait_busy
         * would spin its full 8 s timeout on the first frame), charges the HV
         * rails and enters streaming mode. After this every frame only waits
         * for the previous LUT wave. */
        display_manager_update_partial();
        ble_service_set_streaming_mode(true);
        vs_state       = VS_IDLE;
        vs_frame_count = 0;
        vs_reset_frame();
        vstream_active = true;
        k_work_reschedule(&vs_watchdog_work, K_MSEC(VS_WATCHDOG_MS));
        ble_printf("VSTREAM:ready lut=%s type=RAW/RLE/DRLE/RAW2/RLE2/DRLE2 fmt=AA55 tt LL LL [payload] BB stop=CCDD/CCDE\r\n",
                   partial_mode_name(mode));
        return;
    }
    ble_printf("VSTREAM:unknown\r\n");
}

static void cmd_reboot(char *args)
{
    (void)args;
    ble_printf("REBOOT\r\n");
    /* Save to flash before the (DFU) reboot: carries stats across even when
     * retained RAM does not survive (old MCUboot / power blip). The new
     * firmware consumes this snapshot and marks it spent. */
    persist_save_to_flash();
    k_sleep(K_MSEC(100));   /* flush BLE TX + let flash write settle before reset */
    sys_reboot(SYS_REBOOT_COLD);
}

/* SYSINFO — detailed system information for the host app (DFU page, etc.)
 * Reports: firmware version, build date, uptime, battery, energy consumed,
 * estimated current, MCUboot image version (from VERSION file). */
static void cmd_sysinfo(char *args)
{
    (void)args;
    int mv = battery_read_mv();
    int64_t uptime_s = (int64_t)persist_uptime_sec();
    uint32_t mah_x1000 = display_manager_get_energy_mah_x1000();
    int cur_ua = display_manager_get_estimated_current_ua();

    ble_printf("SYSINFO:fw=%d.%d.%d",
               APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL);
#if defined(APP_BUILD_YEAR)
    ble_printf(" build=%d-%s%d-%s%d_%s%d:%s%d:%s%d",
               APP_BUILD_YEAR,
               APP_BUILD_MONTH < 10 ? "0" : "", APP_BUILD_MONTH,
               APP_BUILD_DAY   < 10 ? "0" : "", APP_BUILD_DAY,
               APP_BUILD_HOUR  < 10 ? "0" : "", APP_BUILD_HOUR,
               APP_BUILD_MIN   < 10 ? "0" : "", APP_BUILD_MIN,
               APP_BUILD_SEC   < 10 ? "0" : "", APP_BUILD_SEC);
#endif
    ble_printf(" uptime=%lld bat=%d mah=%u.%03u cur_ua=%d"
               " boots=%u fwupd=%u refr=%u refrfw=%u\r\n",
               (long long)uptime_s, mv,
               mah_x1000 / 1000, mah_x1000 % 1000, cur_ua,
               persist_boot_count(), persist_fw_update_count(),
               persist_refreshes_total(), persist_refreshes_since_fw());
}

/* STATS — read persisted statistics (live RAM copy + flash record), for manual
 * inspection and the frontend. Line 1 = live values; line 2 = flash snapshot. */
static void cmd_stats(char *args)
{
    (void)args;
    struct persist_report r;
    persist_get_report(&r);

    ble_printf("STATS:uptime=%lld wall=%lld boots=%u fwupd=%u refr=%u refrfw=%u\r\n",
               (long long)r.uptime_total_sec, (long long)r.wall_unix,
               r.boot_count, r.fw_update_count,
               r.refreshes_total, r.refreshes_since_fw);
    ble_printf("STATS:flash=%s fl_uptime=%lld fl_wall=%lld\r\n",
               r.flash_present ? (r.flash_consumed ? "consumed" : "valid") : "none",
               (long long)r.flash_uptime_total_sec, (long long)r.flash_wall_unix);
}

/* DFU:START — host notifies that OTA upload is beginning.
 * DFU:DONE — host notifies that OTA upload completed.
 * Show a status screen so the user knows not to disconnect. */
static void cmd_dfu(char *args)
{
    int w = graphics_get_width();

    display_manager_enable_screensaver(false);
    k_msleep(100);
    graphics_clear(GFX_WHITE);

    if (strncmp(args, "START", 5) == 0) {
        graphics_fill_rect(0, 0, w, 18, GFX_BLACK);
        graphics_draw_string_color(w / 2 - 55, 5, "FIRMWARE UPDATE", GFX_WHITE);
        graphics_draw_string(w / 2 - 65, 35, "Receiving firmware...");
        graphics_draw_string(w / 2 - 70, 55, "Do NOT disconnect!");
        /* Progress bar outline */
        graphics_draw_rect(20, 80, w - 40, 16, GFX_BLACK);
        graphics_fill_rect(22, 82, 20, 12, GFX_BLACK);
        graphics_draw_string(w / 2 - 40, 105, "Please wait...");
        display_manager_force_update();
        ble_printf("DFU:ACK\r\n");
    } else if (strncmp(args, "DONE", 4) == 0) {
        graphics_fill_rect(0, 0, w, 18, GFX_BLACK);
        graphics_draw_string_color(w / 2 - 55, 5, "UPDATE COMPLETE", GFX_WHITE);
        graphics_draw_string(w / 2 - 50, 38, "Firmware loaded!");
        /* Full progress bar */
        graphics_draw_rect(20, 62, w - 40, 16, GFX_BLACK);
        graphics_fill_rect(22, 64, w - 44, 12, GFX_BLACK);
        graphics_draw_string_color(w / 2 - 75, 90, "Reboot to apply", GFX_RED);
        graphics_draw_string(w / 2 - 60, 110, "(or send REBOOT)");
        display_manager_force_update();
        ble_printf("DFU:DONE_ACK\r\n");
    } else {
        ble_printf("DFU:usage START|DONE\r\n");
    }
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
    {"MODE:",       cmd_mode,       "Partial mode: 0=T 1=B 2=S 3=C 4=ToneDark 5=ToneLight 6=ToneBidirFast 7=ToneBidir 8=SoftDark 9=SoftLight"},
    {"PALTEST",     cmd_paltest,    "Render B/W/R palette and spatial dither test"},
    {"TONETEST",    cmd_tonetest,   "Render physical gray accumulation test"},
    {"TEXT:",       cmd_text,       "Draw text on display"},
    {"ROT:",        cmd_rot,        "Set rotation 0-3"},
    {"ANIM",        cmd_anim,       "Run bouncing-ball animation"},
    {"TEST",        cmd_test,       "Infinite partial stress test"},
    {"VSTREAM:",    cmd_vstream,    "Binary stream: VSTREAM:start[:preset]|stop (then binary frames)"},
    /* LUT editor */
    {"LUTW:",       cmd_lutw,       "Write full LUT: LUTW:HH..HH (140 hex)"},
    {"LW:",         cmd_lw,         "Write N LUT bytes: LW:idx:HH.."},
    {"L:",          cmd_l_byte,     "LUT byte: L:n=HH / L:DUMP / L:RESET"},
    {"LUTUSE:",     cmd_lutuse,     "Custom LUT toggle: LUTUSE:0/1"},
    {"LUTSET:",     cmd_lutset,     "Select preset: TURBO|BALANCED|STABLE|CLEAN|TONE_DARK|TONE_LIGHT|TONE_BIDIR_FAST|TONE_BIDIR|TONE_SOFT_DARK|TONE_SOFT_LIGHT"},
    {"VLUT:",       cmd_vlut,       "Virtual LUT: VLUT:slot:base:off=val,... | VLUT:slot | VLUT:OFF | VLUT:LIST | VLUT:CLEAR"},
    {"LGET",        cmd_lget,       "Dump current LUT: replies LUT:0: + LUT:1: lines"},
    {"LTEST",       cmd_ltest,      "LUT test animation: LTEST / LTEST 0"},
    {"HOST:",       cmd_host,       "Machine mode: HOST:1 (TELE: replies) HOST:0"},
    {"STAT",        cmd_stat,       "Telemetry snapshot: frame/last/min/max ms"},
    /* Frame buffers */
    {"FW:",         cmd_fw,         "Write BW frame: FW:offset:HH.."},
    {"RW:",         cmd_rw,         "Write Red frame: RW:offset:HH.."},
    /* System */
    {"REBOOT",      cmd_reboot,     "Cold reboot the device"},
    {"SYSINFO",     cmd_sysinfo,    "System info: version, uptime, battery, energy"},
    {"STATS",       cmd_stats,      "Persisted stats: live + flash record (present/valid/consumed)"},
    {"DFU:",        cmd_dfu,        "DFU display: DFU:START / DFU:DONE"},
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
    /* If a video/animation stream was active, tear it down and restore the
     * screensaver — the stream content is gone and the device should not sit
     * frozen on a half-rendered frame.
     *
     * If the device is in still-photo / image mode (screensaver already off,
     * no active vstream) the user intentionally left a tonal image on the
     * display.  A BLE disconnect must NOT wake the screensaver and overwrite
     * that image — the panel is bistable and the picture persists without
     * power.  We only clean up the binary-stream machine state. */
    if (vstream_active) {
        vs_exit_streaming(true);   /* stream was running → restore saver */
    } else {
        /* No stream active: just reset protocol state, leave saver as-is. */
        ble_service_set_streaming_mode(false);
        display_manager_set_stream_write_red_plane(false);
        k_work_cancel_delayable(&vs_watchdog_work);
        vs_state = VS_IDLE;
    }

    /* Clear session-scoped virtual LUT slots — they are temporary experiments
     * defined over BLE and should not persist across reconnections. */
    ssd1675a_vlut_clear();
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
