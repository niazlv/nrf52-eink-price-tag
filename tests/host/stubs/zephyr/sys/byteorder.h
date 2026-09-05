/* Host stand-in for <zephyr/sys/byteorder.h>: the two little-endian getters
 * the settings migrations use. */
#ifndef STUB_ZEPHYR_SYS_BYTEORDER_H
#define STUB_ZEPHYR_SYS_BYTEORDER_H

#include <stdint.h>

static inline uint16_t sys_get_le16(const uint8_t src[2])
{
	return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static inline uint64_t sys_get_le64(const uint8_t src[8])
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | src[i];
	}
	return v;
}

#endif
