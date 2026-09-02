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

const struct power_profile *power_profile_get(void);

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
