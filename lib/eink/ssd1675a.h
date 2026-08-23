#ifndef SSD1675A_H
#define SSD1675A_H

/*
 * SSD1675A e-paper controller driver — portable C99.
 *
 * No OS, no vendor SDK, no dynamic allocation: the driver talks to the panel
 * only through the six functions in ssd1675a_port.h, and never owns a
 * framebuffer (callers pass their own planes to ssd1675a_display_buffer*).
 * Panel geometry and register defaults live in ssd1675a_config.h.
 *
 * Typical use:
 *
 *     ssd1675a_init();                               // full-refresh setup
 *     ssd1675a_display_buffer(bw, red);              // load both RAM planes
 *     ssd1675a_update_display();                     // ~9 s B/W/R refresh
 *
 * Fast monochrome updates skip the red plane and use a partial waveform:
 *
 *     ssd1675a_init_partial();
 *     ssd1675a_set_partial_mode(EINK_LUT_MODE_TURBO);
 *     ssd1675a_display_buffer_fast(bw);
 *     ssd1675a_update_partial();                     // ~700 ms (HV recharge)
 *
 * For animation, hold the high-voltage rails on across frames — that recharge
 * is the bulk of the cost, and streaming pays it once:
 *
 *     ssd1675a_begin_streaming();                    // ~600 ms, once
 *     while (...) { ssd1675a_display_buffer_fast(bw); ssd1675a_update_frame_stream(); }
 *     ssd1675a_end_streaming();
 */

#include <stdint.h>
#include <stdbool.h>

#include "ssd1675a_config.h"
#include "ssd1675a_port.h"
#include "eink_lut.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Historic spelling of the waveform table size. */
#define SSD1675A_LUT_SIZE EINK_LUT_SIZE

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * Power up, hard-reset and configure the controller for full refreshes.
 * @return false if the port reports no usable display; all other entry points
 *         then become no-ops rather than hanging on a busy-wait.
 */
bool ssd1675a_init(void);

/**
 * Like ssd1675a_init(), but only hard-resets when the controller is asleep or
 * stuck with BUSY already high. Cheap enough to call before every partial
 * update; preserves RAM contents when it can.
 */
bool ssd1675a_init_partial(void);

/** Enable the panel supply rail (idempotent). */
void ssd1675a_power_on(void);

/** Cut the panel supply rail. Loses controller state and RAM. */
void ssd1675a_power_off(void);

/** Put the controller in deep sleep. Needs a re-init to wake. */
void ssd1675a_sleep(void);

/** Poll BUSY until the controller is idle or SSD1675A_BUSY_TIMEOUT_MS passes. */
void ssd1675a_wait_busy(void);

/* ── Geometry ───────────────────────────────────────────────────────────── */

/**
 * Override the compile-time panel size (ssd1675a_config.h) — takes effect at
 * the next init. @p width in pixels (multiple of 8, ≤ 512), @p height in gate
 * lines (≤ 512). Buffers handed to ssd1675a_display_buffer*() must then be
 * ssd1675a_ram_bytes() long. Meant for applications that identify the panel
 * at boot with ssd1675a_probe_ram().
 */
void ssd1675a_set_geometry(int width, int height);
int  ssd1675a_width(void);
int  ssd1675a_height(void);
int  ssd1675a_ram_bytes(void);

/**
 * Scan timing, applied at the next init: 0x3A dummy line period (0..127)
 * and 0x3B gate line width (0..15). Together they set the duration of one
 * LUT subframe — fewer dummy lines / a narrower gate pulse make every refresh
 * proportionally shorter, at the cost of weaker per-line drive. Defaults come
 * from ssd1675a_config.h; see the controller datasheet's frame-rate table.
 */
void    ssd1675a_set_scan_timing(uint8_t dummy_line, uint8_t gate_width);
uint8_t ssd1675a_get_scan_dummy_line(void);
uint8_t ssd1675a_get_scan_gate_width(void);

/* ── RAM planes ─────────────────────────────────────────────────────────── */

/**
 * Load both RAM planes. Each buffer is ssd1675a_ram_bytes() long (that is
 * SSD1675A_RAM_BYTES for the default geometry), 1 bit per pixel, MSB =
 * leftmost pixel. @p red_buffer may be NULL to clear the red plane instead.
 */
void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer);

/** Load only the black/white plane — halves the transfer for partial updates. */
void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer);

/** Load one plane (@p red selects the red RAM) without triggering a refresh.
 *  Lets a host with a single framebuffer fill the planes one after the other,
 *  e.g. a red mask first, then the B/W image. NULL clears the plane. */
void ssd1675a_load_plane(bool red, const uint8_t *buffer);

/** Load only rows @p y0..@p y1 (inclusive, gate lines) of one plane from a
 *  full-size @p buffer, through a RAM window, without touching the rest.
 *  For streaming: a delta frame knows which rows changed, and the bus cost
 *  drops with the row count. The full window is restored afterwards. */
void ssd1675a_load_rows(bool red, const uint8_t *buffer, int y0, int y1);

/** Load both planes without triggering a refresh (tone-servo video path). */
void ssd1675a_display_buffers_fast(const uint8_t *bw_buffer, const uint8_t *red_buffer);

/** Zero the red plane. */
void ssd1675a_clear_red_ram(void);

/* ── Refresh ────────────────────────────────────────────────────────────── */

/** Full B/W/R refresh with the working waveform table. Blocks ~9 s. */
void ssd1675a_update_display(void);

/** Full B/W/R refresh with the panel's factory waveform from OTP (LUT,
 *  voltages and frame timing for the measured temperature) instead of the
 *  working table. The right choice on a panel the working table was never
 *  tuned for. Registers it changed are restored by the next init. */
void ssd1675a_update_display_otp(void);

/** Start a full refresh and return immediately. The caller must either call
 *  ssd1675a_wait_busy() before touching the bus again, or deliberately abort
 *  the refresh part-way (assert reset, cut power) to freeze an intermediate
 *  waveform phase. */
void ssd1675a_trigger_update_nowait(void);

/** Full refresh with the red-flush waveform, then restore the working table. */
void ssd1675a_update_display_flush_red(void);

/** Partial refresh with the waveform eink_lut_select_partial() picks. */
void ssd1675a_update_partial(void);

/* Streaming: begin → N × update_frame_stream → end. See the header comment. */
void ssd1675a_begin_streaming(void);
void ssd1675a_update_frame_stream(void);
/** Trigger a stream frame without waiting; the caller must call
 *  ssd1675a_wait_busy() before the next SPI write. */
void ssd1675a_trigger_frame_stream_nowait(void);
void ssd1675a_end_streaming(void);

/* ── Waveform table ─────────────────────────────────────────────────────── */

/** Seed the working table from the built-in default and publish both tables to
 *  eink_lut. Called automatically by the driver; safe to call early and often
 *  (a port may call it from a pre-main init hook). */
void ssd1675a_lut_init(void);

/** Re-upload the working table (after a mode that swapped it out). */
void ssd1675a_load_default_lut(void);

/** Patch one byte of the working table (the LUTW/LW editor). */
void ssd1675a_set_lut_byte(int index, uint8_t val);
uint8_t ssd1675a_get_lut_byte(int index);

/** Restore the working table to the built-in default and stop using it. */
void ssd1675a_reset_lut(void);

/** Override the VCOM register value applied by the next init (also turns
 *  ssd1675a_set_vcom_from_otp() back off). */
void ssd1675a_set_vcom_register(uint8_t val);

/** Keep the factory VCOM the SW reset loads from OTP instead of writing
 *  SSD1675A_VCOM_DEFAULT — for panels whose VCOM was never measured. */
void ssd1675a_set_vcom_from_otp(bool enable);

/* ── Identification / probing ───────────────────────────────────────────── */
/* These need the read-back path (ssd1675a_port_read) and an awake controller:
 * call them between init and sleep. */

/** Status Bit Read (0x2F): bit 2 = BUSY, bits 1:0 = chip ID. The chip ID is
 *  01 on SSD1675A/B and SSD1619A alike — it does NOT tell panels apart. */
uint8_t ssd1675a_read_status(void);

/** Read @p n parameter bytes of any read command: 0x2D (OTP display option —
 *  VCOM OTP selection, VCOM, display modes, waveform version), 0x2E (10-byte
 *  user ID), 0x1B (temperature register), ... */
void ssd1675a_read_register(uint8_t cmd, uint8_t *buf, int n);

/** Result of ssd1675a_probe_ram(). */
typedef struct {
    int  bytes;           /* bytes written and read back */
    int  match;           /* how many came back equal to what was written */
    int  first_mismatch;  /* index of the first differing byte, -1 if none */
    bool all_ff;          /* every byte read back was 0xFF: no read path */
} ssd1675a_ram_probe_t;

/**
 * RAM-capacity probe — the one measurement that tells a 160x296 controller
 * (SSD1675A/B: 5920 B per plane) from a 400x300 one (SSD1619A: 15000 B)
 * without knowing the panel in advance. Writes @p rows × 50 bytes of a
 * pseudo-random sequence through a 400-px-wide window, reads them back the
 * same way and counts matches. With rows >= 237 the write exceeds BOTH planes
 * of the small controller, so it must lose data whatever its address wrapping
 * does, while the large one returns every byte. Destroys the BW plane; the
 * driver re-applies its own window/timing/LUT afterwards, the caller redraws.
 */
void ssd1675a_probe_ram(int rows, ssd1675a_ram_probe_t *out);

/* ── eink_lut re-exports ────────────────────────────────────────────────── */
/* The waveform *policy* — presets, modes, virtual LUT slots — lives in
 * eink_lut.h, which is panel-independent and reusable across the SSD16xx
 * family. These zero-cost shims keep the historic ssd1675a_* spellings. */

static inline void ssd1675a_set_use_custom_lut(bool use) { eink_lut_set_use_custom(use); }
static inline bool ssd1675a_get_use_custom_lut(void)     { return eink_lut_get_use_custom(); }

static inline void ssd1675a_vlut_clear(void) { eink_lut_vlut_clear(); }
static inline int  ssd1675a_vlut_define(int slot, uint8_t base_mode,
                           const uint8_t *offsets, const uint8_t *values, int count) {
    return eink_lut_vlut_define(slot, base_mode, offsets, values, count);
}
static inline int  ssd1675a_vlut_get_count(void)        { return eink_lut_vlut_get_count(); }
static inline bool ssd1675a_vlut_slot_defined(int slot) { return eink_lut_vlut_slot_defined(slot); }
static inline void ssd1675a_vlut_activate(int slot)     { eink_lut_vlut_activate(slot); }
static inline int  ssd1675a_vlut_active(void)           { return eink_lut_vlut_active(); }

typedef eink_lut_mode_t ssd1675a_partial_mode_t;
#define SSD1675A_PARTIAL_MODE_TURBO            EINK_LUT_MODE_TURBO
#define SSD1675A_PARTIAL_MODE_BALANCED         EINK_LUT_MODE_BALANCED
#define SSD1675A_PARTIAL_MODE_STABLE           EINK_LUT_MODE_STABLE
#define SSD1675A_PARTIAL_MODE_CLEAN            EINK_LUT_MODE_CLEAN
#define SSD1675A_PARTIAL_MODE_TONE_DARK        EINK_LUT_MODE_TONE_DARK
#define SSD1675A_PARTIAL_MODE_TONE_LIGHT       EINK_LUT_MODE_TONE_LIGHT
#define SSD1675A_PARTIAL_MODE_TONE_BIDIR_FAST  EINK_LUT_MODE_TONE_BIDIR_FAST
#define SSD1675A_PARTIAL_MODE_TONE_BIDIR       EINK_LUT_MODE_TONE_BIDIR
#define SSD1675A_PARTIAL_MODE_TONE_SOFT_DARK   EINK_LUT_MODE_TONE_SOFT_DARK
#define SSD1675A_PARTIAL_MODE_TONE_SOFT_LIGHT  EINK_LUT_MODE_TONE_SOFT_LIGHT

static inline void ssd1675a_set_partial_mode(ssd1675a_partial_mode_t mode) { eink_lut_set_mode(mode); }

#ifdef __cplusplus
}
#endif

#endif /* SSD1675A_H */
