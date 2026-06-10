#include "ble_service.h"
#include "../app/commands.h"
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/util.h>
#include <bluetooth/services/nus.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/settings/settings.h>
#include <hal/nrf_ficr.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_service, LOG_LEVEL_INF);

#define DEVICE_NAME_DEV_SUFFIX "-DEV"
#define DEVICE_NAME_SUFFIX_HEX_LEN 6
#define ADV_FAST_INT_MIN 160   /* 100 ms */
#define ADV_FAST_INT_MAX 240   /* 150 ms */
#define ADV_IDLE_INT_MIN 800   /* 500 ms */
#define ADV_IDLE_INT_MAX 1280  /* 800 ms */
#define CON_STATUS_LED DK_LED2

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;
static struct k_work adv_work;
static ble_rx_callback_t app_rx_cb;
static bool ble_connected = false;
static bool ble_streaming_mode = false;
static bool adv_running = false;
static bool advertising_allowed = false;
static char device_name[CONFIG_BT_DEVICE_NAME_MAX + 1];

static const struct bt_le_adv_param adv_param_fast =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_FAST_INT_MIN, ADV_FAST_INT_MAX, NULL);
static const struct bt_le_adv_param adv_param_idle =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_IDLE_INT_MIN, ADV_IDLE_INT_MAX, NULL);

static const struct bt_le_conn_param conn_param_fast =
	BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);      /* 7.5-15 ms, VSTREAM */
static const struct bt_le_conn_param conn_param_idle =
	BT_LE_CONN_PARAM_INIT(80, 160, 4, 400);    /* 100-200 ms + latency, saver */

static size_t device_name_prefix_len(void)
{
	size_t len = strlen(CONFIG_BT_DEVICE_NAME);
	size_t suffix_len = strlen(DEVICE_NAME_DEV_SUFFIX);
	size_t max_prefix_len = sizeof(device_name) - 1 - 1 - DEVICE_NAME_SUFFIX_HEX_LEN;

	if (len > suffix_len &&
	    strcmp(&CONFIG_BT_DEVICE_NAME[len - suffix_len], DEVICE_NAME_DEV_SUFFIX) == 0) {
		len -= suffix_len;
	}

	return MIN(len, max_prefix_len);
}

static bool device_name_suffix_valid(const uint8_t suffix[3])
{
	return ((suffix[0] | suffix[1] | suffix[2]) != 0) &&
	       !(suffix[0] == 0xFF && suffix[1] == 0xFF && suffix[2] == 0xFF);
}

static void build_suffix_from_ficr(uint8_t suffix[3])
{
#if defined(NRF_FICR) && (NRF_FICR_HAS_DEVICE_ID || NRF_FICR_HAS_INFO_DEVICE_ID)
	uint32_t id0 = nrf_ficr_deviceid_get(NRF_FICR, 0);
	uint32_t id1 = nrf_ficr_deviceid_get(NRF_FICR, 1);
	uint32_t mixed = id0 ^ (id1 << 13) ^ (id1 >> 7);

	suffix[0] = (mixed >> 16) & 0xFF;
	suffix[1] = (mixed >> 8) & 0xFF;
	suffix[2] = mixed & 0xFF;
#else
	ARG_UNUSED(suffix);
#endif
}

static void build_device_name(void)
{
	bt_addr_le_t addrs[1];
	size_t count = ARRAY_SIZE(addrs);
	uint8_t suffix[3] = {0};
	size_t prefix_len;

	bt_id_get(addrs, &count);
	if (count > 0) {
		suffix[0] = addrs[0].a.val[2];
		suffix[1] = addrs[0].a.val[1];
		suffix[2] = addrs[0].a.val[0];
	}

	if (!device_name_suffix_valid(suffix)) {
		build_suffix_from_ficr(suffix);
	}

	prefix_len = device_name_prefix_len();
	snprintk(device_name, sizeof(device_name), "%.*s-%02X%02X%02X",
		 (int)prefix_len, CONFIG_BT_DEVICE_NAME,
		 suffix[0], suffix[1], suffix[2]);
}

static void apply_connection_params(void)
{
	const struct bt_le_conn_param *param;
	int err;

	if (!current_conn) {
		return;
	}

	param = ble_streaming_mode ? &conn_param_fast : &conn_param_idle;
	err = bt_conn_le_param_update(current_conn, param);
	if (err && err != -EALREADY) {
		LOG_WRN("Conn param update failed (err %d)", err);
	}
}

static void advertising_start(void)
{
	if (advertising_allowed) {
		k_work_submit(&adv_work);
	}
}

static void adv_work_handler(struct k_work *work)
{
	const struct bt_le_adv_param *param =
		ble_streaming_mode ? &adv_param_fast : &adv_param_idle;
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA(BT_DATA_NAME_COMPLETE, device_name, strlen(device_name)),
	};
	const struct bt_data sd[] = {
		BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
	};
	int err;

	if (!advertising_allowed || current_conn) {
		return;
	}

	if (adv_running) {
		bt_le_adv_stop();
		adv_running = false;
	}

	err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err == -EALREADY) {
		bt_le_adv_stop();
		err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	}

	if (err) {
		LOG_ERR("Advertising failed (err %d)", err);
		return;
	}

	adv_running = true;
	LOG_INF("Advertising %s as %s", ble_streaming_mode ? "fast" : "idle", device_name);
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
		adv_running = false;
		advertising_allowed = false;
		apply_connection_params();
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
	ble_service_set_streaming_mode(false);
	commands_on_disconnect();
}

static void recycled_cb(void)
{
	advertising_allowed = true;
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

	build_device_name();
	err = bt_set_name(device_name);
	if (err) {
		LOG_WRN("Dynamic GAP name failed (err %d), advertising name still unique", err);
	}
	LOG_INF("BLE name: %s", device_name);

	err = bt_nus_init(&nus_cb);
	if (err) LOG_ERR("NUS init err: %d", err);

	k_work_init(&adv_work, adv_work_handler);
	advertising_allowed = true;
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

void ble_service_set_streaming_mode(bool enable)
{
	if (ble_streaming_mode == enable) {
		return;
	}

	ble_streaming_mode = enable;
	apply_connection_params();

	if (!current_conn && advertising_allowed) {
		advertising_start();
	}
}

const char *ble_service_get_device_name(void)
{
	return device_name;
}
