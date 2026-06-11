#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/**
 * @brief Switch BLE between low-power idle mode and high-throughput streaming.
 *
 * Idle mode uses slow connectable advertising / relaxed connection params.
 * Streaming mode requests fast connection params for VSTREAM transfers.
 */
void ble_service_set_streaming_mode(bool enable);

/**
 * @brief true when BLE is in high-throughput VSTREAM mode.
 */
bool ble_service_get_streaming_mode(void);

/**
 * @brief Current runtime advertising/GAP name.
 */
const char *ble_service_get_device_name(void);

#endif // BLE_SERVICE_H
