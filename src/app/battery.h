#ifndef APP_BATTERY_H
#define APP_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the battery measurement module (ADC)
 * 
 * @return 0 on success, negative error code otherwise
 */
int battery_init(void);

/** Returned by battery_read_mv() when the sample could not be taken. Negative
 *  so it can never be mistaken for a low-but-valid voltage. */
#define BATTERY_READ_ERROR (-1)

/**
 * @brief Read battery voltage in millivolts
 *
 * @return Voltage in mV, or BATTERY_READ_ERROR if the ADC read failed. Callers
 *         must skip the sample on error rather than treating it as 0 mV.
 */
int battery_read_mv(void);

/* ── Low-battery protection ──────────────────────────────────────────────────
 *
 * ADC saturates at 3600 mV (gain 1/6, ref 0.6V), so we can't see charge above
 * that. We track voltage DYNAMICS: if voltage was stable at 3600 (fully charged
 * or USB/LBP powered) and then starts dropping, that signals battery discharge.
 *
 * States:
 *   BATT_OK         — normal operation
 *   BATT_LOW        — voltage dropping, inhibit display updates, show warning
 *   BATT_CRITICAL   — prepare final screen, disable BLE, enter system-off
 *
 * Thresholds (tunable):
 *   BATT_LOW_MV       3300   — enter low-power mode (no display updates)
 *   BATT_CRITICAL_MV  3100   — show farewell screen, then deep sleep
 *   BATT_RECOVER_MV   3400   — hysteresis: recover from BATT_LOW if rises back
 *
 * The monitor ONLY triggers if voltage has been stable ≥3500 mV and then
 * starts falling. If the device boots at ~3.3V (LBP/programmer), it stays
 * in BATT_OK — external supply assumed.
 */

#define BATT_LOW_MV        3300
#define BATT_CRITICAL_MV   3100
#define BATT_RECOVER_MV    3400
#define BATT_HISTORY_LEN   8

/** ADC full scale (gain 1/6, ref 0.6 V) — the highest voltage readable. */
#define BATTERY_FULL_MV    3600

typedef enum {
    BATT_OK = 0,
    BATT_LOW,
    BATT_CRITICAL,
    BATT_SHUTDOWN,   /* terminal: waiting for power cycle */
} battery_state_t;

/**
 * @brief Feed a new voltage sample into the battery monitor.
 *        Call periodically (e.g., every screensaver cycle, ~1/min).
 *        Returns the current battery protection state.
 */
battery_state_t battery_monitor_update(int mv);

/**
 * @brief Get current battery protection state without updating.
 */
battery_state_t battery_get_state(void);

/**
 * @brief Charge percentage for a voltage sample, for the on-screen gauge.
 *        0 % at 2000 mV, 100 % at 3000 mV. NOTE: that span sits below the
 *        protection thresholds above, so the gauge reads full until the tag
 *        shuts down — see the comment in battery.c before changing it.
 */
int battery_percent(int mv);

/**
 * @brief Returns true if display updates should be inhibited (BATT_LOW+).
 */
bool battery_display_inhibited(void);

#endif // APP_BATTERY_H
