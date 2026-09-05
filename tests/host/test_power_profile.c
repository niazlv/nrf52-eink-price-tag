/* Host tests for src/app/power_profile.c: the sleep schedule, the night
 * window, the chemistry curves and the settings migrations. The module's
 * neighbours (display manager, BLE service, persisted stats) are stubbed
 * right here; Zephyr's settings API comes from stubs/. */
#include "test.h"
#include "power_profile.h"
#include <zephyr/settings/settings.h>
#include <errno.h>

/* ── neighbours ─────────────────────────────────────────────────────────── */
static bool     saver_active = true;
static int      force_updates;
static int      adv_pushed_s = -1;
static uint64_t energy_now;

bool display_manager_is_screensaver_active(void) { return saver_active; }
void display_manager_force_update(void) { force_updates++; }
void ble_service_set_idle_adv_interval_s(uint8_t seconds) { adv_pushed_s = seconds; }
uint64_t persist_get_energy_uah_x1000(void) { return energy_now; }

static struct tm at(int h, int m, int s)
{
	struct tm t;

	memset(&t, 0, sizeof(t));
	t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
	return t;
}

static int set_profile(int day, int night, int from, int to, int advc, int advp)
{
	struct power_profile p = {
		.tick_day_min = (uint8_t)day, .tick_night_min = (uint8_t)night,
		.night_from_h = (uint8_t)from, .night_to_h = (uint8_t)to,
		.adv_clock_s = (uint8_t)advc, .adv_picture_s = (uint8_t)advp,
	};
	return power_profile_set(&p);
}

/* ── night window ───────────────────────────────────────────────────────── */
static void test_night_window(void)
{
	T_ASSERT_EQ(set_profile(1, 5, 23, 7, 2, 5), 0);       /* wraps midnight */
	T_ASSERT(power_profile_is_night(23));
	T_ASSERT(power_profile_is_night(0));
	T_ASSERT(power_profile_is_night(6));
	T_ASSERT(!power_profile_is_night(7));                 /* [from, to) */
	T_ASSERT(!power_profile_is_night(12));
	T_ASSERT(!power_profile_is_night(22));
	T_ASSERT_EQ(power_profile_tick_minutes(3), 5);
	T_ASSERT_EQ(power_profile_tick_minutes(12), 1);

	T_ASSERT_EQ(set_profile(1, 5, 9, 17, 2, 5), 0);       /* daytime "night" */
	T_ASSERT(!power_profile_is_night(8));
	T_ASSERT(power_profile_is_night(9));
	T_ASSERT(power_profile_is_night(16));
	T_ASSERT(!power_profile_is_night(17));

	T_ASSERT_EQ(set_profile(1, 5, 4, 4, 2, 5), 0);        /* from == to: no night */
	for (int h = 0; h < 24; h++) {
		T_ASSERT(!power_profile_is_night(h));
	}
}

/* ── seconds to the next boundary ───────────────────────────────────────── */
static void test_seconds_to_tick(void)
{
	struct tm t;

	T_ASSERT_EQ(set_profile(1, 1, 0, 0, 2, 5), 0);
	t = at(12, 30, 15); T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 45);
	t = at(12, 30, 0);  T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 60);   /* on the boundary: a full period */
	t = at(12, 30, 59); T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 1);

	T_ASSERT_EQ(set_profile(5, 5, 0, 0, 2, 5), 0);         /* :00 :05 :10 ... */
	t = at(12, 3, 0);   T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 120);
	t = at(12, 5, 0);   T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 300);
	t = at(23, 58, 30); T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 90);   /* across midnight */

	T_ASSERT_EQ(set_profile(1, 15, 23, 7, 2, 5), 0);       /* night: quarter hours */
	t = at(2, 7, 0);    T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 8 * 60);
	t = at(14, 7, 0);   T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 60);

	T_ASSERT_EQ(set_profile(0, 0, 0, 0, 2, 5), 0);         /* never by schedule */
	t = at(12, 0, 0);   T_ASSERT_EQ(power_profile_seconds_to_tick(&t), -1);
	T_ASSERT_EQ(set_profile(1, 0, 23, 7, 2, 5), 0);        /* only at night */
	t = at(1, 0, 0);    T_ASSERT_EQ(power_profile_seconds_to_tick(&t), -1);
	t = at(13, 0, 0);   T_ASSERT_EQ(power_profile_seconds_to_tick(&t), 60);
}

/* ── validation / apply / persist ───────────────────────────────────────── */
static void test_profile_set_validates_and_applies(void)
{
	T_ASSERT_EQ(set_profile(1, 1, 23, 7, 2, 5), 0);
	int saves = settings_stub_save_count;
	int forces = force_updates;

	T_ASSERT_EQ(set_profile(61, 1, 23, 7, 2, 5), -EINVAL);   /* day too long */
	T_ASSERT_EQ(set_profile(1, 1, 24, 7, 2, 5), -EINVAL);    /* hour 24 */
	T_ASSERT_EQ(set_profile(1, 1, 23, 7, 0, 5), -EINVAL);    /* adv 0 */
	T_ASSERT_EQ(set_profile(1, 1, 23, 7, 2, 11), -EINVAL);   /* adv > 10 */
	T_ASSERT_EQ(settings_stub_save_count, saves);            /* nothing written */
	T_ASSERT_EQ(force_updates, forces);                      /* nothing redrawn */
	T_ASSERT_EQ(power_profile_get()->tick_day_min, 1);       /* unchanged */

	/* A valid change persists under pwr/p, redraws once, and pushes the
	 * advertising interval of the mode in force. */
	saver_active = true;
	T_ASSERT_EQ(set_profile(2, 10, 22, 6, 3, 9), 0);
	T_ASSERT(strcmp(settings_stub_last_name, "pwr/p") == 0);
	T_ASSERT_EQ(settings_stub_last_len, sizeof(struct power_profile));
	T_ASSERT_EQ(force_updates, forces + 1);
	T_ASSERT_EQ(adv_pushed_s, 3);
	saver_active = false;
	power_profile_apply();
	T_ASSERT_EQ(adv_pushed_s, 9);
	saver_active = true;
}

static void test_baseline_is_everything_on(void)
{
	const struct power_profile *b = power_profile_baseline();

	T_ASSERT_EQ(b->tick_day_min, 1);
	T_ASSERT_EQ(b->tick_night_min, 1);
	T_ASSERT_EQ(b->night_from_h, b->night_to_h);
	T_ASSERT_EQ(b->adv_clock_s, 2);
}

/* ── chemistry curves ───────────────────────────────────────────────────── */
static void test_soc_curves(void)
{
	bool lb;

	T_ASSERT_EQ(power_battery_set(POWER_CHEM_LIION, 1, 1, 370, false), 0);
	const struct power_battery *b = power_battery_get();

	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3500, &lb), 15);
	T_ASSERT(!lb);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3550, &lb), 20);    /* interpolated */
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3000, &lb), 0);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 2500, &lb), 0);
	/* 1S Li-ion at or above the 3600 mV ADC ceiling: the reading is clamped
	 * and the result is only a lower bound — 3700 looks exactly like 4200. */
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 4200, &lb), 25);
	T_ASSERT(lb);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3700, &lb), 25);
	T_ASSERT(lb);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3600, &lb), 25);
	T_ASSERT(lb);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 0, &lb), -1);      /* no reading */
	T_ASSERT_EQ(power_battery_nominal_mv(b), 3700);
	T_ASSERT_EQ(power_battery_capacity_mah(b), 370);

	/* 2S1P alkaline: pack voltage halves into the per-cell curve. */
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_ALKALINE, 2, 2, 2000, false), 0);
	b = power_battery_get();
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 2400, &lb), 45);
	T_ASSERT(!lb);
	T_ASSERT_EQ(power_battery_soc_from_mv(b, 3200, &lb), 100);
	T_ASSERT_EQ(power_battery_nominal_mv(b), 3000);
	T_ASSERT_EQ(power_battery_capacity_mah(b), 4000);

	T_ASSERT_EQ(power_battery_set(POWER_CHEM_UNKNOWN, 1, 1, 0, false), 0);
	T_ASSERT_EQ(power_battery_soc_from_mv(power_battery_get(), 3000, &lb), -1);
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_MAINS, 1, 1, 0, false), 0);
	T_ASSERT_EQ(power_battery_soc_from_mv(power_battery_get(), 3000, &lb), -1);
	T_ASSERT_EQ(power_battery_nominal_mv(power_battery_get()), 0);
}

static void test_pack_validation_and_epoch(void)
{
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_COUNT, 1, 1, 100, false), -EINVAL);
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_LIION, 0, 1, 100, false), -EINVAL);
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_LIION, 1, 5, 100, false), -EINVAL);

	energy_now = 7000000;   /* 7 mAh drawn all-time */
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_LIION, 1, 1, 370, true), 0);
	T_ASSERT(strcmp(settings_stub_last_name, "pwr/b") == 0);
	T_ASSERT_EQ(power_battery_get()->epoch_uah_x1000, 7000000);
	T_ASSERT_EQ(power_battery_used_mah(), 0);
	energy_now = 9500000;
	T_ASSERT_EQ(power_battery_used_mah(), 2);        /* 2.5 mAh, whole mAh */
	energy_now = 1000;                               /* a recalibration went backwards */
	T_ASSERT_EQ(power_battery_used_mah(), 0);
	energy_now = 9500000;
	T_ASSERT_EQ(power_battery_set(POWER_CHEM_LIION, 1, 1, 400, false), 0);
	T_ASSERT_EQ(power_battery_get()->epoch_uah_x1000, 7000000);   /* not a new pack */
}

/* ── settings load: current and old layouts ─────────────────────────────── */
static void test_load_profile_blobs(void)
{
	T_ASSERT_EQ(set_profile(1, 1, 0, 0, 2, 2), 0);

	struct power_profile full = { 3, 20, 21, 8, 4, 7 };

	T_ASSERT_EQ(settings_stub_load("p", &full, sizeof(full)), 0);
	T_ASSERT_EQ(power_profile_get()->tick_night_min, 20);
	T_ASSERT_EQ(power_profile_get()->adv_picture_s, 7);

	/* An older, shorter blob keeps the current values for what it lacks. */
	uint8_t old4[4] = { 2, 10, 23, 7 };

	T_ASSERT_EQ(settings_stub_load("p", old4, sizeof(old4)), 0);
	T_ASSERT_EQ(power_profile_get()->tick_day_min, 2);
	T_ASSERT_EQ(power_profile_get()->night_to_h, 7);
	T_ASSERT_EQ(power_profile_get()->adv_clock_s, 4);       /* kept */

	/* A corrupt blob is refused and changes nothing. */
	struct power_profile bad = { 99, 1, 0, 0, 2, 2 };

	T_ASSERT_EQ(settings_stub_load("p", &bad, sizeof(bad)), -ENOENT);
	T_ASSERT_EQ(power_profile_get()->tick_day_min, 2);

	T_ASSERT_EQ(settings_stub_load("nope", &full, sizeof(full)), -ENOENT);
}

static void test_load_battery_blobs(void)
{
	struct power_battery cur = { .chem = POWER_CHEM_LIFEPO4, .series = 1, .parallel = 2,
				     .cell_mah = 1500, .epoch_uah_x1000 = 42 };

	T_ASSERT_EQ(settings_stub_load("b", &cur, sizeof(cur)), 0);
	T_ASSERT_EQ(power_battery_get()->parallel, 2);
	T_ASSERT_EQ(power_battery_get()->epoch_uah_x1000, 42);

	/* The first, one-day-old layout: {type, pad, mah, epoch} = 12 bytes.
	 * Type 2 (alkaline) and 4 (NiMH) were 2-cell packs. */
	struct __attribute__((packed)) { uint8_t type, pad; uint16_t mah; uint64_t epoch; } old;

	T_ASSERT_EQ(sizeof(old), 12);
	old.type = 2; old.pad = 0; old.mah = 2500; old.epoch = 7;
	T_ASSERT_EQ(settings_stub_load("b", &old, sizeof(old)), 0);
	T_ASSERT_EQ(power_battery_get()->chem, POWER_CHEM_ALKALINE);
	T_ASSERT_EQ(power_battery_get()->series, 2);
	T_ASSERT_EQ(power_battery_get()->parallel, 1);
	T_ASSERT_EQ(power_battery_get()->cell_mah, 2500);
	T_ASSERT_EQ(power_battery_get()->epoch_uah_x1000, 7);
	old.type = 1;
	T_ASSERT_EQ(settings_stub_load("b", &old, sizeof(old)), 0);
	T_ASSERT_EQ(power_battery_get()->series, 1);
	old.type = 9;                                            /* never a type */
	T_ASSERT_EQ(settings_stub_load("b", &old, sizeof(old)), -ENOENT);
	T_ASSERT_EQ(power_battery_get()->chem, POWER_CHEM_LIION); /* unchanged */

	cur.series = 9;                                          /* corrupt current blob */
	T_ASSERT_EQ(settings_stub_load("b", &cur, sizeof(cur)), -ENOENT);
	T_ASSERT_EQ(power_battery_get()->series, 1);
}

static void test_display_saver_flag(void)
{
	uint8_t pic = 0;

	T_ASSERT(power_display_saver_get());                    /* default: clock */
	T_ASSERT_EQ(settings_stub_load("m", &pic, 1), 0);
	T_ASSERT(!power_display_saver_get());

	int saves = settings_stub_save_count;

	power_display_saver_store(false);                       /* no change: no flash write */
	T_ASSERT_EQ(settings_stub_save_count, saves);
	power_display_saver_store(true);
	T_ASSERT_EQ(settings_stub_save_count, saves + 1);
	T_ASSERT(strcmp(settings_stub_last_name, "pwr/m") == 0);
	T_ASSERT(power_display_saver_get());
}

int main(void)
{
	T_RUN(test_night_window);
	T_RUN(test_seconds_to_tick);
	T_RUN(test_profile_set_validates_and_applies);
	T_RUN(test_baseline_is_everything_on);
	T_RUN(test_soc_curves);
	T_RUN(test_pack_validation_and_epoch);
	T_RUN(test_load_profile_blobs);
	T_RUN(test_load_battery_blobs);
	T_RUN(test_display_saver_flag);
	return t_report("power_profile");
}
