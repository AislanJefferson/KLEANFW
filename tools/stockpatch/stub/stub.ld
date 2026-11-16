/*
 * Stub image appended after the stock firmware.
 *
 * The stock image ends at 0x0801405C; the stub is placed at 0x08014400 (1 KiB
 * aligned) and may grow up to the end of the internal flash at 0x08020000
 * (48,128 bytes). Nothing below the entry point is ever executed, and no
 * address of the stock is rewritten: the only patch in the stock is the
 * SystemInit literal at 0x080028E8.
 *
 * No .data: the stub runs before the stock's __main, so there is no loader to
 * copy an initialised section. Keep every constant in flash (.text/.rodata).
 *
 * RAM:
 *   0x20000000  4 KiB restore copier (mb_flash.c, .MBRamFunc), loaded from
 *               flash only right before the internal image is rewritten
 *   0x20001000  stub .bss (LCD framebuffer, slot headers)
 *   ...         the stock's own stack, already set up at SP = 0x20002FA0
 */

ENTRY(stub_entry)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08014400, LENGTH = 0x0000BC00
    RAM   (rw) : ORIGIN = 0x20000000, LENGTH = 0x00004000
}

SECTIONS
{
    .text : {
        KEEP(*(.text.entry))
        /* Referenced only from the stock, through the bl sites the patcher
         * injects, so nothing inside the stub keeps them alive. */
        KEEP(*(.text.hooks))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* The restore copier executes from RAM after being copied out of flash. */
    __stub_ramfunc_lma = LOADADDR(.text) + SIZEOF(.text);

    .mb_ramfunc 0x20000000 : AT (__stub_ramfunc_lma)
    {
        . = ALIGN(4);
        __mb_ramfunc_start = .;
        KEEP(*(.MBRamFunc))
        KEEP(*(.MBRamFunc*))
        . = ALIGN(4);
        __mb_ramfunc_end = .;
    }

    __mb_ramfunc_load_start = LOADADDR(.mb_ramfunc);

    /* Declared only so it can be asserted empty: nothing copies it. */
    .data : AT (__stub_ramfunc_lma + SIZEOF(.mb_ramfunc))
    {
        *(.data*)
    } > RAM

    .bss 0x20001000 (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.init)
        *(.fini)
    }

    ASSERT(SIZEOF(.data) == 0, "stub must not have .data: no loader runs before it")
    ASSERT(SIZEOF(.text) <= 0x0000BC00, "stub does not fit between 0x08014400 and 0x08020000")
    ASSERT(SIZEOF(.mb_ramfunc) <= 0x1000, "restore copier does not fit in its 4 KiB RAM slot")
    ASSERT(ADDR(.mb_ramfunc) + SIZEOF(.mb_ramfunc) <= 0x20001000,
           "restore copier overlaps the stub's .bss")
    ASSERT(ADDR(.bss) + SIZEOF(.bss) <= 0x20002000, "stub .bss reaches the stock's stack area")
    ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x08020000, "stub runs past the end of the internal flash")
}
