#ifndef OTP_COUNTER_H
#define OTP_COUNTER_H

#include <stdint.h>

/*
 * Monotonic version counter emulated in flash.
 * Stored in last sector — never erased, only appended.
 *
 * Layout: slots of 4 bytes each.
 * 0xFFFFFFFF = unwritten (erased state).
 * Written slots hold actual version numbers in order.
 * Current version = last non-0xFFFFFFFF slot.
 *
 * For the loopback test, otp_read_version() returns 0
 * so any firmware version passes the anti-rollback check.
 */

#define OTP_BASE      0x0807F000UL   /* last 4KB of STM32F446RE flash */
#define OTP_SLOT_SIZE 4U
#define OTP_NUM_SLOTS 64U            /* 64 × 4B = 256 bytes */

/* Read current stored version. Returns 0 if no version written yet. */
uint32_t otp_read_version(void);

/* Write new version into next empty slot. Call only after successful OTA. */
void otp_update_version(uint32_t new_version);

/* Wipe OTP region — ONLY for development/testing. Remove before paper demo. */
void otp_erase_for_testing(void);

#endif
