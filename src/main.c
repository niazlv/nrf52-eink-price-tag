/*
 * Application entry point: brings the modules up in dependency order,
 * then parks the main thread — all work happens in BLE callbacks, the
 * display thread and the system work queue.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/settings/settings.h>
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
#include <zephyr/dfu/mcuboot.h>
#endif
#include "app/display_manager.h"
#include "app/battery.h"
#include "app/commands.h"
#include "app/system_time.h"
#include "app/persist.h"
#include "app/power_profile.h"
#include "app/secauth.h"
#include "app/mesh.h"
#include "ble/ble_service.h"
#include <gfx/graphics.h>
#include <dk_buttons_and_leds.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#ifdef CONFIG_MCUBOOT_IMG_MANAGER
static void confirm_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!boot_is_img_confirmed()) {
        int rc = boot_write_img_confirmed();
        if (rc) {
            LOG_ERR("OTA confirm failed: %d", rc);
        } else {
            LOG_INF("OTA image confirmed");
        }
    }
}
static K_WORK_DELAYABLE_DEFINE(confirm_work, confirm_handler);
#endif

int main(void)
{
    LOG_INF("Starting Application...");

#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    /* Confirm the running image only once it has demonstrably run: 15 s in,
     * from the system work queue. Confirming in the first line of main() made
     * a crashing image permanent — the reset it caused booted the same image
     * again, forever, and the only way back was a wire. Unconfirmed, a reset
     * within this window makes MCUboot swap the previous image back in, and
     * the tag comes back on the firmware that worked. Init here takes well
     * under two seconds, so a healthy image always gets confirmed; the one
     * cost is a power cycle inside the first 15 s of a new image, which
     * simply means updating again. */
    k_work_schedule(&confirm_work, K_SECONDS(15));
#endif

    // 1. Init Base Peripherals
    dk_leds_init();     // ble_service drives the LEDs via dk_set_led_*
    int batt_rc = battery_init();
    if (batt_rc) {
        LOG_ERR("Battery ADC init failed: %d — voltage reads will fail", batt_rc);
    }
    system_time_init(); // Set time from Build Date/Time
    persist_init();     // Validate retained-RAM stats (survives DFU reboot)


    // 2. Init Graphics & Display
    graphics_init();
    display_manager_init();

    // 3. Init commands module (watchdog work item, etc.)
    commands_init();

    // 4. Init BLE with Command Processor
    int err = ble_service_init(commands_process);
    if (err) {
        /* Do not bail out: the panel, the clock and the persisted statistics
         * work without BLE, and returning here would skip the settings load
         * and persist_post_settings() below — the boot would go uncounted and
         * the saved clock unadopted, while the display thread kept running. */
        LOG_ERR("BLE Init failed: %d — continuing without BLE", err);
        if (IS_ENABLED(CONFIG_SETTINGS)) {
            /* subsys_init first: ble_service_init() failed at bt_enable(),
             * before it got to its own settings_load(), so no backend is
             * registered yet and a bare settings_load() would silently load
             * nothing. Both calls are idempotent. */
            settings_subsys_init();
            settings_load();
        }
        secauth_init();   /* resolve the key even with no radio, so SYSINFO
                           * does not report against an all-zero key */
    } else {
        // Settings are now loaded (ble_service_init -> settings_load). Resolve
        // the effective auth key: NVS override > factory_data > compiled default.
        secauth_init();

        // Connectionless flood-mesh: needs the resolved node id
        // (ble_service_init) and the effective key (secauth_init) for PDU
        // signing. Starts scanning + the dispatch thread. Both need a live BT
        // stack, so this is the one step BLE failure has to skip.
        mesh_init();
    }

    // Settings are now loaded (ble_service_init -> settings_load): finalize
    // stats — restore from flash if RAM was lost, adopt saved clock, count boot.
    persist_post_settings();

    /* After the restore above, so the adopted total is not overwritten by it. */
    display_manager_recalibrate_energy();

    /* The persisted sleep profile is loaded now too: push its advertising
     * interval into the radio (the display thread reads the rest itself). */
    power_profile_apply();

    // 5. Draw the first screen; the display thread takes over from here —
    //    unless the stored display mode says this tag is showing a picture, in
    //    which case we draw nothing at all. The panel is bistable, so the image
    //    is still on it: after a reboot, and after a battery change years from
    //    now, the picture comes back by itself and nobody has to re-send it.
    if (power_display_saver_get()) {
        display_manager_update_status();
    } else {
        display_manager_boot_into_picture();
    }

    LOG_INF("System initialized");

    /* Everything from here runs in the display thread, the BLE callbacks and
     * the mesh dispatch thread. Park this one rather than returning, so its
     * stack stays available to whatever main() called into. */
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}