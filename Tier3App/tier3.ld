/* tier3.ld – Linker script for Tier 3 test application
   Placed at TIER3_BASE = 0x08008000 (sectors 2-7, 480 KB available)
*/

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08008000, LENGTH = 480K   /* sectors 2-7 */
    RAM   (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
    /* ── Vector table ─────────────────────────────────────────── */
    .isr_vector : {
        KEEP(*(.isr_vector))
    } >FLASH

    /* ── Code & read-only data ───────────────────────────────── */
    .text : {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    /* ── Large test payload (block-indexed fill) ─────────────── */
    .payload : {
        . = ALIGN(4);
        *(.payload)
        . = ALIGN(4);
    } >FLASH

    /* Discard debug / exception-table cruft */
    /DISCARD/ : {
        libc.a  (*)
        libm.a  (*)
        libgcc.a(*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
