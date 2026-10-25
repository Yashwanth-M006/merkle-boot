#ifndef FLASH_WRITER_H
#define FLASH_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include "merkle_verify.h"   /* for hash_t, HASH_LEN */

/* Tier 3 application base */
#define TIER3_BASE       0x08008000UL

/* Metadata region — entirely contained in Sector 1 */
#define META_BASE        0x08004000UL
#define META_MAGIC_OFF   0x000        /* 4 bytes  — must equal META_MAGIC_VALUE */
#define META_ROOT_OFF    0x004        /* 32 bytes */
#define META_NBLOCKS_OFF 0x024        /* 4 bytes (2 data + 2 pad) */
#define META_VERSION_OFF 0x028        /* 4 bytes  */
#define META_STATUS_OFF  0x02C        /* 4 bytes  */
#define META_LEAVES_OFF  0x030        /* N x 32 bytes */

#define META_MAGIC_ADDR   (META_BASE + META_MAGIC_OFF)
#define META_ROOT_ADDR    (META_BASE + META_ROOT_OFF)
#define META_NBLOCKS_ADDR (META_BASE + META_NBLOCKS_OFF)
#define META_VERSION_ADDR (META_BASE + META_VERSION_OFF)
#define META_STATUS_ADDR  (META_BASE + META_STATUS_OFF)
#define META_LEAVES_ADDR  (META_BASE + META_LEAVES_OFF)

#define META_MAGIC_VALUE        0xDEADF00DUL
/* NOR flash is erased to 0xFF. COMPLETE requires programming a word to 0.
   IN_PROGRESS is the natural erased state (0xFFFFFFFF) — no explicit write needed. */
#define META_STATUS_COMPLETE    0x00000000UL
#define META_STATUS_IN_PROGRESS 0xFFFFFFFFUL

/* Write one 4KB block to flash at addr. Erases containing sector first
   if the sector has not been erased in this OTA session.
   addr must be 4-byte aligned.                                         */
bool flash_write_block(uint32_t addr, const uint8_t *data, uint32_t len);
void flash_writer_session_reset(void);
void flash_writer_skip_erases(void);

/* Write OTA metadata (root, num_blocks, version, all leaves).
   Erases the metadata sector before writing.                           */
bool flash_init_metadata(uint32_t version,
                           uint16_t num_blocks,
                           const hash_t *root);
bool flash_mark_complete(void);

/* Update a single leaf entry in metadata (used during recovery).       */
bool flash_update_leaf(uint16_t block_id, const hash_t *leaf);

/* Erase a specific sector by number (0–7). Dev/test use.               */
bool flash_erase_sector(uint8_t sector);

/* Return the sector number for a given flash address.                  */
uint8_t flash_addr_to_sector(uint32_t addr);

void flash_writer_session_reset(void);

#endif
