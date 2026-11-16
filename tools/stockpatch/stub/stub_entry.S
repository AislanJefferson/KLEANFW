/* Copyright 2026 AislanJefferson
 * https://github.com/AislanJefferson/KLEANFW
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

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

    .section .text.entry, "ax", %progbits
    .global stub_entry
    .type   stub_entry, %function
    .thumb_func
stub_entry:
    push    {lr}
    ldr     r0, stub_stock_system_init
    blx     r0
    bl      stub_boot
    pop     {pc}

/* The stock's own SystemInit (it only programs VTOR). Its address differs per
 * model (K5 V3 0x080111E5, K1 0x08011009), so one stub serves both: the build
 * leaves the erased-flash placeholder and patch_stock.py writes here the value
 * it replaces in the stock's literal at 0x080028E8. */
    .p2align 2
    .global stub_stock_system_init
    .type   stub_stock_system_init, %object
stub_stock_system_init:
    .word   0xFFFFFFFF
    .size stub_stock_system_init, 4
    .size stub_entry, stub_stock_system_init - stub_entry
