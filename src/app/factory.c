#include "factory.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <pm_config.h>
#include <string.h>

/* nRF52 flash is memory-mapped (XIP), so the read-only provisioning block is
 * read directly at its partition address — no flash driver needed. */

#define FACTORY_MAGIC 0x46414331u /* "FAC1" */

/* flags bits */
#define FACTORY_F_AUTH_KEY BIT(0)  /* auth_key[] holds a provisioned key */

/* On-flash provisioning record. APPEND-ONLY: never reorder or resize existing
 * fields; grow only into reserved[] so a block written by an older provisioning
 * tool stays readable. crc32 covers everything after the crc field. */
struct factory_block {
	uint32_t magic;
	uint32_t crc;
	uint16_t version;
	uint8_t  serial_len;
	uint8_t  flags;
	char     serial[FACTORY_SERIAL_MAXLEN + 1];   /* NUL-terminated */
	uint8_t  auth_key[FACTORY_AUTH_KEY_LEN];
	uint8_t  reserved[24];
};

#define FACTORY_CRC_OFFSET offsetof(struct factory_block, version)

#if defined(PM_FACTORY_DATA_ADDRESS)
#define FACTORY_PRESENT 1
static const struct factory_block *const fb =
	(const struct factory_block *)PM_FACTORY_DATA_ADDRESS;
BUILD_ASSERT(sizeof(struct factory_block) <= PM_FACTORY_DATA_SIZE,
	     "factory_block exceeds factory_data partition");
#else
#define FACTORY_PRESENT 0
#endif

static bool block_valid(void)
{
#if FACTORY_PRESENT
	if (fb->magic != FACTORY_MAGIC) {
		return false;
	}
	uint32_t crc = crc32_ieee((const uint8_t *)fb + FACTORY_CRC_OFFSET,
				  sizeof(*fb) - FACTORY_CRC_OFFSET);
	return crc == fb->crc;
#else
	return false;
#endif
}

bool factory_is_provisioned(void)
{
	return block_valid();
}

const char *factory_serial(void)
{
#if FACTORY_PRESENT
	if (block_valid() && fb->serial_len <= FACTORY_SERIAL_MAXLEN) {
		return fb->serial;
	}
#endif
	return "";
}

const uint8_t *factory_auth_key(void)
{
#if FACTORY_PRESENT
	if (block_valid() && (fb->flags & FACTORY_F_AUTH_KEY)) {
		return fb->auth_key;
	}
#endif
	return NULL;
}
