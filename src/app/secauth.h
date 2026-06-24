/*
 * secauth.h — deliberately *basic* command/DFU gate so random peers can not
 * push garbage at a tag. NOT a hardened crypto stack (the real firmware-
 * authenticity guarantee is the MCUboot signing key, enforced by the
 * bootloader). This layer is a shared-key challenge-response:
 *
 *   1. peer sends  AUTH           -> device replies AUTH:CHAL <16-byte nonce hex>
 *   2. peer sends  AUTH <resp>    -> resp must equal AES-128-ECB(key, nonce)
 *                                    (WebCrypto: AES-CBC, IV=0, first block)
 *   3. once authed, privileged commands are accepted for this connection.
 *
 * Effective key precedence: runtime override in NVS > factory_data key >
 * compiled-in default (the base fleet key). The key is replaceable at runtime
 * via SETKEY, but only by a peer that already proved knowledge of the current
 * key. Enforcement (whether the gate is active at all) defaults to OFF so the
 * existing host/DFU flow keeps working until the client speaks the handshake.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Resolve the effective key from NVS/factory/default. Call AFTER settings_load(). */
void secauth_init(void);

bool secauth_enforced(void);    /* is the access gate active? */
bool secauth_is_authed(void);   /* did the current connection authenticate? */
void secauth_session_reset(void); /* clear per-connection state (on disconnect) */

/* Fill nonce_hex (needs >= 33 bytes) with a fresh challenge. 0 on success. */
int  secauth_make_challenge(char *nonce_hex, size_t cap);

/* Verify a 32-hex-char response against the last challenge; sets authed. */
bool secauth_verify(const char *resp_hex);

/* Replace the key (32 hex chars). Requires the session to be authed already.
 * Persists to NVS. Returns 0 on success, negative on error. */
int  secauth_set_key(const char *key_hex);

/* Enable/disable enforcement. Persists to NVS. 0 on success. */
int  secauth_set_enforced(bool on);
