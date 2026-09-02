#ifndef APP_DISPLAY_MANAGER_H
#define APP_DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <eink/ssd1675a.h>

/**
 * @brief Initialize display manager
 */
void display_manager_init(void);

/**
 * @brief What the PROBE command learns about the attached panel controller.
 */
struct panel_probe_report {
    uint8_t status;            /* 0x2F Status Bit Read */
    uint8_t otp[11];           /* 0x2D: VCOM OTP sel, VCOM, display modes, waveform version */
    uint8_t uid[10];           /* 0x2E: 10-byte user ID from OTP */
    ssd1675a_ram_probe_t ram;  /* RAM-capacity probe, 240 rows × 50 B */
};

/**
 * @brief Identify the panel controller: register read-backs plus the RAM
 *        capacity probe. Wakes the panel, scribbles over its RAM, restores the
 *        current frame and powers down like any other idle update. Blocks for
 *        ~0.3 s; never call while a stream is running.
 */
void display_manager_probe_panel(struct panel_probe_report *r);

/** Panel geometry chosen by the boot-time probe: "400x300" or "128x296". */
const char *display_manager_panel_name(void);

/** false on panels driven with a single B/W plane (no red ink available). */
bool display_manager_panel_has_red(void);

/**
 * @brief Host-frame red on a single-plane panel: push the red mask the host
 *        wrote into the (only) frame buffer straight into the controller's
 *        red RAM and keep the panel awake. The host then writes the B/W frame
 *        and sends a plain FAPPLY, which refreshes without clearing red RAM.
 *        No-op on a B/W/R canvas (the red plane is a real buffer there).
 */
void display_manager_stage_red_plane(void);

/** Geometry test screen — border, 10-px ticks, corner tags, diagonal — as a
 *  full refresh. Shows clipped edges, wrong orientation or a stretched axis. */
void display_manager_show_ruler(void);

/** Full refreshes with the panel's OTP waveform (true) or the working LUT
 *  table (false). Defaults to true on the 400x300 panel, false on 128x296. */
void display_manager_set_full_refresh_otp(bool enable);
bool display_manager_get_full_refresh_otp(void);

/**
 * @brief Update the status screen (Time, Date, Battery)
 */
void display_manager_update_status(void);

/**
 * @brief Show specific text on display
 * 
 * @param text Text to display
 */
void display_manager_show_text(const char *text);
void display_manager_show_palette_test(void);
void display_manager_run_tone_test(void);

/**
 * @brief Run a cleaning cycle (7× B/W/R full updates)
 */
void display_manager_clean(void);

/**
 * @brief Deep clean: N× B/W/R full updates. Use after extended animation.
 *        cycles=20 takes ~20 × 3 × ~15s ≈ 15 minutes for full DC reset.
 */
void display_manager_deep_clean(int cycles);

/**
 * @brief Clear display to white
 */
void display_manager_clear(void);

/**
 * @brief Force a display update loop (async or blocking depending on impl)
 *
 * Redraws the current screen, but does NOT decide how: with the screensaver
 * running it only wakes the screensaver thread, and that thread refreshes
 * partially unless its own schedule is due a full cycle. Use it for redraws
 * that follow a small change (a new minute, a rotation).
 */
void display_manager_force_update(void);

/**
 * @brief Redraw now, as a FULL refresh, whatever the screensaver schedule says
 *
 * The partial waveforms drive only the B/W plane and leave every pixel whose
 * red-RAM bit is set untouched, so a panel that has drifted (ghosting after a
 * long-lived screen, a stale red plane) cannot be recovered by any number of
 * partial redraws. This is the "clean it up now" path behind UPDATE/APPLY.
 */
void display_manager_request_full_update(void);

/**
 * @brief Image mode: show the current frame buffers with a full refresh, asynchronously
 *
 * Returns at once; the display thread does the refresh. Use from the BLE RX
 * thread instead of a synchronous update.
 */
void display_manager_request_frame_update(void);

/**
 * @brief Re-estimate the persisted energy total after a power-model change
 *
 * A no-op unless the stored record predates the current model revision. Must be
 * called after persist_post_settings(): the flash restore there would otherwise
 * overwrite the adopted value.
 */
void display_manager_recalibrate_energy(void);

struct power_profile;
/**
 * @brief Average current the power model predicts for a sleep profile, in µA
 *
 * Uses the same constants as the running mAh estimator. picture = screensaver
 * off (no redraws, picture advertising interval).
 */
int display_manager_estimate_avg_ua(const struct power_profile *p, bool mesh_rx,
                                    bool picture);

/**
 * @brief Force a partial (fast) display update (full HV cycle, ~700ms overhead)
 */
void display_manager_update_partial(void);

/**
 * @brief Pipelined partial update: SPI transfer + trigger, but does NOT wait for
 *        BUSY. Caller must call ssd1675a_wait_busy() before the next SPI write.
 *        Only valid when keep_on=true (streaming mode). Falls back to the blocking
 *        variant otherwise.
 */
void display_manager_update_partial_nowait(void);

/**
 * @brief Like display_manager_update_partial_nowait(), but only rows
 *        y0..y1 of the B/W plane are sent to the controller (y1 < y0: none —
 *        the refresh still runs on the RAM as it is). Streaming deltas use
 *        it to pay bus time only for the rows they changed. Falls back to a
 *        full write when the red plane is streamed too.
 */
void display_manager_update_partial_rows_nowait(int y0, int y1);

/**
 * @brief Streaming animation mode: charge HV rails once, then call
 *        update_frame_stream() per frame (~LUT wave time only), then end_streaming().
 */
void display_manager_begin_streaming(void);
void display_manager_update_frame_stream(void);
void display_manager_end_streaming(void);

/**
 * @brief When enabled, streaming partial updates write both BW and red RAM.
 *        Used by tone-servo video, where red RAM is repurposed as a hold bit.
 */
void display_manager_set_stream_write_red_plane(bool enable);

/**
 * @brief Set the partial update mode (0=Turbo, 1=Balanced, 2=Stable,
 *        3=Clean, 4=Tone dark, 5=Tone light, 6=Tone bidir fast,
 *        7=Tone bidir)
 */
void display_manager_set_partial_mode(int mode);

/**
 * @brief Set display rotation
 * 
 * @param rot Rotation 0-3
 */
void display_manager_set_rotation(int rot);

/**
 * @brief Enable or disable automatic screensaver updates
 * 
 * @param enable true to enable, false to disable
 */
void display_manager_enable_screensaver(bool enable);

/**
 * @brief Come up in picture mode without drawing anything (silent update)
 *
 * Call instead of the first display_manager_update_status() at boot when the
 * previous firmware left PERSIST_BF_RESUME_PICTURE: the panel keeps the image
 * it already shows.
 */
void display_manager_boot_into_picture(void);
    
/**
 * @brief Keep display powered strictly ON (for animations)
 */
void display_manager_set_keep_on(bool enable);

/**
 * @brief Check if screensaver is currently active
 */
bool display_manager_is_screensaver_active(void);

#define SCREENSAVER_MODE_STATIC   0
#define SCREENSAVER_MODE_DYNAMIC  1
#define SCREENSAVER_MODE_LUT_TEST 2
/**
 * @brief Set screensaver style (Static/Minute, Dynamic/Animation, or LUT test)
 */
void display_manager_set_screensaver_mode(int mode);

/**
 * @brief Run one LUT test frame (render + partial update + timing measurement)
 */
void display_manager_update_lut_test(void);

/**
 * @brief Reset LUT test frame counter and timing stats
 */
void display_manager_reset_lut_test(void);

/**
 * @brief Query current LUT test timing stats (pass NULL for unused outputs)
 */
void display_manager_get_lut_test_stats(int32_t *frame_out, int32_t *cur_ms_out,
                                        int32_t *min_ms_out, int32_t *max_ms_out);

/**
 * @brief Enable/disable automatic TELE: BLE telemetry after every display update.
 *        Called by commands.c when HOST:1 / HOST:0 is received.
 */
void display_manager_set_tele_enabled(bool en);
bool display_manager_get_tele_enabled(void);

/**
 * @brief Get cumulative energy consumed (mAh × 1000). Persists across DFU.
 */
uint32_t display_manager_get_energy_mah_x1000(void);

/**
 * @brief Fold energy accrued since the last accounting point into the persisted
 *        total. Call right before a stats snapshot / reboot save.
 */
void display_manager_flush_energy(void);

/**
 * @brief Get current estimated power draw (µA)
 */
int display_manager_get_estimated_current_ua(void);

#endif // APP_DISPLAY_MANAGER_H
