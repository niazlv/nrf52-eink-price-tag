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

/* ── RAM planes ─────────────────────────────────────────────────────────── */

/**
 * Load both RAM planes. Each buffer is SSD1675A_RAM_BYTES long, 1 bit per
 * pixel, MSB = leftmost pixel. @p red_buffer may be NULL to clear the red
 * plane instead.
 */
void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer);

/** Load only the black/white plane — halves the transfer for partial updates. */
void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer);

/** Load both planes without triggering a refresh (tone-servo video path). */
void ssd1675a_display_buffers_fast(const uint8_t *bw_buffer, const uint8_t *red_buffer);

/** Zero the red plane. */
void ssd1675a_clear_red_ram(void);

/* ── Refresh ────────────────────────────────────────────────────────────── */

/** Full B/W/R refresh with the working waveform table. Blocks ~9 s. */
void ssd1675a_update_display(void);

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

/** Override the VCOM register value applied by the next init. */
void ssd1675a_set_vcom_register(uint8_t val);

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
