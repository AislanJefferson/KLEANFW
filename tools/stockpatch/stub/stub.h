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

#ifndef STOCKPATCH_STUB_H
#define STOCKPATCH_STUB_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The stub is entered from the stock's Reset_Handler (a blx into
 * stub_entry) and returns to it, so the stock's __main still runs.
 *
 * Timing: the core is still on the reset clock at that point, which
 * stub_delay_init() reads back from the RCC registers.
 */

/* Mirrors enum KEY_Code_e in App/driver/keyboard.h: same numeric values so the
 * stub and the KLEANFW selector can share key handling if it ever moves. */
enum stub_key {
    STUB_KEY_0 = 0,
    STUB_KEY_1,
    STUB_KEY_2,
    STUB_KEY_3,
    STUB_KEY_4,
    STUB_KEY_5,
    STUB_KEY_6,
    STUB_KEY_7,
    STUB_KEY_8,
    STUB_KEY_9,
    STUB_KEY_MENU,
    STUB_KEY_UP,
    STUB_KEY_DOWN,
    STUB_KEY_EXIT,
    STUB_KEY_STAR,
    STUB_KEY_F,
    STUB_KEY_PTT,
    STUB_KEY_SIDE2,
    STUB_KEY_SIDE1,
    STUB_KEY_INVALID
};

void stub_boot(void);
void stub_selector(void);

/* Snapshot / restore of everything the stub reconfigures (stub_hw.c). */
void stub_hw_save(void);
void stub_hw_restore(void);

void stub_delay_init(void);
void stub_delay_us(uint32_t us);
void stub_delay_ms(uint32_t ms);

void stub_gpio_init(void);
bool stub_ptt_pressed(void);
int stub_key_poll(void);

#endif
