#ifndef POWER_PROFILE_H
#define POWER_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * The sleep profile: everything a host can trade for battery life, in one
 * persisted record with one command (PWR) to change it, so the display thread
 * and the radio read a single source of truth instead of growing switches.
 *
 * Redraw intervals are minutes and align to the wall clock — with 5 the clock
 * redraws at :00, :05, :10 ... and shows the time as of that moment. 0 means the
 * schedule never redraws (host commands still do). The night window is
 * [from, to) in hours and may wrap midnight; from == to means there is no
 * night. Night is decided on the tag's own clock, so it is only as right as the
 * last time sync.
 */
struct power_profile {
	uint8_t tick_day_min;    /* 0..60 */
	uint8_t tick_night_min;  /* 0..60 */
	uint8_t night_from_h;    /* 0..23 */
	uint8_t night_to_h;      /* 0..23 */
	uint8_t adv_clock_s;     /* idle advertising interval, 1..10 s, clock mode */
	uint8_t adv_picture_s;   /* the same with the screensaver off (picture mode) */
};

#define POWER_PROFILE_MAX_TICK_MIN 60
#define POWER_PROFILE_MAX_ADV_S    10
/* Longest the display thread sleeps in one go whatever the schedule says:
 * battery protection and the stats roll-up ride on its wake-ups. */
#define POWER_PROFILE_MAX_SLEEP_S  (15 * 60)

/* What the tag runs on: a chemistry, how the cells are wired and what one cell
 * holds. Chemistry gives a rest-voltage curve (state of charge from the pack
 * voltage) and a nominal per-cell voltage; series x parallel turn per-cell
 * numbers into pack numbers; the epoch is the cumulative energy counter at the
 * moment this pack went in, so "used since install" restarts at zero without
 * touching the all-time total. cell_mah 0 = unknown, no days-left estimate.
 *
 * One hard limit sits under all of this: the SAADC measures VDD at gain 1/6
 * against 0.6 V and saturates at 3600 mV. A 1S Li-ion pack (3.0-4.2 V) is
 * therefore only visible in the lower half of its curve; 2xAA, coin cells and
 * anything else that lives under 3.6 V is visible end to end. When the reading
 * sits at the ceiling the curve result is reported as a lower bound. */
enum power_chem {
	POWER_CHEM_UNKNOWN  = 0,   /* no curve, no nominal: capacity only */
	POWER_CHEM_LIION    = 1,   /* Li-ion / Li-Po, 3.0-4.2 V, nominal 3.7 */
	POWER_CHEM_ALKALINE = 2,   /* 1.5 V cells (AA/AAA/LR44...), 1.6 -> 0.9 */
	POWER_CHEM_LIFEPO4  = 3,   /* 2.5-3.65 V, nominal 3.2 */
	POWER_CHEM_NIMH     = 4,   /* 1.0-1.4 V, nominal 1.2 */
	POWER_CHEM_LICOIN   = 5,   /* CR2032/CR2450 LiMnO2: 3.0 V flat, cliff ~2.0 */
	POWER_CHEM_LIFES2   = 6,   /* 1.5 V lithium AA/AAA (LiFeS2): 1.8 -> 1.2, flat */
	POWER_CHEM_MAINS    = 7,   /* USB / mains: nothing depletes, no estimate */
	POWER_CHEM_COUNT
};

struct power_battery {
	uint8_t  chem;             /* enum power_chem */
	uint8_t  series;           /* cells in series, 1..4 */
	uint8_t  parallel;         /* strings in parallel, 1..4 */
	uint8_t  _pad;
	uint16_t cell_mah;         /* nominal capacity of ONE cell, 0 = unknown */
	uint16_t _pad2;
	uint64_t epoch_uah_x1000;  /* persist energy total when it was installed */
};

#define POWER_BATT_MAX_CELLS 4
#define POWER_ADC_CEILING_MV 3600   /* SAADC gain 1/6 x 0.6 V reference */

const struct power_profile *power_profile_get(void);
const struct power_battery *power_battery_get(void);

/* The display mode, persisted under "pwr/m" — in flash, so it survives a
 * reboot AND a battery change. A tag left showing a picture comes back showing
 * it when someone swaps the cell years later, instead of waking into the clock
 * and painting over the image. true = clock/screensaver, false = picture.
 *
 * Only a deliberate choice is stored (SAVER, SS:, a pushed picture); the
 * transient takeovers — CLEAN, CLS, the DFU screens, the test patterns — turn
 * the screensaver off without touching what is remembered, or a normal update
 * would leave the tag stuck on its "UPDATE COMPLETE" screen forever. */
bool power_display_saver_get(void);
void power_display_saver_store(bool clock_mode);

/* Record the pack. new_battery also moves the epoch to now, i.e. "used since
 * install" restarts at zero. Persists under "pwr/b". -EINVAL out of range. */
int power_battery_set(uint8_t chem, uint8_t series, uint8_t parallel,
		      uint16_t cell_mah, bool new_battery);

/* Pack capacity (cell x parallel), mAh; 0 = unknown. */
uint32_t power_battery_capacity_mah(const struct power_battery *b);

/* Pack nominal voltage (per-cell nominal x series), mV; 0 for no chemistry. */
int power_battery_nominal_mv(const struct power_battery *b);

/* State of charge from the pack voltage by the chemistry's rest-voltage curve,
 * 0..100; -1 when the chemistry has no curve. *lower_bound is set when @p mv
 * sits at the ADC ceiling: the pack is at least this full, maybe fuller. */
int power_battery_soc_from_mv(const struct power_battery *b, int mv, bool *lower_bound);

/* mAh drawn since the battery epoch, by the estimator. */
uint32_t power_battery_used_mah(void);

/* The profile "everything on, nothing saved": mesh RX on, a redraw every
 * minute day and night, 2 s advertising. What the tag did before any of
 * this existed; the yardstick every estimate is compared against. */
const struct power_profile *power_profile_baseline(void);

/* Validate, persist and apply. 0, or -EINVAL for an out-of-range field. */
int power_profile_set(const struct power_profile *p);

/* Push the radio side of the profile into the BLE service for the display mode
 * in force. Call once settings are loaded, and whenever the mode flips. */
void power_profile_apply(void);

bool power_profile_is_night(int hour);

/* Redraw interval in force at this hour, minutes; 0 = none. */
int power_profile_tick_minutes(int hour);

/* Seconds until the next redraw boundary for wall time @p t, or -1 when the
 * schedule in force never redraws. */
int power_profile_seconds_to_tick(const struct tm *t);

#endif /* POWER_PROFILE_H */
