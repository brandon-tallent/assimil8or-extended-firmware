OUTPUT_ARCH(arm)
ENTRY(patch_startup)
INCLUDE stock_abi.ld
SECTIONS
{
    . = 0x8006e4f8;
    .patch : {
        KEEP(*(.identity))
        *(.text*) *(.rodata*) *(.data*)
        . = ALIGN(4);
    }
    /* Zeroed storage (patch.h PATCH_STORAGE_*): outside the image, cleared by
     * the reset entry before any other patch code runs. */
    . = 0x9b001000;
    .bss (NOLOAD) : {
        patch_storage_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        patch_storage_end = .;
    }
    /* Blocks the patcher places between copied stock ranges (convolution.S);
     * tools/make_patch.py reads them from here. */
    .splice.preload 0 : { KEEP(*(.splice.preload)) }
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(ADDR(.patch) == 0x8006e4f8, "patch marker moved")
ASSERT(ADDR(.patch) + SIZEOF(.patch) <= 0x8007ba78, "patch exceeds reclaimed demangler")
ASSERT(patch_storage_end <= 0x9b010000, "patch storage reaches the extension image")
