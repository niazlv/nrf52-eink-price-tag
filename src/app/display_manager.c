#include "display_manager.h"
#include "battery.h"
#include "display_screens.h"
#include "system_time.h"
#include "persist.h"
#include "power_profile.h"
#include "mesh.h"
#include "ble/ble_service.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <eink/ssd1675a.h>
#include <gfx/graphics.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_manager, LOG_LEVEL_INF);

/* Cleared when the display port fails to come up; every entry point below
 * bails out on it so a missing panel degrades to "no picture" instead of a
 * thread stuck in a busy-wait. */
static bool display_ready;

static bool screensaver_enabled = true;
static bool keep_display_on = false;
static bool streaming_active = false;
static bool stream_write_red_plane = false;
static int partial_mode_current = 1;

// DC-balance maintenance: after this many streaming partial frames, force one
// full 0xC7 cycle to prevent particle polarization / ghost burn-in.
// At ~230ms/frame (vstream TURBO, measured): 500 frames ≈ 1.9min between
// maintenance passes (~1-2s pause each).
#define STREAM_REFRESH_INTERVAL 500
static int stream_partial_count = 0;

/* Static saver power policy:
 * - first frame and periodic maintenance use a full refresh so the panel has
 *   a known clean baseline;
 * - minute ticks use the TURBO partial LUT so the panel can sleep quickly.
 */
/* Two independent triggers for the full refresh, because the old single redraw
 * counter was quietly doing two jobs at once.
 *
 * The clock is the schedule. Keying the schedule on a redraw counter meant
 * every unplanned redraw counted, and display_manager_force_update() fires from
 * eight command handlers plus display_manager_set_rotation() — so a phone
 * talking to the tag pulled the next ~9 s full cycle arbitrarily close, and
 * "every 20 minutes" really meant "every 20 redraws, however often those come".
 *
 * The count is the ghosting ceiling, and it has to stay. Partial waveforms do
 * not DC-balance the panel; only a full cycle does. Without a cap on CONSECUTIVE
 * partials a busy host could run hundreds of them inside one 20-minute window
 * and never clear the accumulated charge. The streaming path keeps the same
 * guard for the same reason (STREAM_REFRESH_INTERVAL).
 *
 * On an idle tag the minute tick hits the clock first and the ceiling never
 * binds; under host traffic the ceiling takes over.
 *
 * The interval is a day, not the twenty minutes it used to be. At three full
 * cycles an hour the maintenance alone cost ~5.6 mAh/day (72 x 32 mA x 8.7 s)
 * out of a measured 13.6 mAh/day — more than a third of the tag's whole budget
 * — and it bought little: what made long partial runs look bad was the red
 * plane coming up undefined after every power-down, and the driver now puts
 * that back to zero on the wake-up itself.
 *
 * It stays on the clock rather than going away because the red particles do
 * drift under the partial waveforms even though the partial path never writes
 * the red plane, so the panel still needs a cycle that drives and re-fixes red
 * now and then — that is what the red phases of the default table (71% of its
 * time) are there for. Once a day costs 0.08 mAh. */
#define STATIC_SAVER_FULL_INTERVAL 1440      /* minutes between full refreshes (24 h) */
#define STATIC_SAVER_MAX_PARTIALS  1440      /* ceiling on consecutive partials */
#define STATIC_SAVER_PARTIAL_MODE 0
/* Partial refreshes drawn since the last full one; reset by every full one. */
static int static_saver_partials_since_full = 0;
/* Uptime of the last full refresh; 0 = none yet, so the first frame is full. */
static int64_t static_saver_last_full_ms = 0;
/* Set by display_manager_request_full_update(), consumed by the next status
 * frame the screensaver actually draws.
 *
 * Neither trigger above can serve a host asking for a clean panel NOW, and one
 * has to: a partial refresh drives the B/W plane only, and every partial
 * waveform leaves LUT2/LUT3 at zero, so any pixel whose red-RAM bit is set gets
 * no drive at all and keeps whatever it physically was. Ghosting like that —
 * a screen that sat for a long time, a red plane left over from another
 * screen — is invisible to the partial path no matter how often it runs, and
 * only a full cycle rewrites both planes. UPDATE/APPLY is how a host asks.
 *
 * Written by the BLE RX / mesh threads, read and cleared by the screensaver
 * thread under display_lock, after the frame is committed to — a status frame
 * that gives up on the lock must leave the request standing. */
static bool full_refresh_requested = false;

/* Image mode only: someone rendered a frame into the buffers from another
 * thread and wants it on the panel. Set by display_manager_request_frame_update()
 * (the DFU screens), consumed by the display thread — so the BLE RX thread
 * never sits inside an ~8 s full refresh with SMP traffic queued behind it. */
static bool frame_update_requested = false;

static int screensaver_mode = SCREENSAVER_MODE_STATIC;

/* Rough current model for the on-screen mAh estimator. This is not a coulomb
 * counter — there is no shunt on the board — so the figure is only ever as good
 * as the numbers below, and the first set of them was badly out.
 *
 * Recalibrated 2026-09-01 against a fielded tag: it claimed 912 mAh over 67
 * days from a 370 mAh / 1.37 Wh cell that still measured 3.7 V on a meter,
 * i.e. it had drawn about half the pack. The old constants over-reported by
 * roughly 5x. The values below come from the 2026-08-23 power audit rather
 * than from a meter, so treat them as a much better estimate, not as truth;
 * a PPK2 run is what would make them real.
 */
#define POWER_BASE_SLEEP_UA              5   /* System ON idle, RAM retention + RTC, DCDC on */
#define POWER_BLE_ADV_IDLE_UA           10   /* 2-2.5 s interval */
#define POWER_BLE_ADV_FAST_UA          180   /* 100-150 ms: same model, ~18x the duty */
/* Mesh observer scan: duty × RX current. Keep in step with SCAN_WINDOW_MS /
 * SCAN_INTERVAL_MS in mesh.c (30/1000 = 3% of RX ~5.4 mA on DCDC). */
#define POWER_MESH_SCAN_UA             160
/* The four below still have no better source than the original guess. They only
 * run while a phone is connected or an animation plays — minutes a day against
 * the panel and idle terms — so their error barely moves the total. */
#define POWER_BLE_CONN_IDLE_UA         900
#define POWER_BLE_STREAM_UA           4500
#define POWER_DISPLAY_STANDBY_UA       400
#define POWER_DISPLAY_HV_HOLD_UA      8000
/* Panel draws ~3-8 mA while a refresh runs and the MCU k_sleeps through the
 * BUSY wait, so the average over the window is a few mA — not the tens of mA
 * the first model assumed. */
#define POWER_DISPLAY_PARTIAL_UA      5000
#define POWER_DISPLAY_FULL_UA         6000

/* Representative durations, used only to rebuild a historical total (below).
 * A partial is dominated by the ~600 ms cold HV charge; a full cycle is the
 * default waveform's own ~8.7 s. */
#define POWER_PARTIAL_MS               700
#define POWER_FULL_MS                 8700

/* Revision of the model above. Bump it whenever the constants change enough to
 * make an already-accumulated total meaningless: the next boot then re-estimates
 * that total from the two things the record does store honestly — cumulative
 * uptime and the full-refresh count — instead of carrying a figure built with
 * numbers we no longer believe. */
#define POWER_MODEL_VERSION              2

/* Rebuild what the model above would have accumulated over the life the record
 * actually remembers. Only two of its numbers are trustworthy — cumulative
 * uptime and the count of full refreshes — so the rest is reconstructed: the
 * idle floor ran for the whole time, each counted refresh cost one full cycle,
 * and every other minute of uptime cost one partial tick.
 *
 * Where that is wrong: the record does not say how long the tag sat in picture
 * mode (no ticks at all) or with mesh RX on (a much higher floor), so a picture
 * tag comes out over-estimated and a mesh one under-estimated. Both errors are
 * far smaller than the ~5x this replaces.
 *
 * Units: the accumulator counts µAh×1000, i.e. µA × ms / 3600. */
static uint64_t power_estimate_from_history(uint64_t uptime_sec, uint32_t fulls)
{
    const uint64_t idle_ua = POWER_BASE_SLEEP_UA + POWER_BLE_ADV_IDLE_UA;
    uint64_t minutes = uptime_sec / 60U;
    uint64_t partials = (minutes > fulls) ? (minutes - fulls) : 0U;

    uint64_t total = (idle_ua * uptime_sec * 1000U) / 3600U;
    total += ((uint64_t)POWER_DISPLAY_FULL_UA * POWER_FULL_MS / 3600U) * fulls;
    total += ((uint64_t)POWER_DISPLAY_PARTIAL_UA * POWER_PARTIAL_MS / 3600U) * partials;
    return total;
}

/* Average current the model predicts for a profile, in µA: the idle floor
 * (sleep + advertising at the profile's interval + mesh scan if on) plus the
 * day's redraws spread over 24 h. Picture mode has no redraws at all and
 * advertises at the picture interval. The daily full refresh is firmware
 * policy and goes in either way. Same constants as the running estimator,
 * so "days left" and the mAh counter cannot disagree with each other. */
int display_manager_estimate_avg_ua(const struct power_profile *p, bool mesh_rx,
                                    bool picture)
{
    int adv_s = picture ? p->adv_picture_s : p->adv_clock_s;
    int ua = POWER_BASE_SLEEP_UA + POWER_BLE_ADV_IDLE_UA * 2 / (adv_s < 1 ? 1 : adv_s);

    if (mesh_rx) {
        ua += POWER_MESH_SCAN_UA;
    }

    /* µAh per day from the panel */
    uint32_t uah_day = (uint32_t)POWER_DISPLAY_FULL_UA * POWER_FULL_MS / 3600000U;
    if (!picture) {
        int night_h = (p->night_from_h == p->night_to_h) ? 0
                    : (p->night_to_h - p->night_from_h + 24) % 24;
        int day_h = 24 - night_h;
        uint32_t ticks = 0;
        if (p->tick_day_min) {
            ticks += (uint32_t)day_h * 60 / p->tick_day_min;
        }
        if (p->tick_night_min) {
            ticks += (uint32_t)night_h * 60 / p->tick_night_min;
        }
        uah_day += ticks * ((uint32_t)POWER_DISPLAY_PARTIAL_UA * POWER_PARTIAL_MS / 3600000U);
    }
    return ua + (int)(uah_day / 24U);
}

/* Call once at boot, AFTER persist_post_settings() — before it, the flash
 * restore would overwrite whatever we adopt. */
void display_manager_recalibrate_energy(void)
{
    uint64_t before = persist_get_energy_uah_x1000();
    uint64_t rebuilt = power_estimate_from_history(persist_uptime_sec(),
                                                   persist_refreshes_total());

    if (persist_adopt_energy_model(POWER_MODEL_VERSION, rebuilt)) {
        LOG_INF("energy total re-estimated: %u -> %u mAh (model v%d)",
                (unsigned int)(before / 1000000U),
                (unsigned int)(rebuilt / 1000000U), POWER_MODEL_VERSION);
    }
}

/* The cumulative energy total lives in persist (retained RAM + flash) so it
 * survives a DFU reboot just like uptime; only the rate model and the
 * last-accounted timestamp are session-local here. */
static int64_t power_estimate_last_ms = 0;
static int power_estimate_current_ua = POWER_BASE_SLEEP_UA;

/* When true, the device sends TELE: lines after every display update so the
 * host app can track actual frame timing without polling. */
static bool tele_enabled = false;

void display_manager_set_tele_enabled(bool en) { tele_enabled = en; }
bool display_manager_get_tele_enabled(void)     { return tele_enabled; }

/* Forward declarations for public wrappers defined below static helpers */
static uint32_t power_estimate_get_mah_x1000(void);

uint32_t display_manager_get_energy_mah_x1000(void)
{
    return power_estimate_get_mah_x1000();
}

int display_manager_get_estimated_current_ua(void)
{
    return power_estimate_current_ua;
}

static int32_t lut_test_frame    = 0;
static int64_t lut_test_last_ms  = 0;
static int32_t lut_test_cur_ms   = 0;
static int32_t lut_test_min_ms   = 0;
static int32_t lut_test_max_ms   = 0;

// Semaphore to control screensaver loop (1 = run/wake, 0 = wait/timeout)
static K_SEM_DEFINE(sem_screensaver_wake, 0, 1);

/* Serialises the panel across the threads that drive it: the screensaver
 * thread, the BLE RX thread (commands), the mesh dispatch thread (flooded
 * commands) and the system workqueue (vstream watchdog). Two of those are
 * cooperative and preempt the screensaver at any instruction, including
 * halfway through a 4736-byte RAM write. Zephyr mutexes are recursive for the
 * owning thread, so nested paths (update_status -> perform_display_update)
 * are fine.
 *
 * Coverage is the public entry points of THIS module. It is not complete:
 * commands.c's vstream teardown still calls ssd1675a_wait_busy() directly, and
 * update_partial_nowait() releases the lock with BUSY still high by design.
 * Anything added here that talks to the controller must take the lock too.
 *
 * Held across a full refresh, so a waiter can block for ~15 s — the same
 * exposure a command handler already had when it ran the refresh itself. Never
 * hold it across ble_printf(): a notification can block on the BLE TX pool,
 * which only the BLE RX thread drains, and that thread may be waiting here. */
K_MUTEX_DEFINE(display_lock);

#define DISPLAY_LOCK()   k_mutex_lock(&display_lock, K_FOREVER)
#define DISPLAY_UNLOCK() k_mutex_unlock(&display_lock)

/* The screensaver thread's own drawing must not queue behind a long
 * maintenance cycle. display_manager_deep_clean() can own the display for
 * fifteen minutes, and the top of that thread's loop is ALSO the battery
 * protection and persist cycle — waiting K_FOREVER here meant a CLEAN or NUKE
 * silently disabled low-battery shutdown for its whole duration. Skipping a
 * status frame costs nothing: the next tick draws it. Anything that must not
 * be dropped keeps DISPLAY_LOCK(). */
#define DISPLAY_LOCK_OR_SKIP()                                    \
	do {                                                          \
		if (k_mutex_lock(&display_lock, K_SECONDS(2)) != 0) { \
			return;                                       \
		}                                                     \
	} while (0)

static bool should_power_down_after_update(void);
static void load_frame(void);

/* The on-screen gauge is redrawn every frame — up to 10 Hz on the dynamic
 * saver — for a value that moves over minutes, and each read is a blocking
 * SAADC conversion. Ten seconds of staleness is invisible on a battery icon.
 * A failed read is not cached: the negative value makes the next call retry. */
static int cached_batt_mv = -1;
static int64_t cached_batt_ms = 0;

static int battery_mv_for_display(void)
{
    int64_t now = k_uptime_get();

    if (cached_batt_mv < 0 || (now - cached_batt_ms) >= 10 * 1000) {
        cached_batt_mv = battery_read_mv();
        cached_batt_ms = now;
    }
    return cached_batt_mv;
}

/* Full refreshes use the panel's OTP waveform instead of the working table.
 * On by default for a panel the table was never tuned for (the 400x300). */
static bool full_refresh_otp;

static void run_full_refresh(void)
{
    if (full_refresh_otp) {
        ssd1675a_update_display_otp();
    } else {
        ssd1675a_update_display();
    }
}

void display_manager_set_full_refresh_otp(bool enable)
{
    full_refresh_otp = enable;
}

bool display_manager_get_full_refresh_otp(void)
{
    return full_refresh_otp;
}

static void write_partial_stream_buffers(void)
{
    if (stream_write_red_plane) {
        ssd1675a_display_buffers_fast(graphics_get_buffer(), graphics_get_red_buffer());
    } else {
        ssd1675a_display_buffer_fast(graphics_get_buffer());
    }
}

void display_manager_set_stream_write_red_plane(bool enable)
{
    stream_write_red_plane = enable;
}

static int power_estimate_idle_current_ua(void)
{
    int ua = POWER_BASE_SLEEP_UA;
    bool ble_streaming = ble_service_get_streaming_mode();

    if (mesh_get_rx()) {
        ua += POWER_MESH_SCAN_UA;
    }

    if (ble_service_is_connected()) {
        ua += ble_streaming ? POWER_BLE_STREAM_UA : POWER_BLE_CONN_IDLE_UA;
    } else if (ble_streaming) {
        ua += POWER_BLE_ADV_FAST_UA;
    } else {
        /* POWER_BLE_ADV_IDLE_UA is the 2 s figure; the cost is per event, so
         * it scales with whatever interval the sleep profile has in force. */
        const struct power_profile *p = power_profile_get();
        int adv_s = screensaver_enabled ? p->adv_clock_s : p->adv_picture_s;
        ua += POWER_BLE_ADV_IDLE_UA * 2 / (adv_s < 1 ? 1 : adv_s);
    }

    if (streaming_active) {
        ua += POWER_DISPLAY_HV_HOLD_UA;
    } else if (keep_display_on) {
        ua += POWER_DISPLAY_STANDBY_UA;
    } else if (!should_power_down_after_update()) {
        ua += POWER_DISPLAY_STANDBY_UA;
    }

    return ua;
}

static void power_estimate_account_now(void)
{
    int64_t now = k_uptime_get();

    if (power_estimate_last_ms == 0) {
        power_estimate_last_ms = now;
        return;
    }

    int64_t dt_ms = now - power_estimate_last_ms;
    if (dt_ms > 0) {
        persist_add_energy_uah_x1000(
            ((uint64_t)power_estimate_current_ua * (uint64_t)dt_ms) / 3600U);
        power_estimate_last_ms = now;
    }
}

/* Roll the energy accrued since the last accounting point into the persisted
 * total. Cheap; call before a snapshot (tick / pre-reboot save) so it captures
 * the final interval. */
void display_manager_flush_energy(void)
{
    power_estimate_account_now();
}

static void power_estimate_set_current(int current_ua)
{
    power_estimate_account_now();
    power_estimate_current_ua = current_ua;
}

static void power_estimate_resync_idle(void)
{
    power_estimate_set_current(power_estimate_idle_current_ua());
}

static uint32_t power_estimate_get_mah_x1000(void)
{
    power_estimate_account_now();
    return (uint32_t)(persist_get_energy_uah_x1000() / 1000U);
}

static void stop_streaming_if_active(void) {
    if (streaming_active) {
        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        ssd1675a_wait_busy();
        if (stream_write_red_plane) {
            ssd1675a_clear_red_ram();
        }
        ssd1675a_end_streaming();
        ssd1675a_wait_busy();
        streaming_active = false;
        power_estimate_resync_idle();
    }
    stream_write_red_plane = false;
}

static bool should_power_down_after_update(void)
{
    return !keep_display_on &&
           (!screensaver_enabled || screensaver_mode == SCREENSAVER_MODE_STATIC);
}

/* Drive the already-rendered buffers onto the panel as the last thing the
 * device does, then cut power. A FULL refresh, not a partial one: both battery
 * screens are red-heavy, and only the full waveform drives the red plane. The
 * panel is bistable, so the image survives with the rails down. */
static void show_final_screen(void)
{
    if (!display_ready) {
        return;
    }
    /* Bounded wait, unlike everywhere else: this runs on the deep-discharge
     * path, and a long-running CLEAN/NUKE holds the lock for minutes. Powering
     * off on time matters more than a clean handover — on timeout we drive the
     * panel anyway and accept a possibly garbled farewell frame. */
    bool locked = (k_mutex_lock(&display_lock, K_SECONDS(20)) == 0);
    stop_streaming_if_active();
    power_estimate_set_current(POWER_DISPLAY_FULL_UA);
    ssd1675a_init();
    load_frame();
    run_full_refresh();
    persist_add_refresh();
    ssd1675a_sleep();
    ssd1675a_power_off();
    power_estimate_resync_idle();
    if (locked) {
        DISPLAY_UNLOCK();
    }
}

static void power_down_after_idle_update(void)
{
    if (should_power_down_after_update()) {
        ssd1675a_sleep();
        ssd1675a_power_off();
    }
}

void display_manager_probe_panel(struct panel_probe_report *r)
{
    memset(r, 0, sizeof(*r));
    if (!display_ready) {
        r->status = 0xFF;
        r->ram.all_ff = true;
        return;
    }

    DISPLAY_LOCK();
    stop_streaming_if_active();
    power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
    ssd1675a_init_partial();            /* wake; hard-resets if asleep */

    r->status = ssd1675a_read_status();
    ssd1675a_read_register(0x2D, r->otp, sizeof(r->otp));
    ssd1675a_read_register(0x2E, r->uid, sizeof(r->uid));
    ssd1675a_probe_ram(240, &r->ram);   /* 12000 B > both planes of a 160x296 chip */

    /* The probe scribbled over RAM: put the current frame back so the next
     * partial update does not flash garbage. No refresh is triggered. */
    ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    power_down_after_idle_update();
    power_estimate_resync_idle();
    DISPLAY_UNLOCK();
}

static void force_builtin_partial_mode(int mode, bool *prev_custom, int *prev_mode)
{
    if (prev_custom) {
        *prev_custom = ssd1675a_get_use_custom_lut();
    }
    if (prev_mode) {
        *prev_mode = partial_mode_current;
    }

    ssd1675a_set_use_custom_lut(false);
    display_manager_set_partial_mode(mode);
}

static void restore_partial_mode(bool prev_custom, int prev_mode)
{
    display_manager_set_partial_mode(prev_mode);
    ssd1675a_set_use_custom_lut(prev_custom);
}

static int maintenance_countdown(int current_count, int interval)
{
    int next_count = current_count + 1;
    int phase = next_count % interval;

    return (phase == 0) ? 0 : (interval - phase);
}

/* The "A:" field: how much slack is left before the next full refresh, counted
 * against whichever of the two triggers is closer. Both are "how many more
 * ticks", and on the idle minute tick a tick IS a minute, so the number reads
 * the same as the old counter did. */
static int maintenance_countdown_static(int64_t last_full_ms, int64_t now_ms,
                                        int interval_min, int partials_done,
                                        int max_partials)
{
    if (last_full_ms == 0 || full_refresh_requested) {
        return 0;   /* nothing drawn yet, or a host asked: next frame is full */
    }

    int64_t elapsed_min = (now_ms - last_full_ms) / (60 * 1000);
    int by_clock = interval_min - (int)elapsed_min;
    int by_count = max_partials - partials_done;

    int remaining = (by_clock < by_count) ? by_clock : by_count;

    return (remaining < 0) ? 0 : remaining;
}

/* What the boot-time probe decided. 128x296 B/W/R is the compile-time default
 * and the fallback when the read-back path is dead. */
static char panel_name[12] = "128x296";
static bool panel_has_red = true;
/* A host-written red mask sits in the controller's red RAM, panel kept awake;
 * the next FAPPLY must refresh without resetting or clearing it. */
static bool red_staged;

/* Identify the panel controller by RAM capacity (ssd1675a_probe_ram) and size
 * the driver and the canvas for it. A 400x300-class controller (SSD1619A) gets
 * one B/W plane laid out landscape — two 15000 B planes do not fit next to the
 * BLE stack; anything else, including a dead read-back path, keeps the 128x296
 * B/W/R default. Runs once before the first frame, so the RAM the probe
 * scribbles over is never shown. */
static void detect_panel(void)
{
    ssd1675a_ram_probe_t pr;

    DISPLAY_LOCK();
    ssd1675a_init();                /* power, reset, default registers */
    ssd1675a_probe_ram(240, &pr);   /* 12000 B: more than both planes of a 160x296 chip */

    bool large = !pr.all_ff && pr.bytes > 0 && pr.match == pr.bytes;
    if (large && graphics_init_panel(400, 300, false)) {
        ssd1675a_set_geometry(400, 300);
        graphics_set_rotation(0);   /* landscape: 400 wide, source axis along the FPC edge */
        panel_has_red = false;
        snprintf(panel_name, sizeof(panel_name), "400x300");
        /* The working table, VSH2 and VCOM were tuned on the 2.9" panel and
         * give a washed-out red here; the factory waveform set and VCOM in
         * this panel's OTP are the better starting point. OTPLUT:0 reverts. */
        full_refresh_otp = true;
        ssd1675a_set_vcom_from_otp(true);
    } else {
        ssd1675a_set_geometry(SSD1675A_WIDTH, SSD1675A_HEIGHT);
        graphics_init_panel(SSD1675A_WIDTH, SSD1675A_HEIGHT, true);
        graphics_set_rotation(1);
        panel_has_red = true;
        full_refresh_otp = false;
        ssd1675a_set_vcom_from_otp(false);
        snprintf(panel_name, sizeof(panel_name), "%dx%d", SSD1675A_WIDTH, SSD1675A_HEIGHT);
    }

    /* The first real frame re-inits with the chosen geometry; park the panel
     * the way an idle update would. */
    ssd1675a_sleep();
    ssd1675a_power_off();
    DISPLAY_UNLOCK();
    LOG_INF("panel %s (probe %d/%d, all_ff=%d)", panel_name, pr.match, pr.bytes, pr.all_ff);
}

const char *display_manager_panel_name(void)
{
    return panel_name;
}

void display_manager_stage_red_plane(void)
{
    if (!display_ready || panel_has_red) {
        return;
    }
    DISPLAY_LOCK();
    stop_streaming_if_active();
    power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
    ssd1675a_init_partial();            /* wake; hard-resets only if asleep */
    ssd1675a_load_plane(true, graphics_get_buffer());
    red_staged = true;                  /* no power-down: RAM must live until FAPPLY */
    power_estimate_resync_idle();
    DISPLAY_UNLOCK();
}

/* FAPPLY after FAPPLY RED: the panel is still awake with the red mask in its
 * RAM, so skip the hard reset of ssd1675a_init() and load only the B/W plane. */
static void perform_display_update_keep_red(void)
{
    if (!display_ready) return;
    DISPLAY_LOCK();
    power_estimate_set_current(POWER_DISPLAY_FULL_UA);
    ssd1675a_init_partial();
    ssd1675a_load_plane(false, graphics_get_buffer());
    run_full_refresh();
    red_staged = false;
    power_down_after_idle_update();
    power_estimate_resync_idle();
    persist_add_refresh();
    DISPLAY_UNLOCK();
}

bool display_manager_panel_has_red(void)
{
    return panel_has_red;
}

void display_manager_init(void) {
    display_ready = ssd1675a_port_init();
    if (!display_ready) {
        LOG_ERR("Display port unavailable");
        return;
    }
    detect_panel();
    power_estimate_resync_idle();
}

/* ── Scenes ─────────────────────────────────────────────────────────────
 * A full refresh renders through render_scene() so the frame can be rebuilt
 * on demand. On a B/W/R canvas that changes nothing. On a single-plane layout
 * (the 400x300 panel: no RAM for a second 15000 B plane) load_frame() replays
 * the scene as a red mask straight into the controller's red RAM and then
 * re-renders the B/W image — red costs a second render, not a second buffer.
 * The scene is consumed by the refresh, so a frame that was written by other
 * means (host FW:/RW: bytes) is never overwritten by a stale replay; it simply
 * has no red on such a layout. Scenes must be deterministic. */
typedef void (*scene_fn_t)(void *arg);
static scene_fn_t cur_scene;
static void *cur_scene_arg;

static void render_scene(scene_fn_t scene, void *arg)
{
    cur_scene = scene;
    cur_scene_arg = arg;
    graphics_set_render_mode(GFX_RENDER_NORMAL);
    scene(arg);
}

static void load_frame(void)
{
    if (panel_has_red || !cur_scene) {
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    } else {
        graphics_set_render_mode(GFX_RENDER_RED_MASK);
        cur_scene(cur_scene_arg);
        ssd1675a_load_plane(true, graphics_get_buffer());
        graphics_set_render_mode(GFX_RENDER_NORMAL);
        cur_scene(cur_scene_arg);
        ssd1675a_load_plane(false, graphics_get_buffer());
    }
    cur_scene = NULL;
    cur_scene_arg = NULL;
}

/* The scene thunks: one per screen that goes through a full refresh. */
static void scene_text(void *arg)          { display_screens_render_text((const char *)arg); }
static void scene_ruler(void *arg)         { (void)arg; display_screens_render_ruler(panel_name, graphics_get_canvas()->rotation); }
static void scene_palette(void *arg)       { (void)arg; display_screens_render_palette_test(); }
static void scene_status_static(void *arg) { display_screens_render_status_static((const display_status_model_t *)arg); }
static void scene_clear(void *arg)         { graphics_clear((uint8_t)(uintptr_t)arg); }

struct final_scene_args { int mv; int64_t uptime; };
static void scene_shutdown(void *arg)
{
    const struct final_scene_args *a = arg;
    display_screens_render_shutdown(a->mv, a->uptime);
}
static void scene_low_battery(void *arg)
{
    const struct final_scene_args *a = arg;
    display_screens_render_low_battery(a->mv, a->uptime);
}

static void perform_display_update(void) {
    if (!display_ready) return;

    DISPLAY_LOCK();

    power_estimate_set_current(POWER_DISPLAY_FULL_UA);
    ssd1675a_init();
    load_frame();
    run_full_refresh();

    power_down_after_idle_update();
    power_estimate_resync_idle();

    persist_add_refresh();

    DISPLAY_UNLOCK();
}

// Like perform_display_update() but uses the red-clearing LUT (VSL on red
// channel) to actively drive red pigment away.  Called after clean/nuke cycles
// to eliminate the reddish tint left by the "red fixation" phases of the main LUT.
static void perform_display_update_flush_red(void) {
    if (!display_ready) return;
    DISPLAY_LOCK();
    power_estimate_set_current(POWER_DISPLAY_FULL_UA);
    ssd1675a_init();
    load_frame();
    ssd1675a_update_display_flush_red();
    power_down_after_idle_update();
    power_estimate_resync_idle();
    DISPLAY_UNLOCK();
}

static void display_manager_update_static_saver(bool full_refresh)
{
    bool prev_custom;
    int prev_mode;

    force_builtin_partial_mode(STATIC_SAVER_PARTIAL_MODE, &prev_custom, &prev_mode);

    if (full_refresh) {
        stop_streaming_if_active();
        perform_display_update();
    } else {
        display_manager_update_partial();
    }

    restore_partial_mode(prev_custom, prev_mode);
}

void display_manager_update_partial(void) {
    if (!display_ready) return;

    DISPLAY_LOCK();

    if (keep_display_on) {
        stream_partial_count++;

        // Periodic DC-balance refresh: break out of streaming for one 0xC7 cycle
        // to prevent particle polarization ghost burn-in.
        if (streaming_active && (stream_partial_count % STREAM_REFRESH_INTERVAL == 0)) {
            stop_streaming_if_active();
            power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
            ssd1675a_init_partial();
            write_partial_stream_buffers();
            ssd1675a_update_partial();  // 0xC7: full HV cycle with current LUT
            power_estimate_resync_idle();
            DISPLAY_UNLOCK();
            return;                     // streaming restarts on next call
        }

        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        if (!streaming_active) {
            ssd1675a_init_partial();
            write_partial_stream_buffers();
            ssd1675a_begin_streaming();
            streaming_active = true;
        } else {
            write_partial_stream_buffers();
        }
        ssd1675a_update_frame_stream();
        power_estimate_resync_idle();
    } else {
        stop_streaming_if_active();
        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        ssd1675a_init_partial();
        write_partial_stream_buffers();
        ssd1675a_update_partial();
        power_down_after_idle_update();
        power_estimate_resync_idle();
    }

    DISPLAY_UNLOCK();
}



/* Rows y0..y1 of the B/W plane, or everything when the range covers the
 * plane, is empty-but-meant-as-full (y1 == INT_MAX) or the red plane is
 * streamed as well (its rows are not tracked). y1 < y0 writes nothing. */
static void write_partial_stream_rows(int y0, int y1)
{
    const int last = ssd1675a_height() - 1;

    if (stream_write_red_plane || (y0 <= 0 && y1 >= last)) {
        write_partial_stream_buffers();
        return;
    }
    if (y1 < y0) {
        return;
    }
    ssd1675a_load_rows(false, graphics_get_buffer(), y0, y1);
}

static void update_partial_nowait_rows(int y0, int y1) {
    if (!display_ready) return;

    /* Non-streaming path must always block — fall back to regular update. */
    if (!keep_display_on) {
        display_manager_update_partial();
        return;
    }

    /* The lock is released while the refresh is still running: this call
     * deliberately returns with BUSY high and the caller owns the wait. It
     * serialises the SPI burst, not the refresh that follows it. */
    DISPLAY_LOCK();

    stream_partial_count++;

    /* Periodic DC-balance: every STREAM_REFRESH_INTERVAL frames do a full
     * 0xC7 HV cycle. This one always blocks (caller's wait_busy already ran). */
    if (streaming_active && (stream_partial_count % STREAM_REFRESH_INTERVAL == 0)) {
        stop_streaming_if_active();
        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        ssd1675a_init_partial();
        write_partial_stream_buffers();
        ssd1675a_update_partial();
        power_estimate_resync_idle();
        DISPLAY_UNLOCK();
        return;
    }

    power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
    if (!streaming_active) {
        ssd1675a_init_partial();
        write_partial_stream_buffers();     /* first frame: the whole plane */
        ssd1675a_begin_streaming();
        streaming_active = true;
    } else {
        write_partial_stream_rows(y0, y1);
    }
    /* Trigger display refresh but return immediately — display runs in background. */
    ssd1675a_trigger_frame_stream_nowait();

    DISPLAY_UNLOCK();
}

void display_manager_update_partial_nowait(void) {
    update_partial_nowait_rows(0, INT_MAX);
}

void display_manager_update_partial_rows_nowait(int y0, int y1) {
    update_partial_nowait_rows(y0, y1);
}

void display_manager_set_partial_mode(int mode) {
    DISPLAY_LOCK();
    stop_streaming_if_active();  // new LUT must be loaded in next begin_streaming()
    if (mode == 0) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TURBO);
    } else if (mode == 1) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_BALANCED);
    } else if (mode == 2) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_STABLE);
    } else if (mode == 3) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_CLEAN);
    } else if (mode == 4) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_DARK);
    } else if (mode == 5) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_LIGHT);
    } else if (mode == 6) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_BIDIR_FAST);
    } else if (mode == 7) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_BIDIR);
    } else if (mode == 8) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_SOFT_DARK);
    } else if (mode == 9) {
        ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TONE_SOFT_LIGHT);
    } else {
        DISPLAY_UNLOCK();
        return;   /* unknown mode: leave the current one in place */
    }
    partial_mode_current = mode;
    DISPLAY_UNLOCK();
}

void display_manager_begin_streaming(void) {
    if (!display_ready) return;

    DISPLAY_LOCK();
    if (!streaming_active) {
        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        ssd1675a_begin_streaming();
        streaming_active = true;
        power_estimate_resync_idle();
    }
    DISPLAY_UNLOCK();
}

void display_manager_update_frame_stream(void) {
    if (!display_ready) return;

    DISPLAY_LOCK();
    if (!streaming_active) {
        power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
        ssd1675a_begin_streaming();
        streaming_active = true;
    }
    power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
    ssd1675a_update_frame_stream();
    power_estimate_resync_idle();
    DISPLAY_UNLOCK();
}

void display_manager_end_streaming(void) {
    DISPLAY_LOCK();
    stop_streaming_if_active();
    DISPLAY_UNLOCK();
}

void display_manager_reset_lut_test(void) {
    DISPLAY_LOCK();
    lut_test_frame   = 0;
    lut_test_last_ms = 0;
    lut_test_cur_ms  = 0;
    lut_test_min_ms  = 0;
    lut_test_max_ms  = 0;
    stop_streaming_if_active();  // reload LUT on next begin_streaming()
    DISPLAY_UNLOCK();
}

void display_manager_get_lut_test_stats(int32_t *frame_out, int32_t *cur_ms_out,
                                        int32_t *min_ms_out, int32_t *max_ms_out)
{
    if (frame_out)   *frame_out   = lut_test_frame;
    if (cur_ms_out)  *cur_ms_out  = lut_test_cur_ms;
    if (min_ms_out)  *min_ms_out  = lut_test_min_ms;
    if (max_ms_out)  *max_ms_out  = lut_test_max_ms;
}

void display_manager_update_lut_test(void) {
    if (!display_ready) return;

    /* Held across the render too: the frame buffers are shared, so another
     * thread rendering its own screen mid-way would be pushed out here. */
    DISPLAY_LOCK_OR_SKIP();

    int64_t now = k_uptime_get();
    int32_t delta_ms = 0;

    if (lut_test_last_ms != 0) {
        delta_ms = (int32_t)(now - lut_test_last_ms);
        if (lut_test_frame > 0) {
            if (lut_test_min_ms == 0 || delta_ms < lut_test_min_ms) lut_test_min_ms = delta_ms;
            if (delta_ms > lut_test_max_ms) lut_test_max_ms = delta_ms;
        }
    }
    lut_test_last_ms = now;
    lut_test_cur_ms  = delta_ms;

    display_screens_render_lut_test(lut_test_frame, delta_ms,
                                    lut_test_min_ms, lut_test_max_ms,
                                    ssd1675a_get_use_custom_lut());
    lut_test_frame++;

    /* Write BOTH BW and Red buffers so the red track in the ghosting test
     * is visible. ssd1675a_display_buffer_fast() skips the red buffer. */
    power_estimate_set_current(POWER_DISPLAY_PARTIAL_UA);
    if (!streaming_active) {
        ssd1675a_init_partial();
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
        ssd1675a_begin_streaming();
        streaming_active = true;
    } else {
        ssd1675a_display_buffer(graphics_get_buffer(), graphics_get_red_buffer());
    }
    ssd1675a_update_frame_stream();
    power_estimate_resync_idle();

    DISPLAY_UNLOCK();

    /* Send telemetry every frame once we have a valid measurement (frame >= 2). */
    if (tele_enabled && delta_ms > 0) {
        ble_printf("TELE:ltest frame=%d last=%d min=%d max=%d lut=%s\r\n",
                   (int)(lut_test_frame - 1), (int)delta_ms,
                   (int)lut_test_min_ms, (int)lut_test_max_ms,
                   ssd1675a_get_use_custom_lut() ? "custom" : "builtin");
    }
}

void display_manager_set_screensaver_mode(int mode) {
    /* Touches the bus via stop_streaming_if_active() and set_partial_mode(). */
    DISPLAY_LOCK();

    if (mode != screensaver_mode) {
        stop_streaming_if_active();
    }
    screensaver_mode = mode;
    if (mode == SCREENSAVER_MODE_DYNAMIC) {
        display_manager_set_keep_on(true);
        if (!ssd1675a_get_use_custom_lut()) {
            display_manager_set_partial_mode(0); // TURBO default for DSAVER (overrideable via MODE:)
        }
        k_sem_give(&sem_screensaver_wake);
        display_screens_reset_dynamic();
    } else if (mode == SCREENSAVER_MODE_LUT_TEST) {
        display_manager_set_keep_on(true);
        display_manager_reset_lut_test();
        k_sem_give(&sem_screensaver_wake);
    } else {
        static_saver_partials_since_full = 0;
        static_saver_last_full_ms = 0;
        display_manager_set_keep_on(false);
        display_manager_set_partial_mode(STATIC_SAVER_PARTIAL_MODE);
    }
    power_estimate_resync_idle();

    DISPLAY_UNLOCK();
}

void display_manager_update_status(void) {
    if (!display_ready) return;

    /* Render and push as one unit — the frame buffers are global, so an
     * interleaved render from another thread would be pushed out here. */
    DISPLAY_LOCK_OR_SKIP();

    int64_t start_render = k_uptime_get();
    static int32_t last_dur = 0;
    int mv = battery_mv_for_display();
    display_status_model_t model = {0};

    get_system_time(&model.time);
    model.battery_mv = (mv < 0) ? 0 : mv;
    model.battery_percent = battery_percent(mv);
    model.last_render_ms = last_dur;
    model.uptime_sec = (int64_t)persist_uptime_sec(); /* cumulative, survives DFU */
    model.saver_mode = screensaver_mode;
    model.partial_mode = (screensaver_mode == SCREENSAVER_MODE_STATIC)
                         ? STATIC_SAVER_PARTIAL_MODE : partial_mode_current;
    model.custom_lut = (screensaver_mode != SCREENSAVER_MODE_STATIC) &&
                       ssd1675a_get_use_custom_lut();
    model.keep_display_on = keep_display_on;
    model.streaming_active = streaming_active;
    model.power_after_update = should_power_down_after_update();
    model.maintenance_countdown =
        (screensaver_mode == SCREENSAVER_MODE_STATIC)
            ? maintenance_countdown_static(static_saver_last_full_ms, start_render,
                                           STATIC_SAVER_FULL_INTERVAL,
                                           static_saver_partials_since_full,
                                           STATIC_SAVER_MAX_PARTIALS)
            : maintenance_countdown(stream_partial_count, STREAM_REFRESH_INTERVAL);
    model.energy_mah_x1000 = power_estimate_get_mah_x1000();
    model.estimated_current_ua = power_estimate_current_ua;

    bool send_tele = false;
    static int32_t dyn_frame_ctr = 0;

    if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC) {
        display_screens_render_status_dynamic(&model);
        full_refresh_requested = false;   /* the stream has its own maintenance */
        display_manager_update_partial();
        last_dur = (int32_t)(k_uptime_get() - start_render);
        dyn_frame_ctr++;
        send_tele = tele_enabled && (dyn_frame_ctr % 10 == 0);
    } else {
        render_scene(scene_status_static, &model);
        /* Whichever comes first: an explicit request, the schedule, or the
         * ghosting ceiling. See STATIC_SAVER_FULL_INTERVAL for why it takes
         * both of the latter, and full_refresh_requested for why neither is
         * enough on its own. */
        bool full_refresh =
            full_refresh_requested ||
            (static_saver_last_full_ms == 0) ||
            ((start_render - static_saver_last_full_ms) >=
             (int64_t)STATIC_SAVER_FULL_INTERVAL * 60 * 1000) ||
            (static_saver_partials_since_full >= STATIC_SAVER_MAX_PARTIALS);
        full_refresh_requested = false;
        if (full_refresh) {
            static_saver_last_full_ms = start_render;
            static_saver_partials_since_full = 0;
        } else {
            static_saver_partials_since_full++;
        }
        display_manager_update_static_saver(full_refresh);

        last_dur = (int32_t)(k_uptime_get() - start_render);
    }

    /* `model` dies with this frame: a partial tick leaves the scene unconsumed,
     * and it must not be replayed later with a dangling argument. */
    cur_scene = NULL;
    cur_scene_arg = NULL;

    DISPLAY_UNLOCK();

    /* Notify only after unlocking. bt_gatt_notify can block waiting for a TX
     * buffer, and the only context that frees those is the BLE RX thread —
     * which may itself be waiting on this lock to run a display command. */
    if (send_tele) {
        ble_printf("TELE:dynamic frame=%d last=%dms\r\n",
                   (int)dyn_frame_ctr, (int)last_dur);
    }
}

void display_manager_show_text(const char *text) {
    if (!text) return;
    DISPLAY_LOCK();
    render_scene(scene_text, (void *)text);
    perform_display_update();
    DISPLAY_UNLOCK();
}

void display_manager_show_ruler(void)
{
    DISPLAY_LOCK();
    stop_streaming_if_active();
    render_scene(scene_ruler, NULL);
    perform_display_update();
    DISPLAY_UNLOCK();
}

void display_manager_show_palette_test(void) {
    DISPLAY_LOCK();
    stop_streaming_if_active();
    render_scene(scene_palette, NULL);
    perform_display_update();
    DISPLAY_UNLOCK();
}

void display_manager_run_tone_test(void) {
#define TONE_TEST_PASSES 8
    if (!display_ready) return;

    DISPLAY_LOCK();

    bool prev_custom = ssd1675a_get_use_custom_lut();
    int prev_mode = partial_mode_current;

    stop_streaming_if_active();
    stream_partial_count = 0;

    graphics_clear(GFX_WHITE);
    perform_display_update();

    ssd1675a_set_use_custom_lut(false);
    display_manager_set_partial_mode(4);
    display_manager_set_keep_on(true);

    for (int pass = 0; pass < TONE_TEST_PASSES; pass++) {
        display_screens_render_tone_test_pass(pass, TONE_TEST_PASSES);
        display_manager_update_partial();
    }

    display_manager_set_keep_on(false);
    display_manager_set_partial_mode(prev_mode);
    ssd1675a_set_use_custom_lut(prev_custom);
    stream_partial_count = 0;

    DISPLAY_UNLOCK();
#undef TONE_TEST_PASSES
}

void display_manager_clean(void) {
    if (!display_ready) return;
    DISPLAY_LOCK();
    stop_streaming_if_active();
    stream_partial_count = 0;
    for (int i = 0; i < 7; i++) {
        render_scene(scene_clear, (void *)(uintptr_t)GFX_BLACK);
        perform_display_update();
        render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
        perform_display_update();
        render_scene(scene_clear, (void *)(uintptr_t)GFX_RED);
        perform_display_update();
    }
    // Two red-clearing passes: VSL on red channel actively drives pigment away,
    // eliminating the reddish tint left by the "red fixation" phases in lut_data.
    render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
    perform_display_update_flush_red();
    render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
    perform_display_update_flush_red();
    DISPLAY_UNLOCK();
}

void display_manager_deep_clean(int cycles) {
    if (!display_ready) return;
    DISPLAY_LOCK();
    stop_streaming_if_active();
    stream_partial_count = 0;
    // Phase 1: white-only pre-soak — repeated VSL application kills VSH1
    // polarization from streaming without re-applying it (no black phase).
    for (int i = 0; i < cycles; i++) {
        render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
        perform_display_update();
    }
    // Phase 2: W→R cycles — VSL depolarize, then VSH2 drive red.
    // No black phase here either: black (LUT0 Ph4) re-applies 472f of VSH1,
    // which is exactly what caused the red burn-in.
    for (int i = 0; i < cycles; i++) {
        render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
        perform_display_update();
        render_scene(scene_clear, (void *)(uintptr_t)GFX_RED);
        perform_display_update();
    }
    render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
    perform_display_update_flush_red();
    render_scene(scene_clear, (void *)(uintptr_t)GFX_WHITE);
    perform_display_update_flush_red();
    DISPLAY_UNLOCK();
}

void display_manager_clear(void) {
    graphics_clear(GFX_WHITE);
}

void display_manager_enable_screensaver(bool enable) {
    bool prev = screensaver_enabled;
    screensaver_enabled = enable;
    power_profile_apply();          /* picture mode advertises slower */
    power_estimate_resync_idle();
    if (enable && !prev) {
        static_saver_partials_since_full = 0;
        static_saver_last_full_ms = 0;
        k_sem_give(&sem_screensaver_wake);
    } else if (!enable && keep_display_on) {
        /* Entering image mode from dynamic screensaver: stop streaming and
         * power off display so the HV rails don't stay on indefinitely. */
        display_manager_set_keep_on(false);
    }
}

bool display_manager_is_screensaver_active(void) {
    return screensaver_enabled;
}

// Thread Function
static void screensaver_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    // Initial wait to let system boot
    k_sleep(K_SECONDS(2));

    bool low_battery_screen_shown = false;
    int mv = -1;
    battery_state_t bstate = battery_get_state();
    int64_t last_maintenance_ms = 0;
    /* Whether this pass should draw: set by how the previous wait ended. */
    bool tick_due = true;

    while (1) {
        /* ── Battery protection + stats roll-up ──
         *
         * Rate-limited, because this loop does not run at one speed. On the
         * static saver it ticks once a minute and every pass through here was
         * fine; the dynamic and LUT-test modes spin it at 4-10 Hz, and there
         * this block was running hundreds of times more often than anything it
         * measures changes — a blocking SAADC conversion plus persist_tick()'s
         * two CRC32 passes under a spinlock, per animation frame.
         *
         * Thirty seconds is the same cadence the BATT_LOW branch below already
         * settled on, so protection is no less responsive than it was. */
        int64_t now_ms = k_uptime_get();
        if (last_maintenance_ms == 0 ||
            (now_ms - last_maintenance_ms) >= 30 * 1000) {
            last_maintenance_ms = now_ms;

            mv = battery_read_mv();
            /* A failed ADC read must not feed the protection state machine: it
             * would look like 0 mV and power a healthy device off. Keep the last
             * known state and try again next cycle. */
            bstate = (mv < 0) ? battery_get_state()
                              : battery_monitor_update(mv);

            /* Roll cumulative stats forward in retained RAM. RAM-only. */
            power_estimate_account_now();   /* folds the idle interval in too */
            persist_tick();
        }

        if (bstate == BATT_CRITICAL || bstate == BATT_SHUTDOWN) {
            /* Power may be about to be lost: last-breath save to flash. */
            persist_save_to_flash();

            /* Farewell screen: one final full render, then deep sleep */
            LOG_WRN("Battery critical (%d mV) — rendering farewell", mv);
            struct final_scene_args fa = { .mv = mv, .uptime = (int64_t)persist_uptime_sec() };
            render_scene(scene_shutdown, &fa);
            show_final_screen();

            /* Notify over BLE if connected, then wait a moment */
            ble_printf("BATT:SHUTDOWN mv=%d\r\n", mv);
            k_sleep(K_MSEC(500));

            /* Enter system-off: no BLE, no wakeup, only power cycle restarts.
             * This is the terminal state to protect the battery from deep
             * discharge in a boot-loop. */
            LOG_WRN("Entering system-off (deep sleep)");
            sys_poweroff();
            /* Never reached */
            return;
        }

        if (bstate == BATT_LOW) {
            if (!low_battery_screen_shown) {
                /* Power may be about to be lost: last-breath save to flash. */
                persist_save_to_flash();

                /* Show low-battery warning screen once (full clean update) */
                LOG_WRN("Battery low (%d mV) — showing warning, inhibiting display", mv);
                struct final_scene_args fa = { .mv = mv, .uptime = (int64_t)persist_uptime_sec() };
                render_scene(scene_low_battery, &fa);
                show_final_screen();
                low_battery_screen_shown = true;

                ble_printf("BATT:LOW mv=%d\r\n", mv);

                /* Disable screensaver — we've shown the final frame */
                screensaver_enabled = false;
            }
            /* In BATT_LOW: just sleep, keep BLE alive, check battery periodically */
            k_sleep(K_SECONDS(30));
            continue;
        }

        /* Battery recovered from BATT_LOW */
        if (low_battery_screen_shown && bstate == BATT_OK) {
            low_battery_screen_shown = false;
            screensaver_enabled = true;
            ble_printf("BATT:OK mv=%d\r\n", mv);
            LOG_INF("Battery recovered (%d mV) — resuming normal operation", mv);
        }

        /* ── Normal operation ── */
        if (screensaver_enabled && tick_due) {
            if (screensaver_mode == SCREENSAVER_MODE_LUT_TEST) {
                display_manager_update_lut_test();
            } else {
                display_manager_update_status();
            }
        }
        if (!screensaver_enabled && frame_update_requested) {
            frame_update_requested = false;
            cur_scene = NULL;           /* the buffers are the frame; replay nothing */
            cur_scene_arg = NULL;
            perform_display_update();
        }

        if (!screensaver_enabled) {
            /* Image mode: nothing to draw, but the top of this loop is ALSO
             * the battery-protection and persist cycle — blocking forever here
             * silently disabled low-battery shutdown for a tag left on a
             * picture. A finite timeout keeps that cycle alive; with the
             * screensaver off the iteration touches no display state. */
            k_sem_take(&sem_screensaver_wake, K_SECONDS(60));
            tick_due = true;
        } else if (screensaver_mode == SCREENSAVER_MODE_DYNAMIC ||
                   screensaver_mode == SCREENSAVER_MODE_LUT_TEST) {
            k_sem_take(&sem_screensaver_wake, K_MSEC(10));
            tick_due = true;
        } else {
            /* Sleep to the next boundary of the interval the profile has in
             * force (a 5-minute profile redraws at :00, :05, ...), capped so
             * the battery check at the top of the loop keeps running. A
             * capped sleep that did not reach the boundary draws nothing and
             * just re-plans; a host wake-up always draws. Between wake-ups
             * the MCU idles and the panel rail is off — that is the sleep. */
            struct tm t;
            get_system_time(&t);
            int to_tick = power_profile_seconds_to_tick(&t);   /* -1: never */
            int wait_s = (to_tick < 0 || to_tick > POWER_PROFILE_MAX_SLEEP_S)
                         ? POWER_PROFILE_MAX_SLEEP_S : to_tick;
            int rc = k_sem_take(&sem_screensaver_wake, K_SECONDS(wait_s));
            tick_due = (rc == 0) || (wait_s == to_tick);
        }
    }
}

K_THREAD_DEFINE(screensaver_tid, 1536, screensaver_thread, NULL, NULL, NULL, 7, 0, 0);

/* UPDATE/APPLY: refresh fully, now. With the screensaver off this is already
 * what force_update does; with it on, the screensaver thread owns the panel and
 * decides the waveform, so the request has to travel with the wake-up. */
void display_manager_request_full_update(void) {
    if (screensaver_enabled) {
        full_refresh_requested = true;
    }
    display_manager_force_update();
}

/* Put whatever is in the frame buffers on the panel with a full refresh, from
 * the display thread. For callers on the BLE RX thread that must return at
 * once — a synchronous perform_display_update() there stalls every GATT
 * exchange for the length of the refresh, which is how DFU screens used to
 * kill the very transfer they announced. Image mode only; with the saver on,
 * display_manager_request_full_update() is the equivalent. */
void display_manager_request_frame_update(void) {
    frame_update_requested = true;
    k_sem_give(&sem_screensaver_wake);
}

void display_manager_force_update(void) {
    if (screensaver_enabled) {
        k_sem_give(&sem_screensaver_wake);
    } else {
        /* The caller (FAPPLY, host-written planes) owns the buffers: never
         * replay an earlier scene over them. */
        cur_scene = NULL;
        cur_scene_arg = NULL;
        if (red_staged) {
            perform_display_update_keep_red();
        } else {
            perform_display_update();
        }
    }
}

void display_manager_set_rotation(int rot) {
    graphics_set_rotation(rot);
    if (screensaver_enabled) {
         k_sem_give(&sem_screensaver_wake);
    }
}

void display_manager_set_keep_on(bool enable) {
    /* Drops the HV rails and can sleep the panel — bus work, so it takes the
     * lock like every other path that talks to the controller. */
    DISPLAY_LOCK();
    if (!enable) {
        stop_streaming_if_active();
    }
    keep_display_on = enable;
    if (!enable) {
        power_down_after_idle_update();
    }
    power_estimate_resync_idle();
    DISPLAY_UNLOCK();
}
