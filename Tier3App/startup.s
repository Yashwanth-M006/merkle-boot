/* startup.s – Minimal Cortex-M4 vector table for Tier 3 test app */
    .syntax unified
    .cpu    cortex-m4
    .thumb

    .section .isr_vector, "a", %progbits
    .global _vectors
_vectors:
    .word   0x20020000          /* Initial MSP: end of 128 KB SRAM */
    .word   Reset_Handler + 1   /* Reset vector (Thumb bit set) */
    .fill   62, 4, 0            /* NMI … SysTick → all default to 0 */
