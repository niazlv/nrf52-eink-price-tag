#ifndef SSD1675A_CONFIG_H
#define SSD1675A_CONFIG_H

/*
 * Panel configuration for the SSD1675A driver.
 *
 * Every value is a plain #ifndef default, so there are three ways to change
 * one without editing this file:
 *   - define it on the compiler command line (-DSSD1675A_WIDTH=104), or
 *   - define it in your build system's global config header, or
 *   - define SSD1675A_CONFIG_FILE="my_panel.h" and put your overrides there.
 *
 * The defaults describe the 2.9" 128x296 B/W/R module this repo drives. Other
 * SSD16xx panels differ mainly in geometry and in the scan-timing block below;
 * the register writes are derived from these values, not hardcoded.
 */

#ifdef SSD1675A_CONFIG_FILE
#include SSD1675A_CONFIG_FILE
#endif

/* ── Geometry ───────────────────────────────────────────────────────────── */

/** Panel width in pixels. Must be a multiple of 8 (one RAM byte = 8 columns). */
#ifndef SSD1675A_WIDTH
#define SSD1675A_WIDTH 128
#endif

/** Panel height in pixels, i.e. the number of driven gate lines. */
#ifndef SSD1675A_HEIGHT
#define SSD1675A_HEIGHT 296
#endif

/* ── Waveform ───────────────────────────────────────────────────────────── */

/**
 * Upload the driver's own waveform table (command 0x32) at init.
 *
 * Set to 0 to run the controller's OTP waveform instead. The OTP table is
 * often cleaner out of the box, but it is fixed: no partial-update modes, no
 * tone modes, no LUT editing — everything eink_lut.h offers needs this at 1.
 */
#ifndef SSD1675A_USE_CUSTOM_LUT
#define SSD1675A_USE_CUSTOM_LUT 1
#endif

/**
 * VCOM register (0x2C) value. Trades contrast against ghosting and is panel-
 * and batch-specific; sweep it if the display looks washed out or leaves
 * residue. Changeable at runtime via ssd1675a_set_vcom_register().
 */
#ifndef SSD1675A_VCOM_DEFAULT
#define SSD1675A_VCOM_DEFAULT 0x68
#endif

/* ── Panel scan timing ──────────────────────────────────────────────────── */
/* These four go straight into their registers. They come from the vendor's
 * init sequence for a given panel — copy them from your module's datasheet or
 * reference code rather than guessing. */

/** Analog block control (0x74). */
#ifndef SSD1675A_ANALOG_BLOCK
#define SSD1675A_ANALOG_BLOCK 0x54
#endif

/** Digital block control (0x7E). */
#ifndef SSD1675A_DIGITAL_BLOCK
#define SSD1675A_DIGITAL_BLOCK 0x3B
#endif

/** Dummy line period (0x3A). */
#ifndef SSD1675A_DUMMY_LINE
#define SSD1675A_DUMMY_LINE 0x35
#endif

/** Gate line width (0x3B). */
#ifndef SSD1675A_GATE_WIDTH
#define SSD1675A_GATE_WIDTH 0x04
#endif

/** Border waveform control (0x3C). Sets what the frame around the image does. */
#ifndef SSD1675A_BORDER_WAVEFORM
#define SSD1675A_BORDER_WAVEFORM 0x33
#endif

/** Source driving voltage (0x04): VSH1, VSH2, VSL. */
#ifndef SSD1675A_SOURCE_VSH1
#define SSD1675A_SOURCE_VSH1 0x41
#endif
#ifndef SSD1675A_SOURCE_VSH2
#define SSD1675A_SOURCE_VSH2 0xA8
#endif
#ifndef SSD1675A_SOURCE_VSL
#define SSD1675A_SOURCE_VSL 0x32
#endif

/* ── Driver behaviour ───────────────────────────────────────────────────── */

/**
 * Give up on a BUSY wait after this long. A full B/W/R refresh takes ~9 s, so
 * the default leaves plenty of headroom; the timeout only exists so a panel
 * that never releases BUSY (bad wiring, dead supply) degrades into a wrong
 * picture instead of a hung thread.
 */
#ifndef SSD1675A_BUSY_TIMEOUT_MS
#define SSD1675A_BUSY_TIMEOUT_MS 8000
#endif

/** BUSY polling interval. Lower catches completion sooner, at more wakeups. */
#ifndef SSD1675A_BUSY_POLL_MS
#define SSD1675A_BUSY_POLL_MS 2
#endif

/* ── Derived — not user-serviceable ─────────────────────────────────────── */

/** Bytes in one RAM plane (the driver never allocates these; callers do). */
#define SSD1675A_RAM_BYTES ((SSD1675A_WIDTH * SSD1675A_HEIGHT) / 8)

/** Last RAM X address, in bytes. */
#define SSD1675A_RAM_X_END ((SSD1675A_WIDTH / 8) - 1)

/** Last RAM Y address, in gate lines. */
#define SSD1675A_RAM_Y_END (SSD1675A_HEIGHT - 1)

#endif /* SSD1675A_CONFIG_H */
