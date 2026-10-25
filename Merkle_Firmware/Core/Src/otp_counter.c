#include "otp_counter.h"
#include "stm32f4xx_hal.h"

uint32_t otp_read_version(void)
{
    uint32_t version = 0;   /* 0 = nothing written yet */

    for (uint32_t i = 0; i < OTP_NUM_SLOTS; i++) {
        uint32_t v = *(volatile uint32_t*)(OTP_BASE + i * OTP_SLOT_SIZE);
        if (v == 0xFFFFFFFF) break;   /* hit erased region — stop */
        version = v;
    }
    return version;
}

void otp_update_version(uint32_t new_version)
{
    for (uint32_t i = 0; i < OTP_NUM_SLOTS; i++) {
        volatile uint32_t *slot =
            (volatile uint32_t*)(OTP_BASE + i * OTP_SLOT_SIZE);

        if (*slot == 0xFFFFFFFF) {
            HAL_FLASH_Unlock();
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              (uint32_t)slot,
                              new_version);
            HAL_FLASH_Lock();
            return;
        }
    }
    /* All slots full — in production this is a permanent lock state */
    /* For now: silently ignore (flash is full, version is at max) */
}

void otp_erase_for_testing(void)
{
    /* WARNING: removes all version history — dev only */
    FLASH_EraseInitTypeDef e = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Sector       = FLASH_SECTOR_7,   /* sector covering 0x0807F000 */
        .NbSectors    = 1,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t err = 0;
    HAL_FLASH_Unlock();
    HAL_FLASHEx_Erase(&e, &err);
    HAL_FLASH_Lock();
}
