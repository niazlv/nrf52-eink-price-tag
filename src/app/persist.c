#include "persist.h"
#include "system_time.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>

#include <stddef.h>
#include <string.h>
#include <time.h>

/* NOTE: no logging here on purpose — all log backends are disabled in this
 * build (RTT/UART/console off), so log strings would be pure flash waste, and
 * the app sits right at the swap-using-move size limit (56 of 58 sectors). */

#define PERSIST_MAGIC 0x50525331u /* "PRS1" */

/* flags bits */
#define PERSIST_F_CONSUMED BIT(0) /* flash snapshot already used (post real DFU) */

/* Layout is frozen: only ever append into reserved[] so a flash blob written by
 * an older firmware stays compatible. crc covers everything after the crc field. */
struct persist_data {
	uint32_t magic;
	uint32_t crc;
	uint64_t uptime_total_sec;  /* cumulative across all boots */
	int64_t  wall_unix;         /* last known wall clock (unix seconds) */
	int64_t  last_build_unix;   /* build stamp of the last-seen firmware */
	uint32_t boot_count;
	uint32_t fw_update_count;   /* times a different firmware booted */
	uint32_t refreshes_total;   /* full display refreshes, all time */
	uint32_t refreshes_since_fw;/* full refreshes since last firmware change */
	uint32_t flags;             /* PERSIST_F_* — meaningful only in the flash copy */
	uint32_t reserved[7];
};

#define CRC_OFFSET offsetof(struct persist_data, uptime_total_sec)

/*
 * The retained block lives just above the (shrunk) sram0 region. The board
 * overlay (boards/nrf52dk_nrf52832.overlay) shrinks sram0 and declares the
 * `sram_retained` node; the matching MCUboot overlay shrinks sram0 too so the
 * bootloader's stack cannot clobber it across a DFU reboot. On boards without
 * that node we fall back to plain RAM (stats simply won't persist).
 */
#if DT_NODE_EXISTS(DT_NODELABEL(sram_retained))
#define RETAINED_ADDR DT_REG_ADDR(DT_NODELABEL(sram_retained))
#define RETAINED_SIZE DT_REG_SIZE(DT_NODELABEL(sram_retained))
static volatile struct persist_data *const rd =
	(volatile struct persist_data *)RETAINED_ADDR;
BUILD_ASSERT(sizeof(struct persist_data) <= RETAINED_SIZE,
	     "persist_data exceeds reserved retained region");
#else
#warning "No sram_retained node: stats will NOT survive reboot on this board"
static volatile struct persist_data rd_storage;
static volatile struct persist_data *const rd = &rd_storage;
#endif

static struct k_spinlock lock;
static int64_t session_base_sec; /* uptime_total at the start of this session */
static bool ram_was_valid;

/* Set during settings_load() if a flash copy is present. */
static struct persist_data flash_copy;
static bool flash_copy_valid;

static uint32_t calc_crc_of(const struct persist_data *p)
{
	return crc32_ieee((const uint8_t *)p + CRC_OFFSET,
			  sizeof(struct persist_data) - CRC_OFFSET);
}

/* Caller must hold `lock`. */
static void reseal_locked(void)
{
	rd->magic = PERSIST_MAGIC;
	rd->crc = calc_crc_of((const struct persist_data *)rd);
}

static bool retained_valid(void)
{
	return rd->magic == PERSIST_MAGIC &&
	       rd->crc == calc_crc_of((const struct persist_data *)rd);
}

static bool flash_blob_ok(void)
{
	return flash_copy_valid && flash_copy.magic == PERSIST_MAGIC &&
	       flash_copy.crc == calc_crc_of(&flash_copy);
}

/* Caller must hold `lock`. Roll the live counters forward in RAM. */
static void refresh_live_locked(void)
{
	rd->uptime_total_sec = (uint64_t)(session_base_sec + k_uptime_get() / 1000);
	rd->wall_unix = get_system_time_unix();
}

void persist_init(void)
{
	ram_was_valid = retained_valid();
	if (ram_was_valid) {
		/* Survived a warm reboot (DFU / crash): keep accumulated stats. */
		session_base_sec = (int64_t)rd->uptime_total_sec;
	} else {
		memset((void *)rd, 0, sizeof(struct persist_data));
		session_base_sec = 0;
	}

	K_SPINLOCK(&lock) {
		reseal_locked();
	}
}

void persist_post_settings(void)
{
	const int64_t build_unix = (int64_t)system_time_get_build_unix();
	bool real_update = false;

	K_SPINLOCK(&lock) {
		/* Recovery: RAM was lost (cold boot / old MCUboot clobbered it) but
		 * flash holds a snapshot. Only restore an UNCONSUMED valid blob —
		 * a consumed one is kept for read-out but must not resurrect as live
		 * state after a power loss. */
		if (!ram_was_valid && flash_blob_ok() &&
		    !(flash_copy.flags & PERSIST_F_CONSUMED)) {
			memcpy((void *)rd, &flash_copy, sizeof(struct persist_data));
			session_base_sec = (int64_t)rd->uptime_total_sec;
		}

		/* Live RAM is never "consumed". */
		rd->flags = 0;

		/* Firmware change (DFU or reflash): build timestamp differs. A *real*
		 * update is one where we had a previous build on record. */
		if (rd->last_build_unix != build_unix) {
			if (rd->last_build_unix != 0) {
				rd->fw_update_count++;
				real_update = true;
			}
			rd->last_build_unix = build_unix;
			rd->refreshes_since_fw = 0;
		}

		/* Adopt saved wall clock only if it is newer than the build time
		 * (across a DFU reboot it is; the build clock would be stale). */
		if (rd->wall_unix > build_unix) {
			set_system_time_unix((time_t)rd->wall_unix);
		}

		rd->boot_count++;
		reseal_locked();
	}

	/* After a real firmware update, mark the pre-DFU flash snapshot as spent:
	 * it has been carried into the live RAM copy, so it must not be restored
	 * again after a later power loss (it would be stale). The bytes remain in
	 * flash for read-out (STATS shows flash=consumed). One flash write. */
	if (real_update && flash_blob_ok() &&
	    !(flash_copy.flags & PERSIST_F_CONSUMED)) {
		flash_copy.flags |= PERSIST_F_CONSUMED;
		flash_copy.crc = calc_crc_of(&flash_copy);
		(void)settings_save_one("ps/d", &flash_copy, sizeof(flash_copy));
	}
}

void persist_tick(void)
{
	K_SPINLOCK(&lock) {
		refresh_live_locked();
		reseal_locked();
	}
}

void persist_add_refresh(void)
{
	K_SPINLOCK(&lock) {
		rd->refreshes_total++;
		rd->refreshes_since_fw++;
		reseal_locked();
	}
}

uint64_t persist_uptime_sec(void)
{
	return (uint64_t)(session_base_sec + k_uptime_get() / 1000);
}

void persist_save_to_flash(void)
{
	struct persist_data snap;

	K_SPINLOCK(&lock) {
		refresh_live_locked();
		reseal_locked();
		memcpy(&snap, (const void *)rd, sizeof(snap));
	}

	/* Fresh snapshot: explicitly unconsumed, recompute crc after clearing. */
	snap.flags &= ~PERSIST_F_CONSUMED;
	snap.crc = calc_crc_of(&snap);

	if (settings_save_one("ps/d", &snap, sizeof(snap)) == 0) {
		/* Mirror to our in-RAM view so STATS reflects the new flash record. */
		flash_copy = snap;
		flash_copy_valid = true;
	}
}

uint32_t persist_boot_count(void)        { return rd->boot_count; }
uint32_t persist_fw_update_count(void)   { return rd->fw_update_count; }
uint32_t persist_refreshes_total(void)   { return rd->refreshes_total; }
uint32_t persist_refreshes_since_fw(void){ return rd->refreshes_since_fw; }

void persist_get_report(struct persist_report *out)
{
	if (!out) {
		return;
	}

	K_SPINLOCK(&lock) {
		out->uptime_total_sec =
			(uint64_t)(session_base_sec + k_uptime_get() / 1000);
		out->wall_unix = get_system_time_unix();
		out->boot_count = rd->boot_count;
		out->fw_update_count = rd->fw_update_count;
		out->refreshes_total = rd->refreshes_total;
		out->refreshes_since_fw = rd->refreshes_since_fw;
	}

	out->flash_present = flash_copy_valid;
	out->flash_consumed = flash_copy_valid &&
			      (flash_copy.flags & PERSIST_F_CONSUMED);
	out->flash_uptime_total_sec =
		flash_copy_valid ? flash_copy.uptime_total_sec : 0;
	out->flash_wall_unix = flash_copy_valid ? flash_copy.wall_unix : 0;
}

/* Settings backend: a single binary blob under "ps/d". */
static int persist_settings_set(const char *name, size_t len,
				settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "d", NULL) && len == sizeof(flash_copy)) {
		int rc = read_cb(cb_arg, &flash_copy, sizeof(flash_copy));
		if (rc >= 0) {
			flash_copy_valid = true;
			return 0;
		}
		return rc;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(persist, "ps", NULL, persist_settings_set,
			       NULL, NULL);
