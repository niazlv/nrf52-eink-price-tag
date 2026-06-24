/*
 * factory.h — read-only access to the per-device provisioning block written
 * once at the factory into the `factory_data` flash partition (v2 layout only).
 *
 * The application NEVER writes this partition; a wired provisioning step lays
 * down a serial number and an authorization key at production time. OTA can not
 * create or move a partition, which is why it must exist in the layout from the
 * very first flash.
 *
 * On the legacy layout (no factory_data partition) every getter reports
 * "unprovisioned" so the single source tree still builds and runs unchanged.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FACTORY_SERIAL_MAXLEN 15
#define FACTORY_AUTH_KEY_LEN  16

/* True if a valid provisioning block is present (magic + crc verified). */
bool factory_is_provisioned(void);

/* NUL-terminated serial string, or "" if unprovisioned. */
const char *factory_serial(void);

/* Pointer to the 16-byte auth key, or NULL if unprovisioned / key not set. */
const uint8_t *factory_auth_key(void);
