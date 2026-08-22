#include "secauth.h"
#include "factory.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/crypto.h>
#include <errno.h>
#include <string.h>

#define SEC_KEY_LEN 16

/* ── Compiled-in default shared ("base") key ─────────────────────────────────
 * Every device ships knowing this key. It is replaceable at runtime by a peer
 * that proves knowledge of the *current* key (SETKEY); the replacement lives in
 * NVS and takes precedence. A factory_data-provisioned key also overrides this
 * default. CHANGE THESE BYTES before a production batch and keep them out of any
 * public copy of the sources. */
static const uint8_t default_key[SEC_KEY_LEN] = {
	0x9e, 0x1c, 0x47, 0xb3, 0x52, 0xa8, 0x0f, 0xd6,
	0x71, 0x2b, 0xc4, 0x88, 0x3a, 0xe5, 0x96, 0x10,
};

static uint8_t key[SEC_KEY_LEN];   /* effective key, resolved in secauth_init() */
static bool    enforce;            /* gate active? (persisted "sec/en") */

/* Per-connection state. BT_MAX_CONN=1, so a single set of statics is enough. */
static uint8_t nonce[SEC_KEY_LEN];
static bool    have_nonce;
static bool    authed;

/* Filled by the settings backend during settings_load(); resolved in init(). */
static uint8_t nvs_key[SEC_KEY_LEN];
static bool    nvs_key_present;

/* ── tiny hex helpers ─────────────────────────────────────────────────────── */
static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int hex_to_bin(const char *hex, uint8_t *out, size_t out_len)
{
	for (size_t i = 0; i < out_len; i++) {
		int hi = hexval(hex[i * 2]);
		int lo = hexval(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static void bin_to_hex(const uint8_t *bin, size_t len, char *out)
{
	static const char hx[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++) {
		out[i * 2]     = hx[bin[i] >> 4];
		out[i * 2 + 1] = hx[bin[i] & 0xF];
	}
	out[len * 2] = '\0';
}

void secauth_init(void)
{
	/* Effective key precedence: runtime (NVS) > factory_data > compiled default. */
	const uint8_t *fkey = factory_auth_key();

	if (nvs_key_present) {
		memcpy(key, nvs_key, SEC_KEY_LEN);
	} else if (fkey) {
		memcpy(key, fkey, SEC_KEY_LEN);
	} else {
		memcpy(key, default_key, SEC_KEY_LEN);
	}
}

bool secauth_enforced(void)  { return enforce; }
bool secauth_is_authed(void) { return authed; }
const uint8_t *secauth_key(void) { return key; }

void secauth_session_reset(void)
{
	authed = false;
	have_nonce = false;
	memset(nonce, 0, sizeof(nonce));
}

int secauth_make_challenge(char *nonce_hex, size_t cap)
{
	if (cap < SEC_KEY_LEN * 2 + 1) {
		return -1;
	}
	if (bt_rand(nonce, sizeof(nonce)) != 0) {
		return -1;
	}
	have_nonce = true;
	bin_to_hex(nonce, sizeof(nonce), nonce_hex);
	return 0;
}

bool secauth_verify(const char *resp_hex)
{
	uint8_t resp[SEC_KEY_LEN];
	uint8_t expected[SEC_KEY_LEN];

	if (!have_nonce) {
		return false;
	}
	have_nonce = false;   /* one-shot: a challenge is good for exactly one try */

	if (strlen(resp_hex) < SEC_KEY_LEN * 2 ||
	    hex_to_bin(resp_hex, resp, SEC_KEY_LEN) != 0) {
		return false;
	}
	/* expected = AES-128-ECB(key, nonce) via the BLE controller's AES. */
	if (bt_encrypt_be(key, nonce, expected) != 0) {
		return false;
	}

	/* A failed attempt does not revoke an already-authenticated session — the
	 * no-nonce path above leaves it alone too, and a typo'd second AUTH is no
	 * reason to drop a peer that already proved knowledge of the key. */
	if (memcmp(resp, expected, SEC_KEY_LEN) != 0) {
		return false;
	}
	authed = true;
	return true;
}

int secauth_set_key(const char *key_hex)
{
	uint8_t newkey[SEC_KEY_LEN];

	if (!authed) {
		return -1;   /* must prove knowledge of the current key first */
	}
	if (strlen(key_hex) < SEC_KEY_LEN * 2 ||
	    hex_to_bin(key_hex, newkey, SEC_KEY_LEN) != 0) {
		return -2;
	}
	if (settings_save_one("sec/k", newkey, SEC_KEY_LEN) != 0) {
		return -3;
	}
	memcpy(key, newkey, SEC_KEY_LEN);
	memcpy(nvs_key, newkey, SEC_KEY_LEN);
	nvs_key_present = true;
	return 0;
}

int secauth_set_enforced(bool on)
{
	uint8_t b = on ? 1 : 0;

	if (settings_save_one("sec/en", &b, 1) != 0) {
		return -1;
	}
	enforce = on;
	return 0;
}

/* ── settings backend ("sec/k" = key, "sec/en" = enforce flag) ─────────────── */
static int sec_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "k", NULL) && len == SEC_KEY_LEN) {
		if (read_cb(cb_arg, nvs_key, SEC_KEY_LEN) >= 0) {
			nvs_key_present = true;
			return 0;
		}
	} else if (settings_name_steq(name, "en", NULL) && len == 1) {
		uint8_t b = 0;
		if (read_cb(cb_arg, &b, 1) >= 0) {
			enforce = (b != 0);
			return 0;
		}
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(secauth, "sec", NULL, sec_settings_set, NULL, NULL);
