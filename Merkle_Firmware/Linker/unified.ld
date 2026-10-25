/* 
 * unified.ld
 * 
 * Compile with: gcc -E -x c -P -DTIER1 unified.ld -o build/tier1_processed.ld
 * Then link with: gcc ... -T build/tier1_processed.ld
 */

#if defined(TIER1)
  #define FLASH_ORIGIN 0x08000000
  #define FLASH_LENGTH 16K
  #define RAM_LENGTH   8K
#elif defined(TIER2)
  #define FLASH_ORIGIN 0x08004000
  #define FLASH_LENGTH 16K
  #define RAM_LENGTH   8K
#elif defined(TIER3)
  #define FLASH_ORIGIN 0x08008000
  #define FLASH_LENGTH 440K
  #define RAM_LENGTH   128K
#else
  #error "Must define TIER1, TIER2, or TIER3"
#endif

MEMORY {
  FLASH (rx)  : ORIGIN = FLASH_ORIGIN, LENGTH = FLASH_LENGTH
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = RAM_LENGTH
}

SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text       : { *(.text*) *(.rodata*) } > FLASH
  .data       : { *(.data*) } > RAM AT > FLASH
  .bss        : { *(.bss*) *(COMMON) } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
