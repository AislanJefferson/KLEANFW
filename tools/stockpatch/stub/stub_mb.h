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

#ifndef STOCKPATCH_STUB_MB_H
#define STOCKPATCH_STUB_MB_H

#include <stdint.h>

#include "driver/mb_flash.h"

/* Bring up SPI2 in polled mode (no DMA, no interrupts) exactly as the flash
 * driver expects it. Required before any slot access. */
void stub_flash_init(void);

/* Fill headers/status with MB_ValidateSlot() for every slot. */
void stub_scan_slots(mb_slot_header_t *headers, uint8_t *status);

/* Slot the active-state marker says is running; 0 when unknown. */
uint8_t stub_running_slot(void);

/* Bind the slot to its own config bank and reflash the internal image.
 * Only returns on failure (success resets the MCU). `progress_line` is a
 * 128-byte buffer used for the on-panel progress gauge. */
uint8_t stub_restore_slot(uint8_t slot, uint8_t *progress_line);

/* Resolve which config bank this boot must use, publish its base address to
 * the SPI hooks and return it. Requires stub_flash_init(). */
uint32_t stub_bank_apply(void);

/* When the bank does not carry the stock's ownership mark yet (erased, or
 * holding another firmware's config), make it a copy of bank 0 - the Main
 * config the unpatched stock is known to boot from - and mark it. No-op for
 * bank 0, for a bank the stock already owns, or when bank 0 is erased. */
void stub_bank_seed(uint32_t base);

const char *stub_slot_error(uint8_t err);

#endif
