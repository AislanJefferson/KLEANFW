/*
 * Entry point injected through the stock's SystemInit pointer
 * (literal at 0x080028E8 in the stock, patched to 0x08014401).
 *
 * The stock's Reset_Handler does:
 *     ldr r0, =SystemInit
 *     blx r0
 * so this runs inside that call, on the stock's own stack, before __main.
 * Returning normally resumes the stock: __main then initialises .data/.bss
 * and the stock boots as usual.
 */

    .syntax unified
    .cpu cortex-m0plus
    .thumb

    /* Stock SystemInit: sets SCB->VTOR to the stock's vector table. */
    .equ STOCK_SYSTEM_INIT, 0x080111E5

    .section .text.entry, "ax", %progbits
    .global stub_entry
    .type   stub_entry, %function
    .thumb_func
stub_entry:
    push    {lr}
    ldr     r0, =STOCK_SYSTEM_INIT
    blx     r0
    bl      stub_boot
    pop     {pc}
    .ltorg
    .size stub_entry, . - stub_entry
