/*
 * Slot access for the stock stub.
 *
 * The multiboot format itself is not duplicated here: validation, the
 * active-state marker and the restore path are App/driver/mb_flash_logic.c,
 * built into the stub unchanged, so the on-flash format stays byte-identical
 * to what KLEANFW writes.
 */

#include "stub_mb.h"

#include <stddef.h>

#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_spi.h"

/* -------------------------------------------------------------------------- */
/* Small libc bits                                                           */
/*                                                                            */
/* The stub is linked with -nostdlib; GCC still emits calls to these for      */
/* struct assignment and array zeroing in mb_flash_logic.c.                   */
/* -------------------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    while (n--) {
        if (*x != *y)
            return *x < *y ? -1 : 1;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* -------------------------------------------------------------------------- */
/* PY25Q16 shims                                                             */
/*                                                                            */
/* mb_flash_logic.c only needs these two to keep the stock's sector cache     */
/* coherent. The stub never uses that cache: it talks to SPI2 directly and    */
/* the stock re-initialises its own cache when it boots.                     */
/* -------------------------------------------------------------------------- */

void PY25Q16_Flush(void)
{
}

void PY25Q16_InvalidateCache(void)
{
}

/* -------------------------------------------------------------------------- */
/* SPI2                                                                       */
/* -------------------------------------------------------------------------- */

void stub_flash_init(void)
{
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

    LL_GPIO_InitTypeDef gpio;
    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull       = LL_GPIO_PULL_UP;

    gpio.Pin       = LL_GPIO_PIN_0; /* SCK */
    gpio.Alternate = LL_GPIO_AF8_SPI2;
    LL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin       = LL_GPIO_PIN_1 | LL_GPIO_PIN_2; /* MOSI, MISO */
    gpio.Alternate = LL_GPIO_AF9_SPI2;
    LL_GPIO_Init(GPIOA, &gpio);

    /* Chip select: PA3, idle high, driven by mb_flash_logic.c. */
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_3);
    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_OUTPUT;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Pin        = LL_GPIO_PIN_3;
    LL_GPIO_Init(GPIOA, &gpio);
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_3);

    LL_SPI_InitTypeDef spi;
    LL_SPI_StructInit(&spi);
    spi.Mode              = LL_SPI_MODE_MASTER;
    spi.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi.ClockPhase        = LL_SPI_PHASE_2EDGE;
    spi.ClockPolarity     = LL_SPI_POLARITY_HIGH;
    spi.BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2;
    spi.BitOrder          = LL_SPI_MSB_FIRST;
    spi.NSS               = LL_SPI_NSS_SOFT;
    spi.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    LL_SPI_Init(SPI2, &spi);
    LL_SPI_Enable(SPI2);
}

/* -------------------------------------------------------------------------- */
/* Slots                                                                      */
/* -------------------------------------------------------------------------- */

void stub_scan_slots(mb_slot_header_t *headers, uint8_t *status)
{
    for (uint8_t slot = 0; slot < MB_SLOT_COUNT; slot++)
        status[slot] = MB_ValidateSlot(slot, &headers[slot], NULL);
}

uint8_t stub_running_slot(void)
{
    mb_state_t state;

    if (MB_ReadActiveState(&state) == MB_MARK_VALID &&
        state.firmware_slot < MB_SLOT_COUNT)
        return state.firmware_slot;

    return MB_SLOT_BACKUP;
}

uint8_t stub_restore_slot(uint8_t slot, uint8_t *progress_line)
{
    const uint8_t err = MB_SetActiveSlot(slot);

    if (err != MB_OK)
        return err;

    return MB_RestoreSlot(slot, progress_line);
}

/* Word read by the SPI trampolines in stub_hooks.S. Above the stock's initial
 * stack pointer (0x20002FA0); the stock never reads or writes it. */
#define BANK_WORD (*(volatile uint32_t *)0x20003FFCu)

void stub_bank_apply(void)
{
    mb_state_t state;
    uint32_t base = 0;

    /* The marker records which image should be running and which config bank
     * it owns. It is only trusted when the internal image really is that
     * image: a stock flashed straight over USB (no multiboot) must keep using
     * bank 0 - the KLEANFW Main config - and must not touch the marker. */
    if (MB_ReadActiveState(&state) == MB_MARK_VALID && MB_InternalMatchesState(&state))
        base = MB_BankBase(state.config_bank);

    BANK_WORD = base;
}

const char *stub_slot_error(uint8_t err)
{
    switch (err) {
    case MB_OK:
        return "OK";
    case MB_ERR_MAGIC:
        return "empty";
    case MB_ERR_VERSION:
        return "new header";
    case MB_ERR_NOT_COMMITTED:
        return "incomplete";
    case MB_ERR_SIZE:
        return "bad size";
    case MB_ERR_CRC:
        return "CRC ERROR";
    case MB_ERR_SPI:
        return "SPI ERROR";
    case MB_ERR_SLOT:
        return "bad index";
    case MB_ERR_AUTH:
        return "auth";
    case MB_ERR_RAM_LOAD:
        return "RAM LOAD ERROR";
    default:
        return "error";
    }
}
