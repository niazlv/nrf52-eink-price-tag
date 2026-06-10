#include "ble_service.h"
#include "../app/commands.h"
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <bluetooth/services/nus.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <stdarg.h>

LOG_MODULE_REGISTER(ble_service, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)
#define CON_STATUS_LED DK_LED2

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;
static struct k_work adv_work;
static ble_rx_callback_t app_rx_cb;
static bool ble_connected = false;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) LOG_ERR("Advertising failed (err %d)", err);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	if (err) {
		LOG_ERR("Connection failed (err 0x%02x)", err);
	} else {
		bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
		LOG_INF("Connected %s", addr);
		current_conn = bt_conn_ref(conn);
		dk_set_led_on(CON_STATUS_LED);
        ble_connected = true;
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);
	if (auth_conn) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
		dk_set_led_off(CON_STATUS_LED);
	}
    ble_connected = false;
    commands_on_disconnect();
}

static void recycled_cb(void)
{
	advertising_start();
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (!err) LOG_INF("Security changed: %s level %u", addr, level);
	else LOG_WRN("Security failed: %s err %d", addr, err);
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};

// Auth callbacks omitted for brevity/simplicity unless needed, code had them before.
// I'll re-add them to be safe if `CONFIG_BT_NUS_SECURITY_ENABLED` is on.
#if defined(CONFIG_BT_NUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Passkey for %s: %06u", addr, passkey);
}
static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey) {
	char addr[BT_ADDR_LE_STR_LEN];
	auth_conn = bt_conn_ref(conn);
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Passkey for %s: %06u", addr, passkey);
}
static void auth_cancel(struct bt_conn *conn) {
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing cancelled: %s", addr);
}
static void pairing_complete(struct bt_conn *conn, bool bonded) {
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing failed: %s, reason %d", addr, reason);
}
static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};
static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#endif

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
    if (app_rx_cb) {
        app_rx_cb(data, len);
    }
}

static struct bt_nus_cb nus_cb = {
	.received = bt_receive_cb,
};

int ble_service_init(ble_rx_callback_t rx_cb) {
    int err;
    app_rx_cb = rx_cb;

    // Assuming dk_leds_init was called in main, but we can call it here if needed.
    // dk_leds_init(); // Safe to call multiple times? Probably.

    #ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) LOG_ERR("Auth cb err: %d", err);
	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) LOG_ERR("Auth info cb err: %d", err);
    #endif

    err = bt_enable(NULL);
	if (err) return err;

	LOG_INF("Bluetooth initialized");

    if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load(); 
	}

	err = bt_nus_init(&nus_cb);
	if (err) LOG_ERR("NUS init err: %d", err);

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

    return 0;
}

int ble_service_send(const char *data, uint16_t len) {
    if (current_conn) {
        return bt_nus_send(current_conn, data, len);
    }
    return -ENOTCONN;
}

void ble_printf(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        ble_service_send(buf, len);
        LOG_INF("%s", buf); // Also log to RTT/UART
    }
}

int ble_service_is_connected(void) {
    return ble_connected ? 1 : 0;
}
