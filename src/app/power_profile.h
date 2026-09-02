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

/* What the tag runs on. Type is informational (the web picks presets by it);
 * capacity feeds the days-left estimate; epoch is the cumulative energy
 * counter at the moment this battery went in, so "used" can restart at zero
 * without touching the all-time total. 0 mAh = unknown, no estimate. */
enum power_battery_type {
	POWER_BATT_UNKNOWN  = 0,
	POWER_BATT_LIION    = 1,   /* Li-ion / Li-Po cell */
	POWER_BATT_ALKALINE = 2,   /* 2xAA / 2xAAA alkaline */
	POWER_BATT_LIFEPO4  = 3,
	POWER_BATT_NIMH     = 4,
};

struct power_battery {
	uint8_t  type;             /* enum power_battery_type */
	uint8_t  _pad;
	uint16_t mah;              /* nominal capacity, 0 = unknown */
	uint64_t epoch_uah_x1000;  /* persist energy total when it was installed */
};

const struct power_profile *power_profile_get(void);
const struct power_battery *power_battery_get(void);

/* Record the battery. new_battery also moves the epoch to now, i.e. "used
 * since install" restarts at zero. Persists under "pwr/b". */
int power_battery_set(uint8_t type, uint16_t mah, bool new_battery);

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
