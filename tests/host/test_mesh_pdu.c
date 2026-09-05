/* Host tests for src/app/mesh_pdu.c: the mesh wire format, its AES-CMAC and
 * the per-source replay window. The AES block comes from aes128.c here; on
 * the tag it is the BLE controller's. */
#include "test.h"
#include "aes128.h"
#include "mesh_pdu.h"

static int aes_fn(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	aes128_encrypt_block(key, in, out);
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void unhex(const char *hex, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		out[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
	}
}

static int eq_hex(const uint8_t *bin, const char *hex, size_t n)
{
	uint8_t want[64];

	unhex(hex, want, n);
	return memcmp(bin, want, n) == 0;
}

/* ── the test's own AES, against FIPS-197 appendix C.1 ─────────────────── */
static void test_aes_fips197(void)
{
	uint8_t key[16], pt[16], ct[16];

	unhex("000102030405060708090a0b0c0d0e0f", key, 16);
	unhex("00112233445566778899aabbccddeeff", pt, 16);
	aes128_encrypt_block(key, pt, ct);
	T_ASSERT(eq_hex(ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16));
}

/* ── AES-CMAC against the RFC 4493 vectors ──────────────────────────────── */
static void test_cmac_rfc4493(void)
{
	uint8_t key[16], msg[64], mac[16];

	unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
	unhex("6bc1bee22e409f96e93d7e117393172a"
	      "ae2d8a571e03ac9c9eb76fac45af8e51"
	      "30c81c46a35ce411e5fbc1191a0a52ef"
	      "f69f2445df4f9b17ad2b417be66c3710", msg, 64);

	mesh_cmac(aes_fn, key, msg, 0, mac);
	T_ASSERT_MSG(eq_hex(mac, "bb1d6929e95937287fa37d129b756746", 16), "len 0");
	mesh_cmac(aes_fn, key, msg, 16, mac);
	T_ASSERT_MSG(eq_hex(mac, "070a16b46b4d4144f79bdd9dd04a287c", 16), "len 16");
	mesh_cmac(aes_fn, key, msg, 40, mac);
	T_ASSERT_MSG(eq_hex(mac, "dfa66747de9ae63030ca32611497c827", 16), "len 40");
	mesh_cmac(aes_fn, key, msg, 64, mac);
	T_ASSERT_MSG(eq_hex(mac, "51f0bebf7e3b9d92fc49741779363cfe", 16), "len 64");

	/* The subkey cache must not leak across keys. */
	uint8_t key2[16];

	unhex("000102030405060708090a0b0c0d0e0f", key2, 16);
	mesh_cmac(aes_fn, key2, msg, 16, mac);
	T_ASSERT(!eq_hex(mac, "070a16b46b4d4144f79bdd9dd04a287c", 16));
	mesh_cmac(aes_fn, key, msg, 16, mac);
	T_ASSERT_MSG(eq_hex(mac, "070a16b46b4d4144f79bdd9dd04a287c", 16), "back to key 1");
}

/* ── PDU build / parse / verify ─────────────────────────────────────────── */
static const uint8_t KEY[16] = {0x9e, 0x1c, 0x47, 0xb3, 0x52, 0xa8, 0x0f, 0xd6,
				0x71, 0x2b, 0xc4, 0x88, 0x3a, 0xe5, 0x96, 0x10};
static const uint8_t SRC[3] = {0xAA, 0xBB, 0xCC};
static const uint8_t DST_ID[3] = {0x11, 0x22, 0x33};

static void test_payload_budget(void)
{
	T_ASSERT_EQ(mesh_pdu_max_payload(MESH_PDU_DST_ALL), 14);
	T_ASSERT_EQ(mesh_pdu_max_payload(MESH_PDU_DST_GROUP), 13);
	T_ASSERT_EQ(mesh_pdu_max_payload(MESH_PDU_DST_ID), 11);
	T_ASSERT_EQ(mesh_pdu_dst_len(0x7F), 0);   /* unknown type: no address */
}

static void test_build_parse_roundtrip(void)
{
	const uint8_t types[3] = {MESH_PDU_DST_ALL, MESH_PDU_DST_GROUP, MESH_PDU_DST_ID};
	const uint8_t grp = 5;

	for (int t = 0; t < 3; t++) {
		uint8_t dst_type = types[t];
		const uint8_t *dst = dst_type == MESH_PDU_DST_GROUP ? &grp : DST_ID;
		uint8_t plen = mesh_pdu_max_payload(dst_type);
		uint8_t payload[16];
		uint8_t pdu[MESH_PDU_MAX + 4];
		struct mesh_pdu_view v;

		for (int i = 0; i < plen; i++) {
			payload[i] = (uint8_t)('0' + i);
		}
		memset(pdu, 0xEE, sizeof(pdu));

		int len = mesh_pdu_build(pdu, 4, 0x1234, SRC, dst_type, dst,
					 0x19, payload, plen, aes_fn, KEY);

		T_ASSERT_EQ(len, MESH_PDU_MAX);           /* a full payload fills the adv */
		T_ASSERT_EQ(pdu[MESH_PDU_MAX], 0xEE);     /* and not a byte more */
		T_ASSERT(mesh_pdu_parse(pdu, (uint8_t)len, &v));
		T_ASSERT_EQ(v.ttl, 4);
		T_ASSERT_EQ(v.seq, 0x1234);
		T_ASSERT(memcmp(v.src, SRC, 3) == 0);
		T_ASSERT_EQ(v.dst_type, dst_type);
		T_ASSERT_EQ(v.dst_len, mesh_pdu_dst_len(dst_type));
		T_ASSERT(v.dst_len == 0 || memcmp(v.dst, dst, v.dst_len) == 0);
		T_ASSERT_EQ(v.opcode, 0x19);
		T_ASSERT_EQ(v.plen, plen);
		T_ASSERT(memcmp(v.payload, payload, plen) == 0);
		T_ASSERT_EQ(v.body_len, len - MESH_MAC_LEN);
		T_ASSERT(mesh_pdu_verify(pdu, (uint8_t)len, aes_fn, KEY));

		/* One byte over budget is refused, never truncated. */
		T_ASSERT_EQ(mesh_pdu_build(pdu, 4, 1, SRC, dst_type, dst, 0x19,
					   payload, (uint8_t)(plen + 1), aes_fn, KEY), -1);
	}

	/* Empty payload is fine. */
	uint8_t pdu[MESH_PDU_MAX];
	int len = mesh_pdu_build(pdu, 1, 7, SRC, MESH_PDU_DST_ALL, NULL, 0x02, NULL, 0, aes_fn, KEY);

	T_ASSERT_EQ(len, MESH_HDR_MIN + 1 + MESH_MAC_LEN);
	T_ASSERT(mesh_pdu_verify(pdu, (uint8_t)len, aes_fn, KEY));
}

static void test_mac_covers_everything_but_ttl(void)
{
	uint8_t pdu[MESH_PDU_MAX];
	int len = mesh_pdu_build(pdu, 4, 42, SRC, MESH_PDU_DST_GROUP, (const uint8_t *)"\x05",
				 0x19, (const uint8_t *)"3", 1, aes_fn, KEY);

	T_ASSERT(len > 0);

	/* Relays rewrite the TTL: the MAC must survive that. */
	for (uint8_t ttl = 0; ttl < 16; ttl++) {
		uint8_t copy[MESH_PDU_MAX];
		struct mesh_pdu_view v;

		memcpy(copy, pdu, (size_t)len);
		mesh_pdu_set_ttl(copy, ttl);
		T_ASSERT(mesh_pdu_parse(copy, (uint8_t)len, &v));
		T_ASSERT_EQ(v.ttl, ttl);
		T_ASSERT(mesh_pdu_verify(copy, (uint8_t)len, aes_fn, KEY));
	}

	/* Every other bit of the body is authenticated. Byte 1's high nibble is
	 * the version and is covered; only the TTL nibble is masked. */
	for (int i = 0; i < len; i++) {
		for (int bit = 0; bit < 8; bit++) {
			uint8_t copy[MESH_PDU_MAX];
			bool ttl_bit = (i == 1 && bit < 4);

			memcpy(copy, pdu, (size_t)len);
			copy[i] ^= (uint8_t)(1u << bit);
			if (i == 0) {
				/* magic: rejected at parse, verify must say no too */
				T_ASSERT(!mesh_pdu_verify(copy, (uint8_t)len, aes_fn, KEY));
			} else {
				T_ASSERT_MSG(mesh_pdu_verify(copy, (uint8_t)len, aes_fn, KEY) == ttl_bit,
					     "byte %d bit %d", i, bit);
			}
		}
	}

	/* Wrong key. */
	uint8_t other[16];

	memcpy(other, KEY, 16);
	other[15] ^= 1;
	T_ASSERT(!mesh_pdu_verify(pdu, (uint8_t)len, aes_fn, other));
}

static void test_parse_rejects_malformed(void)
{
	uint8_t pdu[MESH_PDU_MAX + 1];
	struct mesh_pdu_view v;
	int len = mesh_pdu_build(pdu, 4, 1, SRC, MESH_PDU_DST_ID, DST_ID, 0x19,
				 (const uint8_t *)"12345678901", 11, aes_fn, KEY);

	T_ASSERT_EQ(len, MESH_PDU_MAX);
	T_ASSERT(mesh_pdu_parse(pdu, (uint8_t)len, &v));

	T_ASSERT(!mesh_pdu_parse(pdu, MESH_HDR_MIN + MESH_MAC_LEN, &v));      /* no room for opcode */
	T_ASSERT(!mesh_pdu_parse(pdu, MESH_PDU_MAX + 1, &v));                 /* too long */
	/* Minimum length for ALL, but this PDU says ID: the 3-byte address eats
	 * the opcode and runs into the MAC. */
	T_ASSERT(!mesh_pdu_parse(pdu, MESH_HDR_MIN + 1 + MESH_MAC_LEN, &v));
	/* The shortest ID PDU still parses. */
	T_ASSERT(mesh_pdu_parse(pdu, MESH_HDR_MIN + 3 + 1 + MESH_MAC_LEN, &v));
	T_ASSERT_EQ(v.plen, 0);

	pdu[0] = 0xE4;
	T_ASSERT(!mesh_pdu_parse(pdu, (uint8_t)len, &v));
}

/* ── Replay list ────────────────────────────────────────────────────────── */
static const uint8_t A[3] = {1, 1, 1};
static const uint8_t B[3] = {2, 2, 2};

static void test_rpl_basic(void)
{
	struct mesh_rpl r;

	mesh_rpl_reset(&r);
	T_ASSERT(mesh_rpl_check(&r, A, 100, 0));         /* unknown source */
	mesh_rpl_commit(&r, A, 100, 0);
	T_ASSERT(!mesh_rpl_check(&r, A, 100, 1));        /* exact duplicate (relay echo) */
	T_ASSERT(mesh_rpl_check(&r, A, 101, 1));         /* next */
	T_ASSERT(mesh_rpl_check(&r, B, 100, 1));         /* other source, own window */
	mesh_rpl_commit(&r, A, 101, 1);
	T_ASSERT(!mesh_rpl_check(&r, A, 100, 2));
	T_ASSERT(!mesh_rpl_check(&r, A, 101, 2));
	T_ASSERT(mesh_rpl_check(&r, A, 99, 2));          /* late arrival via a longer path */
	mesh_rpl_commit(&r, A, 99, 2);
	T_ASSERT(!mesh_rpl_check(&r, A, 99, 3));         /* ...once */
	T_ASSERT(mesh_rpl_check(&r, A, 98, 3));
}

static void test_rpl_window_edges(void)
{
	struct mesh_rpl r;

	mesh_rpl_reset(&r);
	mesh_rpl_commit(&r, A, 100, 0);
	T_ASSERT(mesh_rpl_check(&r, A, 100 - (MESH_RPL_WINDOW - 1), 1));   /* oldest inside */
	T_ASSERT(!mesh_rpl_check(&r, A, 100 - MESH_RPL_WINDOW, 1));        /* just outside */
	T_ASSERT(!mesh_rpl_check(&r, A, 1, 1));                            /* far behind */

	/* A big forward jump clears the bitmap: everything just below the new
	 * high mark is unseen and still inside the window. */
	mesh_rpl_commit(&r, A, 1000, 1);
	T_ASSERT(mesh_rpl_check(&r, A, 999, 2));
	T_ASSERT(mesh_rpl_check(&r, A, 1000 - (MESH_RPL_WINDOW - 1), 2));
	T_ASSERT(!mesh_rpl_check(&r, A, 1000 - MESH_RPL_WINDOW, 2));
	T_ASSERT(!mesh_rpl_check(&r, A, 100, 2));                          /* old high mark: gone */

	/* A small jump shifts the bitmap and keeps history. */
	mesh_rpl_commit(&r, A, 1003, 3);
	T_ASSERT(!mesh_rpl_check(&r, A, 1000, 3));
	T_ASSERT(mesh_rpl_check(&r, A, 1001, 3));
	T_ASSERT(mesh_rpl_check(&r, A, 1002, 3));
}

static void test_rpl_wraps(void)
{
	struct mesh_rpl r;

	mesh_rpl_reset(&r);
	mesh_rpl_commit(&r, A, 65535, 0);
	T_ASSERT(mesh_rpl_check(&r, A, 0, 1));           /* 0 follows 65535 */
	T_ASSERT(mesh_rpl_check(&r, A, 10, 1));
	mesh_rpl_commit(&r, A, 2, 1);
	T_ASSERT(!mesh_rpl_check(&r, A, 65535, 2));      /* now inside the window and seen */
	T_ASSERT(mesh_rpl_check(&r, A, 65534, 2));       /* inside, unseen */
	T_ASSERT(!mesh_rpl_check(&r, A, 2, 2));
	T_ASSERT(mesh_rpl_check(&r, A, 3, 2));
	T_ASSERT(!mesh_rpl_check(&r, A, 65535 - 40, 2)); /* outside */
}

static void test_rpl_forgets_after_silence(void)
{
	struct mesh_rpl r;

	mesh_rpl_reset(&r);
	mesh_rpl_commit(&r, A, 5000, 0);
	/* A restarted source (old firmware, wiped settings) starts low again.
	 * Until it has been silent a week that is indistinguishable from a
	 * replay, and treated as one. */
	T_ASSERT(!mesh_rpl_check(&r, A, 3, 100));
	T_ASSERT(!mesh_rpl_check(&r, A, 3, MESH_RPL_EXPIRE_S - 1));
	T_ASSERT(!mesh_rpl_check(&r, A, 4900, MESH_RPL_EXPIRE_S - 1));
	T_ASSERT(!mesh_rpl_check(&r, A, 5000, MESH_RPL_EXPIRE_S - 1));
	/* Silent a week: forgotten, anything goes — the documented trade. */
	T_ASSERT(mesh_rpl_check(&r, A, 3, MESH_RPL_EXPIRE_S));
	T_ASSERT(mesh_rpl_check(&r, A, 4900, MESH_RPL_EXPIRE_S));
	T_ASSERT(mesh_rpl_check(&r, A, 5000, MESH_RPL_EXPIRE_S));

	mesh_rpl_commit(&r, A, 3, MESH_RPL_EXPIRE_S);
	T_ASSERT(!mesh_rpl_check(&r, A, 3, MESH_RPL_EXPIRE_S + 1));    /* fresh window */
	T_ASSERT(mesh_rpl_check(&r, A, 4, MESH_RPL_EXPIRE_S + 1));
	T_ASSERT(mesh_rpl_check(&r, A, 1, MESH_RPL_EXPIRE_S + 1));     /* inside it, unseen */
	T_ASSERT(mesh_rpl_check(&r, A, 5000, MESH_RPL_EXPIRE_S + 1));  /* "newer" again: the price */
	/* The silence clock restarted with the commit. */
	mesh_rpl_commit(&r, A, 40, MESH_RPL_EXPIRE_S + 1);
	T_ASSERT(!mesh_rpl_check(&r, A, 2, MESH_RPL_EXPIRE_S + 2));
	T_ASSERT(mesh_rpl_check(&r, A, 2, 2 * MESH_RPL_EXPIRE_S + 1));
}

static void test_rpl_eviction(void)
{
	struct mesh_rpl r;
	uint8_t src[MESH_RPL_SLOTS + 1][3];

	mesh_rpl_reset(&r);
	for (int i = 0; i <= MESH_RPL_SLOTS; i++) {
		src[i][0] = 0x10; src[i][1] = 0x20; src[i][2] = (uint8_t)i;
	}
	/* Fill every slot; source 3 was heard from most recently, 0 longest ago. */
	for (int i = 0; i < MESH_RPL_SLOTS; i++) {
		mesh_rpl_commit(&r, src[i], 500, (uint32_t)(10 + i));
	}
	mesh_rpl_commit(&r, src[0], 501, 50);   /* source 0 speaks again: now newest */
	for (int i = 0; i < MESH_RPL_SLOTS; i++) {
		T_ASSERT(!mesh_rpl_check(&r, src[i], 500, 60));
	}
	/* A ninth source evicts the one silent longest: source 1 (t=11). */
	mesh_rpl_commit(&r, src[MESH_RPL_SLOTS], 7, 60);
	T_ASSERT(mesh_rpl_check(&r, src[1], 500, 61));          /* forgotten */
	T_ASSERT(!mesh_rpl_check(&r, src[0], 501, 61));         /* kept */
	T_ASSERT(!mesh_rpl_check(&r, src[2], 500, 61));         /* kept */
	T_ASSERT(!mesh_rpl_check(&r, src[MESH_RPL_SLOTS], 7, 61));
}

static void test_rpl_check_is_pure(void)
{
	struct mesh_rpl r, before;

	mesh_rpl_reset(&r);
	mesh_rpl_commit(&r, A, 10, 0);
	before = r;
	(void)mesh_rpl_check(&r, A, 11, 1);
	(void)mesh_rpl_check(&r, B, 11, 1);
	T_ASSERT(memcmp(&before, &r, sizeof(r)) == 0);   /* a forgery cannot poison it */
}

int main(void)
{
	T_RUN(test_aes_fips197);
	T_RUN(test_cmac_rfc4493);
	T_RUN(test_payload_budget);
	T_RUN(test_build_parse_roundtrip);
	T_RUN(test_mac_covers_everything_but_ttl);
	T_RUN(test_parse_rejects_malformed);
	T_RUN(test_rpl_basic);
	T_RUN(test_rpl_window_edges);
	T_RUN(test_rpl_wraps);
	T_RUN(test_rpl_forgets_after_silence);
	T_RUN(test_rpl_eviction);
	T_RUN(test_rpl_check_is_pure);
	return t_report("mesh_pdu");
}
