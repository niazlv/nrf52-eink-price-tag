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

/** Current connection's TX PHY: 2 (LE 2M), 1 (LE 1M) or 0 (no connection /
 *  PHY info unavailable). For SYSINFO, so a host can see what it got. */
int ble_service_get_phy(void);

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
 * @brief Set the idle (not streaming, not connected) advertising interval
 *
 * 1..10 s, applied immediately if idle advertising is running, otherwise on
 * the next start. The window keeps the 25% spread of the 2.0-2.5 s default.
 */
void ble_service_set_idle_adv_interval_s(uint8_t seconds);

/**
 * @brief Current runtime advertising/GAP name.
 */
const char *ble_service_get_device_name(void);

/**
 * @brief Set a user-defined name. The immutable per-device id (FICR/addr hex)
 * is always appended in parentheses and cannot be removed. Persists to NVS.
 * Pass NULL or "" to clear back to the default name. Returns bt_set_name() rc.
 */
int ble_service_set_custom_name(const char *name);

/**
 * @brief Current user-defined name part ("" if none set).
 */
const char *ble_service_get_custom_name(void);

/**
 * @brief The immutable 3-byte node id (same hex shown as (XXXXXX) in the name).
 *        Used as the mesh PDU source/destination address.
 */
void ble_service_get_node_id(uint8_t out[3]);

/**
 * @brief Borrow the legacy adv instance for a non-connectable mesh beacon.
 *
 * Time-shares the single advertiser: stops normal advertising and broadcasts
 * the given manufacturer-data payload. Legal while connected (non-connectable
 * adv forms no connection). Call ble_service_beacon_end() to restore normal
 * advertising. Returns 0 on success.
 *
 * @param mfg  Manufacturer-data bytes (company id + mesh PDU), <= 29 bytes.
 * @param len  Length of @p mfg.
 */
int ble_service_beacon_set(const uint8_t *mfg, uint8_t len);

/**
 * @brief Stop the mesh beacon and restore normal connectable advertising.
 */
void ble_service_beacon_end(void);

#endif // BLE_SERVICE_H
