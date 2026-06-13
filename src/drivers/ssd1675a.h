#ifndef SSD1675A_H
#define SSD1675A_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "soft_spi.h"
#include <lib/eink_lut.h>  /* portable LUT presets + selection logic */

// Pin Definitions
#define PIN_BUSY 6
#define PIN_RST  7
#define PIN_VCC  19

// Display Dimensions
#define SSD1675A_WIDTH  128
#define SSD1675A_HEIGHT 296

// Functions
void ssd1675a_init(const struct device *gpio_dev);
void ssd1675a_init_partial(const struct device *gpio_dev);
void ssd1675a_power_on(void);
void ssd1675a_power_off(void);
void ssd1675a_display_buffer(const uint8_t *bw_buffer, const uint8_t *red_buffer);
void ssd1675a_display_buffer_fast(const uint8_t *bw_buffer);
void ssd1675a_display_buffers_fast(const uint8_t *bw_buffer, const uint8_t *red_buffer);
void ssd1675a_clear_red_ram(void);
void ssd1675a_update_display(void);
void ssd1675a_sleep(void);
void ssd1675a_set_vcom_register(uint8_t val);
void ssd1675a_set_lut_byte(int index, uint8_t val);
uint8_t ssd1675a_get_lut_byte(int index);
void ssd1675a_reset_lut(void);

/* Force all updates (full and partial) to use lut_data[] loaded via LUTW/LW.
 * When false, partial updates use the built-in turbo/balanced/stable tables.
 * The LUT *policy* lives in lib/eink_lut.h; these are zero-cost re-export shims
 * so existing ssd1675a_* callers don't change. */
static inline void ssd1675a_set_use_custom_lut(bool use) { eink_lut_set_use_custom(use); }
static inline bool ssd1675a_get_use_custom_lut(void)     { return eink_lut_get_use_custom(); }

void ssd1675a_wait_busy(void);

#define SSD1675A_LUT_SIZE EINK_LUT_SIZE

/* ── Virtual LUT slots (session-scoped, BLE-programmable) — shims into eink_lut */
static inline void ssd1675a_vlut_clear(void) { eink_lut_vlut_clear(); }
static inline int  ssd1675a_vlut_define(int slot, uint8_t base_mode,
                           const uint8_t *offsets, const uint8_t *values, int count) {
    return eink_lut_vlut_define(slot, base_mode, offsets, values, count);
}
static inline int  ssd1675a_vlut_get_count(void)        { return eink_lut_vlut_get_count(); }
static inline bool ssd1675a_vlut_slot_defined(int slot) { return eink_lut_vlut_slot_defined(slot); }
static inline void ssd1675a_vlut_activate(int slot)     { eink_lut_vlut_activate(slot); }
static inline int  ssd1675a_vlut_active(void)           { return eink_lut_vlut_active(); }

/* Partial-update modes — the enum now lives in lib/eink_lut.h (eink_lut_mode_t).
 * These aliases keep the historic ssd1675a_* names so callers don't change. */
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
void ssd1675a_update_partial(void);
void ssd1675a_begin_streaming(void);
void ssd1675a_update_frame_stream(void);
void ssd1675a_trigger_frame_stream_nowait(void);
void ssd1675a_end_streaming(void);
void ssd1675a_load_default_lut(void);
void ssd1675a_update_display_flush_red(void);

#endif // SSD1675A_H
