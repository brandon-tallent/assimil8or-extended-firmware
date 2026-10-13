OUTPUT_ARCH(arm)
ENTRY(extension_header)
INCLUDE stock_abi.ld
INCLUDE overlay.ld
SECTIONS
{
    . = 0x9b010000;
    .extension : {
        KEEP(*(.extension_header))
        *(.text*) *(.rodata*) *(.data*)
        . = ALIGN(8);
    }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(8); }
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(ADDR(.extension) == 0x9b010000, "extension base moved")
ASSERT(. <= 0x9b100000, "extension exceeds its reserved range")
