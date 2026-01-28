/*
 * Refactored Main
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include "app/display_manager.h"
#include "app/battery.h"
#include "app/commands.h"
#include "ble/ble_service.h"
#include "lib/graphics.h"

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

    // 1. Init Base Peripherals
    configure_gpio();
    battery_init();
    system_time_init(); // Set time from Build Date/Time
    
    // 2. Init Graphics & Display
    // graphics_init is from lib/graphics.h
    graphics_init(); 
    display_manager_init();

    // 3. Init BLE with Command Processor
    int err = ble_service_init(commands_process);
    if (err) {
        LOG_ERR("BLE Init failed: %d", err);
        return 0;
    }

    // 4. Thread will handle initial screen
    // display_manager_update_status();
    
    LOG_INF("System Initialized & Ready");

    // 5. Main Loop
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}