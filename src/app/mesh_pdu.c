#include "mesh_pdu.h"

#include <string.h>

/* ── AES-CMAC (RFC 4493) ────────────────────────────────────────────────── */

static void shift_left(const uint8_t in[16], uint8_t out[16])
{
	uint8_t carry = 0;

	for (int i = 15; i >= 0; i--) {
		out[i] = (uint8_t)((in[i] << 1) | carry);
		carry = (in[i] & 0x80) ? 1 : 0;
	}
}

/* Subkeys depend on the key alone. Deriving them costs one AES block — on the
 * tag that is a blocking HCI round-trip to the controller — so they are
 * cached against a copy of the key. Comparing the key IS the invalidation:
 * a SETKEY needs no hook here. */
static uint8_t cached_key[16], cached_k1[16], cached_k2[16];
static bool cache_valid;

static void subkeys(mesh_aes_fn aes, const uint8_t *k, uint8_t k1[16], uint8_t k2[16])
{
	if (cache_valid && memcmp(cached_key, k, 16) == 0) {
		memcpy(k1, cached_k1, 16);
		memcpy(k2, cached_k2, 16);
		return;
	}

	uint8_t zero[16] = {0}, l[16];

	aes(k, zero, l);
	shift_left(l, k1);
	if (l[0] & 0x80) {
		k1[15] ^= 0x87;
	}
	shift_left(k1, k2);
	if (k1[0] & 0x80) {
		k2[15] ^= 0x87;
	}

	memcpy(cached_key, k, 16);
	memcpy(cached_k1, k1, 16);
	memcpy(cached_k2, k2, 16);
	cache_valid = true;
}

void mesh_cmac(mesh_aes_fn aes, const uint8_t key[16],
	       const uint8_t *msg, size_t len, uint8_t out[16])
{
	uint8_t k1[16], k2[16], x[16] = {0}, y[16], last[16];

	subkeys(aes, key, k1, k2);

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
		aes(key, y, x);
	}
	for (int i = 0; i < 16; i++) {
		y[i] = x[i] ^ last[i];
	}
	aes(key, y, out);
}

/* MAC input: the PDU body with the TTL nibble cleared. */
static void mac_over_body(const uint8_t *pdu, uint8_t body_len,
			  mesh_aes_fn aes, const uint8_t key[16], uint8_t out[MESH_MAC_LEN])
{
	uint8_t tmp[MESH_PDU_MAX], full[16];

	memcpy(tmp, pdu, body_len);
	tmp[1] &= 0xF0;
	mesh_cmac(aes, key, tmp, body_len, full);
	memcpy(out, full, MESH_MAC_LEN);
}

/* ── PDU layout ─────────────────────────────────────────────────────────── */

uint8_t mesh_pdu_dst_len(uint8_t dst_type)
{
	switch (dst_type) {
	case MESH_PDU_DST_GROUP: return 1;
	case MESH_PDU_DST_ID:    return 3;
	default:                 return 0;
	}
}

uint8_t mesh_pdu_max_payload(uint8_t dst_type)
{
	return (uint8_t)(MESH_PDU_MAX - MESH_HDR_MIN - mesh_pdu_dst_len(dst_type)
			 - 1 /*opcode*/ - MESH_MAC_LEN);
}

bool mesh_pdu_parse(const uint8_t *pdu, uint8_t len, struct mesh_pdu_view *v)
{
	if (len < MESH_HDR_MIN + 1 + MESH_MAC_LEN || len > MESH_PDU_MAX) {
		return false;
	}
	if (pdu[0] != MESH_MAGIC) {
		return false;
	}

	uint8_t dl = mesh_pdu_dst_len(pdu[7]);
	uint8_t opcode_off = (uint8_t)(MESH_HDR_MIN + dl);

	if (opcode_off + 1 + MESH_MAC_LEN > len) {
		return false;   /* truncated: the address ran into the MAC */
	}

	v->ttl      = pdu[1] & 0x0F;
	v->seq      = (uint16_t)(pdu[2] | (pdu[3] << 8));
	v->src      = &pdu[4];
	v->dst_type = pdu[7];
	v->dst      = &pdu[8];
	v->dst_len  = dl;
	v->opcode   = pdu[opcode_off];
	v->payload  = &pdu[opcode_off + 1];
	v->plen     = (uint8_t)(len - (opcode_off + 1) - MESH_MAC_LEN);
	v->mac      = &pdu[len - MESH_MAC_LEN];
	v->body_len = (uint8_t)(len - MESH_MAC_LEN);
	return true;
}

bool mesh_pdu_verify(const uint8_t *pdu, uint8_t len, mesh_aes_fn aes, const uint8_t key[16])
{
	struct mesh_pdu_view v;
	uint8_t want[MESH_MAC_LEN];

	if (!mesh_pdu_parse(pdu, len, &v)) {
		return false;
	}
	mac_over_body(pdu, v.body_len, aes, key, want);
	return memcmp(want, v.mac, MESH_MAC_LEN) == 0;
}

int mesh_pdu_build(uint8_t *out, uint8_t ttl, uint16_t seq, const uint8_t src[3],
		   uint8_t dst_type, const uint8_t *dst,
		   uint8_t opcode, const uint8_t *payload, uint8_t plen,
		   mesh_aes_fn aes, const uint8_t key[16])
{
	uint8_t dl = mesh_pdu_dst_len(dst_type);

	if (plen > mesh_pdu_max_payload(dst_type)) {
		return -1;
	}

	uint8_t p = 0;

	out[p++] = MESH_MAGIC;
	out[p++] = (uint8_t)((MESH_VER << 4) | (ttl & 0x0F));
	out[p++] = (uint8_t)(seq & 0xFF);
	out[p++] = (uint8_t)(seq >> 8);
	memcpy(&out[p], src, 3); p += 3;
	out[p++] = dst_type;
	if (dl) {
		memcpy(&out[p], dst, dl);
		p += dl;
	}
	out[p++] = opcode;
	if (plen) {
		memcpy(&out[p], payload, plen);
		p += plen;
	}
	mac_over_body(out, p, aes, key, &out[p]);
	p += MESH_MAC_LEN;
	return p;
}

void mesh_pdu_set_ttl(uint8_t *pdu, uint8_t ttl)
{
	pdu[1] = (uint8_t)((pdu[1] & 0xF0) | (ttl & 0x0F));
}

/* ── Replay list ────────────────────────────────────────────────────────── */

static struct mesh_rpl_ent *rpl_find(struct mesh_rpl *r, const uint8_t src[3])
{
	for (int i = 0; i < MESH_RPL_SLOTS; i++) {
		if (r->e[i].used && memcmp(r->e[i].src, src, 3) == 0) {
			return &r->e[i];
		}
	}
	return NULL;
}

/* A source quiet for a week is forgotten: whatever it sends next starts a
 * fresh window — see the header. */
static bool expired(const struct mesh_rpl_ent *e, uint32_t now_s)
{
	return (now_s - e->last_s) >= MESH_RPL_EXPIRE_S;
}

bool mesh_rpl_check(const struct mesh_rpl *r, const uint8_t src[3], uint16_t seq, uint32_t now_s)
{
	const struct mesh_rpl_ent *e = rpl_find((struct mesh_rpl *)r, src);

	if (!e) {
		return true;              /* never heard from: anything goes */
	}

	if (expired(e, now_s)) {
		return true;
	}

	int16_t d = (int16_t)(seq - e->seq);   /* serial arithmetic across the wrap */

	if (d > 0) {
		return true;              /* newer than anything accepted so far */
	}
	if (-d >= MESH_RPL_WINDOW) {
		return false;             /* older than the window: cannot tell, drop */
	}
	return (e->win & (1u << (uint32_t)(-d))) == 0;
}

void mesh_rpl_commit(struct mesh_rpl *r, const uint8_t src[3], uint16_t seq, uint32_t now_s)
{
	struct mesh_rpl_ent *e = rpl_find(r, src);

	if (!e) {
		/* New source: a free slot, else the one heard from longest ago. */
		struct mesh_rpl_ent *victim = &r->e[0];

		for (int i = 0; i < MESH_RPL_SLOTS; i++) {
			if (!r->e[i].used) {
				victim = &r->e[i];
				break;
			}
			if ((now_s - r->e[i].last_s) > (now_s - victim->last_s)) {
				victim = &r->e[i];
			}
		}
		e = victim;
		memcpy(e->src, src, 3);
		e->used = 1;
		e->seq = seq;
		e->win = 1;
		e->last_s = now_s;
		return;
	}

	if (expired(e, now_s)) {
		e->seq = seq;
		e->win = 1;
		e->last_s = now_s;
		return;
	}

	int16_t d = (int16_t)(seq - e->seq);

	if (d > 0) {
		e->win = (d >= MESH_RPL_WINDOW) ? 0 : (e->win << (uint32_t)d);
		e->win |= 1;
		e->seq = seq;
	} else if (-d < MESH_RPL_WINDOW) {
		e->win |= 1u << (uint32_t)(-d);
	}
	e->last_s = now_s;
}

void mesh_rpl_reset(struct mesh_rpl *r)
{
	memset(r, 0, sizeof(*r));
}
