#include "mesh.h"
#include "commands.h"
#include "cmd_opcodes.h"
#include "secauth.h"
#include "ble/ble_service.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

/* ── Protocol constants ─────────────────────────────────────────────────── */
#define MESH_MAGIC       0xE5u
#define MESH_VER         0x0u
#define MESH_TTL_DEFAULT 4u          /* fleet diameter in hops */
#define MESH_COMPANY     0xFFFFu     /* "internal/test" company id */
#define MESH_MAC_LEN     4u          /* truncated AES-CMAC */
#define MESH_PDU_MAX     27u         /* fits one legacy adv (31 - 4 AD/company) */
#define MESH_HDR_MIN     (1 + 1 + 2 + 3 + 1)   /* magic,ver/ttl,seq,src,dst_type */
#define MESH_PAYLOAD_MAX (MESH_PDU_MAX - MESH_HDR_MIN - 1 /*opcode*/ - MESH_MAC_LEN)

/* ── Tunables ───────────────────────────────────────────────────────────── */
/* Air time per (re)broadcast. Must exceed the scan interval below so a 30 ms
 * scan window is guaranteed to overlap the beacon at least once. */
#define BEACON_MS        300
#define DEDUP_SLOTS      16
#define RXQ_DEPTH        6
#define TXRING_SLOTS     4
#define MESH_STACK_SIZE  2560        /* dispatch runs blocking display handlers
                                      * (UPDATE/FAST go deep into the SPI driver);
                                      * sized with margin since ASSERT/stack
                                      * sentinel are off — an overflow is silent. */

/* ── Node identity / config ─────────────────────────────────────────────── */
static uint8_t  my_id[3];
static uint16_t my_seq;
static uint8_t  my_group;           /* persisted "mesh/g" */

/* ── Dedup cache (src,seq) ring ─────────────────────────────────────────── */
struct seen_ent { uint8_t src[3]; uint16_t seq; bool used; };
static struct seen_ent seen[DEDUP_SLOTS];
static uint8_t seen_idx;

static bool seen_check_and_add(const uint8_t src[3], uint16_t seq)
{
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        if (seen[i].used && seen[i].seq == seq &&
            memcmp(seen[i].src, src, 3) == 0) {
            return true;   /* already processed */
        }
    }
    memcpy(seen[seen_idx].src, src, 3);
    seen[seen_idx].seq = seq;
    seen[seen_idx].used = true;
    seen_idx = (seen_idx + 1) % DEDUP_SLOTS;
    return false;
}

/* ── Work item / job queue ──────────────────────────────────────────────── */
#define JOB_RX   0
#define JOB_ORIG 1
struct mesh_job {
    uint8_t kind;
    uint8_t len;
    uint8_t data[28];   /* RX: raw PDU; ORIG: [dst_type][dst..][opcode][payload] */
};
K_MSGQ_DEFINE(mesh_q, sizeof(struct mesh_job), RXQ_DEPTH, 4);

static K_THREAD_STACK_DEFINE(mesh_stack, MESH_STACK_SIZE);
static struct k_thread mesh_thread_data;
static k_tid_t mesh_tid;

/* ── TX beacon ring (drained on the system workqueue) ───────────────────── */
struct tx_ent { uint8_t len; uint8_t bytes[MESH_PDU_MAX]; };
static struct tx_ent tx_ring[TXRING_SLOTS];
static uint8_t tx_head, tx_count;
static struct k_spinlock tx_lock;
static atomic_t beaconing = ATOMIC_INIT(0);
static struct k_work_delayable tx_work;

static void tx_enqueue(const uint8_t *pdu, uint8_t len)
{
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    if (tx_count < TXRING_SLOTS) {
        uint8_t slot = (tx_head + tx_count) % TXRING_SLOTS;
        memcpy(tx_ring[slot].bytes, pdu, len);
        tx_ring[slot].len = len;
        tx_count++;
    }
    k_spin_unlock(&tx_lock, key);

    /* Kick the pump only if idle; if a beacon is in flight its timer will pull
     * the next entry, so we must not reschedule (that would cut it short). */
    if (!atomic_get(&beaconing)) {
        k_work_schedule(&tx_work, K_NO_WAIT);
    }
}

static void tx_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    struct tx_ent ent;
    bool have = false;
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    if (tx_count > 0) {
        ent = tx_ring[tx_head];
        tx_head = (tx_head + 1) % TXRING_SLOTS;
        tx_count--;
        have = true;
    }
    k_spin_unlock(&tx_lock, key);

    if (!have) {
        /* Queue drained — hand the adv instance back to normal advertising. */
        if (atomic_get(&beaconing)) {
            ble_service_beacon_end();
            atomic_set(&beaconing, 0);
        }
        return;
    }

    /* Replace the beacon payload in place (beacon_set stops the old one first),
     * so back-to-back relays don't churn back to connectable adv between them. */
    uint8_t mfg[2 + MESH_PDU_MAX];
    mfg[0] = (uint8_t)(MESH_COMPANY & 0xFF);
    mfg[1] = (uint8_t)(MESH_COMPANY >> 8);
    memcpy(&mfg[2], ent.bytes, ent.len);

    if (ble_service_beacon_set(mfg, (uint8_t)(2 + ent.len)) == 0) {
        atomic_set(&beaconing, 1);
    }
    k_work_schedule(&tx_work, K_MSEC(BEACON_MS));
}

/* ── AES-CMAC (RFC 4493) over the controller's AES block (bt_encrypt_be) ─── */
static void cmac_shift_left(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = (in[i] & 0x80) ? 1 : 0;
    }
}

static void cmac_subkeys(const uint8_t *k, uint8_t k1[16], uint8_t k2[16])
{
    uint8_t zero[16] = {0}, l[16];
    bt_encrypt_be(k, zero, l);
    cmac_shift_left(l, k1);
    if (l[0] & 0x80) {
        k1[15] ^= 0x87;
    }
    cmac_shift_left(k1, k2);
    if (k1[0] & 0x80) {
        k2[15] ^= 0x87;
    }
}

/* Compute the 4-byte truncated CMAC of msg[0..len) into out[0..4). */
static void mesh_mac(const uint8_t *msg, size_t len, uint8_t out[MESH_MAC_LEN])
{
    const uint8_t *k = secauth_key();
    uint8_t k1[16], k2[16], x[16] = {0}, y[16], last[16], full[16];
    cmac_subkeys(k, k1, k2);

    size_t blocks = (len + 15) / 16;
    bool complete;
    if (blocks == 0) {
        blocks = 1;
        complete = false;
    } else {
        complete = (len % 16) == 0;
    }

    size_t last_off = 16 * (blocks - 1);
    if (complete) {
        for (int i = 0; i < 16; i++) {
            last[i] = msg[last_off + i] ^ k1[i];
        }
    } else {
        size_t rem = len % 16;
        for (size_t i = 0; i < 16; i++) {
            uint8_t b = (i < rem) ? msg[last_off + i] : (i == rem ? 0x80 : 0x00);
            last[i] = b ^ k2[i];
        }
    }

    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++) {
            y[i] = x[i] ^ msg[16 * b + i];
        }
        bt_encrypt_be(k, y, x);
    }
    for (int i = 0; i < 16; i++) {
        y[i] = x[i] ^ last[i];
    }
    bt_encrypt_be(k, y, full);
    memcpy(out, full, MESH_MAC_LEN);
}

/* MAC input is the PDU minus the trailing MAC, with the TTL nibble masked out
 * (relays mutate TTL, so it must not be authenticated). */
static void mesh_mac_over_pdu(const uint8_t *pdu, uint8_t body_len,
                             uint8_t out[MESH_MAC_LEN])
{
    uint8_t tmp[MESH_PDU_MAX];
    memcpy(tmp, pdu, body_len);
    tmp[1] &= 0xF0;   /* clear ttl nibble */
    mesh_mac(tmp, body_len, out);
}

/* ── PDU layout helpers ─────────────────────────────────────────────────── */
static uint8_t dst_len(uint8_t dst_type)
{
    switch (dst_type) {
    case MESH_DST_GROUP: return 1;
    case MESH_DST_ID:    return 3;
    default:             return 0;
    }
}

/* ── Core: validate, dedup, relay, and locally apply one PDU ─────────────── */
static void mesh_handle_pdu(const uint8_t *pdu, uint8_t len, bool trusted)
{
    if (len < MESH_HDR_MIN + 1 + MESH_MAC_LEN || len > MESH_PDU_MAX) {
        return;
    }
    if (pdu[0] != MESH_MAGIC) {
        return;
    }

    uint8_t ttl     = pdu[1] & 0x0F;
    uint16_t seq    = (uint16_t)(pdu[2] | (pdu[3] << 8));
    const uint8_t *src = &pdu[4];
    uint8_t dst_type = pdu[7];
    uint8_t dl = dst_len(dst_type);

    uint8_t opcode_off = 8 + dl;
    if (opcode_off + 1 + MESH_MAC_LEN > len) {
        return;   /* truncated / malformed */
    }
    const uint8_t *dst = &pdu[8];
    uint8_t opcode  = pdu[opcode_off];
    const uint8_t *payload = &pdu[opcode_off + 1];
    uint8_t plen = len - (opcode_off + 1) - MESH_MAC_LEN;
    const uint8_t *mac = &pdu[len - MESH_MAC_LEN];

    /* Ignore our own floods heard back over the air (originate path is trusted
     * and must still run locally + relay). */
    if (!trusted && memcmp(src, my_id, 3) == 0) {
        return;
    }

    if (!trusted) {
        uint8_t want[MESH_MAC_LEN];
        mesh_mac_over_pdu(pdu, (uint8_t)(len - MESH_MAC_LEN), want);
        if (memcmp(want, mac, MESH_MAC_LEN) != 0) {
            return;   /* not from a fleet member */
        }
    }

    if (seen_check_and_add(src, seq)) {
        return;   /* duplicate / loop / replay */
    }

    /* Relay first (with TTL-1) so the flood keeps spreading even if the local
     * handler below blocks on a slow e-ink refresh. */
    if (ttl > 0) {
        uint8_t relay[MESH_PDU_MAX];
        memcpy(relay, pdu, len);
        relay[1] = (uint8_t)((MESH_VER << 4) | (ttl - 1));
        tx_enqueue(relay, len);
    }

    /* Addressed to us? */
    bool for_me = (dst_type == MESH_DST_ALL) ||
                  (dst_type == MESH_DST_GROUP && dl == 1 && dst[0] == my_group) ||
                  (dst_type == MESH_DST_ID && dl == 3 && memcmp(dst, my_id, 3) == 0);
    if (!for_me) {
        return;
    }

    char args[MESH_PAYLOAD_MAX + 1];
    uint8_t n = plen > MESH_PAYLOAD_MAX ? MESH_PAYLOAD_MAX : plen;
    memcpy(args, payload, n);
    args[n] = '\0';
    cmd_dispatch_opcode(opcode, args, CMD_ORIGIN_MESH);
}

/* Build + sign a fresh PDU from an originate descriptor, then handle it. */
static void mesh_do_originate(const uint8_t *d, uint8_t dlen)
{
    if (dlen < 1) {
        return;
    }
    uint8_t dst_type = d[0];
    uint8_t dl = dst_len(dst_type);
    if (1 + dl + 1 > dlen) {
        return;
    }
    uint8_t opcode = d[1 + dl];
    const uint8_t *payload = &d[1 + dl + 1];
    uint8_t plen = dlen - (1 + dl + 1);
    if (plen > MESH_PAYLOAD_MAX) {
        plen = MESH_PAYLOAD_MAX;
    }

    uint8_t pdu[MESH_PDU_MAX];
    uint8_t p = 0;
    pdu[p++] = MESH_MAGIC;
    pdu[p++] = (uint8_t)((MESH_VER << 4) | (MESH_TTL_DEFAULT & 0x0F));
    pdu[p++] = (uint8_t)(my_seq & 0xFF);
    pdu[p++] = (uint8_t)(my_seq >> 8);
    my_seq++;
    memcpy(&pdu[p], my_id, 3); p += 3;
    pdu[p++] = dst_type;
    memcpy(&pdu[p], &d[1], dl); p += dl;
    pdu[p++] = opcode;
    memcpy(&pdu[p], payload, plen); p += plen;

    mesh_mac_over_pdu(pdu, p, &pdu[p]); p += MESH_MAC_LEN;

    mesh_handle_pdu(pdu, p, true);
}

/* ── Dispatch thread ────────────────────────────────────────────────────── */
static void mesh_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct mesh_job job;
    while (1) {
        k_msgq_get(&mesh_q, &job, K_FOREVER);
        if (job.kind == JOB_ORIG) {
            mesh_do_originate(job.data, job.len);
        } else {
            mesh_handle_pdu(job.data, job.len, false);
        }
    }
}

bool mesh_is_dispatch_thread(void)
{
    return mesh_tid != NULL && k_current_get() == mesh_tid;
}

/* ── Scanner: copy candidate PDUs to the job queue, do nothing heavy here ── */
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *buf)
{
    ARG_UNUSED(addr); ARG_UNUSED(rssi); ARG_UNUSED(adv_type);

    const uint8_t *d = buf->data;
    uint16_t n = buf->len, i = 0;
    while (i + 1 < n) {
        uint8_t flen = d[i];
        if (flen == 0 || i + 1 + flen > n) {
            break;
        }
        uint8_t ftype = d[i + 1];
        if (ftype == BT_DATA_MANUFACTURER_DATA && flen >= 1 + 2) {
            const uint8_t *md = &d[i + 2];
            uint8_t mdlen = flen - 1;   /* manufacturer payload (incl company) */
            if (mdlen >= 2 + MESH_HDR_MIN + 1 + MESH_MAC_LEN &&
                md[0] == (uint8_t)(MESH_COMPANY & 0xFF) &&
                md[1] == (uint8_t)(MESH_COMPANY >> 8) &&
                md[2] == MESH_MAGIC) {
                uint8_t plen = mdlen - 2;
                if (plen <= MESH_PDU_MAX) {
                    struct mesh_job job = { .kind = JOB_RX, .len = plen };
                    memcpy(job.data, &md[2], plen);
                    (void)k_msgq_put(&mesh_q, &job, K_NO_WAIT);
                }
            }
        }
        i += 1 + flen;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */
int mesh_originate(enum mesh_dst dst, const uint8_t *dst_val,
                   uint8_t opcode, const uint8_t *payload, uint8_t plen)
{
    struct mesh_job job = { .kind = JOB_ORIG };
    uint8_t dl = dst_len((uint8_t)dst);
    if (plen > MESH_PAYLOAD_MAX) {
        plen = MESH_PAYLOAD_MAX;
    }
    uint8_t p = 0;
    job.data[p++] = (uint8_t)dst;
    if (dl && dst_val) {
        memcpy(&job.data[p], dst_val, dl);
    }
    p += dl;
    job.data[p++] = opcode;
    if (plen && payload) {
        memcpy(&job.data[p], payload, plen);
    }
    p += plen;
    job.len = p;

    return k_msgq_put(&mesh_q, &job, K_NO_WAIT);
}

uint8_t mesh_get_group(void) { return my_group; }

int mesh_set_group(uint8_t gid)
{
    my_group = gid;
    return settings_save_one("mesh/g", &gid, 1);
}

static int mesh_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    if (settings_name_steq(name, "g", NULL) && len == 1) {
        if (read_cb(cb_arg, &my_group, 1) >= 0) {
            return 0;
        }
    }
    return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(meshcfg, "mesh", NULL, mesh_settings_set, NULL, NULL);

void mesh_init(void)
{
    ble_service_get_node_id(my_id);

    k_work_init_delayable(&tx_work, tx_work_fn);

    mesh_tid = k_thread_create(&mesh_thread_data, mesh_stack,
                               K_THREAD_STACK_SIZEOF(mesh_stack),
                               mesh_thread_fn, NULL, NULL, NULL,
                               K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    k_thread_name_set(mesh_tid, "mesh");

    /* Low-duty scan (~15%): 30 ms window every 200 ms. Gentle on the connection
     * (vstream throughput) and battery; BEACON_MS (300) > the 200 ms interval, so
     * a window is guaranteed to catch each flooded beacon. */
    struct bt_le_scan_param sp = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x0140,   /* 200 ms */
        .window   = 0x0030,   /* 30 ms */
    };
    int err = bt_le_scan_start(&sp, scan_cb);
    if (err) {
        /* Non-fatal: TX/originate still works; this node just won't relay/receive. */
    }
}
