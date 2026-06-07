#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Callback for received data
 */
typedef void (*ble_rx_callback_t)(const void *data, uint16_t len);

/**
 * @brief Initialize BLE stack and NUS service
 * 
 * @param rx_cb Callback function when data is received over NUS
 * @return 0 on success
 */
int ble_service_init(ble_rx_callback_t rx_cb);

/**
 * @brief Send data via NUS
 * 
 * @param data Pointer to data
 * @param len Length of data
 * @return 0 on success
 */
int ble_service_send(const char *data, uint16_t len);

/**
 * @brief Formatted print to BLE (like printf)
 * 
 * @param fmt Format string
 * @param ... Arguments
 */
void ble_printf(const char *fmt, ...);

/**
 * @brief Check if BLE is connected
 */
int ble_service_is_connected(void);

#endif // BLE_SERVICE_H
