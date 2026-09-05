#include "aes128.h"
#include <string.h>

/* The S-box is generated, not typed in: the multiplicative inverse in
 * GF(2^8) followed by the affine transform. A typo in a 256-entry table is
 * the kind of bug the FIPS-197 vector in the test catches, but not having
 * the table at all is simpler still. */
static uint8_t sbox[256];
static int sbox_ready;

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b) {
		if (b & 1) {
			p ^= a;
		}
		a = xtime(a);
		b >>= 1;
	}
	return p;
}

static uint8_t rotl8(uint8_t v, int n)
{
	return (uint8_t)((v << n) | (v >> (8 - n)));
}

static void sbox_init(void)
{
	for (int x = 0; x < 256; x++) {
		uint8_t inv = 0;

		if (x) {
			uint8_t r = 1, base = (uint8_t)x;
			int e = 254;

			while (e) {
				if (e & 1) {
					r = gmul(r, base);
				}
				base = gmul(base, base);
				e >>= 1;
			}
			inv = r;
		}
		sbox[x] = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^
				    rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
	}
	sbox_ready = 1;
}

static void expand_key(const uint8_t key[16], uint8_t rk[11][16])
{
	static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
					 0x20, 0x40, 0x80, 0x1b, 0x36};
	uint8_t w[44][4];

	for (int i = 0; i < 4; i++) {
		memcpy(w[i], &key[4 * i], 4);
	}
	for (int i = 4; i < 44; i++) {
		uint8_t t[4];

		memcpy(t, w[i - 1], 4);
		if (i % 4 == 0) {
			uint8_t t0 = t[0];

			t[0] = (uint8_t)(sbox[t[1]] ^ rcon[i / 4 - 1]);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[t0];
		}
		for (int j = 0; j < 4; j++) {
			w[i][j] = (uint8_t)(w[i - 4][j] ^ t[j]);
		}
	}
	for (int r = 0; r < 11; r++) {
		for (int c = 0; c < 4; c++) {
			memcpy(&rk[r][4 * c], w[4 * r + c], 4);
		}
	}
}

/* State is column-major: byte (row r, column c) is s[4*c + r]. */
static void sub_bytes(uint8_t s[16])
{
	for (int i = 0; i < 16; i++) {
		s[i] = sbox[s[i]];
	}
}

static void shift_rows(uint8_t s[16])
{
	uint8_t t[16];

	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			t[4 * c + r] = s[4 * ((c + r) % 4) + r];
		}
	}
	memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16])
{
	for (int c = 0; c < 4; c++) {
		uint8_t *col = &s[4 * c];
		uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];

		col[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
		col[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
		col[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
		col[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
	}
}

static void add_round_key(uint8_t s[16], const uint8_t rk[16])
{
	for (int i = 0; i < 16; i++) {
		s[i] ^= rk[i];
	}
}

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t rk[11][16];
	uint8_t s[16];

	if (!sbox_ready) {
		sbox_init();
	}
	expand_key(key, rk);
	memcpy(s, in, 16);
	add_round_key(s, rk[0]);
	for (int round = 1; round < 10; round++) {
		sub_bytes(s);
		shift_rows(s);
		mix_columns(s);
		add_round_key(s, rk[round]);
	}
	sub_bytes(s);
	shift_rows(s);
	add_round_key(s, rk[10]);
	memcpy(out, s, 16);
}
