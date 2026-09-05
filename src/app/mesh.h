#ifndef APP_MESH_H
#define APP_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include "mesh_pdu.h"

/*
 * Minimal connectionless flood-mesh between tags (managed flooding, the same
 * principle as BT Mesh but stripped to fit the flash/RAM budget — no
 * provisioning, no models, legacy advertising only).
 *
 * A phone connects to ONE tag over NUS and issues a broadcast (BCAST); that tag
 * originates a signed PDU into the air as a non-connectable beacon. Every tag
 * scans (observer), and on a fresh, authentic PDU it (a) re-beacons it with
 * TTL-1 so the command floods across the fleet, and (b) executes it locally if
 * it is one of the addressees (ALL / its group / its id).
 *
 * Security: each PDU carries a 4-byte truncated AES-CMAC over the fleet secauth
 * key (so random devices can't inject), plus a per-source sequence number
 * checked against a persisted sliding window, so duplicates, relay echoes and
 * replays of captured PDUs are dropped. The wire format, the MAC and the
 * window live in mesh_pdu.h; this file is the radio, the threads and the
 * persistence around them.
 */

/* Destination types, by their wire values. */
enum mesh_dst {
    MESH_DST_ALL   = MESH_PDU_DST_ALL,     /* every tag */
    MESH_DST_GROUP = MESH_PDU_DST_GROUP,   /* tags whose group id matches dst[0] */
    MESH_DST_ID    = MESH_PDU_DST_ID,      /* the single tag whose node id matches dst[0..2] */
};

/* Resolve node id, start scanning, spawn the dispatch thread. Call once after
 * ble_service_init() (settings loaded, identity ready) and secauth_init(). */
void mesh_init(void);

/* True iff the caller runs on the mesh dispatch thread. ble_printf() uses it to
 * drop replies from mesh-originated commands; the command core uses it to gate
 * which opcodes may run over the air. Safe before mesh_init() (returns false). */
bool mesh_is_dispatch_thread(void);

/* Originate a broadcast command. Builds + signs a PDU (src = this node, fresh
 * seq, default TTL) on the mesh thread, floods it, and applies it locally if
 * addressed. Only enqueues, so it is safe to call from the BLE/command context.
 *
 * @param dst      ALL / GROUP / ID.
 * @param dst_val  GROUP: 1 group-id byte; ID: 3 node-id bytes; ALL: ignored.
 * @param opcode   command opcode (cmd_opcodes.h).
 * @param payload  argument bytes (may be NULL).
 * @param plen     payload length. The budget depends on @p dst, because the
 *                 address field shares the 27-byte PDU: 14 bytes for ALL,
 *                 13 for GROUP, 11 for ID.
 * @return 0 if queued, -EMSGSIZE if the payload exceeds that budget, or
 *         another negative errno if the queue is full.
 */
int mesh_originate(enum mesh_dst dst, const uint8_t *dst_val,
                   uint8_t opcode, const uint8_t *payload, uint8_t plen);

/* This node's group id (default 0). Persisted in NVS under "mesh/g". */
uint8_t mesh_get_group(void);
int     mesh_set_group(uint8_t gid);

/* Mesh receive (observer scan) on/off (default on). Persisted under "mesh/rx".
 * The scan is the tag's dominant idle consumer, so a node that is only driven
 * over a direct NUS connection should turn it off. With RX off the node still
 * originates broadcasts but neither executes nor relays flooded commands —
 * turning it back on over the mesh is therefore impossible, only over NUS. */
bool mesh_get_rx(void);
int  mesh_set_rx(bool enable);

/* Drop the replay-protection list: every source starts over at whatever it
 * sends next. The operator's remedy for a gateway whose counter went
 * backwards (wiped settings) — see mesh_pdu.h. Runs on the mesh thread;
 * returns 0 if queued, else the k_msgq error. Must only be reachable over the
 * connected link, never from a flood. */
int mesh_forget_peers(void);

#endif /* APP_MESH_H */
