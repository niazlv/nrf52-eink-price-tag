#ifndef AES128_H
#define AES128_H
/* Software AES-128 block encryption for the host tests — the tag uses the
 * BLE controller's AES instead (bt_encrypt_be). Not for production use. */
#include <stdint.h>
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
#endif
