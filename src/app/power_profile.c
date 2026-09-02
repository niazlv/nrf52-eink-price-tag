#include "power_profile.h"
#include "display_manager.h"
#include "../ble/ble_service.h"
#include "persist.h"

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

static struct power_battery battery = { .series = 1, .parallel = 1 };

static const struct power_profile baseline = {
	.tick_day_min   = 1,
	.tick_night_min = 1,
	.night_from_h   = 0,
	.night_to_h     = 0,
	.adv_clock_s    = 2,
	.adv_picture_s  = 2,
};

/* Rest-voltage curves, per cell, ascending. Typical room-temperature figures
 * from the usual discharge charts, not measured on these tags: good enough to
 * tell "half" from "nearly flat", not to promise a percent. The interesting
 * shapes: Li-ion is a slope with a knee at the bottom, alkaline slides the
 * whole way, coin and LiFeS2 cells sit flat for most of their life and then
 * fall off a cliff — which is exactly why a voltage reading alone lies about
 * them and the coulomb estimate is kept alongside. */
struct curve_pt { uint16_t mv; uint8_t pct; };

static const struct curve_pt curve_liion[] = {
	{3000, 0}, {3300, 5}, {3500, 15}, {3600, 25}, {3700, 50},
	{3800, 65}, {3900, 78}, {4000, 88}, {4100, 95}, {4200, 100},
};
static const struct curve_pt curve_alkaline[] = {
	{900, 0}, {1000, 5}, {1100, 15}, {1150, 30}, {1200, 45},
	{1250, 60}, {1300, 75}, {1400, 90}, {1500, 97}, {1600, 100},
};
static const struct curve_pt curve_lifepo4[] = {
	{2500, 0}, {2900, 5}, {3100, 10}, {3200, 30}, {3250, 50},
	{3300, 80}, {3350, 95}, {3600, 100},
};
static const struct curve_pt curve_nimh[] = {
	{1000, 0}, {1100, 10}, {1150, 25}, {1200, 50}, {1250, 75},
	{1300, 90}, {1400, 100},
};
static const struct curve_pt curve_licoin[] = {
	{2000, 0}, {2400, 5}, {2600, 15}, {2800, 40}, {2900, 70},
	{2950, 85}, {3000, 95}, {3100, 100},
};
static const struct curve_pt curve_lifes2[] = {
	{1200, 0}, {1350, 10}, {1450, 40}, {1550, 70}, {1650, 90}, {1800, 100},
};

struct chem_info {
	const struct curve_pt *pts;
	uint8_t n;
	uint16_t nominal_mv;
};

static const struct chem_info chem_table[POWER_CHEM_COUNT] = {
	[POWER_CHEM_UNKNOWN]  = { NULL, 0, 0 },
	[POWER_CHEM_LIION]    = { curve_liion,    ARRAY_SIZE(curve_liion),    3700 },
	[POWER_CHEM_ALKALINE] = { curve_alkaline, ARRAY_SIZE(curve_alkaline), 1500 },
	[POWER_CHEM_LIFEPO4]  = { curve_lifepo4,  ARRAY_SIZE(curve_lifepo4),  3200 },
	[POWER_CHEM_NIMH]     = { curve_nimh,     ARRAY_SIZE(curve_nimh),     1200 },
	[POWER_CHEM_LICOIN]   = { curve_licoin,   ARRAY_SIZE(curve_licoin),   3000 },
	[POWER_CHEM_LIFES2]   = { curve_lifes2,   ARRAY_SIZE(curve_lifes2),   1500 },
	[POWER_CHEM_MAINS]    = { NULL, 0, 0 },
};

const struct power_profile *power_profile_get(void)
{
	return &profile;
}

const struct power_profile *power_profile_baseline(void)
{
	return &baseline;
}

const struct power_battery *power_battery_get(void)
{
	return &battery;
}

static bool battery_valid(const struct power_battery *b)
{
	return b->chem < POWER_CHEM_COUNT &&
	       b->series >= 1 && b->series <= POWER_BATT_MAX_CELLS &&
	       b->parallel >= 1 && b->parallel <= POWER_BATT_MAX_CELLS;
}

int power_battery_set(uint8_t chem, uint8_t series, uint8_t parallel,
		      uint16_t cell_mah, bool new_battery)
{
	struct power_battery b = battery;

	b.chem = chem;
	b.series = series;
	b.parallel = parallel;
	b.cell_mah = cell_mah;
	if (!battery_valid(&b)) {
		return -EINVAL;
	}
	if (new_battery) {
		b.epoch_uah_x1000 = persist_get_energy_uah_x1000();
	}
	battery = b;
	return settings_save_one("pwr/b", &battery, sizeof(battery));
}

uint32_t power_battery_capacity_mah(const struct power_battery *b)
{
	return (uint32_t)b->cell_mah * b->parallel;
}

int power_battery_nominal_mv(const struct power_battery *b)
{
	return chem_table[b->chem].nominal_mv * b->series;
}

int power_battery_soc_from_mv(const struct power_battery *b, int mv, bool *lower_bound)
{
	const struct chem_info *c = &chem_table[b->chem];

	*lower_bound = false;
	if (!c->pts || mv <= 0) {
		return -1;
	}
	if (mv >= POWER_ADC_CEILING_MV) {
		mv = POWER_ADC_CEILING_MV;
		*lower_bound = true;
	}
	int cell_mv = mv / b->series;

	if (cell_mv <= c->pts[0].mv) {
		return 0;
	}
	if (cell_mv >= c->pts[c->n - 1].mv) {
		return 100;
	}
	for (int i = 1; i < c->n; i++) {
		if (cell_mv <= c->pts[i].mv) {
			const struct curve_pt *a = &c->pts[i - 1], *z = &c->pts[i];
			return a->pct + (cell_mv - a->mv) * (z->pct - a->pct) / (z->mv - a->mv);
		}
	}
	return 100;
}

uint32_t power_battery_used_mah(void)
{
	uint64_t now = persist_get_energy_uah_x1000();

	/* A model recalibration can shrink the total below an older epoch;
	 * read that as "nothing measurable since", not as a negative number. */
	if (now <= battery.epoch_uah_x1000) {
		return 0;
	}
	return (uint32_t)((now - battery.epoch_uah_x1000) / 1000000U);
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
	if (settings_name_steq(name, "b", NULL)) {
		if (len == sizeof(battery)) {
			struct power_battery tmp;

			if (read_cb(cb_arg, &tmp, sizeof(tmp)) >= 0 && battery_valid(&tmp)) {
				battery = tmp;
				return 0;
			}
		} else if (len == 12) {
			/* The one-day-old first layout: {type, pad, mah, epoch}. The
			 * type numbers 1..4 carry over as chemistries; "mah" was the
			 * pack, which for the 2-cell chemistries means 2S1P. */
			struct { uint8_t type, pad; uint16_t mah; uint64_t epoch; } old;

			if (read_cb(cb_arg, &old, sizeof(old)) >= 0 && old.type <= POWER_CHEM_NIMH) {
				battery.chem = old.type;
				battery.series = (old.type == POWER_CHEM_ALKALINE ||
						  old.type == POWER_CHEM_NIMH) ? 2 : 1;
				battery.parallel = 1;
				battery.cell_mah = old.mah;
				battery.epoch_uah_x1000 = old.epoch;
				return 0;
			}
		}
		return -ENOENT;
	}
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
