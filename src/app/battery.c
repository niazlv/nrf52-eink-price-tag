#include "battery.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/pm/pm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

// ADC Channel Config
// Input logic: Measure VDD directly if possible via Internal Input.
// Gain 1/6, Ref Internal (0.6V), 10 bit.
// Max Input: 0.6V * 6 = 3.6V.
// Range: 0..1023 maps to 0..3.6V

static struct adc_channel_cfg channel_cfg = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .input_positive = SAADC_CH_PSELP_PSELP_VDD // Measure VDD
};

int battery_init(void) {
    if (!device_is_ready(adc_dev)) {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }
    
    int err = adc_channel_setup(adc_dev, &channel_cfg);
    if (err) {
        LOG_ERR("ADC channel setup failed: %d", err);
        return err;
    }

    return 0;
}

int battery_read_mv(void) {
    /* Per-call sequence + buffer: several threads sample the battery (display,
     * BLE commands), and a shared adc_sequence would let one publish another's
     * result. */
    int16_t sample = 0;
    struct adc_sequence seq = {
        .channels    = BIT(0),
        .buffer      = &sample,
        .buffer_size = sizeof(sample),
        .resolution  = 10,
    };

    if (!device_is_ready(adc_dev)) {
        return BATTERY_READ_ERROR;
    }

    int err = adc_read(adc_dev, &seq);
    if (err) {
        /* Never report 0 mV here: the caller's protection state machine reads
         * anything at or below BATT_CRITICAL_MV as "shut down now", so a single
         * transient ADC failure would power off a fully charged device. */
        LOG_ERR("ADC read failed: %d", err);
        return BATTERY_READ_ERROR;
    }

    // 10 bit resolution, Ref 0.6V, Gain 1/6.
    // Val = (Input / 3.6) * 1023
    // Input = Val * 3.6 / 1023
    // mV = Val * 3600 / 1023
    int32_t val = sample;
    if (val < 0) val = 0;

    return (val * 3600) / 1023;
}

/* ── Low-battery protection monitor ─────────────────────────────────────────
 *
 * Logic:
 * - Keep a rolling history of voltage samples.
 * - If we've seen ≥4 samples at or above 3500 mV ("was high"), we know we're
 *   on a real battery that was charged.
 * - If the device boots with voltage already below 3500 mV, we assume external
 *   supply (LBP/programmer) and never trigger protection.
 * - Once "was high" is set and voltage drops below BATT_LOW_MV → BATT_LOW.
 * - If voltage recovers above BATT_RECOVER_MV → back to BATT_OK (hysteresis).
 * - Below BATT_CRITICAL_MV → BATT_CRITICAL (one-way, no recovery).
 */

#define BATT_WAS_HIGH_THRESHOLD  3500
#define BATT_WAS_HIGH_COUNT      4

static battery_state_t batt_state = BATT_OK;
static int batt_history[BATT_HISTORY_LEN];
static int batt_history_idx = 0;
static int batt_history_count = 0;
static int batt_high_seen = 0;    /* how many samples were ≥3500 */
static bool batt_was_high = false; /* latched: battery was definitely charged */

battery_state_t battery_get_state(void)
{
    return batt_state;
}

int battery_percent(int mv)
{
    /* Moved here from display_manager so the gauge sits next to the protection
     * thresholds — but the 2.0–3.0 V mapping is UNCHANGED from where it lived
     * before, deliberately.
     *
     * Known inconsistency, left as-is because the right curve depends on the
     * cell and the board: this reaches 100 % at 3000 mV, which is below
     * BATT_CRITICAL_MV (3100), so a discharging tag shows a full gauge right
     * up to the shutdown screen. Re-anchoring it to
     * BATT_CRITICAL_MV..BATTERY_FULL_MV would fix that, but it also pins any
     * board running off a regulated 3.0/3.3 V rail (LBP/programmer, which
     * battery.h documents as an expected setup) at 0–40 % forever. Needs a
     * decision on real hardware, not a guess. */
    if (mv < 0) {
        return 0;   /* failed read — show empty rather than stale */
    }
    if (mv > 3000) {
        return 100;
    }
    if (mv < 2000) {
        return 0;
    }
    return (mv - 2000) / 10;
}

bool battery_display_inhibited(void)
{
    return batt_state >= BATT_LOW;
}

battery_state_t battery_monitor_update(int mv)
{
    /* Don't transition out of terminal states */
    if (batt_state == BATT_SHUTDOWN) {
        return BATT_SHUTDOWN;
    }

    /* Feed history */
    batt_history[batt_history_idx] = mv;
    batt_history_idx = (batt_history_idx + 1) % BATT_HISTORY_LEN;
    if (batt_history_count < BATT_HISTORY_LEN) {
        batt_history_count++;
    }

    /* Track if we've seen high voltage (real battery, charged) */
    if (mv >= BATT_WAS_HIGH_THRESHOLD) {
        batt_high_seen++;
        if (batt_high_seen >= BATT_WAS_HIGH_COUNT) {
            batt_was_high = true;
        }
    }

    /* If device never saw high voltage, don't trigger protection.
     * This handles LBP/programmer/USB scenarios where VDD is ~3.3V from start. */
    if (!batt_was_high) {
        return batt_state;
    }

    /* State machine */
    switch (batt_state) {
    case BATT_OK:
        if (mv <= BATT_CRITICAL_MV) {
            batt_state = BATT_CRITICAL;
            LOG_WRN("Battery CRITICAL: %d mV — preparing shutdown", mv);
        } else if (mv <= BATT_LOW_MV) {
            /* Check trend: only trigger if voltage is actually falling.
             * Compare current with average of history. */
            int sum = 0;
            for (int i = 0; i < batt_history_count; i++) {
                sum += batt_history[i];
            }
            int avg = sum / batt_history_count;
            if (avg <= BATT_LOW_MV + 50) {
                /* Sustained low, not a transient dip during display update */
                batt_state = BATT_LOW;
                LOG_WRN("Battery LOW: %d mV (avg %d) — inhibiting display", mv, avg);
            }
        }
        break;

    case BATT_LOW:
        if (mv <= BATT_CRITICAL_MV) {
            batt_state = BATT_CRITICAL;
            LOG_WRN("Battery CRITICAL: %d mV — preparing shutdown", mv);
        } else if (mv >= BATT_RECOVER_MV) {
            /* Recovered (maybe USB plugged in, or transient) */
            batt_state = BATT_OK;
            LOG_INF("Battery recovered: %d mV", mv);
        }
        break;

    case BATT_CRITICAL:
        /* One-way: after farewell screen, transition to SHUTDOWN */
        batt_state = BATT_SHUTDOWN;
        break;

    default:
        break;
    }

    return batt_state;
}
