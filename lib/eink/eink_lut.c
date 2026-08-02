#include "eink_lut.h"

/* The base driver's editable working table and const default, bound at init.
 * NULL until eink_lut_bind_base_tables() runs; selection falls back to
 * lut_balanced in that window so a stray early refresh still gets valid bytes. */
static const uint8_t *base_working;
static const uint8_t *base_default;

static bool use_custom = false;
static eink_lut_mode_t current_mode = EINK_LUT_MODE_BALANCED;

void eink_lut_bind_base_tables(const uint8_t *working, const uint8_t *default_tbl) {
    base_working = working;
    base_default = default_tbl;
}

void eink_lut_set_use_custom(bool use) { use_custom = use; }
bool eink_lut_get_use_custom(void)     { return use_custom; }

void            eink_lut_set_mode(eink_lut_mode_t mode) { current_mode = mode; }
eink_lut_mode_t eink_lut_get_mode(void)                 { return current_mode; }

// Preserved as "Stable/Reddish" (Works but ~1.1s)
static const uint8_t lut_stable[] = {
    0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x6A, 0x9B, 0x9B, 0x9B, 0x9B, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Timings
    0x02, 0x06, 0x02, 0x06, 0x01, // Phase 0
    0x02, 0x06, 0x02, 0x06, 0x01, // Phase 1
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 2
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 3
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 4 (Red) - CLEARED
    0x00, 0x00, 0x00, 0x00, 0x00, // Phase 5
    0x00, 0x00, 0x00, 0x00, 0x00  // Phase 6
};

// Balanced LUT — 2-phase: pre-erase (12f) + main drive (100f) ≈ 896ms wave.
// Ph0: drive pixels OPPOSITE their destination to clear ghost content.
//   LUT0 (black dest): VSL pre-erase; LUT1 (white dest): VSH1 pre-erase.
// Ph1: drive to correct final value.
//   LUT0: VSH1; LUT1: VSL; LUT2/3: VSH2 (red channel).
// Supports red pixels when Red RAM is written (LTEST mode).
// ~TEST VARIANT A — send feedback to tune Ph timings.
static const uint8_t lut_balanced[] = {
    0xAA, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 black: Ph0=VSL, Ph1=VSH1
    0x55, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 white: Ph0=VSH1, Ph1=VSL
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT2 red:   Ph0=VSH2, Ph1=VSH2
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT3 red+bw: same
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT4 VCOM=0
    0x06, 0x06, 0x00, 0x00, 0x00,              // Ph0: TA=6, TB=6, RP=0 → 12f=96ms pre-erase
    0x32, 0x32, 0x00, 0x00, 0x00,              // Ph1: TA=50, TB=50, RP=0 → 100f=800ms main
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// Turbo LUT — direct B&W drive, 14 subframes.
// LUT0 (black): VSH1 both TA and TB periods.
// LUT1 (white): VSL  both TA and TB periods.
// DC-balanced: TA=7 VSH1 + TB=7 VSH1 for black; TA=7 VSL + TB=7 VSL for white.
// Use with streaming (begin_streaming/update_frame_stream) to avoid HV overhead.
//
// TIMING (measured 2026-06, vstream telemetry): one subframe ≈ 15ms with the
// current scan settings (0x3A dummy=0x35, 0x3B gate width=0x04), NOT the 8ms
// originally assumed. So this wave is 14 × 15 ≈ 210ms (expected 112ms).
// Measured fps over BLE vstream: 4.4 (TURBO), 5.0 (10-subframe custom),
// where shorter LUTs visibly wash out moving pixels — static areas re-drive
// every frame, a moving pixel gets only one pass. User verdict: 10-subframe
// quality loss is not worth +0.6fps; TURBO is the quality baseline.
// Next untapped lever: shorten the subframe itself via 0x3A/0x3B (53 dummy
// lines on a 296-line panel ≈ +18% scan time alone).
static const uint8_t lut_turbo[] = {
    0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 (black): Ph0=VSH1
    0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 (white): Ph0=VSL
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT2: no red
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT3: no red
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT4: VCOM=0
    0x07, 0x07, 0x00, 0x00, 0x00,              // Ph0: TA=7, TB=7, RP=0 → 14f=112ms
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// Tone LUTs — directional no-op waveforms for accumulation tests.
// TONE_DARK: only BW RAM black pixels get a short VSH1 pulse; white pixels
// are left floating. Repeating masked passes creates physical gray levels.
// TONE_LIGHT is the inverse: only white pixels get a VSL pulse.
// Timing: 4 subframes per pass with current scan settings (~60ms measured
// scale), intentionally weaker than TURBO's 14-subframe full B/W drive.
static const uint8_t lut_tone_dark[] = {
    0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 black: VSH1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 white: no-op
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_tone_light[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 black: no-op
    0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 white: VSL
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// Tone video LUTs — weak bidirectional pulses. Unlike TONE_DARK/TONE_LIGHT
// still-photo modes, every refresh may push black pixels darker and white
// pixels lighter. The host encodes grayscale as temporal duty-cycle masks, so
// one display refresh still equals one video output frame.
static const uint8_t lut_tone_bidir_fast[] = {
    0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 black: VSH1
    0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 white: VSL
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00,              // 2 subframes: fastest, weakest
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lut_tone_bidir[] = {
    0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT0 black: VSH1
    0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // LUT1 white: VSL
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x00, 0x00,              // 4 subframes: stronger tone step
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// lut_anim and lut_clean removed — TURBO covers fast animation, CLEAN uses lut_data_default.

// LUT for red-channel flush after clean/nuke cycles.
// LUT0/LUT1: same BW waveform as lut_data_default.
// LUT2/LUT3: 0xAA = VSL (negative voltage) for all transitions — actively
//   drives red pigment particles away from the viewing surface.
// Ph4 timing zeroed: skips the main red-drive phase so we don't re-activate.
static const uint8_t lut_flush_red[] = {
    /* VS section */
    /* LUT0 BLACK */ 0x22, 0x11, 0x10, 0x00, 0x10, 0x00, 0x00,
    /* LUT1 WHITE */ 0x11, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00,
    /* LUT2 RED   */ 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    /* LUT3 RED   */ 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    /* LUT4 VCOM  */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Timing — same as lut_data_default but Ph4 zeroed */
    /* Ph0 */ 0x00, 0x14, 0x00, 0x12, 0x01,
    /* Ph1 */ 0x06, 0x06, 0x06, 0x06, 0x02,
    /* Ph2 */ 0x14, 0x14, 0x14, 0x14, 0x01,
    /* Ph3 */ 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Ph4 */ 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Ph5 */ 0x14, 0x14, 0x14, 0x3B, 0x00,
    /* Ph6 */ 0x14, 0x14, 0x14, 0x3B, 0x00,
};

const uint8_t *eink_lut_flush_red(void) { return lut_flush_red; }

/* Virtual soft-tone LUT buffer.
 * Built on demand from lut_tone_dark / lut_tone_light by patching only the
 * Phase-0 timing bytes (indices 35 & 36 = TA and TB).
 * TONE_DARK/LIGHT use TA=TB=2 (4 subframes, ~60ms, str≈0.36).
 * TONE_SOFT_DARK/LIGHT use TA=TB=1 (2 subframes, ~30ms, str≈0.18).
 * No extra static RAM — one shared 70-byte scratch buffer. */
static uint8_t lut_soft_buf[EINK_LUT_SIZE];

/* ── Virtual LUT slots ────────────────────────────────────────────────────────
 * Up to VLUT_MAX_SLOTS runtime-defined LUTs that are delta-patches over any
 * built-in base mode. Defined via BLE command "VLUT:" within a session and
 * cleared on VLUT:CLEAR or next reboot. Each slot stores:
 *   - base mode index (0..9 = same as partial mode enum)
 *   - patch pairs: up to VLUT_MAX_PATCHES (offset, value) per slot
 * At LUT selection time: base table is copied to lut_soft_buf, then patches
 * applied. Compact: only storing diffs, no full 70-byte copies per slot.
 */
#define VLUT_MAX_SLOTS   8
#define VLUT_MAX_PATCHES 16

typedef struct {
    uint8_t base_mode;                 /* index into partial_mode enum */
    uint8_t patch_count;               /* 0 = slot empty/unused */
    struct { uint8_t off; uint8_t val; } patches[VLUT_MAX_PATCHES];
} vlut_slot_t;

static vlut_slot_t vlut_slots[VLUT_MAX_SLOTS];
static int vlut_active_slot = -1;  /* -1 = no virtual slot active */

void eink_lut_vlut_clear(void) {
    memset(vlut_slots, 0, sizeof(vlut_slots));
    vlut_active_slot = -1;
}

int eink_lut_vlut_define(int slot, uint8_t base_mode,
                         const uint8_t *offsets, const uint8_t *values, int count) {
    if (slot < 0 || slot >= VLUT_MAX_SLOTS) return -1;
    if (count < 0 || count > VLUT_MAX_PATCHES) return -2;
    vlut_slots[slot].base_mode = base_mode;
    vlut_slots[slot].patch_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        vlut_slots[slot].patches[i].off = offsets[i];
        vlut_slots[slot].patches[i].val = values[i];
    }
    return 0;
}

int eink_lut_vlut_get_count(void) { return VLUT_MAX_SLOTS; }

bool eink_lut_vlut_slot_defined(int slot) {
    return (slot >= 0 && slot < VLUT_MAX_SLOTS && vlut_slots[slot].patch_count > 0);
}

void eink_lut_vlut_activate(int slot) {
    vlut_active_slot = (slot >= 0 && slot < VLUT_MAX_SLOTS) ? slot : -1;
}

int eink_lut_vlut_active(void) { return vlut_active_slot; }

/* Get base LUT pointer by mode index (does NOT apply patches). */
static const uint8_t *base_lut_for_mode(uint8_t mode) {
    switch (mode) {
        case 0: return lut_turbo;
        case 1: return lut_balanced;
        case 2: return lut_stable;
        case 3: return base_default ? base_default : lut_balanced;
        case 4: return lut_tone_dark;
        case 5: return lut_tone_light;
        case 6: return lut_tone_bidir_fast;
        case 7: return lut_tone_bidir;
        /* 8,9 are themselves derived — use their base */
        case 8: return lut_tone_dark;
        case 9: return lut_tone_light;
        default: return lut_balanced;
    }
}

static const uint8_t *make_vlut(int slot) {
    vlut_slot_t *s = &vlut_slots[slot];
    const uint8_t *base = base_lut_for_mode(s->base_mode);
    memcpy(lut_soft_buf, base, EINK_LUT_SIZE);
    for (int i = 0; i < s->patch_count; i++) {
        if (s->patches[i].off < EINK_LUT_SIZE) {
            lut_soft_buf[s->patches[i].off] = s->patches[i].val;
        }
    }
    return lut_soft_buf;
}

static const uint8_t *make_soft_lut(const uint8_t *base)
{
    memcpy(lut_soft_buf, base, EINK_LUT_SIZE);
    lut_soft_buf[35] = 0x01;   /* Ph0 TA = 1 subframe */
    lut_soft_buf[36] = 0x01;   /* Ph0 TB = 1 subframe */
    return lut_soft_buf;
}

const uint8_t *eink_lut_select_partial(void) {
    /* Virtual slot takes priority if active and defined */
    if (!use_custom && vlut_active_slot >= 0 &&
        eink_lut_vlut_slot_defined(vlut_active_slot)) {
        return make_vlut(vlut_active_slot);
    }
    if (use_custom) return base_working ? base_working : lut_balanced;
    switch (current_mode) {
        case EINK_LUT_MODE_TURBO:    return lut_turbo;
        case EINK_LUT_MODE_BALANCED: return lut_balanced;
        case EINK_LUT_MODE_STABLE:   return lut_stable;
        case EINK_LUT_MODE_CLEAN:    return base_default ? base_default : lut_balanced;
        case EINK_LUT_MODE_TONE_DARK:        return lut_tone_dark;
        case EINK_LUT_MODE_TONE_LIGHT:       return lut_tone_light;
        case EINK_LUT_MODE_TONE_BIDIR_FAST:  return lut_tone_bidir_fast;
        case EINK_LUT_MODE_TONE_BIDIR:       return lut_tone_bidir;
        case EINK_LUT_MODE_TONE_SOFT_DARK:   return make_soft_lut(lut_tone_dark);
        case EINK_LUT_MODE_TONE_SOFT_LIGHT:  return make_soft_lut(lut_tone_light);
        default:                             return lut_balanced;
    }
}
