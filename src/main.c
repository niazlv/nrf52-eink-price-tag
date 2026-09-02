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

int main(void)
{
    LOG_INF("Starting Application...");

#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    /* Confirm the running image FIRST, before any other init.
     * Moving this here prevents revert if later init is slow or fails. */
    if (!boot_is_img_confirmed()) {
        int confirm_rc = boot_write_img_confirmed();
        if (confirm_rc) {
            LOG_ERR("Early OTA confirm failed: %d", confirm_rc);
        } else {
            LOG_INF("OTA image confirmed (early)");
        }
    }
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

    // 5. Draw the first screen; the display thread takes over from here.
    display_manager_update_status();

    LOG_INF("System initialized");

    /* Everything from here runs in the display thread, the BLE callbacks and
     * the mesh dispatch thread. Park this one rather than returning, so its
     * stack stays available to whatever main() called into. */
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}