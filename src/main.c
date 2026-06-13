/*
 * Refactored Main
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
#include <zephyr/dfu/mcuboot.h>
#endif
#include "app/display_manager.h"
#include "app/battery.h"
#include "app/commands.h"
#include "app/system_time.h"
#include "app/persist.h"
#include "ble/ble_service.h"
#include "lib/graphics.h"
#include <dk_buttons_and_leds.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void configure_gpio(void)
{
    // Initialize Button/LED library if needed, but BLE service handles LEDs via direct access or we should init here.
    // The original code used dk_library.
    dk_leds_init();
}

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
    configure_gpio();
    battery_init();
    system_time_init(); // Set time from Build Date/Time
    persist_init();     // Validate retained-RAM stats (survives DFU reboot)
    
    // 2. Init Graphics & Display
    // graphics_init is from lib/graphics.h
    graphics_init(); 
    display_manager_init();

    // 3. Init commands module (watchdog work item, etc.)
    commands_init();

    // 4. Init BLE with Command Processor
    int err = ble_service_init(commands_process);
    if (err) {
        LOG_ERR("BLE Init failed: %d", err);
        return 0;
    }

    // Settings are now loaded (ble_service_init -> settings_load): finalize
    // stats — restore from flash if RAM was lost, adopt saved clock, count boot.
    persist_post_settings();

    // 4. Thread will handle initial screen
    display_manager_update_status();

    LOG_INF("System Initialized & Ready - Auto Starting TEST");
    
    // Auto-start TEST
    // cmd_test(NULL);

    // 5. Main Loop (Unreachable if cmd_test loops forever)
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}