#ifndef APP_PERSIST_H
#define APP_PERSIST_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Persistent statistics that survive an OTA DFU.
 *
 * Strategy (hybrid, see persist.c):
 *   - Live store is a small block in RETAINED RAM (top of SRAM, carved out in
 *     the board + MCUboot devicetree overlays). nRF52 keeps SRAM contents
 *     across a *soft* reset as long as VDD is present, so this survives the
 *     warm reboot that finishes a DFU (and ordinary crashes), with ZERO flash
 *     wear. It does NOT survive a real power loss.
 *   - Flash (settings/NVS) is written only as a "last breath" on low battery,
 *     so the stats also survive a power loss without wearing the flash.
 */

/* Validate / set up the retained-RAM block. Call EARLY in main(), AFTER
 * system_time_init() and BEFORE settings_load(). */
void persist_init(void);

/* Finalize after settings_load(): restore from flash if retained RAM was lost
 * (cold boot / power loss), adopt a saved wall-clock newer than the build time,
 * detect a firmware change, and bump the boot counter. */
void persist_post_settings(void);

/* Periodic tick: refresh cumulative uptime + wall clock into retained RAM and
 * reseal the checksum. RAM-only, cheap — call roughly once per minute. */
void persist_tick(void);

/* Count one full display refresh. */
void persist_add_refresh(void);

/* Cumulative device uptime in seconds (survives DFU / warm reboot). */
uint64_t persist_uptime_sec(void);

/* Cumulative estimated energy (µAh×1000), survives DFU like uptime. The power
 * estimator in display_manager owns the rate model and pushes increments via
 * persist_add_energy_uah_x1000(); persist owns the persisted total. */
void persist_add_energy_uah_x1000(uint64_t delta);
uint64_t persist_get_energy_uah_x1000(void);

/* Replace the persisted energy total with a re-estimate, once, when the record
 * was written by an older power model than @p ver. Returns true if it did.
 * Call it after persist_post_settings(), or the flash restore undoes it. */
bool persist_adopt_energy_model(uint32_t ver, uint64_t uah_x1000);

/* Write the current stats to flash as a FRESH (unconsumed) snapshot. RARE —
 * called on low-battery/shutdown and right before a DFU reboot, when the live
 * RAM copy may be about to be lost. Keeps flash wear minimal. */
void persist_save_to_flash(void);

/* Reporting accessors. */
uint32_t persist_boot_count(void);
uint32_t persist_fw_update_count(void);
uint32_t persist_refreshes_total(void);
uint32_t persist_refreshes_since_fw(void);

/* Snapshot of both the live (RAM) stats and the flash record, for read-out
 * over BLE (STATS command / frontend). */
struct persist_report {
	/* Live values (current session, cumulative). */
	uint64_t uptime_total_sec;
	int64_t  wall_unix;
	uint32_t boot_count;
	uint32_t fw_update_count;
	uint32_t refreshes_total;
	uint32_t refreshes_since_fw;
	uint64_t energy_uah_x1000; /* cumulative power estimate (µAh×1000) */
	/* Flash record state. */
	bool     flash_present;   /* a blob was loaded from flash */
	bool     flash_consumed;  /* spent after a real firmware update */
	uint64_t flash_uptime_total_sec;
	int64_t  flash_wall_unix;
};

void persist_get_report(struct persist_report *out);

#endif /* APP_PERSIST_H */
