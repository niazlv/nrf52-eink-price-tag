#include "ssd1675a.h"

#include <string.h>

/* Snapshot of the tuned default — used by ssd1675a_reset_lut() and as the
 * factory baseline. The mutable working table below is seeded from this.
 *
 * v5-balanced: bright BWR + DC-compensated Ph2 for all groups.
 * Measured: ~8707ms full update. DC balance: BLK=0, WHT=0, RED=-3.3
 * Ph2 added as ratchet extension + charge compensation (LUT2/3: 0xA8=VSL×3).
 * Ph5+Ph6: 0xFF fixation for red reveal. Ph4: RP=7 main red drive. */
static const uint8_t lut_data_default[EINK_LUT_SIZE] = {
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
 * lut_data_default; patched via ssd1675a_set_lut_byte() (LUTW/LW). */
static uint8_t lut_data[EINK_LUT_SIZE];
static bool lut_seeded;

static uint8_t vcom_register_value = SSD1675A_VCOM_DEFAULT;
static bool controller_sleeping = true;
static bool port_ready;

void ssd1675a_lut_init(void)
{
    if (lut_seeded) {
        return;
    }
    memcpy(lut_data, lut_data_default, sizeof(lut_data));
    eink_lut_bind_base_tables(lut_data, lut_data_default);
    lut_seeded = true;
}

/* Every entry point that touches the bus goes through this, so no call order
 * can catch the driver half-initialised: the port is brought up on first use
 * (ssd1675a_port_init is idempotent) and the waveform table is seeded before
 * anything can ask eink_lut which table to upload.
 *
 * Returns false when there is no usable display — callers then no-op instead
 * of driving a dead bus and blocking on a busy-wait that never clears. */
static bool bus_ready(void)
{
    ssd1675a_lut_init();
    if (!port_ready) {
        port_ready = ssd1675a_port_init();
    }
    return port_ready;
}

void ssd1675a_set_lut_byte(int index, uint8_t val)
{
    ssd1675a_lut_init();
    if (index >= 0 && index < (int)sizeof(lut_data)) {
        lut_data[index] = val;
    }
}

uint8_t ssd1675a_get_lut_byte(int index)
{
    ssd1675a_lut_init();
    if (index >= 0 && index < (int)sizeof(lut_data)) {
        return lut_data[index];
    }
    return 0;
}

void ssd1675a_reset_lut(void)
{
    ssd1675a_lut_init();
    memcpy(lut_data, lut_data_default, sizeof(lut_data));
    eink_lut_set_use_custom(false);
}

/* ── Bus helpers ────────────────────────────────────────────────────────── */

static void send_cmd(uint8_t cmd)
{
    ssd1675a_port_write9(cmd, false);
}

static void send_data(uint8_t data)
{
    ssd1675a_port_write9(data, true);
}

/* Upload a full EINK_LUT_SIZE-byte waveform table via command 0x32. */
static void upload_lut(const uint8_t *lut)
{
    send_cmd(0x32);
    for (int i = 0; i < EINK_LUT_SIZE; i++) {
        send_data(lut[i]);
    }
}

void ssd1675a_wait_busy(void)
{
    int remaining = SSD1675A_BUSY_TIMEOUT_MS;

    if (!bus_ready()) {
        return;
    }
    while (ssd1675a_port_busy() && remaining > 0) {
        ssd1675a_port_delay_ms(SSD1675A_BUSY_POLL_MS);
        remaining -= SSD1675A_BUSY_POLL_MS;
    }
}

static void set_ram_pointer(int x, int y)
{
    send_cmd(0x4E);
    send_data(x & 0xFF);
    send_cmd(0x4F);
    send_data(y & 0xFF);
    send_data((y >> 8) & 0xFF);
}

static void configure_registers(void)
{
    send_cmd(0x74); // Set analog block control
    send_data(SSD1675A_ANALOG_BLOCK);

    send_cmd(0x7E); // Set digital block control
    send_data(SSD1675A_DIGITAL_BLOCK);

    send_cmd(0x01); // Driver output: (gate lines - 1), then scan direction flags
    send_data(SSD1675A_RAM_Y_END & 0xFF);
    send_data((SSD1675A_RAM_Y_END >> 8) & 0xFF);
    send_data(0x00);

    send_cmd(0x3A); // Dummy line period, part of panel scan timing
    send_data(SSD1675A_DUMMY_LINE);

    send_cmd(0x3B); // Gate line width, part of panel scan timing
    send_data(SSD1675A_GATE_WIDTH);

    send_cmd(0x3C); // Border waveform control
    send_data(SSD1675A_BORDER_WAVEFORM);

    send_cmd(0x11); // Data entry mode
    send_data(0x03); // Auto-increment X and Y after RAM writes

    send_cmd(0x44); // RAM X range, in bytes
    send_data(0x00);
    send_data(SSD1675A_RAM_X_END);

    send_cmd(0x45); // RAM Y range, in gate lines
    send_data(0x00);
    send_data(0x00);
    send_data(SSD1675A_RAM_Y_END & 0xFF);
    send_data((SSD1675A_RAM_Y_END >> 8) & 0xFF);

    send_cmd(0x04); // Source driving voltage settings
    send_data(SSD1675A_SOURCE_VSH1);
    send_data(SSD1675A_SOURCE_VSH2);
    send_data(SSD1675A_SOURCE_VSL);

    send_cmd(0x2C); // VCOM voltage, affects contrast and ghosting
    send_data(vcom_register_value);

#if SSD1675A_USE_CUSTOM_LUT
    upload_lut(lut_data); // Waveform table for pixel transitions
#endif
}

static void reset_controller(void)
{
    ssd1675a_port_reset(true);
    ssd1675a_port_delay_ms(10);
    ssd1675a_port_reset(false);
    ssd1675a_wait_busy();

    send_cmd(0x12); // SW reset
    ssd1675a_wait_busy();
    controller_sleeping = false;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

bool ssd1675a_init(void)
{
    ssd1675a_lut_init();

    port_ready = ssd1675a_port_init();
    if (!port_ready) {
        return false;
    }

    ssd1675a_power_on();
    reset_controller();
    configure_registers();
    return true;
}

bool ssd1675a_init_partial(void)
{
    ssd1675a_lut_init();

    port_ready = ssd1675a_port_init();
    if (!port_ready) {
        return false;
    }

    // Safe to call if the rail is already on.
    ssd1675a_power_on();

    // Recovery: if BUSY is already HIGH the controller is stuck (e.g. mid-operation
    // reflash, or HV rails never discharged).  Hard-reset to unblock it.
    // RAM will be lost, but a corrupted FB is better than an infinite busy-wait.
    if (controller_sleeping || ssd1675a_port_busy()) {
        reset_controller();
    }

    // Re-configure registers (may be lost after sleep or the reset above)
    configure_registers();
    controller_sleeping = false;
    return true;
}

void ssd1675a_set_vcom_register(uint8_t val)
{
    vcom_register_value = val;
}

void ssd1675a_power_on(void)
{
    ssd1675a_port_power(true);
    ssd1675a_port_delay_ms(10);
}

void ssd1675a_power_off(void)
{
    ssd1675a_port_power(false);
    controller_sleeping = true;
}

void ssd1675a_sleep(void)
{
    if (!bus_ready()) {
        return;
    }
    send_cmd(0x10); // Deep sleep
    send_data(0x01);
    controller_sleeping = true;
}

/* ── Refresh ────────────────────────────────────────────────────────────── */

void ssd1675a_update_display(void)
{
    if (!bus_ready()) {
        return;
    }
    ssd1675a_load_default_lut(); // Ensure the working table is active
    send_cmd(0x22);
    send_data(0xC7); // Enable CLK+analog → display mode 1 → disable analog+CLK
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_trigger_update_nowait(void)
{
    if (!bus_ready()) {
        return;
    }
    ssd1675a_load_default_lut();
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
}

void ssd1675a_update_partial(void)
{
    if (!bus_ready()) {
        return;
    }
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

void ssd1675a_begin_streaming(void)
{
    if (!bus_ready()) {
        return;
    }
    upload_lut(eink_lut_select_partial());
    // 0xC0: Enable CLK + Enable Analog only (no display yet).
    // Charges ±15V rails — one-time cost ~600ms.
    send_cmd(0x22);
    send_data(0xC0);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_update_frame_stream(void)
{
    if (!bus_ready()) {
        return;
    }
    // 0x04: Display Mode 1 only — HV rails already on, no power cycling.
    // BUSY duration = LUT wave time only (subframes × ~15ms; TURBO ≈ 210ms).
    send_cmd(0x22);
    send_data(0x04);
    send_cmd(0x20);
    ssd1675a_wait_busy();
}

void ssd1675a_trigger_frame_stream_nowait(void)
{
    if (!bus_ready()) {
        return;
    }
    /* Same as update_frame_stream but does NOT wait for BUSY.
     * Caller must call ssd1675a_wait_busy() before the next SPI write. */
    send_cmd(0x22);
    send_data(0x04);
    send_cmd(0x20);
}

void ssd1675a_end_streaming(void)
{
    if (!bus_ready()) {
        return;
    }
    // 0x03: Disable Analog + CLK — powers down ±15V rails.
    send_cmd(0x22);
    send_data(0x03);
    send_cmd(0x20);
}

void ssd1675a_load_default_lut(void)
{
    if (!bus_ready()) {
        return;
    }
    ssd1675a_lut_init();
    upload_lut(lut_data);
}

void ssd1675a_update_display_flush_red(void)
{
    if (!bus_ready()) {
        return;
    }
    upload_lut(eink_lut_flush_red());
    send_cmd(0x22);
    send_data(0xC7);
    send_cmd(0x20);
    ssd1675a_wait_busy();
    ssd1675a_load_default_lut();
}

/* ── RAM planes ─────────────────────────────────────────────────────────── */

/* A NULL buffer writes zeros — the caller's way of clearing a plane. */
static void write_plane(uint8_t ram_cmd, const uint8_t *buffer)
{
    set_ram_pointer(0, 0);
    send_cmd(ram_cmd);
    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        send_data(buffer ? buffer[i] : 0x00);
    }
}

void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer)
{
    if (!bus_ready()) {
        return;
    }
    write_plane(0x24, bw_buffer);
    write_plane(0x26, red_buffer);
}

void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer)
{
    if (!bus_ready()) {
        return;
    }
    // Skipping the red plane halves the transfer time. Partial LUTs disable the
    // red phase, so whatever is left in red RAM is ignored.
    write_plane(0x24, bw_buffer);
}

void ssd1675a_display_buffers_fast(const uint8_t *bw_buffer, const uint8_t *red_buffer)
{
    if (!bus_ready()) {
        return;
    }
    // Write both RAM planes without a full display update. Tone-servo video
    // repurposes the red plane as a second control bit:
    //   00 = darken, 01 = lighten, 10/11 = hold/no-op via LUT2/LUT3.
    write_plane(0x24, bw_buffer);
    write_plane(0x26, red_buffer);
}

void ssd1675a_clear_red_ram(void)
{
    if (!bus_ready()) {
        return;
    }
    write_plane(0x26, NULL);
}
