/* JT-4000 application: flash after the 32 KiB bootloader, minus the 16-byte trailer. */
MEMORY {
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 0x18000 - 16
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}
/* The bootloader loads this value into SP and then pops 8 bytes from it before jumping
 * to the reset vector, so it must point at least 8 bytes below the end of RAM. */
_estack = ORIGIN(RAM) + LENGTH(RAM) - 16;
ENTRY(Reset_Handler)
SECTIONS {
    .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
    .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
