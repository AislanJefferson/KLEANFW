/*
 * Per-bank address remap for the stock's SPI flash driver.
 *
 * The stock emits the 24-bit address of every read/page-program/erase as three
 * bytes extracted from r4:
 *
 *     20 02 00 0e     lsls r0, r4, #8    ; lsrs r0, r0, #24   -> byte 2
 *     20 04 00 0e     lsls r0, r4, #16   ; lsrs r0, r0, #24   -> byte 1
 *
 * patch_stock.py replaces each of those 4-byte pairs with a bl to one of the
 * trampolines below. r4 itself is never modified, so the driver's sector-cache
 * tag comparison keeps seeing the logical address and the cache stays coherent.
 *
 * Rule, identical to PY25Q16_BANK_SHARED_FROM: everything below 0x10000 belongs
 * to a config bank and is offset by bank_base; calibration, the boot logo and
 * the marker are shared and never remapped.
 *
 * Cortex-M0+ has no 32-bit-register forms for ldr/adds, so the only scratch
 * available is a low register: r1 is saved around the call (r0 is the output by
 * contract, since the displaced instructions wrote it).
 */

    .syntax unified
    .cpu cortex-m0plus
    .thumb

/* Last word of RAM. Above the stock's initial stack pointer (0x20002FA0), and
 * no literal in the stock points at or above it. Written by stub_bank_apply(). */
    .equ BANK_WORD, 0x20003FFC

    .section .text.hooks, "ax", %progbits

/* Returns (addr + bank_base) >> 16 & 0xFF for addresses below 0x10000, and
 * addr >> 16 & 0xFF otherwise. */
    .global tramp_addr_hi
    .type   tramp_addr_hi, %function
    .thumb_func
tramp_addr_hi:
    push    {r1}
    ldr     r1, =BANK_WORD
    ldr     r1, [r1]
    lsrs    r0, r4, #16          /* Z set when the address is below 0x10000 */
    beq     1f
    movs    r1, #0
1:  adds    r0, r4, r1
    lsrs    r0, r0, #16
    uxtb    r0, r0
    pop     {r1}
    bx      lr

/* Same for the middle byte: (addr + bank_base) >> 8 & 0xFF. */
    .global tramp_addr_lo
    .type   tramp_addr_lo, %function
    .thumb_func
tramp_addr_lo:
    push    {r1}
    ldr     r1, =BANK_WORD
    ldr     r1, [r1]
    lsrs    r0, r4, #16
    beq     1f
    movs    r1, #0
1:  adds    r0, r4, r1
    lsrs    r0, r0, #8
    uxtb    r0, r0
    pop     {r1}
    bx      lr

    .ltorg
