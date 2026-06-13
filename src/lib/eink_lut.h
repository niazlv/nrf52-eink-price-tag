#ifndef EINK_LUT_H
#define EINK_LUT_H

/*
 * Portable e-ink waveform presets + selection logic for SSD16xx-family
 * controllers (SSD1675A and relatives that take a 70-byte command-0x32 LUT:
 * a 35-byte VS section of 5 LUTs × 7 phases, then a 35-byte timing section of
 * 7 phases × {TA TB TC TD RP}).
 *
 * This module is deliberately free of any hardware / Zephyr dependency: it only
 * owns the *extra* preset tables and the policy for choosing one. The base
 * driver keeps the editable working table and the const default; it hands those
 * two pointers here once via eink_lut_bind_base_tables() and then asks
 * eink_lut_select_partial() which 70 bytes to upload for the next refresh.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* VS section (35) + timing section (35). */
#define EINK_LUT_SIZE 70

/* Partial/streaming refresh modes. Integer values are part of the public BLE
 * "MODE:" protocol and the VLUT base-mode field — do NOT reorder. */
typedef enum {
    EINK_LUT_MODE_TURBO = 0,    /* MODE:0 — 14f=112ms, direct B&W, use with streaming */
    EINK_LUT_MODE_BALANCED,     /* MODE:1 — ~896ms, pre-erase + main, red support */
    EINK_LUT_MODE_STABLE,       /* MODE:2 — ~1.1s, legacy (preserved) */
    EINK_LUT_MODE_CLEAN,        /* MODE:3 — v5-balanced full LUT (deep clean) */
    EINK_LUT_MODE_TONE_DARK,    /* MODE:4 — black-only pulse for tonal accumulation */
    EINK_LUT_MODE_TONE_LIGHT,   /* MODE:5 — white-only pulse for tonal accumulation */
    EINK_LUT_MODE_TONE_BIDIR_FAST, /* MODE:6 — weak B/W pulse for tone video */
    EINK_LUT_MODE_TONE_BIDIR,      /* MODE:7 — stronger B/W pulse for tone video */
    EINK_LUT_MODE_TONE_SOFT_DARK,  /* MODE:8 — soft black pulse (TA=TB=1, ~half strength) */
    EINK_LUT_MODE_TONE_SOFT_LIGHT, /* MODE:9 — soft white pulse (TA=TB=1, ~half strength) */
} eink_lut_mode_t;

/* Bind the base driver's two tables (each EINK_LUT_SIZE bytes):
 *   working     — the editable/"custom" table the driver uploads on full updates
 *   default_tbl — the const factory default (used by CLEAN mode and VLUT base 3)
 * Call once before the first eink_lut_select_partial(). */
void eink_lut_bind_base_tables(const uint8_t *working, const uint8_t *default_tbl);

/* Force all partial updates to use the bound working ("custom") table loaded via
 * LUTW/LW. When false, partial updates use the built-in presets below. */
void eink_lut_set_use_custom(bool use);
bool eink_lut_get_use_custom(void);

void            eink_lut_set_mode(eink_lut_mode_t mode);
eink_lut_mode_t eink_lut_get_mode(void);

/* ── Virtual LUT slots (session-scoped, BLE-programmable) ─────────────── */
void eink_lut_vlut_clear(void);
int  eink_lut_vlut_define(int slot, uint8_t base_mode,
                          const uint8_t *offsets, const uint8_t *values, int count);
int  eink_lut_vlut_get_count(void);
bool eink_lut_vlut_slot_defined(int slot);
void eink_lut_vlut_activate(int slot);
int  eink_lut_vlut_active(void);

/* Returns the EINK_LUT_SIZE-byte table to upload for the current partial/
 * streaming refresh, honouring use_custom → active VLUT slot → current mode
 * (including on-the-fly soft derivation). The pointer is valid until the next
 * call (soft/VLUT results share one scratch buffer); upload it immediately. */
const uint8_t *eink_lut_select_partial(void);

/* Pointer to the red-channel flush table (VSL on red to drive pigment away). */
const uint8_t *eink_lut_flush_red(void);

#endif /* EINK_LUT_H */
