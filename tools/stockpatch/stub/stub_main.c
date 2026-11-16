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

#include "stub.h"
#include "stub_mb.h"

static void stub_boot_body(void)
{
    stub_gpio_init();
    stub_delay_init();

    /* The config bank must be resolved before the stock's first config read,
     * which happens right after we return. Cheap: 24 bytes of SPI (the marker)
     * plus a CRC over the internal image. */
    stub_flash_init();
    const uint32_t base = stub_bank_apply();

    /*
     * Fast path: the overwhelmingly common boot pays nothing more. A single
     * sample is enough to rule the selector out, so no 20 ms delay is added.
     * Same rule as BOOT_GetMode(): MENU again 20 ms later, no PTT.
     */
    if (stub_key_poll() == STUB_KEY_MENU) {
        stub_delay_ms(20);
        if (stub_key_poll() == STUB_KEY_MENU && !stub_ptt_pressed())
            stub_selector(); /* returns on EXIT; a restore resets the MCU */
    }

    /* After the selector, not before: a restore chosen there must not pay for
     * (or be blocked by) a seed of the bank it is leaving. */
    stub_bank_seed(base);
}

void stub_boot(void)
{
    stub_hw_save();
    stub_boot_body();
    /* Every peripheral, pin and clock the stub touched goes back to the state
     * the stock would have found without the stub. */
    stub_hw_restore();
}
