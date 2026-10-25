#include "flash_writer.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* -- Sector tracking -----------------------------------------
   We erase each sector exactly once per OTA session.
   Bit i set = sector i already erased this session.          */
static uint8_t erased_sectors = 0;

void flash_writer_session_reset(void)
{
    erased_sectors = 0;   /* call at start of every fresh OTA */
}

void flash_writer_skip_erases(void)
{
    erased_sectors = 0xFF; /* call for recovery so it doesn't wipe intact blocks */
}

/* -- Address ? sector map (STM32F446RE) ---------------------  */
uint8_t flash_addr_to_sector(uint32_t addr)
{
    if (addr < 0x08004000) return 0;
    if (addr < 0x08008000) return 1;
    if (addr < 0x0800C000) return 2;
    if (addr < 0x08010000) return 3;
    if (addr < 0x08020000) return 4;
    if (addr < 0x08040000) return 5;
    if (addr < 0x08060000) return 6;
    return 7;
}

static uint32_t sector_to_hal(uint8_t s)
{
    /* HAL sector constants are FLASH_SECTOR_0 .. FLASH_SECTOR_7 */
    static const uint32_t map[] = {
        FLASH_SECTOR_0, FLASH_SECTOR_1, FLASH_SECTOR_2, FLASH_SECTOR_3,
        FLASH_SECTOR_4, FLASH_SECTOR_5, FLASH_SECTOR_6, FLASH_SECTOR_7
    };
    return (s < 8) ? map[s] : FLASH_SECTOR_7;
}

/* -- Erase one sector ----------------------------------------  */
bool flash_erase_sector(uint8_t sector)
{
    FLASH_EraseInitTypeDef e = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Sector       = sector_to_hal(sector),
        .NbSectors    = 1,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,   /* 2.7–3.6V */
    };
    uint32_t err = 0xFFFFFFFF;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &err);
    HAL_FLASH_Lock();

    return (st == HAL_OK && err == 0xFFFFFFFF);
}

/* -- Write one 4KB block -------------------------------------  */
bool flash_write_block(uint32_t addr, const uint8_t *data, uint32_t len)
{
    /* Safety: never overwrite Tier 1 (Sector 0) or Tier 2 + Metadata (Sector 1) */
    if (addr < 0x08008000) return false;
    /* Must be word-aligned */
    if (addr & 0x3)         return false;
    /* Must be a multiple of 4 bytes */
    if (len  & 0x3)         return false;

    uint8_t sector = flash_addr_to_sector(addr);

    /* Erase sector once per session */
    if (!(erased_sectors & (1 << sector))) {
        if (!flash_erase_sector(sector)) return false;
        erased_sectors |= (1 << sector);
    }

    /* Program word by word (32-bit) */
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, 4);   /* safe unaligned read */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              addr + i, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }
    HAL_FLASH_Lock();

    /* Readback verify */
    for (uint32_t i = 0; i < len; i++) {
        if (*(volatile uint8_t*)(addr + i) != data[i]) return false;
    }
    return true;
}

/* -- Write metadata sector -----------------------------------
   Layout at META_BASE:
     [0x000]  root        32 bytes
     [0x020]  num_blocks   2 bytes
     [0x022]  version      4 bytes
     [0x026]  leaves      num_blocks × 32 bytes               */
bool flash_init_metadata(uint32_t version, uint16_t num_blocks, const hash_t *root)
{
    /* Erase sector 1 (contains META_BASE) */
    if (!flash_erase_sector(1)) return false;

    HAL_FLASH_Unlock();

    /* Helper: write arbitrary bytes as words */
    #define WR_WORDS(base_addr, src, nbytes)                      \
        do {                                                       \
            const uint8_t *_s = (const uint8_t*)(src);           \
            uint32_t _n = (nbytes);                               \
            uint32_t _a = (base_addr);                            \
            for (uint32_t _i = 0; _i < _n; _i += 4) {           \
                uint32_t _w = 0xFFFFFFFF;                         \
                memcpy(&_w, _s + _i,                             \
                       (_n - _i >= 4) ? 4 : (_n - _i));          \
                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,     \
                                      _a + _i, _w) != HAL_OK) {  \
                    HAL_FLASH_Lock(); return false;               \
                }                                                  \
            }                                                      \
        } while(0)

    /* Write magic number FIRST so we can detect valid metadata */
    uint32_t magic = META_MAGIC_VALUE;
    WR_WORDS(META_MAGIC_ADDR, &magic, 4);

    WR_WORDS(META_ROOT_ADDR,    root->h,    HASH_LEN);

    uint8_t nb_buf[4] = { num_blocks & 0xFF,
                           (num_blocks >> 8) & 0xFF, 0xFF, 0xFF };
    WR_WORDS(META_NBLOCKS_ADDR, nb_buf, 4);

    uint8_t ver_buf[4] = { version & 0xFF,
                            (version >> 8) & 0xFF,
                            (version >> 16) & 0xFF,
                            (version >> 24) & 0xFF };
    WR_WORDS(META_VERSION_ADDR, ver_buf, 4);

    /* Status is implicitly IN_PROGRESS (0xFFFFFFFF = erased state).
       flash_mark_complete() will program it to 0x00000000 on success. */

    HAL_FLASH_Lock();
    return true;
}

bool flash_mark_complete(void) {
    HAL_FLASH_Unlock();
    uint32_t status = META_STATUS_COMPLETE;
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, META_STATUS_ADDR, status) != HAL_OK) {
        HAL_FLASH_Lock(); return false;
    }
    HAL_FLASH_Lock();
    return true;
}

/* -- Update a single leaf during recovery --------------------
   Leaf slot is already erased (whole META sector erased on OTA).
   Cannot re-erase without losing other leaves.
   Solution: re-write entire metadata. Caller passes full leaf array. */
bool flash_update_leaf(uint16_t block_id, const hash_t *new_leaf)
{
    /* Read existing leaves into RAM, patch, rewrite metadata */
    /* Caller (tier2_main.c) holds the full leaf array in RAM during
       recovery, so this function is a thin wrapper there.
       Here we just write the one slot — valid because sector was
       already erased at OTA start and we only write 0?1 bits.       */
    uint32_t slot_addr = META_LEAVES_ADDR + (uint32_t)block_id * HASH_LEN;

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < HASH_LEN; i += 4) {
        uint32_t word;
        memcpy(&word, new_leaf->h + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              slot_addr + i, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }
    HAL_FLASH_Lock();
    return true;
}
