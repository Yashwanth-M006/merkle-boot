/* tier3_main.c – Dummy Tier 3 application for OTA testing
   Blinks LD2 (PA5) to confirm the firmware is running after OTA.
*/
#include <stdint.h>

#define RCC_AHB1ENR  (*(volatile uint32_t*)0x40023830)
#define GPIOA_MODER  (*(volatile uint32_t*)0x40020000)
#define GPIOA_ODR    (*(volatile uint32_t*)0x40020014)

static void delay(volatile uint32_t n) { while (n--); }

void Reset_Handler(void)
{
    /* Enable GPIOA clock */
    RCC_AHB1ENR |= (1U << 0);
    /* PA5 → output (MODER bits 11:10 = 01) */
    GPIOA_MODER &= ~(3U << 10);
    GPIOA_MODER |=  (1U << 10);

    /* Blink LD2 rapidly so we can confirm Tier 3 is alive */
    while (1) {
        GPIOA_ODR ^= (1U << 5);
        delay(200000);
    }
}

/* The 300 KB payload is appended by build.py after compilation. */
