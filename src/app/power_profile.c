#include "power_profile.h"
#include "display_manager.h"
#include "../ble/ble_service.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <errno.h>
#include <string.h>

/* Defaults: the clock as it always was by day, one redraw in five at night
 * (23:00-07:00), the 2 s idle advertising it has had since 3.4.1 — and 5 s for
 * a picture, which nobody is looking for in a hurry. */
static struct power_profile profile = {
	.tick_day_min   = 1,
	.tick_night_min = 5,
	.night_from_h   = 23,
	.night_to_h     = 7,
	.adv_clock_s    = 2,
	.adv_picture_s  = 5,
};

const struct power_profile *power_profile_get(void)
{
	return &profile;
}

static bool valid(const struct power_profile *p)
{
	return p->tick_day_min <= POWER_PROFILE_MAX_TICK_MIN &&
	       p->tick_night_min <= POWER_PROFILE_MAX_TICK_MIN &&
	       p->night_from_h < 24 && p->night_to_h < 24 &&
	       p->adv_clock_s >= 1 && p->adv_clock_s <= POWER_PROFILE_MAX_ADV_S &&
	       p->adv_picture_s >= 1 && p->adv_picture_s <= POWER_PROFILE_MAX_ADV_S;
}

bool power_profile_is_night(int hour)
{
	const struct power_profile *p = &profile;

	if (p->night_from_h == p->night_to_h) {
		return false;
	}
	if (p->night_from_h < p->night_to_h) {
		return hour >= p->night_from_h && hour < p->night_to_h;
	}
	/* wraps midnight, e.g. 23..7 */
	return hour >= p->night_from_h || hour < p->night_to_h;
}

int power_profile_tick_minutes(int hour)
{
	return power_profile_is_night(hour) ? profile.tick_night_min
					    : profile.tick_day_min;
}

int power_profile_seconds_to_tick(const struct tm *t)
{
	int interval = power_profile_tick_minutes(t->tm_hour);

	if (interval <= 0) {
		return -1;
	}
	int period = interval * 60;
	int sod = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
	int left = period - (sod % period);

	return left < 1 ? 1 : left;
}

void power_profile_apply(void)
{
	bool picture = !display_manager_is_screensaver_active();

	ble_service_set_idle_adv_interval_s(picture ? profile.adv_picture_s
						    : profile.adv_clock_s);
}

int power_profile_set(const struct power_profile *p)
{
	if (!valid(p)) {
		return -EINVAL;
	}
	profile = *p;
	power_profile_apply();
	/* Wake the display thread so it re-plans its sleep on the new schedule
	 * (and redraws once, which doubles as the visible acknowledgement). */
	display_manager_force_update();
	return settings_save_one("pwr/p", &profile, sizeof(profile));
}

static int pwr_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "p", NULL)) {
		/* A shorter blob from an older layout keeps today's defaults for
		 * whatever it does not carry; a longer one is truncated. */
		struct power_profile tmp = profile;
		size_t n = MIN(len, sizeof(tmp));

		if (read_cb(cb_arg, &tmp, n) >= 0 && valid(&tmp)) {
			profile = tmp;
			return 0;
		}
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(pwrcfg, "pwr", NULL, pwr_settings_set, NULL, NULL);
