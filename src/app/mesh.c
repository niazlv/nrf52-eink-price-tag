#include "mesh.h"
#include "mesh_pdu.h"
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

/* The wire format, the MAC and the replay window live in mesh_pdu.c (plain
 * C99, host-tested). This file owns everything that needs the OS or the
 * radio: threads, queues, the beacon pump, settings, and the identity. */

/* ── Protocol constants ─────────────────────────────────────────────────── */
#define MESH_TTL_DEFAULT 4u          /* fleet diameter in hops */
#define MESH_COMPANY     0xFFFFu     /* "internal/test" company id */

/* ── Tunables ───────────────────────────────────────────────────────────── */
/* Observer scan duty. RX is ~6-10 mA on the nRF52832, so window/interval is
 * the single largest term in the tag's whole power budget: 30/200 (15%) cost
 * ~1-1.5 mA average, 30/1000 (3%) costs ~0.2-0.3 mA. Keep POWER_MESH_SCAN_UA
 * in display_manager.c in step with this ratio. */
#define SCAN_WINDOW_MS   30
#define SCAN_INTERVAL_MS 1000
/* Air time per (re)broadcast. Must exceed SCAN_INTERVAL_MS so a full scan
 * window is guaranteed to land inside the beacon at least once per hop. */
#define BEACON_MS        1200
#define RXQ_DEPTH        6
#define TXRING_SLOTS     4
#define MESH_STACK_SIZE  2560        /* dispatch runs blocking display handlers
                                      * (UPDATE/FAST go deep into the SPI driver)
                                      * and the settings writes below; sized
                                      * with margin — the MPU guard catches an
                                      * overflow, but as a reboot. */

/* ── Node identity / config ─────────────────────────────────────────────── */
static uint8_t  my_id[3];
static uint8_t  my_group;           /* persisted "mesh/g" */
static uint8_t  rx_enabled = 1;     /* persisted "mesh/rx"; scan on/off */
static bool     scanning;           /* live state of bt_le_scan */

/* ── Originator sequence number ─────────────────────────────────────────────
 *
 * Peers drop anything at or below the last seq they accepted from us
 * (mesh_pdu.h, replay list), so the counter must never go backwards across a
 * reboot. It used to restart at zero on every boot: a rebooted gateway's
 * first broadcasts were silently dropped as duplicates, and any captured PDU
 * could be replayed once the 16-entry dedup ring had forgotten it.
 *
 * Now the last value used is saved to settings ("mesh/seq") on every
 * originate, and a boot resumes one past it. A BCAST is a user action, a few
 * a day at most, so the flash cost is nothing; and advancing only by real
 * broadcasts is what keeps the 16-bit serial arithmetic far from its 32768
 * blind spot (mesh_pdu.h). A tag with no saved counter — first boot on this
 * firmware, or wiped settings — starts at SEQ_FIRST, above anything the old
 * firmware ever reached within one boot, so an upgraded gateway is newer
 * than its own past to every peer.
 *
 * Rolling out: a tag still on the old firmware restarts at zero on every
 * boot, and a peer on this firmware then ignores it for a week (mesh_pdu.h,
 * MESH_RPL_EXPIRE_S) or until MESHRX FORGET. Update the gateway first. */
#define SEQ_FIRST 1024u
static uint16_t my_seq = SEQ_FIRST;    /* next value to use; settings load may raise it */

static uint16_t next_seq(void)
{
	uint16_t s = my_seq++;

	(void)settings_save_one("mesh/seq", &s, sizeof(s));
	return s;
}

/* ── Replay list ────────────────────────────────────────────────────────────
 *
 * Touched only on the mesh thread (commit, save, forget), so it needs no
 * lock. Persisted under "mesh/rpl" in the compact form from mesh_pdu.h, so a
 * reboot — every OTA is one — does not hand an attacker a fresh pass through
 * their captured PDUs.
 *
 * When to write: a PDU accepted RPL_SAVE_MIN_S or more after the last write
 * is saved right away, before its handler runs — that handler may hold this
 * thread for minutes (a flooded NUKE), and the unsaved window must not grow
 * with it. PDUs accepted inside that quiet period are covered by one
 * deferred write RPL_SAVE_MIN_S after the first of them (k_work_schedule on
 * a pending item is a no-op, so a burst costs one write, not one per PDU).
 * The deferred write still runs on this thread (JOB_SAVE) rather than on the
 * system workqueue: settings + NVS + the flash driver want more stack than
 * the 1280-byte workqueue has to spare. */
static struct mesh_rpl rpl;
static struct k_work_delayable rpl_save_work;
static uint32_t rpl_saved_s;        /* uptime of the last write */
static bool     rpl_dirty;          /* commits since that write */
#define RPL_SAVE_MIN_S 10
#define RPL_SAVE_DELAY K_SECONDS(RPL_SAVE_MIN_S)

/* ── Work item / job queue ──────────────────────────────────────────────── */
#define JOB_RX     0
#define JOB_ORIG   1
#define JOB_SAVE   2
#define JOB_FORGET 3
struct mesh_job {
    uint8_t kind;
    uint8_t len;
    uint8_t data[28];   /* RX: raw PDU; ORIG: [dst_type][dst..][opcode][payload] */
};
K_MSGQ_DEFINE(mesh_q, sizeof(struct mesh_job), RXQ_DEPTH, 4);

static K_THREAD_STACK_DEFINE(mesh_stack, MESH_STACK_SIZE);
static struct k_thread mesh_thread_data;
static k_tid_t mesh_tid;

static void rpl_save_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    struct mesh_job job = { .kind = JOB_SAVE };

    if (k_msgq_put(&mesh_q, &job, K_NO_WAIT) != 0) {
        /* The queue is full of RX jobs. Losing this flush would leave the
         * burst it covers unsaved until the next PDU happens to arrive, so
         * come back in a second instead. */
        (void)k_work_reschedule(&rpl_save_work, K_SECONDS(1));
    }
}

/* Mesh thread only. */
static void rpl_save_now(uint32_t now_s)
{
    uint8_t blob[MESH_RPL_BLOB_MAX];
    size_t n = mesh_rpl_export(&rpl, blob, sizeof(blob));

    if (n && settings_save_one("mesh/rpl", blob, n) == 0) {
        rpl_saved_s = now_s;
        rpl_dirty = false;
    }
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(k_uptime_get() / 1000);
}

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

/* ── Core: validate, dedup, relay, and locally apply one PDU ─────────────── */
static void mesh_handle_pdu(const uint8_t *pdu, uint8_t len, bool trusted)
{
    struct mesh_pdu_view v;

    if (!mesh_pdu_parse(pdu, len, &v)) {
        return;   /* malformed or truncated */
    }

    /* Ignore our own floods heard back over the air (originate path is trusted
     * and must still run locally + relay). */
    if (!trusted && memcmp(v.src, my_id, 3) == 0) {
        return;
    }

    uint32_t now_s = uptime_s();

    if (!trusted) {
        /* Cheap reject first: a PDU already handled — a relay echo, a
         * duplicate, a replay — costs nothing to drop, and in a fleet most
         * receptions are exactly that. The check is read-only, so running it
         * before the MAC is safe. */
        if (!mesh_rpl_check(&rpl, v.src, v.seq, now_s)) {
            return;
        }

        /* Snapshot the key: the MAC takes several controller round-trips,
         * and SETKEY can rewrite the live key from another thread meanwhile. */
        uint8_t key[16];

        memcpy(key, secauth_key(), sizeof(key));
        if (!mesh_pdu_verify(pdu, len, bt_encrypt_be, key)) {
            return;   /* not from a fleet member — and NOT remembered, so a
                       * forgery cannot poison the window */
        }

        mesh_rpl_commit(&rpl, v.src, v.seq, now_s);
        rpl_dirty = true;
        if (rpl_saved_s == 0 || (now_s - rpl_saved_s) >= RPL_SAVE_MIN_S) {
            rpl_save_now(now_s);          /* before the handler below can block */
        } else {
            (void)k_work_schedule(&rpl_save_work, RPL_SAVE_DELAY);
        }
    }

    /* Relay first (with TTL-1) so the flood keeps spreading even if the local
     * handler below blocks on a slow e-ink refresh. */
    if (v.ttl > 0) {
        uint8_t relay[MESH_PDU_MAX];

        memcpy(relay, pdu, len);
        mesh_pdu_set_ttl(relay, (uint8_t)(v.ttl - 1));
        tx_enqueue(relay, len);
    }

    /* Addressed to us? */
    bool for_me = (v.dst_type == MESH_DST_ALL) ||
                  (v.dst_type == MESH_DST_GROUP && v.dst_len == 1 && v.dst[0] == my_group) ||
                  (v.dst_type == MESH_DST_ID && v.dst_len == 3 && memcmp(v.dst, my_id, 3) == 0);
    if (!for_me) {
        return;
    }

    char args[MESH_PAYLOAD_MAX + 1];
    uint8_t n = v.plen > MESH_PAYLOAD_MAX ? MESH_PAYLOAD_MAX : v.plen;

    memcpy(args, v.payload, n);
    args[n] = '\0';
    cmd_dispatch_opcode(v.opcode, args, CMD_ORIGIN_MESH);
}

/* Build + sign a fresh PDU from an originate descriptor, then handle it. */
static void mesh_do_originate(const uint8_t *d, uint8_t dlen)
{
    if (dlen < 1) {
        return;
    }
    uint8_t dst_type = d[0];
    uint8_t dl = mesh_pdu_dst_len(dst_type);

    if (1 + dl + 1 > dlen) {
        return;
    }
    uint8_t opcode = d[1 + dl];
    const uint8_t *payload = &d[1 + dl + 1];
    uint8_t plen = (uint8_t)(dlen - (1 + dl + 1));
    uint8_t key[16];
    uint8_t pdu[MESH_PDU_MAX];

    memcpy(key, secauth_key(), sizeof(key));
    int len = mesh_pdu_build(pdu, MESH_TTL_DEFAULT, next_seq(), my_id,
                             dst_type, &d[1], opcode, payload, plen,
                             bt_encrypt_be, key);
    if (len < 0) {
        return;   /* mesh_originate() already refused oversize payloads */
    }
    mesh_handle_pdu(pdu, (uint8_t)len, true);
}

/* ── Dispatch thread ────────────────────────────────────────────────────── */
static void mesh_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct mesh_job job;
    while (1) {
        k_msgq_get(&mesh_q, &job, K_FOREVER);
        switch (job.kind) {
        case JOB_ORIG:
            mesh_do_originate(job.data, job.len);
            break;
        case JOB_SAVE:
            if (rpl_dirty) {
                rpl_save_now(uptime_s());
            }
            break;
        case JOB_FORGET:
            mesh_rpl_reset(&rpl);
            rpl_dirty = true;
            rpl_save_now(uptime_s());
            break;
        default:
            mesh_handle_pdu(job.data, job.len, false);
            break;
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
    uint8_t dl = mesh_pdu_dst_len((uint8_t)dst);

    /* Reject rather than truncate: a silently shortened command line reaches
     * every node as a different (usually unparseable) command, and the caller
     * would still be told the flood was queued. */
    if (plen > mesh_pdu_max_payload((uint8_t)dst)) {
        return -EMSGSIZE;
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

/* Start the observer role. Failure is non-fatal everywhere this is called:
 * TX/originate still works, the node just won't relay/receive. */
static void mesh_scan_start(void)
{
    if (scanning) {
        return;
    }
    struct bt_le_scan_param sp = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = SCAN_INTERVAL_MS * 8 / 5,   /* ms → 0.625 ms units */
        .window   = SCAN_WINDOW_MS * 8 / 5,
    };
    if (bt_le_scan_start(&sp, scan_cb) == 0) {
        scanning = true;
    }
}

bool mesh_get_rx(void) { return rx_enabled != 0; }

int mesh_forget_peers(void)
{
    struct mesh_job job = { .kind = JOB_FORGET };

    return k_msgq_put(&mesh_q, &job, K_NO_WAIT);
}

int mesh_set_rx(bool enable)
{
    rx_enabled = enable ? 1 : 0;
    if (enable) {
        mesh_scan_start();
    } else if (scanning) {
        (void)bt_le_scan_stop();
        scanning = false;
    }
    return settings_save_one("mesh/rx", &rx_enabled, 1);
}

static int mesh_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    if (settings_name_steq(name, "g", NULL) && len == 1) {
        if (read_cb(cb_arg, &my_group, 1) >= 0) {
            return 0;
        }
    }
    if (settings_name_steq(name, "rx", NULL) && len == 1) {
        if (read_cb(cb_arg, &rx_enabled, 1) >= 0) {
            return 0;
        }
    }
    if (settings_name_steq(name, "seq", NULL) && len == sizeof(uint16_t)) {
        uint16_t last;

        if (read_cb(cb_arg, &last, sizeof(last)) >= 0) {
            my_seq = (uint16_t)(last + 1);   /* resume one past the last one sent */
            return 0;
        }
    }
    if (settings_name_steq(name, "rpl", NULL) && len <= MESH_RPL_BLOB_MAX) {
        uint8_t blob[MESH_RPL_BLOB_MAX];

        /* A blob this firmware does not understand leaves the list empty:
         * one replay pass after such an update is the price of a format
         * change, and mesh_rpl_import() is written so the format need not
         * change with the struct. */
        if (read_cb(cb_arg, blob, len) >= 0 && mesh_rpl_import(&rpl, blob, len)) {
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
    k_work_init_delayable(&rpl_save_work, rpl_save_fn);

    mesh_tid = k_thread_create(&mesh_thread_data, mesh_stack,
                               K_THREAD_STACK_SIZEOF(mesh_stack),
                               mesh_thread_fn, NULL, NULL, NULL,
                               K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    k_thread_name_set(mesh_tid, "mesh");

    /* Observer role is optional (persisted "mesh/rx", default on): a tag that
     * only ever needs direct NUS control can drop the scan entirely — it is
     * the dominant idle consumer even at low duty. */
    if (rx_enabled) {
        mesh_scan_start();
    }
}
