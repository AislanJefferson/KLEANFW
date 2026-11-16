#include "stub.h"
#include "stub_mb.h"

void stub_boot(void)
{
    stub_gpio_init();
    stub_delay_init();

    /* The config bank must be resolved before the stock's first config read,
     * which happens right after we return. Cheap: 24 bytes of SPI (the marker)
     * plus a CRC over the internal image. */
    stub_flash_init();
    stub_bank_apply();

    /*
     * Fast path: the overwhelmingly common boot pays nothing more. A single
     * sample is enough to rule the selector out, so no 20 ms delay is added.
     */
    if (stub_key_poll() != STUB_KEY_MENU)
        return;

    /* Same rule as BOOT_GetMode(): MENU again 20 ms later, no PTT. */
    stub_delay_ms(20);

    if (stub_key_poll() != STUB_KEY_MENU || stub_ptt_pressed())
        return;

    stub_selector();

    /* EXIT from the selector: leave the panel as it is and let the stock boot.
     * Its own __main and display init take over from here. */
}
