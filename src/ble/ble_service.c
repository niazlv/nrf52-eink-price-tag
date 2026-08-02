#include "ble_service.h"
#include "../app/commands.h"
#include "../app/mesh.h"
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
#define ADV_BEACON_INT_MIN 48  /* 30 ms — fast so a flooded PDU is caught quickly */
#define ADV_BEACON_INT_MAX 80  /* 50 ms */
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
/* User-settable name part; "" = use the default. The immutable per-device id
 * (FICR/addr hex) is always appended in parentheses and cannot be removed. */
static char custom_name[CONFIG_BT_DEVICE_NAME_MAX + 1];

static const struct bt_le_adv_param adv_param_fast =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_FAST_INT_MIN, ADV_FAST_INT_MAX, NULL);
static const struct bt_le_adv_param adv_param_idle =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_IDLE_INT_MIN, ADV_IDLE_INT_MAX, NULL);
/* Non-connectable, non-scannable beacon for mesh PDUs. opts=0 → no CONN bit, so
 * it never forms a connection and is legal to run even while connected to the
 * phone (the single legacy adv instance is idle during a connection). */
static const struct bt_le_adv_param adv_param_beacon =
	BT_LE_ADV_PARAM_INIT(0, ADV_BEACON_INT_MIN, ADV_BEACON_INT_MAX, NULL);

/* True while the adv instance is borrowed for a mesh beacon. adv_work_handler
 * must not clobber it; ble_service_beacon_end() restores normal advertising. */
static bool beacon_active;

/* Apple accessory guidelines: Interval Min >= 15 ms and
 * Interval Max >= Interval Min + 15 ms, otherwise macOS/iOS rejects the
 * request and the connection stays at the (slow) default interval. */
static const struct bt_le_conn_param conn_param_fast =
	BT_LE_CONN_PARAM_INIT(12, 24, 0, 400);     /* 15-30 ms, VSTREAM */
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

/* The immutable 3-byte node id: BLE identity address high bytes, or a FICR mix
 * if the address is unusable. Same bytes shown in the device name (XXXXXX) and
 * used as the mesh PDU source/destination address. */
static void get_node_id3(uint8_t out[3])
{
	bt_addr_le_t addrs[1];
	size_t count = ARRAY_SIZE(addrs);
	uint8_t suffix[3] = {0};

	bt_id_get(addrs, &count);
	if (count > 0) {
		suffix[0] = addrs[0].a.val[2];
		suffix[1] = addrs[0].a.val[1];
		suffix[2] = addrs[0].a.val[0];
	}

	if (!device_name_suffix_valid(suffix)) {
		build_suffix_from_ficr(suffix);
	}

	out[0] = suffix[0];
	out[1] = suffix[1];
	out[2] = suffix[2];
}

void ble_service_get_node_id(uint8_t out[3])
{
	get_node_id3(out);
}

static void build_device_name(void)
{
	uint8_t suffix[3];
	size_t prefix_len;

	get_node_id3(suffix);

	if (custom_name[0] != '\0') {
		/* "<user name> (XXXXXX)" — the parenthesised id is permanent. */
		const size_t tail = sizeof(" (XXXXXX)") - 1;   /* 9 chars */
		size_t max_user = sizeof(device_name) - 1 - tail;
		snprintk(device_name, sizeof(device_name), "%.*s (%02X%02X%02X)",
			 (int)max_user, custom_name,
			 suffix[0], suffix[1], suffix[2]);
	} else {
		prefix_len = device_name_prefix_len();
		snprintk(device_name, sizeof(device_name), "%.*s-%02X%02X%02X",
			 (int)prefix_len, CONFIG_BT_DEVICE_NAME,
			 suffix[0], suffix[1], suffix[2]);
	}
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

	if (beacon_active) {
		return;   /* a mesh beacon owns the adv instance; it will restore us */
	}

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

static void request_phy_2m(struct bt_conn *conn)
{
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	int err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);

	if (err) {
		LOG_WRN("2M PHY request failed (err %d)", err);
	}
#else
	ARG_UNUSED(conn);
#endif
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
		request_phy_2m(conn);
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	/* interval is in 1.25 ms units. Surface the granted values to the host
	 * so vstream can tell whether the central honored the fast params. */
	LOG_INF("Conn params: int=%u.%02ums lat=%u to=%ums",
		(interval * 125) / 100, (interval * 125) % 100, latency, timeout * 10);
	ble_printf("TELE:conn int=%u.%02ums lat=%u to=%ums\r\n",
		   (interval * 125) / 100, (interval * 125) % 100, latency, timeout * 10);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	LOG_INF("PHY: tx=%u rx=%u", param->tx_phy, param->rx_phy);
	ble_printf("TELE:phy tx=%uM rx=%uM\r\n",
		   param->tx_phy == BT_GAP_LE_PHY_2M ? 2 : 1,
		   param->rx_phy == BT_GAP_LE_PHY_2M ? 2 : 1);
}
#endif

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
	.le_param_updated = le_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated   = le_phy_updated,
#endif
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

/* Persisted user name under "cfg/name". */
static int cfg_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "name", NULL)) {
		if (len >= sizeof(custom_name)) {
			len = sizeof(custom_name) - 1;
		}
		int rc = read_cb(cb_arg, custom_name, len);
		if (rc >= 0) {
			custom_name[rc] = '\0';
			return 0;
		}
		return rc;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(blecfg, "cfg", NULL, cfg_settings_set, NULL, NULL);

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

int ble_service_beacon_set(const uint8_t *mfg, uint8_t len)
{
    /* Borrow the single legacy adv instance for a non-connectable beacon.
     * `mfg` is the manufacturer-specific data payload (company id + mesh PDU);
     * BT_DATA wraps it with the length + 0xFF AD type. Safe while connected:
     * non-connectable adv never forms a second connection. */
    struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, len),
    };
    int err;

    if (adv_running || beacon_active) {
        bt_le_adv_stop();
        adv_running = false;
    }
    beacon_active = true;
    err = bt_le_adv_start(&adv_param_beacon, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        beacon_active = false;
        LOG_WRN("Beacon start failed (err %d)", err);
    }
    return err;
}

void ble_service_beacon_end(void)
{
    if (!beacon_active) {
        return;
    }
    bt_le_adv_stop();
    beacon_active = false;
    /* Restore the normal connectable adv if we're idle and allowed to. */
    if (advertising_allowed && !current_conn) {
        advertising_start();
    }
}

void ble_printf(const char *fmt, ...) {
    /* Commands dispatched from a mesh flood run on the mesh thread; their replies
     * are meaningless to a locally-connected phone (they belong to the remote
     * originator), so drop them. Phase 2 will route them back into the mesh. */
    if (mesh_is_dispatch_thread()) {
        return;
    }
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

bool ble_service_get_streaming_mode(void)
{
	return ble_streaming_mode;
}

const char *ble_service_get_device_name(void)
{
	return device_name;
}

int ble_service_set_custom_name(const char *name)
{
	if (!name) {
		name = "";
	}
	strncpy(custom_name, name, sizeof(custom_name) - 1);
	custom_name[sizeof(custom_name) - 1] = '\0';

	/* Persist including the NUL so an empty string clears the entry cleanly. */
	(void)settings_save_one("cfg/name", custom_name, strlen(custom_name) + 1);

	build_device_name();
	int err = bt_set_name(device_name);

	if (!current_conn && advertising_allowed) {
		advertising_start();   /* re-advertise with the new name */
	}
	return err;
}

const char *ble_service_get_custom_name(void)
{
	return custom_name;
}
