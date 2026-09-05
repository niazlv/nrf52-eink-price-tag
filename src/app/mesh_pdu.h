#ifndef APP_MESH_PDU_H
#define APP_MESH_PDU_H

/*
 * Mesh PDU codec, authentication and replay protection — plain C99, no OS or
 * Bluetooth dependency, so the whole wire layer is testable on a host
 * (tests/host/test_mesh_pdu.c). mesh.c owns the radio, the threads and the
 * persistence; this file owns the bytes.
 *
 * Wire PDU (inside manufacturer-data AD, company 0xFFFF, <= 27 bytes):
 *   [MAGIC:1][ver<<4|ttl:1][seq:2 LE][src:3][dst_type:1][dst:0/1/3][opcode:1]
 *   [payload:N][mac:4]
 * The MAC is a truncated AES-CMAC (RFC 4493) over everything before it, with
 * the TTL nibble masked to zero because relays decrement it in flight.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_MAGIC       0xE5u
#define MESH_VER         0x0u
#define MESH_MAC_LEN     4u
#define MESH_PDU_MAX     27u        /* one legacy adv: 31 - 4 (AD hdr + company) */
#define MESH_HDR_MIN     (1 + 1 + 2 + 3 + 1)   /* magic, ver/ttl, seq, src, dst_type */
/* Payload room with the shortest (ALL) address; see mesh_pdu_max_payload(). */
#define MESH_PAYLOAD_MAX (MESH_PDU_MAX - MESH_HDR_MIN - 1 /*opcode*/ - MESH_MAC_LEN)

#define MESH_PDU_DST_ALL   0u
#define MESH_PDU_DST_GROUP 1u
#define MESH_PDU_DST_ID    2u

/* One AES-128 block encryption: out = AES(key, in). On the tag this is the
 * controller's bt_encrypt_be(); the host tests plug in a software AES.
 * Returns 0 on success. */
typedef int (*mesh_aes_fn)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* Parsed view of a PDU. Pointers alias the caller's buffer. */
struct mesh_pdu_view {
	uint8_t        ttl;
	uint16_t       seq;
	const uint8_t *src;        /* 3 bytes */
	uint8_t        dst_type;
	const uint8_t *dst;        /* dst_len bytes (0 for ALL) */
	uint8_t        dst_len;
	uint8_t        opcode;
	const uint8_t *payload;
	uint8_t        plen;
	const uint8_t *mac;        /* MESH_MAC_LEN bytes */
	uint8_t        body_len;   /* bytes covered by the MAC (len - MESH_MAC_LEN) */
};

/* Address field length for a destination type (0 / 1 / 3). Unknown types map
 * to 0 so a malformed dst_type still parses as "not for me". */
uint8_t mesh_pdu_dst_len(uint8_t dst_type);

/* Payload bytes that fit once this destination's address is in the PDU:
 * 14 for ALL, 13 for GROUP, 11 for ID. */
uint8_t mesh_pdu_max_payload(uint8_t dst_type);

/* Split a received PDU into fields. False on any length or magic violation;
 * the MAC is NOT checked here (see mesh_pdu_verify). */
bool mesh_pdu_parse(const uint8_t *pdu, uint8_t len, struct mesh_pdu_view *v);

/* Check the trailing MAC of a PDU against @p key. False on a malformed PDU,
 * a wrong MAC, or an AES failure. */
bool mesh_pdu_verify(const uint8_t *pdu, uint8_t len, mesh_aes_fn aes, const uint8_t key[16]);

/* Assemble and sign a PDU into @p out (>= MESH_PDU_MAX bytes).
 * @return the PDU length, or -1 if the payload does not fit for @p dst_type
 *         or the AES callback failed. */
int mesh_pdu_build(uint8_t *out, uint8_t ttl, uint16_t seq, const uint8_t src[3],
		   uint8_t dst_type, const uint8_t *dst,
		   uint8_t opcode, const uint8_t *payload, uint8_t plen,
		   mesh_aes_fn aes, const uint8_t key[16]);

/* Rewrite the TTL nibble of a PDU in place (relay path). The MAC stays valid
 * because it never covered the TTL. */
void mesh_pdu_set_ttl(uint8_t *pdu, uint8_t ttl);

/* Full 16-byte AES-CMAC (RFC 4493). Exposed for the host test vectors; the
 * PDU functions above use the first MESH_MAC_LEN bytes of it. Subkeys are
 * cached against a copy of the key, so back-to-back calls with the same key
 * cost one AES block per 16 bytes of message and nothing more. Not
 * thread-safe: call from one thread at a time.
 * @return false if the AES callback failed; @p out is then unspecified and
 *         nothing was cached (a failed derivation must not poison the key). */
bool mesh_cmac(mesh_aes_fn aes, const uint8_t key[16],
	       const uint8_t *msg, size_t len, uint8_t out[16]);

/* ── Replay protection list ─────────────────────────────────────────────────
 *
 * Per-source sliding window, the way IPsec and BT Mesh do it: for each source
 * remember the highest sequence number accepted and a 32-bit bitmap of the
 * ones just below it. A PDU is accepted if its seq is newer than the highest,
 * or lies inside the window and has not been seen. Everything else — an exact
 * duplicate, a relay echo, a replay of a captured PDU — is dropped.
 *

 * Sequence numbers are 16-bit and compared with serial arithmetic, so the
 * counter may wrap. The arithmetic has one blind spot: once a source has
 * advanced 32768 past a captured PDU, that PDU reads as "newer" again. The
 * originator only advances by real broadcasts (mesh.c saves the counter on
 * every one, so a reboot costs a single step), which puts that at tens of
 * thousands of BCASTs after the capture — years for this fleet.
 *
 * A source whose counter went backwards — old firmware restarted it at zero
 * on every boot, and a wiped tag starts over — would be locked out forever by
 * a strict rule. The one concession: a source not heard from for
 * MESH_RPL_EXPIRE_S is forgotten, and whatever it sends next is taken at face
 * value. That re-opens replay against a tag for exactly one source, only
 * after that source has been silent a whole week, and only until it speaks
 * again; a deliberate trade for not bricking the mesh on a wiped gateway.
 * The silence is measured in the receiving tag's own uptime and restarts
 * with each of its reboots (there is no trustworthy wall clock after a power
 * loss), so the week is a floor, not an exact figure; the operator's direct
 * remedy is MESHRX FORGET over NUS, which drops the whole list. (BT Mesh's
 * answer is "re-provision the node"; this fleet has no provisioner.)
 *
 * Slots are few (RAM is counted in bytes on this tag); the least recently
 * heard source is evicted when a ninth one appears, which makes its old PDUs
 * replayable once until it is re-learned. mesh.c persists the list so a
 * reboot does not reset it — not the struct, but the compact (src, seq) form
 * from mesh_rpl_export(): layout-independent, and a loaded entry treats
 * everything at or below its seq as seen. */
#define MESH_RPL_SLOTS    8
#define MESH_RPL_WINDOW   32
#define MESH_RPL_EXPIRE_S (7u * 24u * 3600u)

struct mesh_rpl_ent {
	uint8_t  src[3];
	uint8_t  used;
	uint16_t seq;       /* highest accepted */
	uint16_t _pad;
	uint32_t win;       /* bit i set: seq - i was accepted (bit 0 = seq itself) */
	uint32_t last_s;    /* uptime (s) of the last accepted PDU from this source */
};

struct mesh_rpl {
	struct mesh_rpl_ent e[MESH_RPL_SLOTS];
};

/* True if a PDU (src, seq) would be accepted now. Read-only: safe to run on an
 * unauthenticated PDU as a cheap early reject. */
bool mesh_rpl_check(const struct mesh_rpl *r, const uint8_t src[3], uint16_t seq, uint32_t now_s);

/* Record an accepted PDU. Call only after the MAC verified — an unverified
 * entry would let anyone poison the window and get real commands dropped. */
void mesh_rpl_commit(struct mesh_rpl *r, const uint8_t src[3], uint16_t seq, uint32_t now_s);

/* Forget all sources. */
void mesh_rpl_reset(struct mesh_rpl *r);

/* Persistent form: [version:1] then, per used slot, [src:3][seq:2 LE]. The
 * windows and the silence clock are not stored — a loaded entry rejects
 * everything at or below its seq, which is the safe reading after a reboot.
 * Export returns the number of bytes written (0 if @p cap is too small for
 * the header); import replaces the whole list and returns false on a blob
 * it does not understand (the list is then empty). */
#define MESH_RPL_BLOB_VER 1
#define MESH_RPL_BLOB_MAX (1 + MESH_RPL_SLOTS * 5)
size_t mesh_rpl_export(const struct mesh_rpl *r, uint8_t *out, size_t cap);
bool   mesh_rpl_import(struct mesh_rpl *r, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_MESH_PDU_H */
